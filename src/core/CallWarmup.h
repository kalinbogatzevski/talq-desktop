#pragma once

// CallWarmup -- pay a call's one-time GStreamer plugin-load cost while the phone
// is ringing or the dial is still joining the room, instead of inside the first
// publisher / subscriber build.
//
// Field 2026-10-29, the first call after launch on both machines: the publisher
// build took 2.85 s on one and 5.95 s on the other, and nearly all of it was the
// first use of each plugin — the first encoder layer 1.1-1.2 s, then 11-14 ms for
// the second and third layer on the SAME plugin; webrtcbin 0.5-1.0 s; the
// webrtcsrc plugin another 0.4 s when the subscriber was built. A plugin stays
// loaded for the life of the process, so only the first call after launch pays it.
//
// Loading is all this does: no element is instantiated, so no device is opened and
// no camera, microphone or encoder session is touched. The hardware encoder that
// gets loaded is the one makeWebrtcVideoEncoder() would try first, under the same
// gates — the NVENC "can't open a session here" latch, the software latch and the
// out-of-process probe cache — so a machine that must not touch a given encoder
// never has its plugin loaded early either.
//
// Started from CallManager::setState on Incoming / Outgoing / Connecting, once per
// process. A call that starts building while this still runs simply waits on
// GStreamer's own plugin-load lock for the remainder, which is never longer than
// loading it cold. TALQ_NO_CALL_WARMUP=1 turns it off (A/B timing of a call).

#include <atomic>

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <gst/gst.h>

#include "VideoEncoderUtil.h"   // the encoder gates + HwEncoderProbe.h

namespace talq {

// The hardware (or software) H.264 encoder makeWebrtcVideoEncoder() would take
// first on this machine, or nullptr. Mirrors the gates in that loop; keep the two
// in step.
inline const char *firstViableVideoEncoder()
{
    const bool forceX264 = qEnvironmentVariableIsSet("TALQ_TEST_X264")
                           || talqForceSoftwareVideo().load();
    static const char *const order[] = { "nvh264enc", "qsvh264enc", "mfh264enc", "x264enc" };
    for (const char *name : order) {
        if (forceX264 && g_strcmp0(name, "x264enc") != 0) continue;
        if (!g_strcmp0(name, "nvh264enc") && talqAvoidNvenc().load()) continue;
        if (talqHwEncoderProbeExcludes(name)) continue;
        if (GstElementFactory *f = gst_element_factory_find(name)) {
            gst_object_unref(f);
            return name;
        }
    }
    return nullptr;
}

// The H.264 decoder decodebin picks first (highest rank), or nullptr. A new ref.
inline GstElementFactory *topH264Decoder()
{
    GstElementFactory *top = nullptr;
    if (GstCaps *h264 = gst_caps_new_empty_simple("video/x-h264")) {
        GList *all = gst_element_factory_list_get_elements(
            GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO,
            GST_RANK_MARGINAL);
        GList *cand = gst_element_factory_list_filter(all, h264, GST_PAD_SINK, FALSE);
        cand = g_list_sort(cand, gst_plugin_feature_rank_compare_func);
        if (cand) top = GST_ELEMENT_FACTORY(gst_object_ref(cand->data));
        gst_plugin_feature_list_free(cand);
        gst_plugin_feature_list_free(all);
        gst_caps_unref(h264);
    }
    return top;
}

// Blocking; runs on a worker thread. Safe to call from any thread: GStreamer
// serialises plugin loads and a plugin that is already loaded costs nothing.
inline void warmCallPluginsNow()
{
    QElapsedTimer total;
    total.start();
    int loaded = 0;
    QStringList slow;   // only the loads that were worth noticing

    auto loadFactory = [&](GstElementFactory *f, const char *label) {
        QElapsedTimer t;
        t.start();
        if (GstPluginFeature *feature = gst_plugin_feature_load(GST_PLUGIN_FEATURE(f)))
            gst_object_unref(feature);
        ++loaded;
        if (t.elapsed() >= 50)
            slow << QStringLiteral("%1 %2 ms").arg(QString::fromUtf8(label)).arg(t.elapsed());
    };
    auto warm = [&](const char *name) {
        if (!name || QCoreApplication::closingDown()) return;
        if (GstElementFactory *f = gst_element_factory_find(name)) {
            loadFactory(f, name);
            gst_object_unref(f);
        }
    };

    // In the order a call first needs them: the publisher (webrtcbin, the echo
    // canceller and its loopback reference, the encoder, the camera), then the
    // subscriber that follows it (webrtcsrc, its depay/decoder, the sink).
    static const char *const publishFactories[] = {
        "webrtcbin", "nicesrc", "dtlssrtpdec", "srtpenc", "rtpbin", "rtpopuspay",
        "rtph264pay", "h264parse", "rtpgccbwe", "opusenc", "webrtcdsp", "webrtcechoprobe",
        "wasapi2src", nullptr
    };
    for (int i = 0; publishFactories[i]; ++i) warm(publishFactories[i]);
    warm(firstViableVideoEncoder());
    warm("mfvideosrc");

    static const char *const subscribeFactories[] = {
        "webrtcsrc", "decodebin3", "rtph264depay", "opusdec", "wasapi2sink", nullptr
    };
    for (int i = 0; subscribeFactories[i]; ++i) warm(subscribeFactories[i]);
    if (GstElementFactory *dec = topH264Decoder()) {
        if (!QCoreApplication::closingDown())
            loadFactory(dec, GST_OBJECT_NAME(dec));
        gst_object_unref(dec);
    }

    qInfo().noquote() << "CallWarmup: loaded" << loaded << "plugin feature(s) in"
                      << total.elapsed() << "ms"
                      << (slow.isEmpty() ? QString() : QStringLiteral("— ") + slow.join(QStringLiteral(", ")));
}

// Once per process; returns at once. Cheap to call from every state change.
inline void startCallWarmup()
{
    static std::atomic<bool> started{false};
    if (qEnvironmentVariableIsSet("TALQ_NO_CALL_WARMUP")) return;
    if (started.exchange(true)) return;
    QThreadPool::globalInstance()->start([] { warmCallPluginsNow(); });
}

} // namespace talq
