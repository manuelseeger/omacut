#include "ffmpeg.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <cmath>

namespace ffmpeg {

namespace {
// Upper bound on a synchronous probe so a hung/unresponsive file (e.g. a stalled
// network mount) can't freeze the caller — load() runs probe() on the UI thread.
constexpr int kProbeTimeoutMs = 15000;
// Poll granularity while waiting on a thumbnail child, so cancellation is prompt.
constexpr int kThumbPollMs = 50;
}

QString toolPath(const QString &tool) {
    return QStandardPaths::findExecutable(tool);
}

VideoInfo probe(const QString &path) {
    VideoInfo info;
    info.path = path;

    const QString ffprobe = toolPath("ffprobe");
    if (ffprobe.isEmpty()) {
        info.error = "`ffprobe` was not found on your PATH. Install ffmpeg.";
        return info;
    }

    QProcess proc;
    proc.start(ffprobe, {
        "-v", "error",
        "-print_format", "json",
        "-show_format",
        "-show_streams",
        path,
    });
    if (!proc.waitForFinished(kProbeTimeoutMs)) {
        proc.kill();
        proc.waitForFinished(-1);
        info.error = "ffprobe timed out reading this file.";
        return info;
    }

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        info.error = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        if (info.error.isEmpty())
            info.error = "ffprobe failed.";
        return info;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(proc.readAllStandardOutput());
    const QJsonObject root = doc.object();
    const QJsonArray streams = root.value("streams").toArray();
    QJsonObject stream;
    for (const QJsonValue &value : streams) {
        const QJsonObject candidate = value.toObject();
        const QString type = candidate.value("codec_type").toString();
        if (type == QStringLiteral("video") && info.videoStream < 0) {
            info.videoStream = candidate.value("index").toInt(-1);
            info.videoCodec = candidate.value("codec_name").toString();
            stream = candidate;
        } else if (type == QStringLiteral("audio") && info.audioStream < 0) {
            info.audioStream = candidate.value("index").toInt(-1);
            info.audioCodec = candidate.value("codec_name").toString();
        }
    }
    if (info.videoStream < 0) {
        info.error = "No video stream found in this file.";
        return info;
    }
    const QStringList mp4Video = {QStringLiteral("h264"), QStringLiteral("hevc"),
                                  QStringLiteral("mpeg4"), QStringLiteral("av1")};
    const QStringList mp4Audio = {QStringLiteral("aac"), QStringLiteral("mp3"),
                                  QStringLiteral("ac3"), QStringLiteral("eac3"),
                                  QStringLiteral("alac")};
    if (!mp4Video.contains(info.videoCodec))
        info.copyUnavailableReason = QStringLiteral("MP4 cannot copy the %1 video stream.")
                                         .arg(info.videoCodec.isEmpty() ? QStringLiteral("unknown") : info.videoCodec);
    else if (info.audioStream >= 0 && !mp4Audio.contains(info.audioCodec))
        info.copyUnavailableReason = QStringLiteral("MP4 cannot copy the %1 audio stream.")
                                         .arg(info.audioCodec.isEmpty() ? QStringLiteral("unknown") : info.audioCodec);

    info.width = stream.value("width").toInt();
    info.height = stream.value("height").toInt();

    // Duration can live on the stream or on the container.
    QString durationStr = stream.value("duration").toString();
    if (durationStr.isEmpty())
        durationStr = root.value("format").toObject().value("duration").toString();
    if (durationStr.isEmpty()) {
        info.error = "Could not determine the video duration.";
        return info;
    }

    info.duration = durationStr.toDouble();
    bool validStart = false;
    const double startTime = root.value("format").toObject().value("start_time")
                                 .toString().toDouble(&validStart);
    if (validStart && std::isfinite(startTime))
        info.containerStartSeconds = startTime;

    info.ok = info.duration > 0.0;
    if (!info.ok)
        info.error = "Video has a zero or invalid duration.";
    return info;
}

QImage thumbnail(const QString &path, double time, int height,
                 const std::atomic<bool> *cancel) {
    const QString ffmpeg = toolPath("ffmpeg");
    if (ffmpeg.isEmpty())
        return {};

    QProcess proc;
    proc.start(ffmpeg, {
        "-loglevel", "error",
        "-ss", QString::number(qMax(time, 0.0), 'f', 3),
        "-i", path,
        "-frames:v", "1",
        "-vf", QString("scale=-1:%1").arg(height),
        "-f", "image2pipe",
        "-vcodec", "mjpeg",
        "pipe:1",
    });

    // Poll instead of waitForFinished(-1) so a cancel request can kill the child
    // promptly — otherwise the std::future destructor in ThumbWorker would block
    // the UI thread until ffmpeg finishes on its own.
    while (!proc.waitForFinished(kThumbPollMs)) {
        if (proc.state() == QProcess::NotRunning)
            break;  // failed to start, or exited between polls
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            proc.kill();
            proc.waitForFinished(-1);
            return {};
        }
    }

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return {};

    const QByteArray data = proc.readAllStandardOutput();
    QImage img;
    img.loadFromData(data, "JPEG");
    return img;
}

bool nearestKeyframe(const QByteArray &line, double start, double end,
                     double startTime, double *resolved) {
    const QList<QByteArray> fields = line.trimmed().split(',');
    if (fields.size() >= 2 && fields.at(1).contains('K')) {
        bool valid = false;
        const double time = fields.at(0).toDouble(&valid) - startTime;
        if (valid && std::isfinite(time) && time >= 0 && time < end
            && (*resolved < 0 || std::abs(time - start) < std::abs(*resolved - start)
                || (std::abs(time - start) == std::abs(*resolved - start) && time < *resolved)))
            *resolved = time;
    }
    return *resolved >= 0;
}

QStringList trimArgs(const QString &src, const QString &dst, double start, double end,
                     const ExportSettings &settings) {
    QStringList args = {"-y", "-loglevel", "error", "-progress", "pipe:1"};
    const bool copy = std::holds_alternative<StreamCopySettings>(settings);
    // -ss before -i seeks fast; -t gives the output duration. +faststart puts
    // the moov atom up front so shared clips start playing before they finish
    // downloading.
    args << "-ss" << QString::number(start, 'f', copy ? 6 : 3)
         << "-i" << src
         << "-t" << QString::number(qMax(end - start, 0.0), 'f', copy ? 6 : 3);
    // Cap the shorter side, judged on the decoded (rotation-applied) frame, so
    // portrait and landscape both keep their aspect ratio. -2 keeps the other
    // side divisible by two, which libx264 requires.
    if (copy) {
        const auto streams = std::get<StreamCopySettings>(settings);
        args << "-map" << QStringLiteral("0:%1").arg(streams.videoStream);
        if (streams.audioStream >= 0)
            args << "-map" << QStringLiteral("0:%1").arg(streams.audioStream);
        args << "-c" << "copy" << "-avoid_negative_ts" << "make_zero";
    } else {
        const int scaleHeight = std::get<EncodeSettings>(settings).scaleHeight;
        if (scaleHeight > 0)
            args << "-vf"
                 << QString("scale='if(gt(iw,ih),-2,%1)':'if(gt(iw,ih),%1,-2)'").arg(scaleHeight);
        args << "-c:v" << "libx264" << "-preset" << "veryfast"
             << "-crf" << "18" << "-c:a" << "aac";
    }
    args << "-movflags" << "+faststart" << dst;
    return args;
}

}  // namespace ffmpeg
