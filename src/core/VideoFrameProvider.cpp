#include "VideoFrameProvider.h"
#include <QDebug>

VideoFrameProvider::VideoFrameProvider(QObject *parent)
    : QObject(parent)
{
}

void VideoFrameProvider::setVideoSink(QVideoSink *sink)
{
    if (m_videoSink != sink) {
        m_videoSink = sink;
        emit videoSinkChanged();
    }
}

void VideoFrameProvider::feedFrame(GstSample *sample)
{
    if (!sample) return;

    // Always count frames, even without a videoSink (for test harness)
    m_frameCount++;
    if (m_frameCount <= 3 || m_frameCount % 100 == 0)
        qDebug() << "VideoFrameProvider: frame" << m_frameCount << (m_videoSink ? "(has sink)" : "(no sink)");
    emit frameCountChanged();

    if (!m_hasVideo) {
        m_hasVideo = true;
        emit hasVideoChanged();
    }

    GstCaps *caps = gst_sample_get_caps(sample);
    if (!caps) return;

    GstStructure *s = gst_caps_get_structure(caps, 0);
    int width = 0, height = 0;
    gst_structure_get_int(s, "width", &width);
    gst_structure_get_int(s, "height", &height);
    if (width <= 0 || height <= 0) return;

    const gchar *format = gst_structure_get_string(s, "format");
    QVideoFrameFormat::PixelFormat pixFmt = QVideoFrameFormat::Format_Invalid;
    if (format && g_strcmp0(format, "I420") == 0)
        pixFmt = QVideoFrameFormat::Format_YUV420P;
    else if (format && g_strcmp0(format, "NV12") == 0)
        pixFmt = QVideoFrameFormat::Format_NV12;
    else {
        qWarning() << "VideoFrameProvider: unsupported pixel format" << (format ? format : "null");
        return;
    }

    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) return;

    const qint64 ySize = (qint64)width * height;
    const qint64 uvSize = (qint64)(width / 2) * (height / 2);
    const bool bufferValid = (qint64)(ySize + 2 * uvSize) <= (qint64)map.size;

    // Convert I420 to QImage for widget display
    if (bufferValid && pixFmt == QVideoFrameFormat::Format_YUV420P) {
        const uchar *yPlane = map.data;
        const uchar *uPlane = map.data + ySize;
        const uchar *vPlane = map.data + ySize + uvSize;
        QImage img(width, height, QImage::Format_RGB32);
        for (int row = 0; row < height; ++row) {
            auto *dst = reinterpret_cast<QRgb*>(img.scanLine(row));
            for (int col = 0; col < width; ++col) {
                int y = yPlane[row * width + col];
                int u = uPlane[(row / 2) * (width / 2) + col / 2] - 128;
                int v = vPlane[(row / 2) * (width / 2) + col / 2] - 128;
                int r = qBound(0, y + ((359 * v) >> 8), 255);
                int g = qBound(0, y - ((88 * u + 183 * v) >> 8), 255);
                int b = qBound(0, y + ((454 * u) >> 8), 255);
                dst[col] = qRgb(r, g, b);
            }
        }
        emit imageReady(img);
    }

    // Also feed QVideoSink if present
    if (bufferValid && m_videoSink) {
        QVideoFrameFormat fmt(QSize(width, height), pixFmt);
        QVideoFrame frame(fmt);
        if (frame.map(QVideoFrame::WriteOnly)) {
            memcpy(frame.bits(0), map.data, ySize);
            if (frame.planeCount() >= 3) {
                memcpy(frame.bits(1), map.data + ySize, uvSize);
                memcpy(frame.bits(2), map.data + ySize + uvSize, uvSize);
            }
            frame.unmap();
            m_videoSink->setVideoFrame(frame);
        }
    }

    gst_buffer_unmap(buf, &map);
}
