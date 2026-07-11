#include "core/MicTester.h"

#include <QDebug>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QtConcurrent>

MicTester::MicTester(QObject *parent)
    : QObject(parent)
{
    // Poll the GStreamer bus from the Qt thread (same approach as the call
    // pipelines, which avoid a bus watch on the GLib loop). 50 ms keeps the
    // meter responsive without busy-spinning.
    m_busTimer.setInterval(50);
    connect(&m_busTimer, &QTimer::timeout, this, &MicTester::pollBus);
}

MicTester::~MicTester()
{
    teardownPipeline();
}

bool MicTester::start(const QString &deviceId)
{
    // FAST PATH — reuse the resident pipeline. gst_element_factory_make(
    // "wasapi2src") does a WinRT audio-endpoint enumeration that takes 4-6 s
    // EVERY call and marshals to the main COM STA, freezing the Settings dialog
    // on every open. set_state(PLAYING) on an already-built pipeline is ~7 ms.
    // So if we already built a pipeline for THIS device, just re-open the device
    // and restart the meter — no factory_make.
    if (m_pipeline && m_pipelineReady && m_pipelineDevice == deviceId) {
        m_startGen.fetch_add(1, std::memory_order_relaxed);
        gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
        m_busTimer.start();
        return true;
    }

    // First build, or the selected device changed — drop any resident pipeline
    // and build a fresh one OFF the UI thread (the 4-6 s factory_make must never
    // block the GUI). This full build happens at most once per device.
    teardownPipeline();   // bumps m_startGen
    m_pipelineDevice = deviceId;

    // Capture the post-teardown generation. If another start()/stop()/teardown
    // happens before this open finishes, the token will no longer match and
    // the worker's result is discarded (see the completion below).
    const quint64 gen = m_startGen.load(std::memory_order_relaxed);
    QPointer<MicTester> guard(this);
    const QString dev = deviceId;

    // Open the device OFF the UI thread. Cold wasapi2src init (WASAPI open +
    // first-time, Defender-scanned plugin-DLL load) blocks set_state(PLAYING)
    // for seconds; doing it here on the UI thread froze the Settings dialog on
    // open (measured ~4.5 s). The worker builds + PLAYs the pipeline; only the
    // finished pipeline is handed back to the UI thread to adopt.
    m_startFuture = QtConcurrent::run([guard, gen, dev]() {
        GstElement *pipeline = buildAndOpen(dev);   // heavy: build + set_state(PLAYING)

        // Hand the result to the GUI thread. qApp is the context object (always
        // alive while the app runs) so the functor is delivered even if the
        // MicTester was destroyed meanwhile; the QPointer + generation check
        // inside decide adopt-vs-discard. The worker itself never touches any
        // MicTester member, so it is safe to outlive `this`.
        QMetaObject::invokeMethod(qApp, [guard, gen, pipeline]() {
            MicTester *self = guard.data();
            if (!self || self->m_startGen.load(std::memory_order_relaxed) != gen) {
                // Superseded (stopped / restarted / destroyed before we
                // finished): release the device we opened so nothing leaks and
                // no stray mic handle survives.
                if (pipeline) {
                    gst_element_set_state(pipeline, GST_STATE_NULL);
                    gst_object_unref(pipeline);
                }
                return;
            }
            if (!pipeline)
                return;   // open failed on both the selected device and default
            // Adopt on the GUI thread and start the (GUI-thread) bus poll so
            // the level meter begins moving. Mark ready so the next open reuses
            // this pipeline (set_state PLAYING) instead of rebuilding it.
            self->m_pipeline = pipeline;
            self->m_pipelineReady = true;
            self->m_busTimer.start();
        }, Qt::QueuedConnection);
    });

    return true;
}

GstElement *MicTester::buildAndOpen(const QString &deviceId)
{
    // Build src -> audioconvert -> audioresample -> level -> fakesink. Runs on a
    // worker thread — the first wasapi2src creation + WASAPI device open can take
    // a moment and must never block the GUI. The built pipeline is reused across
    // Settings opens (see start()), so this only runs once per device.
    auto tryOpen = [](const QString &dev) -> GstElement * {
        GstElement *src      = gst_element_factory_make("wasapi2src", "src");
        GstElement *conv     = gst_element_factory_make("audioconvert", nullptr);
        GstElement *resample = gst_element_factory_make("audioresample", nullptr);
        GstElement *level    = gst_element_factory_make("level", "lvl");
        GstElement *sink     = gst_element_factory_make("fakesink", nullptr);
        if (!src || !conv || !resample || !level || !sink) {
            qWarning() << "MicTester: failed to create capture chain elements";
            auto freeIf = [](GstElement *e) { if (e) gst_object_unref(e); };
            freeIf(src); freeIf(conv); freeIf(resample); freeIf(level); freeIf(sink);
            return nullptr;
        }
        g_object_set(level, "post-messages", TRUE,
                     "interval", (guint64)50000000, nullptr);
        g_object_set(sink, "sync", FALSE, nullptr);
        if (!dev.isEmpty())
            g_object_set(src, "device", dev.toUtf8().constData(), nullptr);

        GstElement *pipeline = gst_pipeline_new("mic-test");
        gst_bin_add_many(GST_BIN(pipeline), src, conv, resample, level, sink, nullptr);
        if (!gst_element_link_many(src, conv, resample, level, sink, nullptr)) {
            qWarning() << "MicTester: failed to link capture chain";
            gst_object_unref(pipeline);
            return nullptr;
        }

        GstStateChangeReturn r = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (r == GST_STATE_CHANGE_FAILURE) {
            qWarning() << "MicTester: device" << dev << "failed to open";
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            return nullptr;
        }
        return pipeline;
    };

    // Try the selected device; if it won't open, fall back to the SYSTEM
    // DEFAULT (no device set) so the meter still works — mirrors the call
    // publisher's tiered fallback.
    GstElement *pipeline = tryOpen(deviceId);
    if (!pipeline && !deviceId.isEmpty()) {
        qWarning() << "MicTester: selected device failed to open — falling back "
                      "to the system default";
        pipeline = tryOpen(QString());
    }
    return pipeline;
}

void MicTester::stop()
{
    // Keep the built pipeline RESIDENT — just invalidate any in-flight build,
    // stop the meter, and close the device (NULL). The next start() re-opens it
    // with a ~7 ms set_state(PLAYING) instead of a 4-6 s factory_make rebuild.
    m_startGen.fetch_add(1, std::memory_order_relaxed);
    m_busTimer.stop();
    if (m_pipeline)
        gst_element_set_state(m_pipeline, GST_STATE_NULL);  // close device, keep object
    emit level(0.0);   // drop the meter to silence when the test ends
}

void MicTester::pollBus()
{
    if (!m_pipeline) return;
    GstBus *bus = gst_element_get_bus(m_pipeline);
    GstMessage *msg;
    while ((msg = gst_bus_pop(bus)) != nullptr) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ELEMENT) {
            const GstStructure *s = gst_message_get_structure(msg);
            if (s && g_strcmp0(gst_structure_get_name(s), "level") == 0) {
                // Peak dB per channel -> 0..1, squared for visual dynamic range
                // (identical mapping to PublishPipeline's level handling so the
                // test meter matches the in-call level feel).
                GValueArray *arr = nullptr;
                gst_structure_get(s, "peak", G_TYPE_VALUE_ARRAY, &arr, nullptr);
                if (arr && arr->n_values > 0) {
                    gdouble db = g_value_get_double(arr->values);
                    double lvl = qBound(0.0, (db + 100.0) / 100.0, 1.0);
                    lvl = lvl * lvl;
                    emit level(lvl);
                }
                if (arr) g_value_array_free(arr);
            }
        }
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

void MicTester::teardownPipeline()
{
    // Fully destroy the pipeline (device change / destruction). Invalidate any
    // async open still in flight: its completion will see the bumped token,
    // discard the pipeline it built, and never adopt it. This makes
    // start-then-immediately-close and rapid device-changes safe — the worker
    // never races this teardown because it only touches the pipeline it owns
    // locally, never m_pipeline.
    m_startGen.fetch_add(1, std::memory_order_relaxed);
    m_busTimer.stop();
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
    m_pipelineReady = false;
    m_pipelineDevice.clear();
}
