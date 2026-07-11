#include "BgPreviewSource.h"

#include "core/BackgroundEngine.h"

#include <QDebug>
#include <QPointer>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

namespace {
// 0.40.3 — cap the LONG side at 640 px and let videoscale derive the
// other dimension from the camera's native pixel-aspect-ratio. Forcing
// both width AND height (we used to hard-code 640×360, a 16:9 box)
// squished 4:3 webcams vertically — the preview showed a stretched
// face that didn't match the actual call output, which IS aspect-
// correct because the call pipeline doesn't apply this same caps box.
constexpr int kPreviewW   = 640;
constexpr int kPreviewFps = 15;
} // namespace

BgPreviewSource::BgPreviewSource(BackgroundEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
{
}

BgPreviewSource::~BgPreviewSource()
{
    stop();
}

bool BgPreviewSource::start()
{
    if (m_running) return true;

    m_pipeline = gst_pipeline_new("bg-preview");
    if (!m_pipeline) {
        emit unavailable(QStringLiteral("Failed to create preview pipeline"));
        return false;
    }

    // Camera source - mirror PublishPipeline's preference: mfvideosrc
    // first (modern Media Foundation, low CPU), ksvideosrc fallback
    // for older Windows installs.
    m_source = gst_element_factory_make("mfvideosrc", "preview-cam");
    if (!m_source)
        m_source = gst_element_factory_make("ksvideosrc", "preview-cam");
    if (!m_source) {
        emit unavailable(QStringLiteral("No camera source factory available"));
        stop();
        return false;
    }

    GstElement *decode  = gst_element_factory_make("decodebin", "preview-decode");
    GstElement *convert = gst_element_factory_make("videoconvert", "preview-conv");
    GstElement *scale   = gst_element_factory_make("videoscale", "preview-scale");
    GstElement *rate    = gst_element_factory_make("videorate", "preview-rate");
    GstElement *caps    = gst_element_factory_make("capsfilter", "preview-caps");
    m_appsink           = gst_element_factory_make("appsink", "preview-sink");

    if (!decode || !convert || !scale || !rate || !caps || !m_appsink) {
        // Each gst_element_factory_make returns a floating reference. If
        // we bail out before gst_bin_add_many takes ownership we MUST
        // unref every non-null local; otherwise they leak and the next
        // start() refuses to register elements with the same names.
        if (decode)    gst_object_unref(decode);
        if (convert)   gst_object_unref(convert);
        if (scale)     gst_object_unref(scale);
        if (rate)      gst_object_unref(rate);
        if (caps)      gst_object_unref(caps);
        if (m_appsink) gst_object_unref(m_appsink);
        m_appsink = nullptr;
        emit unavailable(QStringLiteral("Failed to create preview chain elements"));
        stop();
        return false;
    }

    {
        // width + pixel-aspect-ratio=1/1 + framerate; height is derived
        // by videoscale from the camera's input DAR (so 4:3 camera ->
        // 640x480 preview, 16:9 camera -> 640x360 preview). The display
        // QLabel uses Qt::KeepAspectRatio to letterbox into its fixed
        // 320x180 box, so a 4:3 frame lands as 240x180 with side bars.
        QByteArray capsStr = QString(
            "video/x-raw,format=BGRx,width=%1,"
            "pixel-aspect-ratio=1/1,framerate=%2/1")
            .arg(kPreviewW).arg(kPreviewFps).toLatin1();
        GstCaps *c = gst_caps_from_string(capsStr.constData());
        g_object_set(caps, "caps", c, nullptr);
        gst_caps_unref(c);
    }

    g_object_set(m_appsink,
        "emit-signals", TRUE,
        "max-buffers", 1,
        "drop", TRUE,
        "sync", FALSE,
        nullptr);
    g_signal_connect(m_appsink, "new-sample",
                     G_CALLBACK(onNewSample), this);

    gst_bin_add_many(GST_BIN(m_pipeline),
        m_source, decode, convert, scale, rate, caps, m_appsink, nullptr);

    if (!gst_element_link(m_source, decode)) {
        emit unavailable(QStringLiteral("Camera source could not link to decodebin"));
        stop();
        return false;
    }
    if (!gst_element_link_many(convert, scale, rate, caps, m_appsink, nullptr)) {
        emit unavailable(QStringLiteral("Preview chain link failed"));
        stop();
        return false;
    }
    // decodebin's src pad is dynamic; wire it to videoconvert once it appears.
    g_signal_connect(decode, "pad-added",
        G_CALLBACK(+[](GstElement *, GstPad *pad, gpointer ud) {
            GstElement *target = static_cast<GstElement *>(ud);
            GstPad *sink = gst_element_get_static_pad(target, "sink");
            if (sink && !gst_pad_is_linked(sink))
                gst_pad_link(pad, sink);
            if (sink) gst_object_unref(sink);
        }), convert);

    GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        emit unavailable(QStringLiteral(
            "Camera busy or unavailable (already in use by a call?)"));
        stop();
        return false;
    }
    m_running = true;
    return true;
}

void BgPreviewSource::stop()
{
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
    m_source  = nullptr;   // owned by the pipeline
    m_appsink = nullptr;
    m_running = false;
}

GstFlowReturn BgPreviewSource::onNewSample(GstElement *appsink, gpointer userData)
{
    auto *self = static_cast<BgPreviewSource *>(userData);
    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink));
    if (!sample) return GST_FLOW_OK;

    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstCaps   *caps = gst_sample_get_caps(sample);
    GstStructure *s = caps ? gst_caps_get_structure(caps, 0) : nullptr;
    int w = 0, h = 0;
    if (s) {
        gst_structure_get_int(s, "width",  &w);
        gst_structure_get_int(s, "height", &h);
    }
    if (!buf || w <= 0 || h <= 0) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    QImage snapshot;
    GstMapInfo map;
    if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
        if (qint64(map.size) >= qint64(w) * h * 4) {
            // Format_RGB32 byte-aliases BGRx on little-endian; .copy()
            // detaches before unmap.
            QImage wrap(map.data, w, h, w * 4, QImage::Format_RGB32);
            snapshot = wrap.copy();
        }
        gst_buffer_unmap(buf, &map);
    }
    gst_sample_unref(sample);
    if (snapshot.isNull()) return GST_FLOW_OK;

    // Run the engine ON THIS STREAMING THREAD, not the UI thread.
    // BackgroundEngine::processFrame is thread-safe: it internally routes
    // the heavy GL + ORT segmentation work to the BackgroundWorker thread
    // (which owns the GL context) via a blocking hop and hands back the
    // composited frame. The in-call publish path
    // (PublishPipeline::onBgSample) already calls it exactly this way from
    // its own GStreamer streaming thread.
    //
    // This used to hop to the Qt/UI thread FIRST and call processFrame
    // there — but processFrame then BLOCKS on the worker (its internal
    // Qt::BlockingQueuedConnection), so the UI thread stalled for the whole
    // first-frame ONNX-Runtime session build (~6 s cold) and every heavy
    // inference after it (~1.3 s first, tens of ms warm). That is what
    // froze the Settings dialog on open whenever a Blur/Image background
    // was active. Doing the work here keeps the UI thread free; only the
    // finished QImage is marshalled to the UI for display. The appsink is
    // max-buffers=1 + drop=true, so a slow frame is dropped, never queued.
    //
    // Engine lifetime: the dialog constructs the engine BEFORE this source,
    // so Qt tears the source down first — stop() sets the pipeline to NULL
    // and joins this streaming thread before the engine is destroyed, so
    // the engine is always alive for the duration of this callback.
    QImage processed = snapshot;
    if (BackgroundEngine *e = self->m_engine.data();
        e && e->mode() != BackgroundEngine::Mode::None) {
        processed = e->processFrame(snapshot);
    }

    // Marshal only the finished frame to the UI thread for display.
    QPointer<BgPreviewSource> guard(self);
    QMetaObject::invokeMethod(self, [guard, processed]() {
        if (guard) emit guard->imageReady(processed);
    }, Qt::QueuedConnection);

    return GST_FLOW_OK;
}
