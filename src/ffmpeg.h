#pragma once

#include <QByteArray>
#include <QImage>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <variant>

#include <atomic>

// Thin wrappers around the ffmpeg/ffprobe command-line tools.
namespace ffmpeg {

struct VideoInfo {
    QString path;
    double duration = 0.0;  // seconds
    double containerStartSeconds = 0.0;
    int width = 0;
    int height = 0;
    int videoStream = -1;
    int audioStream = -1;
    QString videoCodec;
    QString audioCodec;
    QString copyUnavailableReason;
    bool ok = false;
    QString error;
};

// Probe a file for a usable video stream and duration (runs ffprobe).
VideoInfo probe(const QString &path);

// Grab a single frame at `time` seconds, scaled to `height` px.
// Returns a null QImage on failure. If `cancel` is set and flips to true while
// the ffmpeg child is running, the child is killed and a null QImage returned.
QImage thumbnail(const QString &path, double time, int height = 90,
                 const std::atomic<bool> *cancel = nullptr);

struct EncodeSettings { int scaleHeight = 0; };
struct StreamCopySettings { int videoStream; int audioStream; };
using ExportSettings = std::variant<EncodeSettings, StreamCopySettings>;

// Build the ffmpeg argument list that writes [start, end] of src to dst.
QStringList trimArgs(const QString &src, const QString &dst, double start, double end,
                     const ExportSettings &settings = EncodeSettings{});

bool nearestKeyframe(const QByteArray &line, double start, double end,
                     double startTime, double *resolved);

// Locate a tool on PATH; returns empty string if missing.
QString toolPath(const QString &tool);

}  // namespace ffmpeg

Q_DECLARE_METATYPE(ffmpeg::ExportSettings)
