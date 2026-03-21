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
    if (!m_videoSink || !sample) return;

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

    QVideoFrameFormat fmt(QSize(width, height), pixFmt);
    QVideoFrame frame(fmt);
    if (frame.map(QVideoFrame::WriteOnly)) {
        int ySize = width * height;
        int uvSize = (width / 2) * (height / 2);
        const uchar *src = map.data;
        if ((qsizetype)(ySize + 2 * uvSize) <= (qsizetype)map.size) {
            memcpy(frame.bits(0), src, ySize);
            if (frame.planeCount() >= 3) {
                memcpy(frame.bits(1), src + ySize, uvSize);
                memcpy(frame.bits(2), src + ySize + uvSize, uvSize);
            }
        }
        frame.unmap();
        m_videoSink->setVideoFrame(frame);

        if (!m_hasVideo) {
            m_hasVideo = true;
            emit hasVideoChanged();
        }
    }

    gst_buffer_unmap(buf, &map);
}
