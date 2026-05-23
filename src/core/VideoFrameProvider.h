#pragma once
#include <QObject>
#include <QImage>
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
    Q_PROPERTY(int frameCount READ frameCount NOTIFY frameCountChanged)

public:
    explicit VideoFrameProvider(QObject *parent = nullptr);

    QVideoSink *videoSink() const { return m_videoSink; }
    void setVideoSink(QVideoSink *sink);
    bool hasVideo() const { return m_hasVideo; }
    int frameCount() const { return m_frameCount; }
    // Last decoded frame's pixel dimensions (BGRx, post-decode). Empty
    // QSize before the first frame arrives. Used by the call telemetry
    // overlay to surface each stream's current resolution.
    QSize lastFrameSize() const { return m_lastSize; }

    void feedFrame(GstSample *sample);
    // #20 — alternate entry for already-decoded RGBA frames (e.g.
    // PublishPipeline routes the preview sample through BackgroundEngine
    // first and feeds the composited QImage straight here, skipping the
    // GstSample → QImage path). Caller MUST be on the Qt main thread.
    void feedQImage(const QImage &img);

signals:
    void videoSinkChanged();
    void hasVideoChanged();
    void frameCountChanged();
    void imageReady(const QImage &image);

private:
    QVideoSink *m_videoSink = nullptr;
    bool m_hasVideo = false;
    int m_frameCount = 0;
    QSize m_lastSize;
};
