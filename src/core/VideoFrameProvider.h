#pragma once
#include <QObject>
#include <QVideoSink>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

class VideoFrameProvider : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVideoSink* videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)

public:
    explicit VideoFrameProvider(QObject *parent = nullptr);

    QVideoSink *videoSink() const { return m_videoSink; }
    void setVideoSink(QVideoSink *sink);
    bool hasVideo() const { return m_hasVideo; }

    void feedFrame(GstSample *sample);

signals:
    void videoSinkChanged();
    void hasVideoChanged();

private:
    QVideoSink *m_videoSink = nullptr;
    bool m_hasVideo = false;
};
