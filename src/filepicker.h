#pragma once

#include <QList>
#include <QObject>
#include <QUrl>
#include "ffmpeg.h"

class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    virtual void openVideo() = 0;
    virtual void exportVideo(const QUrl &suggestedUrl, double start, double end,
                             const QList<int> &encodeScaleHeights, ffmpeg::ExportSettings settings) = 0;

signals:
    void openSelected(const QUrl &url);
    void exportSelected(const QUrl &url, double start, double end,
                        ffmpeg::ExportSettings settings);
    void failed(const QString &message);
};
