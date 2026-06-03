#include "core/MicTester.h"

#include <QDebug>

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
    cleanup();
}

bool MicTester::start(const QString &deviceId)
{
    cleanup();   // restart cleanly if already running

    // Build src -> audioconvert -> audioresample -> level -> fakesink. The
    // device id (a wasapi2 strid) can contain characters that gst_parse_launch
    // would choke on, so parse the fixed chain by element NAME and set the
    // device property programmatically.
    GError *err = nullptr;
    m_pipeline = gst_parse_launch(
        "wasapi2src name=src ! audioconvert ! audioresample ! "
        "level name=lvl post-messages=true interval=50000000 ! "
        "fakesink sync=false",
        &err);
    if (!m_pipeline) {
        qWarning() << "MicTester: failed to build pipeline:"
                   << (err ? err->message : "unknown");
        if (err) g_error_free(err);
        return false;
    }
    if (err) { g_error_free(err); err = nullptr; }

    if (!deviceId.isEmpty()) {
        if (GstElement *src = gst_bin_get_by_name(GST_BIN(m_pipeline), "src")) {
            g_object_set(src, "device", deviceId.toUtf8().constData(), nullptr);
            gst_object_unref(src);
        }
    }

    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING)
            == GST_STATE_CHANGE_FAILURE) {
        // Couldn't open the device (busy / unresolvable id / OS-blocked) — the
        // meter staying flat is the user's signal that this device won't work.
        qWarning() << "MicTester: device" << deviceId << "failed to open";
        cleanup();
        return false;
    }
    m_busTimer.start();
    return true;
}

void MicTester::stop()
{
    cleanup();
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

void MicTester::cleanup()
{
    m_busTimer.stop();
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
}
