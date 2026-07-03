#include "core/SubscribeWebrtcSrc.h"
#include "core/VideoFrameProvider.h"
#include "core/LeakStats.h"
#include <QDebug>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QUrl>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>   // 0.51.x AEC: push decoded audio to the publisher far-end mixer
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

// ── Custom Signallable GObject ────────────────────────────────────────
// gst-plugins-rs webrtcsrc takes a construct-only `signaller` GObject
// implementing the `GstRSWebRTCSignallableIface` interface (from
// gst-plugin-webrtc src/signaller/iface.rs, plugin 0.15.0).
//
// CRUCIAL: the interface vtable (SignallableInterface) holds *Rust* `fn`
// pointers taking *Rust* types — `fn(&Signallable, &str, ...)` where
// `&str` is a 2-word fat pointer and `Option<String>` a 3-word struct,
// using the Rust (not extern "C") calling convention. A C struct/function
// can NEVER be ABI-compatible with that, so writing the vtable from C
// segfaults the moment webrtcsrc calls `(vtable.send_sdp)(...)`. The Rust
// defaults are confirmed empty no-ops, so we DO NOT touch the vtable
// (NULL interface_init keeps the safe defaults) and drive the whole
// handshake through GObject *signals*, which marshal via GValue/GType —
// a stable C ABI. webrtcsrc EMITS start/stop/send-session-description/
// send-ice/end-session ON the signaller (we connect); we EMIT
// session-started→session-description(offer)→handle-ice back into it.
// All logic stays in SubscribeWebrtcSrc; the GObject is a bare shell.
namespace {

struct TalqSig      { GObject parent; };
struct TalqSigClass { GObjectClass parent; };

enum { PROP_0, PROP_MANUAL_SDP_MUNGING };

void talqsig_get_property(GObject *, guint prop_id, GValue *value, GParamSpec *pspec)
{
    if (prop_id == PROP_MANUAL_SDP_MUNGING) g_value_set_boolean(value, FALSE);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(nullptr, prop_id, pspec);
}
void talqsig_set_property(GObject *, guint, const GValue *, GParamSpec *) {}

void talqsig_class_init(gpointer g_class, gpointer)
{
    auto *oc = G_OBJECT_CLASS(g_class);
    oc->get_property = talqsig_get_property;
    oc->set_property = talqsig_set_property;
    // Interface property — an implementor MUST override it or g_object_get
    // on the signaller criticals. read-only, always FALSE (webrtcsrc owns
    // SDP munging).
    g_object_class_override_property(oc, PROP_MANUAL_SDP_MUNGING,
                                     "manual-sdp-munging");
}

// GLib does NOT copy an interface's default vtable into a *static C*
// implementor — with a NULL interface_init our SignallableInterface
// slots stay g_malloc0'd (NULL). webrtcsrc's Rust RUN_LAST class
// handlers do `(vtable.start)(this)` etc., so a NULL slot is an
// instant 0xC0000005. We can't synthesise those slots from C (they
// take Rust types / Rust ABI), but the Rust default_init already
// filled the interface's DEFAULT vtable with the safe Rust no-op
// fns. Copy those pointer VALUES (just pointers — copying is
// ABI-neutral; we never call them ourselves) into our vtable.
//
// gst-plugins-rs 0.15.0 SignallableInterface = GTypeInterface + these
// exactly 5 fn-ptr slots, in this order (src/signaller/iface.rs). We
// copy by field, NOT memcpy+g_type_query: g_type_query does not work
// on interface types (class_size==0 → size underflow → the gigabyte
// memcpy that WAS this crash, between ck1 and ck2).
struct SignallableVtableABI {
    GTypeInterface parent;
    gpointer start;
    gpointer stop;
    gpointer send_sdp;
    gpointer add_ice;
    gpointer end_session;
};

void talqsig_iface_init(gpointer g_iface, gpointer)
{
    GType it = g_type_from_name("GstRSWebRTCSignallableIface");
    if (!it) return;
    gpointer dflt = g_type_default_interface_peek(it);
    if (!dflt) dflt = g_type_default_interface_ref(it);  // populate + keep
    if (!dflt) return;
    auto *d = static_cast<SignallableVtableABI*>(dflt);
    auto *m = static_cast<SignallableVtableABI*>(g_iface);
    m->start       = d->start;        // = Rust Signallable::start (no-op)
    m->stop        = d->stop;
    m->send_sdp    = d->send_sdp;
    m->add_ice     = d->add_ice;
    m->end_session = d->end_session;
}

GType talq_signaller_get_type()
{
    static gsize t = 0;
    if (g_once_init_enter(&t)) {
        GType base = g_type_register_static_simple(
            G_TYPE_OBJECT, "TalqWebrtcSignaller",
            sizeof(TalqSigClass), (GClassInitFunc)talqsig_class_init,
            sizeof(TalqSig), nullptr, (GTypeFlags)0);
        GType iface = g_type_from_name("GstRSWebRTCSignallableIface");
        if (iface) {
            // interface_init copies the Rust no-op default vtable into our
            // per-type vtable (GLib won't, and a zeroed vtable = NULL-call
            // crash). We never synthesise the slots ourselves.
            GInterfaceInfo info = { talqsig_iface_init, nullptr, nullptr };
            g_type_add_interface_static(base, iface, &info);
        } else {
            qWarning() << "SubscribeWebrtcSrc: GstRSWebRTCSignallableIface "
                          "missing — libgstrswebrtc not loaded";
        }
        g_once_init_leave(&t, base);
    }
    return t;
}

// webrtcsrc's turn-servers / video-codecs / audio-codecs are
// "GstValueArray of gchararray", NOT a GStrv. Passing a char** to
// g_object_set makes GLib read garbage as the array length (the 51 GB
// g_malloc abort). Build a proper GST_TYPE_ARRAY GValue instead.
void setStringArrayProp(GstElement *e, const char *prop,
                        const QList<QByteArray> &vals)
{
    GValue arr = G_VALUE_INIT;
    g_value_init(&arr, GST_TYPE_ARRAY);
    GValue s = G_VALUE_INIT;
    g_value_init(&s, G_TYPE_STRING);
    for (const QByteArray &v : vals) {
        g_value_set_string(&s, v.constData());
        gst_value_array_append_value(&arr, &s);
    }
    g_value_unset(&s);
    g_object_set_property(G_OBJECT(e), prop, &arr);
    g_value_unset(&arr);
}

// DTLS-SRTP deep-dive: dump only the negotiation-critical SDP lines
// (no ICE pwd / no full SDP). a=setup decides the DTLS client/server
// role — if offer and answer pick the SAME role, DTLS deadlocks and
// srtp never keys. ssrc/rtpmap reveal the SSRC-caps mismatch.
void logSdpKeys(const char *tag, const QString &sdp)
{
    const auto lines = sdp.split('\n');
    for (const auto &raw : lines) {
        const QString l = raw.trimmed();
        if (l.startsWith("m=") || l.startsWith("a=setup") ||
            l.startsWith("a=fingerprint") || l.startsWith("a=ice-ufrag") ||
            l.startsWith("a=mid") || l.startsWith("a=group") ||
            l.startsWith("a=rtpmap") || l.startsWith("a=ssrc") ||
            l.startsWith("a=sendrecv") || l.startsWith("a=recvonly") ||
            l.startsWith("a=sendonly") || l.startsWith("a=inactive") ||
            l.startsWith("a=rtcp-mux") || l.startsWith("a=bundle-only"))
            qDebug().noquote() << "  [" << tag << "]" << l.left(90);
    }
}
} // namespace

SubscribeWebrtcSrc::SubscribeWebrtcSrc(const QString &remoteSessionId, QObject *parent)
    : QObject(parent), m_remoteSessionId(remoteSessionId)
{
    m_videoProvider = new VideoFrameProvider(this);
    m_sessionId = remoteSessionId.isEmpty() ? QStringLiteral("talq") : remoteSessionId;
}

SubscribeWebrtcSrc::~SubscribeWebrtcSrc() { stop(); }

GObject *SubscribeWebrtcSrc::makeSignaller()
{
    auto *s = (TalqSig*)g_object_new(talq_signaller_get_type(), nullptr);
    return s ? G_OBJECT(s) : nullptr;
}

bool SubscribeWebrtcSrc::start(const QString &stunServer,
                               const QList<TurnServer> &turnServers,
                               const QString &audioOutputDeviceId)
{
    m_audioOutputDeviceId = audioOutputDeviceId;

    // Load the plugin (NOT instantiate) so the interface GType exists
    // before we build our signaller — webrtcsrc's `signaller` is
    // construct-only and must be passed at element construction.
    GstElementFactory *f = gst_element_factory_find("webrtcsrc");
    if (!f || !gst_plugin_feature_load(GST_PLUGIN_FEATURE(f))) {
        if (f) gst_object_unref(f);
        emit error("webrtcsrc unavailable — is libgstrswebrtc deployed?");
        return false;
    }
    qDebug() << "SubscribeWebrtcSrc[ck1]: webrtcsrc plugin loaded";
    m_signaller = makeSignaller();
    if (!m_signaller) {
        gst_object_unref(f);
        emit error("Failed to create WebRTC signaller");
        return false;
    }
    qDebug() << "SubscribeWebrtcSrc[ck2]: signaller created type"
             << g_type_name(G_TYPE_FROM_INSTANCE(m_signaller));

    m_pipeline = gst_pipeline_new(nullptr);
    {
        const char *names[1] = { "signaller" };
        GValue v = G_VALUE_INIT;
        // Concrete type, NOT G_TYPE_OBJECT: g_object_set_property validates
        // g_type_is_a(value_type, pspec->value_type) and the `signaller`
        // pspec's type is the Signallable interface. TalqWebrtcSignaller
        // is_a that interface; bare G_TYPE_OBJECT is not → silently dropped.
        g_value_init(&v, talq_signaller_get_type());
        g_value_set_object(&v, m_signaller);
        m_webrtcsrc = gst_element_factory_make_with_properties("webrtcsrc", 1, names, &v);
        g_value_unset(&v);
    }
    gst_object_unref(f);
    if (!m_pipeline || !m_webrtcsrc) {
        emit error("Failed to create webrtcsrc element");
        cleanup();
        return false;
    }
    qDebug() << "SubscribeWebrtcSrc[ck3]: webrtcsrc element constructed";

    if (!stunServer.isEmpty()) {
        QString gstStun = stunServer;
        if (gstStun.startsWith("stun:") && !gstStun.startsWith("stun://"))
            gstStun = "stun://" + gstStun.mid(5);
        g_object_set(m_webrtcsrc, "stun-server", gstStun.toUtf8().constData(), nullptr);
    }
    {
        QStringList urls;
        for (const auto &turn : turnServers)
            for (const auto &url : turn.urls) {
                QString u = url;
                u.remove(QRegularExpression("\\?transport=.*$"));
                if (u.startsWith("turn:")  && !u.startsWith("turn://"))  u.replace("turn:",  "turn://");
                if (u.startsWith("turns:") && !u.startsWith("turns://")) u.replace("turns:", "turns://");
                u.replace("://", QString("://%1:%2@").arg(
                    QString(QUrl::toPercentEncoding(turn.username)),
                    QString(QUrl::toPercentEncoding(turn.credential))));
                urls << u;
            }
        if (!urls.isEmpty()) {
            QList<QByteArray> enc;
            for (const auto &u : urls) enc << u.toUtf8();
            setStringArrayProp(m_webrtcsrc, "turn-servers", enc);
        }
    }
    setStringArrayProp(m_webrtcsrc, "video-codecs",
                       { QByteArrayLiteral("H264"), QByteArrayLiteral("VP8") });

    // D5 — raise the receive jitterbuffer latency (default ~200 ms) so a choppy /
    // jittery uplink (bursty loss, big inter-packet gaps — Pavel's link) is RIDDEN
    // OUT by buffering instead of dropping late packets and freezing the decoder.
    // 400 ms balances more cushion against conversational lag. Guarded on property
    // existence (gst-plugins-rs versions vary).
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(m_webrtcsrc), "latency"))
        g_object_set(m_webrtcsrc, "latency", (guint)400, nullptr);

    g_signal_connect(m_webrtcsrc, "pad-added",
                     G_CALLBACK(&SubscribeWebrtcSrc::onPadAdded), this);

    // Wire the signaller signals BEFORE the pipeline reaches PLAYING —
    // webrtcsrc emits "start" on the signaller during its own state
    // change. These connected handlers run before the Rust no-op default
    // class closure (RUN_LAST), so we get every payload.
    //   webrtcsrc → us:  start / stop / send-session-description (our
    //   answer) / send-ice (our local ICE) / end-session.
    g_signal_connect(m_signaller, "start",
                     G_CALLBACK(&SubscribeWebrtcSrc::sigStart), this);
    g_signal_connect(m_signaller, "stop",
                     G_CALLBACK(&SubscribeWebrtcSrc::sigStop), this);
    g_signal_connect(m_signaller, "send-session-description",
                     G_CALLBACK(&SubscribeWebrtcSrc::sigSendSdp), this);
    g_signal_connect(m_signaller, "send-ice",
                     G_CALLBACK(&SubscribeWebrtcSrc::sigSendIce), this);
    g_signal_connect(m_signaller, "end-session",
                     G_CALLBACK(&SubscribeWebrtcSrc::sigEndSession), this);
    g_signal_connect(m_signaller, "webrtcbin-ready",
                     G_CALLBACK(&SubscribeWebrtcSrc::onWebrtcbinReady), this);

    qDebug() << "SubscribeWebrtcSrc[ck4]: signaller signals connected";
    gst_bin_add(GST_BIN(m_pipeline), m_webrtcsrc);

    GstBus *bus = gst_element_get_bus(m_pipeline);
    m_busWatchId = gst_bus_add_watch(bus, [](GstBus *, GstMessage *msg, gpointer ud) -> gboolean {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *e = nullptr; gchar *d = nullptr;
            gst_message_parse_error(msg, &e, &d);
            const gchar *sn = GST_MESSAGE_SRC(msg) ? GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)) : "?";
            qWarning().nospace() << "SubscribeWebrtcSrc ERROR from " << sn << ": "
                                 << (e ? e->message : "?") << " | " << (d ? d : "");
            auto *self = static_cast<SubscribeWebrtcSrc*>(ud);
            QPointer<SubscribeWebrtcSrc> g(self);
            // B2 — prefix the SOURCE element name (e.g. "d3d11h264dec0") so the
            // CallManager handler can classify a decoder-domain fault and fall
            // back to software decode. The GError message alone often omits it.
            QString m = QStringLiteral("[%1] %2")
                            .arg(QString::fromUtf8(sn),
                                 e ? QString::fromUtf8(e->message)
                                   : QStringLiteral("pipeline error"));
            QMetaObject::invokeMethod(self, [g, m]() { if (g) emit g->error(m); },
                                      Qt::QueuedConnection);
            g_clear_error(&e); g_free(d);
        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ELEMENT) {
            // `level` element on the playback chain posts the decoded remote
            // audio peak here → forward as a 0..1 level so this peer's VU meter
            // moves. Same dB→level mapping + perceptual curve the publisher uses
            // for the self meter (PublishPipeline::pollBus), so both read alike.
            const GstStructure *ls = gst_message_get_structure(msg);
            if (ls && g_strcmp0(gst_structure_get_name(ls), "level") == 0) {
                GValueArray *arr = nullptr;
                gst_structure_get(ls, "peak", G_TYPE_VALUE_ARRAY, &arr, nullptr);
                if (arr && arr->n_values > 0) {
                    gdouble db = g_value_get_double(arr->values);
                    double lvl = qBound(0.0, (db + 100.0) / 100.0, 1.0);
                    lvl = lvl * lvl;   // perceptual curve (match self meter)
                    auto *self = static_cast<SubscribeWebrtcSrc*>(ud);
                    QPointer<SubscribeWebrtcSrc> g(self);
                    QMetaObject::invokeMethod(self, [g, lvl]() {
                        if (g) emit g->audioLevelUpdated(lvl);
                    }, Qt::QueuedConnection);
                }
                if (arr) g_value_array_free(arr);
            }
        }
        return TRUE;
    }, this);
    gst_object_unref(bus);

    qDebug() << "SubscribeWebrtcSrc[ck5]: setting pipeline PLAYING";
    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        emit error("Failed to start webrtcsrc pipeline");
        cleanup();
        return false;
    }
    m_running = true;
    qDebug() << "SubscribeWebrtcSrc: started for" << m_remoteSessionId.left(20);
    // If the Janus offer already arrived, this no-ops until webrtcsrc
    // emits "start" (sigStart re-drives it); otherwise it fires now.
    feedOfferToSignaller();
    return true;
}

void SubscribeWebrtcSrc::stop()
{
    if (!m_running && !m_pipeline) return;
    cleanup();
    m_running = false;
    qInfo() << "SubscribeWebrtcSrc: stopped";
}

void SubscribeWebrtcSrc::setFarEndAppsrc(GstElement *appsrc)
{
    // Qt thread (before start(), and again on a publisher REBUILD to re-point at
    // the new mixer). REF the appsrc so our streaming-thread pushes stay valid
    // across a publisher-teardown race; atomically swap and unref the previous
    // one. The final ref is dropped in cleanup() after this pipeline's NULL.
    GstElement *fresh = appsrc ? GST_ELEMENT(gst_object_ref(appsrc)) : nullptr;
    GstElement *prev;
    {
        std::lock_guard<std::mutex> lk(m_farAppsrcMutex);
        prev = m_farAppsrcExt;
        m_farAppsrcExt = fresh;
    }
    // Unref the old OUTSIDE the lock. A concurrent onAecPlayoutSample either
    // grabbed its own ref under the lock before this swap (so `prev` stays alive
    // through its push) or runs after and reads `fresh`.
    if (prev) gst_object_unref(prev);
}

GstFlowReturn SubscribeWebrtcSrc::onAecPlayoutSample(GstAppSink *sink, gpointer ud)
{
    auto *self = static_cast<SubscribeWebrtcSrc*>(ud);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    // Take a ref on the current appsrc UNDER the lock, so a concurrent
    // setFarEndAppsrc/cleanup swap-then-unref (rebuild re-point) can't free it
    // while we push. Push OUTSIDE the lock (push_sample can block on a full
    // queue; we must not hold the lock across it). push_sample doesn't take
    // ownership of the sample — we unref it.
    GstElement *appsrc = nullptr;
    {
        std::lock_guard<std::mutex> lk(self->m_farAppsrcMutex);
        appsrc = self->m_farAppsrcExt;
        if (appsrc) gst_object_ref(appsrc);
    }
    if (appsrc) {
        gst_app_src_push_sample(GST_APP_SRC(appsrc), sample);
        gst_object_unref(appsrc);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

namespace {
// Heap context for the subscriber get-stats promise: ONLY a copy of the
// shared-ptr inbound-stats cell — no `self` pointer. The async reply
// callback writes the cell and never touches SubscribeWebrtcSrc, so a late
// reply that races this subscriber's teardown/rebuild is harmless (the cell
// outlives the object). Freed by the promise's GDestroyNotify. Mirrors
// PublishPipeline's PubStatsCtx exactly (see PublishPipeline.cpp).
struct SubStatsCtx {
    std::shared_ptr<SubscribeWebrtcSrc::InboundStatsCell> cell;
};
void subStatsCtxFree(gpointer p) { delete static_cast<SubStatsCtx *>(p); }

// The exact GValue type GstWebRTCStats fields use for a given stat was NOT
// verified anywhere in this repo before this feature (no prior inbound-rtp
// stats consumer existed) — only the GstWebRTCStatsType enum is declared in
// the local gstreamer-1.0 headers, not field shapes. Try the plausible
// integer representations defensively instead of hard-coding one; if NONE
// match, the caller warns once (loud, not a silently-inert glyph).
bool getStatCounter(const GstStructure *s, const char *field, quint64 &out)
{
    guint64 u64 = 0;
    if (gst_structure_get_uint64(s, field, &u64)) { out = u64; return true; }
    gint64 i64 = 0;
    if (gst_structure_get_int64(s, field, &i64)) { out = i64 < 0 ? 0 : quint64(i64); return true; }
    guint u32 = 0;
    if (gst_structure_get_uint(s, field, &u32)) { out = u32; return true; }
    gint i32 = 0;
    if (gst_structure_get_int(s, field, &i32)) { out = i32 < 0 ? 0 : quint64(i32); return true; }
    return false;
}

// "jitter" per W3C/GstWebRTC convention is a gdouble in SECONDS; fall back to
// an integer representation defensively (same reasoning as above).
bool getStatSeconds(const GstStructure *s, const char *field, double &outSeconds)
{
    gdouble d = 0.0;
    if (gst_structure_get_double(s, field, &d)) { outSeconds = d; return true; }
    guint64 u64 = 0;
    if (gst_structure_get_uint64(s, field, &u64)) { outSeconds = double(u64); return true; }
    return false;
}
} // namespace

void SubscribeWebrtcSrc::pollInboundRtp()
{
    if (!m_webrtcbin || m_shuttingDown.load()) return;
    // webrtcbin "get-stats" is async: it replies via a GstPromise carrying a
    // GstStructure of all RTC stats. onInboundStatsReady parses it on a
    // GStreamer thread and writes the shared cell that CallManager's
    // SignalQualityPolicy reads on the next 2 s tick. Exactly mirrors
    // PublishPipeline::pollOutboundRtp() (see the safety comment there).
    auto *ctx = new SubStatsCtx{ m_inboundStats };
    GstPromise *promise =
        gst_promise_new_with_change_func(onInboundStatsReady, ctx, subStatsCtxFree);
    g_signal_emit_by_name(m_webrtcbin, "get-stats", nullptr, promise);
}

void SubscribeWebrtcSrc::onInboundStatsReady(GstPromise *promise, gpointer userData)
{
    // Runs on a GStreamer thread. Touches ONLY the shared cell (which
    // outlives this object) — never `this` — so a late reply during
    // subscriber teardown/rebuild is safe (mirrors PublishPipeline's
    // onStatsReady).
    auto *ctx = static_cast<SubStatsCtx *>(userData);
    std::shared_ptr<InboundStatsCell> cell = ctx->cell;

    const GstStructure *reply = gst_promise_get_reply(promise);
    quint64 sumLost = 0, sumRecv = 0;
    double maxJitterSec = 0.0;
    bool foundLost = false, foundRecv = false;
    bool sawInboundRtp = false;
    if (reply) {
        // Gated RCA logging — TALQ_DEBUG_STATS=1. Risk #2 from the design
        // review: the field names/units below are standard GstWebRTC/W3C
        // convention but were never verified against THIS installed
        // gst-plugins-bad/gst-plugins-rs build before this feature, so a
        // one-time dump of the raw reply makes a stat-shape mismatch loud
        // instead of a silently-inert glyph.
        if (const char *dbg = std::getenv("TALQ_DEBUG_STATS"); dbg && *dbg == '1') {
            static std::atomic<bool> dumped{false};
            if (!dumped.exchange(true)) {
                gchar *s = gst_structure_to_string(reply);
                qDebug() << "SubscribeWebrtcSrc: inbound-rtp get-stats reply =" << s;
                g_free(s);
            }
        }
        // Flat structure whose fields are themselves GstStructures, one per
        // stat object (same technique as PublishPipeline::onStatsReady).
        // SUM packets-lost/packets-received across every inbound-rtp entry
        // (audio + video/simulcast legs) — matches how packets-sent is
        // summed on the publish side. Jitter is NOT summable across legs, so
        // take the MAX (the worst leg) as the tie-break signal instead.
        const int n = gst_structure_n_fields(reply);
        for (int i = 0; i < n; ++i) {
            const gchar *name = gst_structure_nth_field_name(reply, i);
            const GValue *val = gst_structure_get_value(reply, name);
            if (!val || !GST_VALUE_HOLDS_STRUCTURE(val)) continue;
            const GstStructure *s = static_cast<const GstStructure *>(
                g_value_get_boxed(val));
            if (!s) continue;
            GstWebRTCStatsType type;
            if (!(gst_structure_get(s, "type", GST_TYPE_WEBRTC_STATS_TYPE, &type, nullptr)
                  && type == GST_WEBRTC_STATS_INBOUND_RTP))
                continue;
            sawInboundRtp = true;
            quint64 lost = 0, recv = 0;
            if (getStatCounter(s, "packets-lost", lost)) { sumLost += lost; foundLost = true; }
            if (getStatCounter(s, "packets-received", recv)) { sumRecv += recv; foundRecv = true; }
            double jitterSec = 0.0;
            if (getStatSeconds(s, "jitter", jitterSec) && jitterSec > maxJitterSec)
                maxJitterSec = jitterSec;
        }
    }
    gst_promise_unref(promise);

    if (foundLost && foundRecv) {
        cell->packetsLost.store(sumLost, std::memory_order_relaxed);
        cell->packetsReceived.store(sumRecv, std::memory_order_relaxed);
        cell->jitterUs.store(quint64(qMax(0.0, maxJitterSec) * 1e6), std::memory_order_relaxed);
        cell->valid.store(true, std::memory_order_relaxed);
    } else if (sawInboundRtp) {
        // An inbound-rtp object exists but its packets-lost/packets-received
        // could not be read: a stat-shape mismatch on this libgstwebrtc would
        // leave the signal-quality glyph silently INERT (never sees valid
        // data -> never buckets -> stays on its optimistic default forever).
        // Surface once, loudly, so it is caught rather than a silent no-op.
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            qWarning() << "SubscribeWebrtcSrc: inbound-rtp stat has no readable "
                          "packets-lost/packets-received — signal-quality glyph "
                          "is INERT (libgstwebrtc stat-shape mismatch)";
    }
}

void SubscribeWebrtcSrc::cleanup()
{
    m_shuttingDown.store(true);   // 0.52.17 — block a late streaming-thread onPadAdded from re-pointing m_videoAppsink
    if (m_keyframeWatchdog) m_keyframeWatchdog->stop();   // no PLI on a torn-down pipeline
    if (m_livenessWatchdog) m_livenessWatchdog->stop();   // D1 — same, persistent watchdog
    if (m_busWatchId > 0) { g_source_remove(m_busWatchId); m_busWatchId = 0; }
    if (m_webrtcsrc)    g_signal_handlers_disconnect_by_data(m_webrtcsrc, this);
    if (m_webrtcbin)    g_signal_handlers_disconnect_by_data(m_webrtcbin, this);
    // 0.52.17 — snapshot the appsink AND the pipeline together under m_videoSinkMutex,
    // then do the (slow) disconnect / unref / worker-spawn OUTSIDE the lock. onPadAdded
    // attaches conv+sink to m_pipeline AND ref-stores m_videoAppsink under the SAME lock
    // while re-checking m_shuttingDown, so the two are SERIALIZED against teardown:
    // cleanup either sees onPadAdded's appsink+pipeline here (and tears them down) or
    // onPadAdded runs after and bails on m_shuttingDown / null m_pipeline. This closes
    // BOTH (a) the hangup UAF — g_signal_handlers_disconnect_by_data on a FREED appsink,
    // the exact 0.52.16 crash (the deleteLater() fix was downstream of here) — via OUR
    // ref (taken in onPadAdded) keeping the appsink alive across the disconnect, AND
    // (b) the teardown race where onPadAdded could gst_bin_add_many(GST_BIN(null)) →
    // crash, or orphan its appsink ref → leak.
    GstElement *vsink = nullptr, *pipe = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_videoSinkMutex);
        vsink = m_videoAppsink; m_videoAppsink = nullptr;
        pipe  = m_pipeline;     m_pipeline     = nullptr;
    }
    if (vsink) {
        g_signal_handlers_disconnect_by_data(vsink, this);
        gst_object_unref(vsink);   // drop the ref taken in onPadAdded
    }
    if (m_aecPlayoutSink) g_signal_handlers_disconnect_by_data(m_aecPlayoutSink, this); // AEC single-pipeline
    if (m_signaller)    g_signal_handlers_disconnect_by_data(m_signaller, this);
    m_webrtcbin = nullptr;  // webrtcsrc-owned; not unref'd
    if (pipe) {
        // 0.43.0 — detach the NULL transition to a worker thread: the synchronous
        // set_state(NULL) of a webrtcsrc + decodebin3 + HW decoder can block the Qt
        // main thread on a pad stream lock / GPU teardown when a peer leaves.
        // 0.51.x AEC fix: hand our ref on the publisher's far-end appsrc to the worker
        // so it's unrefed AFTER this pipeline's NULL — by then the AEC appsink + its
        // streaming thread are stopped, so no push can race the unref.
        GstElement *farAppsrc;
        { std::lock_guard<std::mutex> lk(m_farAppsrcMutex); farAppsrc = m_farAppsrcExt; m_farAppsrcExt = nullptr; }
        std::thread([pipe, farAppsrc]() {
            gst_element_set_state(pipe, GST_STATE_NULL);
            gst_object_unref(pipe);
            if (farAppsrc) gst_object_unref(farAppsrc);
        }).detach();
    }
    if (m_signaller) { g_object_unref(m_signaller); m_signaller = nullptr; }
    m_webrtcsrc = nullptr;
    m_videoConvert = nullptr;
    // m_videoAppsink already nulled+unref'd under m_videoSinkMutex above
    m_aecPlayoutSink = nullptr; // owned by the pipeline (freed with it)
    // AEC fix: if there was no pipeline, the worker block above didn't run — drop
    // our appsrc ref here (null if the worker already took it).
    GstElement *fa;
    { std::lock_guard<std::mutex> lk(m_farAppsrcMutex); fa = m_farAppsrcExt; m_farAppsrcExt = nullptr; }
    if (fa) gst_object_unref(fa);
    m_offerDelivered = false;
    m_webrtcStarted = false;
    m_pendingRemoteIce.clear();
}

void SubscribeWebrtcSrc::setRemoteOffer(const QString &sdp)
{
    m_pendingOffer = sdp;          // held until webrtcsrc has emitted "start"
    feedOfferToSignaller();
}

// Janus is the offerer in the MCU subscriber model, so webrtcsrc is the
// ANSWERER. The answerer handshake is: session-started (webrtcsrc builds
// its webrtcbin/session) → session-description with the OFFER (webrtcsrc
// sets remote, creates the answer, emits "send-session-description" back)
// → handle-ice for each remote candidate. We must NOT emit
// "session-requested" with a null offer — that makes webrtcsrc generate
// its OWN offer and glare with Janus. Runs once, only when both the offer
// is in hand and webrtcsrc has emitted "start".
void SubscribeWebrtcSrc::feedOfferToSignaller()
{
    QMutexLocker lk(&m_feedMutex);
    if (!m_signaller || !m_webrtcStarted || m_offerDelivered ||
        m_pendingOffer.isEmpty())
        return;

    const QByteArray sdpUtf8 = m_pendingOffer.toUtf8();
    GstSDPMessage *msg = nullptr;
    if (gst_sdp_message_new(&msg) != GST_SDP_OK ||
        gst_sdp_message_parse_buffer((const guint8*)sdpUtf8.constData(),
                                     sdpUtf8.size(), msg) != GST_SDP_OK) {
        if (msg) gst_sdp_message_free(msg);
        lk.unlock();
        emit error("Could not parse the incoming video offer");
        return;
    }
    GstWebRTCSessionDescription *desc =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, msg);

    const QByteArray sid  = m_sessionId.toUtf8();
    const QByteArray peer = m_remoteSessionId.toUtf8();
    qDebug() << "SubscribeWebrtcSrc: handshake → session-started + offer"
                " (session" << m_sessionId.left(12) << ")";
    logSdpKeys("OFFER", m_pendingOffer);
    g_signal_emit_by_name(m_signaller, "session-started",
                          sid.constData(), peer.constData());
    g_signal_emit_by_name(m_signaller, "session-description",
                          sid.constData(), desc);
    gst_webrtc_session_description_free(desc);
    m_offerDelivered = true;
    // Remote ICE is NOT flushed here via "handle-ice" (same unreliable
    // run-stage path as send-ice). webrtcsrc emits "webrtcbin-ready"
    // right after this; onWebrtcbinReady flushes m_pendingRemoteIce
    // straight into webrtcbin.
}

void SubscribeWebrtcSrc::addIceCandidate(const QString &candidate,
                                         int sdpMLineIndex, const QString &sdpMid)
{
    Q_UNUSED(sdpMid)
    // Prefer the authoritative webrtcbin path (set once webrtcbin-ready
    // fired). Until then queue; onWebrtcbinReady flushes the backlog.
    if (m_webrtcbin) {
        qDebug() << "SubscribeWebrtcSrc: addIceCandidate → webrtcbin mline"
                 << sdpMLineIndex << candidate.left(50);
        const QByteArray cand = candidate.toUtf8();
        g_signal_emit_by_name(m_webrtcbin, "add-ice-candidate",
                               (guint)sdpMLineIndex, cand.constData());
        return;
    }
    qDebug() << "SubscribeWebrtcSrc: addIceCandidate QUEUED (no webrtcbin yet)"
                " mline" << sdpMLineIndex;
    m_pendingRemoteIce.append({ sdpMLineIndex, candidate });
}

// webrtcsrc emitted "start" on the signaller — its handlers are now
// connected and it is ready for the session. Re-drive the offer feed
// (deferred to the Qt loop so it never re-enters this emission).
gboolean SubscribeWebrtcSrc::sigStart(GObject *, gpointer ud)
{
    auto *self = static_cast<SubscribeWebrtcSrc*>(ud);
    {
        QMutexLocker lk(&self->m_feedMutex);
        self->m_webrtcStarted = true;
    }
    qDebug() << "SubscribeWebrtcSrc: webrtcsrc started signaller";
    QPointer<SubscribeWebrtcSrc> g(self);
    QMetaObject::invokeMethod(self, [g]() { if (g) g->feedOfferToSignaller(); },
                              Qt::QueuedConnection);
    return TRUE;
}

gboolean SubscribeWebrtcSrc::sigStop(GObject *, gpointer)
{
    qDebug() << "SubscribeWebrtcSrc: signaller stop";
    return TRUE;
}

gboolean SubscribeWebrtcSrc::sigEndSession(GObject *, const gchar *sid, gpointer ud)
{
    qInfo() << "SubscribeWebrtcSrc: end-session" << (sid ? sid : "?");
    // The SFU ended THIS subscriber feed (publisher migrated / renegotiated
    // — e.g. peer toggled their camera). Tell CallManager so it can
    // re-subscribe, instead of letting the follow-on webrtcbin ICE
    // "failed" tear the whole call down. Queued: we're on a GStreamer/
    // signaller thread; deliver on the Qt event loop.
    auto *self = static_cast<SubscribeWebrtcSrc*>(ud);
    if (self) {
        QPointer<SubscribeWebrtcSrc> g(self);
        QMetaObject::invokeMethod(self, [g]() {
            if (g) emit g->sessionEnded();
        }, Qt::QueuedConnection);
    }
    return TRUE;
}

// webrtcsrc exposes its internal webrtcbin. The send-ice signal lacks
// .run_last() (unlike send-session-description), so from a C signaller
// our connected handler can be pre-empted by the RUN_FIRST class
// closure + Break accumulator and never fire — leaving the subscriber
// with zero local ICE → DTLS never keys. Bind ICE straight to
// webrtcbin instead (plain GStreamer C API): authoritative, no signal
// run-stage ambiguity.
void SubscribeWebrtcSrc::onWebrtcbinReady(GObject *, const gchar *peerId,
                                          GstElement *webrtcbin, gpointer ud)
{
    auto *self = static_cast<SubscribeWebrtcSrc*>(ud);
    if (!self || !webrtcbin) return;
    self->m_webrtcbin = webrtcbin;  // webrtcsrc-owned; do not unref
    qDebug() << "SubscribeWebrtcSrc: webrtcbin-ready peer"
             << (peerId ? peerId : "?") << "→ driving ICE on webrtcbin";
    // Disable ICE-TCP gathering, matching PublishPipeline. webrtcsrc doesn't
    // expose ice-tcp on itself -- it lives on the nested ice-agent
    // (GstWebRTCNice) that only becomes reachable once webrtcbin exists, i.e.
    // right here. See PublishPipeline.cpp's ice-agent block for the full
    // rationale (Janus runs ICE-TCP disabled cluster-wide; libnice
    // unconditionally rejects TCP-transport remote candidates either way).
    {
        GObject *iceAgent = nullptr;
        g_object_get(webrtcbin, "ice-agent", &iceAgent, nullptr);
        if (iceAgent) {
            g_object_set(iceAgent, "ice-tcp", FALSE, nullptr);
            g_object_unref(iceAgent);
        }
    }
    g_signal_connect(webrtcbin, "on-ice-candidate",
                     G_CALLBACK(&SubscribeWebrtcSrc::onWebrtcbinLocalIce), self);
    // Propagate the REAL ICE connection state. Without this iceStateChanged
    // is never emitted, so CallManager never sees the subscriber connect
    // and the call is stuck "Connecting" forever even though media flows.
    g_signal_connect(webrtcbin, "notify::ice-connection-state",
                     G_CALLBACK(&SubscribeWebrtcSrc::onWebrtcbinIceConnState), self);
    // webrtcbin is created mid-handshake and may already be past CONNECTED
    // before we attached the notify — emit the current state once so we
    // don't wait forever for an edge that already fired.
    SubscribeWebrtcSrc::onWebrtcbinIceConnState(webrtcbin, nullptr, self);
    // Flush any remote candidates that arrived before the bin existed.
    for (const auto &c : self->m_pendingRemoteIce) {
        const QByteArray cand = c.second.toUtf8();
        g_signal_emit_by_name(webrtcbin, "add-ice-candidate",
                               (guint)c.first, cand.constData());
    }
    self->m_pendingRemoteIce.clear();
}

void SubscribeWebrtcSrc::onWebrtcbinLocalIce(GstElement *, guint mlineIndex,
                                             gchar *candidate, gpointer ud)
{
    auto *self = static_cast<SubscribeWebrtcSrc*>(ud);
    if (!self || !candidate) return;
    qDebug() << "SubscribeWebrtcSrc: webrtcbin LOCAL ICE mline" << mlineIndex
             << QString::fromUtf8(candidate).left(50);
    QString c = QString::fromUtf8(candidate);
    int idx = (int)mlineIndex;
    QPointer<SubscribeWebrtcSrc> g(self);
    QMetaObject::invokeMethod(self, [g, c, idx]() {
        if (g) emit g->iceCandidateReady(c, idx, QString());
    }, Qt::QueuedConnection);
}

// webrtcbin's ICE connection state changed (or queried once on bind).
// Maps GstWebRTCICEConnectionState → the strings CallManager expects and
// emits iceStateChanged so the call can leave "Connecting" / go Active.
void SubscribeWebrtcSrc::onWebrtcbinIceConnState(GstElement *webrtcbin,
                                                 GParamSpec *, gpointer ud)
{
    auto *self = static_cast<SubscribeWebrtcSrc*>(ud);
    if (!self || !webrtcbin) return;
    GstWebRTCICEConnectionState st = GST_WEBRTC_ICE_CONNECTION_STATE_NEW;
    g_object_get(webrtcbin, "ice-connection-state", &st, nullptr);
    if ((int)st == self->m_lastIceConnState) return;   // de-dup notify spam
    self->m_lastIceConnState = (int)st;
    const char *s = "new";
    switch (st) {
        case GST_WEBRTC_ICE_CONNECTION_STATE_NEW:          s = "new";          break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CHECKING:     s = "checking";     break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED:    s = "connected";    break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED:    s = "completed";    break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_FAILED:       s = "failed";       break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED: s = "disconnected"; break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CLOSED:       s = "closed";       break;
        default: break;
    }
    QString state = QString::fromLatin1(s);
    qInfo() << "SubscribeWebrtcSrc: webrtcbin ICE connection state ->" << state;
    // The moment ICE is connected, send a keyframe request upstream so
    // webrtcsrc emits an RTCP PLI to the MCU. Janus forwards it to the
    // publisher; the publisher's encoder generates a fresh I-frame. This
    // recovers from any bootstrap-time packet loss (first ~RTT of RTP
    // arrives before the receive jitter buffer / DTLS / ICE checks have
    // fully settled) that would otherwise leave the decoder in concealment
    // until the publisher's next periodic keyframe — the field-reported
    // "caller-side always sees the callee choppy" pattern.
    if (st == GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED ||
        st == GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED) {
        if (self->m_pipeline) {
            GstStructure *ks = gst_structure_new("GstForceKeyUnit",
                "all-headers", G_TYPE_BOOLEAN, TRUE, nullptr);
            GstEvent *kev = gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM, ks);
            gst_element_send_event(self->m_pipeline, kev);
            qInfo() << "SubscribeWebrtcSrc: requested PLI on ICE-connected";
        }
    }
    QPointer<SubscribeWebrtcSrc> g(self);
    QMetaObject::invokeMethod(self, [g, state]() {
        if (!g) return;
        emit g->iceStateChanged(state);
        // Keyframe-stall watchdog (main thread). The single PLI above can be lost
        // in the same bootstrap window as the IDR; if no frame decodes within a
        // few seconds the tile stays frozen on "Starting a screen share…" /
        // concealment. Re-request a keyframe every 2.5 s, up to 5×, until the
        // first frame arrives (m_decodedAnyFrame flips on the streaming thread).
        const bool connected = (state == QLatin1String("connected")
                                || state == QLatin1String("completed"));
        if (!connected || g->m_decodedAnyFrame.load(std::memory_order_relaxed))
            return;
        if (!g->m_keyframeWatchdog) {
            g->m_keyframeWatchdog = new QTimer(g);
            g->m_keyframeWatchdog->setInterval(2500);
            QObject::connect(g->m_keyframeWatchdog, &QTimer::timeout, g, [g]() {
                if (!g || !g->m_keyframeWatchdog) return;
                if (g->m_decodedAnyFrame.load(std::memory_order_relaxed)
                    || g->m_keyframePliRetriesLeft <= 0 || !g->m_pipeline) {
                    g->m_keyframeWatchdog->stop();
                    return;
                }
                --g->m_keyframePliRetriesLeft;
                GstStructure *ks = gst_structure_new(
                    "GstForceKeyUnit", "all-headers", G_TYPE_BOOLEAN, TRUE, nullptr);
                gst_element_send_event(g->m_pipeline,
                    gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM, ks));
                qInfo() << "SubscribeWebrtcSrc: keyframe-watchdog re-requested PLI,"
                        << g->m_keyframePliRetriesLeft << "retries left";
            });
        }
        g->m_keyframePliRetriesLeft = 5;
        g->m_keyframeWatchdog->start();

        // D1 — arm the PERSISTENT decode-liveness watchdog (once). Unlike the
        // bootstrap watchdog above it never disarms on the first frame: it nudges
        // a keyframe whenever the decoder starts CONCEALING (frames keep arriving
        // but content is frozen → distinct-fps collapses) or the inter-frame gap
        // blows out, recovering a mid-call freeze before the ~12 s full rebuild.
        if (!g->m_livenessWatchdog) {
            g->m_livenessWatchdog = new QTimer(g);
            g->m_livenessWatchdog->setInterval(2000);
            QObject::connect(g->m_livenessWatchdog, &QTimer::timeout, g, [g]() {
                if (!g || !g->m_pipeline) return;
                if (!g->m_decodedAnyFrame.load(std::memory_order_relaxed)) {
                    g->m_livenessBadTicks = 0; return;   // bootstrap watchdog owns pre-first-frame
                }
                if (g->m_livenessPliCooldown > 0) --g->m_livenessPliCooldown;
                const int fps  = g->m_rxFps.load(std::memory_order_relaxed);
                const int dist = g->m_rxDistinctFps.load(std::memory_order_relaxed);
                // CONCEALMENT only: frames keep arriving (fps>=3) but content is
                // frozen (distinct<=1) — a fresh keyframe can recover that. A merely
                // LOW framerate is NOT a receive stall a PLI fixes (and a forced
                // keyframe only burdens an already bandwidth-starved sender), so we
                // do NOT trigger on the inter-frame gap. A muted/idle peer sends no
                // frames (fps<3) so it is naturally excluded.
                const bool concealing = (fps >= 3 && dist <= 1);
                if (!concealing) { g->m_livenessBadTicks = 0; return; }
                if (++g->m_livenessBadTicks < 3) return;        // ~6 s of concealment before the first PLI
                if (g->m_livenessPliCooldown > 0) return;       // rate-limit: ~12 s between forced keyframes
                g->m_livenessBadTicks = 0;
                g->m_livenessPliCooldown = 6;                   // 6 ticks × 2 s
                gst_element_send_event(g->m_pipeline, gst_event_new_custom(
                    GST_EVENT_CUSTOM_UPSTREAM,
                    gst_structure_new("GstForceKeyUnit", "all-headers", G_TYPE_BOOLEAN, TRUE, nullptr)));
                qInfo() << "SubscribeWebrtcSrc: liveness watchdog -> PLI (concealing feed)";
            });
            g->m_livenessWatchdog->start();
        }
    }, Qt::QueuedConnection);
}

// webrtcsrc produced the LOCAL answer — forward to spreed.
gboolean SubscribeWebrtcSrc::sigSendSdp(GObject *, const gchar *,
                                        GstWebRTCSessionDescription *desc,
                                        gpointer ud)
{
    auto *self = static_cast<SubscribeWebrtcSrc*>(ud);
    if (!self || !desc || !desc->sdp) return TRUE;
    gchar *txt = gst_sdp_message_as_text(desc->sdp);
    QString sdp = QString::fromUtf8(txt ? txt : "");
    g_free(txt);
    qDebug() << "SubscribeWebrtcSrc: local answer ready (" << sdp.size() << "B)";
    logSdpKeys("ANSWER", sdp);
    QPointer<SubscribeWebrtcSrc> g(self);
    QMetaObject::invokeMethod(self, [g, sdp]() {
        if (g) emit g->localAnswerReady(sdp);
    }, Qt::QueuedConnection);
    return TRUE;
}

// webrtcsrc produced a LOCAL ICE candidate — forward to spreed.
gboolean SubscribeWebrtcSrc::sigSendIce(GObject *, const gchar *,
                                        const gchar *candidate, guint mlineIndex,
                                        const gchar *mid, gpointer ud)
{
    auto *self = static_cast<SubscribeWebrtcSrc*>(ud);
    if (!self) return TRUE;
    if (self->m_webrtcbin) {  // webrtcbin path owns local ICE; avoid dupes
        qDebug() << "SubscribeWebrtcSrc: sigSendIce (ignored — webrtcbin path)";
        return TRUE;
    }
    qDebug() << "SubscribeWebrtcSrc: sigSendIce LOCAL candidate mline"
             << mlineIndex << QString::fromUtf8(candidate ? candidate : "").left(50);
    QString c = QString::fromUtf8(candidate ? candidate : "");
    QString m = QString::fromUtf8(mid ? mid : "");
    int idx = (int)mlineIndex;
    QPointer<SubscribeWebrtcSrc> g(self);
    QMetaObject::invokeMethod(self, [g, c, idx, m]() {
        if (g) emit g->iceCandidateReady(c, idx, m);
    }, Qt::QueuedConnection);
    return TRUE;
}

void SubscribeWebrtcSrc::onPadAdded(GstElement *, GstPad *pad, gpointer ud)
{
    auto *self = static_cast<SubscribeWebrtcSrc*>(ud);
    // 0.52.17 — the Qt main thread is tearing this subscriber down; do NOT build or
    // attach a new appsink into a pipeline that cleanup() is already NULLing (m_pipeline
    // is about to go null on a worker), and do not re-point m_videoAppsink mid-teardown.
    if (self->m_shuttingDown.load()) return;
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    if (!caps) return;
    const GstStructure *s = gst_caps_get_structure(caps, 0);
    const gchar *name = gst_structure_get_name(s);
    const bool isVideo = name && g_str_has_prefix(name, "video");
    const bool isAudio = name && g_str_has_prefix(name, "audio");
    gst_caps_unref(caps);

    if (isVideo && !self->m_videoAppsink) {
        GstElement *conv = gst_element_factory_make("videoconvert", nullptr);
        GstElement *sink = gst_element_factory_make("appsink", nullptr);
        if (!conv || !sink) { if (conv) gst_object_unref(conv); if (sink) gst_object_unref(sink); return; }
        GstCaps *sc = gst_caps_from_string("video/x-raw,format=BGRx");
        g_object_set(sink, "emit-signals", TRUE, "caps", sc,
                     "drop", TRUE, "max-buffers", 1, "sync", FALSE, nullptr);
        gst_caps_unref(sc);
        g_signal_connect(sink, "new-sample",
                         G_CALLBACK(&SubscribeWebrtcSrc::onVideoNewSample), self);
        // 0.52.17 — attach into the pipeline AND publish our own appsink ref ATOMICALLY
        // under the lock, re-checking m_shuttingDown + snapshotting m_pipeline. cleanup()
        // (Qt main thread) sets the flag and nulls m_pipeline + snapshots m_videoAppsink
        // under the SAME lock, so this either runs fully first (cleanup then frees what
        // we attached + drops our ref) or it sees teardown and bails — closing BOTH the
        // gst_bin_add_many(GST_BIN(null)) crash AND the orphaned-appsink-ref leak. Our
        // ref (mirrors m_farAppsrcExt) keeps the appsink alive across cleanup's
        // disconnect (the 0.52.16 hangup UAF). gst_bin_add_many sinks the floating ref,
        // so gst_object_ref is a genuine SECOND reference.
        bool attached = false;
        {
            std::lock_guard<std::mutex> lk(self->m_videoSinkMutex);
            if (!self->m_shuttingDown.load() && self->m_pipeline) {
                self->m_videoConvert = conv;
                gst_bin_add_many(GST_BIN(self->m_pipeline), conv, sink, nullptr);
                self->m_videoAppsink = static_cast<GstElement*>(gst_object_ref(sink));
                attached = true;
            }
        }
        if (!attached) {   // teardown won the race — never attached; free what we made
            gst_object_unref(conv);
            gst_object_unref(sink);
            return;
        }
        gst_element_link(conv, sink);
        gst_element_sync_state_with_parent(conv);
        gst_element_sync_state_with_parent(sink);
        GstPad *cs = gst_element_get_static_pad(conv, "sink");
        gst_pad_link(pad, cs);
        gst_object_unref(cs);
        qInfo() << "SubscribeWebrtcSrc: video pad linked (decoded)";
    } else if (isAudio) {
        // Plain read: set on the Qt thread before start() (published by the PLAYING
        // transition); a rebuild re-point only happens later, mid-call.
        // 0.52.4 — audio playout ALWAYS uses a per-subscriber wasapi2sink (built
        // below). The 0.51.x "single-clock AEC" design sent ALL remote playout
        // THROUGH the publisher's shared far-end mixer + one wasapi2sink, so any
        // hiccup in that mixer (latency wedge, NOT_NEGOTIATED, sink stall) silenced
        // the WHOLE call while the VU meter — tapped upstream — still moved. That
        // shipped twice as "no audio at all" (0.52.2; and 0.52.3 even after the
        // appsrc-latency fix cleared the wedge — the mixer reconciled yet stayed
        // silent). Audibility must NEVER be hostage to the AEC tail. So play out
        // directly per-subscriber and leave echo cancellation non-load-bearing:
        // the publisher's far-end tail still exists but is fed only its silent
        // keepalive, so AEC at the speaker is effectively off for now (use
        // headphones to avoid echo) until a SAFE reference TAP is re-added off this
        // per-sub chain. m_aecPlayoutSink / onAecPlayoutSample are now unused.
        GstElement *conv = gst_element_factory_make("audioconvert", nullptr);
        GstElement *res  = gst_element_factory_make("audioresample", nullptr);
        GstElement *sink = gst_element_factory_make("wasapi2sink", nullptr);
        if (!sink) sink = gst_element_factory_make("autoaudiosink", nullptr);
        if (!conv || !res || !sink) {
            if (conv) gst_object_unref(conv); if (res) gst_object_unref(res);
            if (sink) gst_object_unref(sink); return;
        }
        if (!self->m_audioOutputDeviceId.isEmpty())
            g_object_set(sink, "device",
                         self->m_audioOutputDeviceId.toUtf8().constData(), nullptr);

        // Playback chain (proven): conv → res → [level] → wasapi2sink.
        // The `level` element measures the decoded remote audio and posts peak
        // messages on the bus → this peer's VU meter in the call UI (the remote
        // mic indicator). It sits AFTER res / BEFORE the sink so it measures
        // exactly what is played out, and is purely a passthrough on the audio.
        // If the element can't be created we fall back to the original direct
        // chain (meter stays still, but playback is never affected).
        GstElement *lvl = gst_element_factory_make("level", nullptr);
        if (lvl)
            g_object_set(lvl, "post-messages", TRUE,
                         "interval", (guint64)100000000, nullptr);
        // 0.52.17 — attach into the pipeline under the lock, re-checking m_shuttingDown
        // + snapshotting m_pipeline so a concurrent hangup teardown (cleanup nulls
        // m_pipeline under the SAME lock) can't make gst_bin_add_many see a null bin
        // (crash). Same guard as the video pad above; the audio chain stores no ref'd
        // member, so on a lost race we just free the elements we created.
        bool attached = false;
        {
            std::lock_guard<std::mutex> lk(self->m_videoSinkMutex);
            if (!self->m_shuttingDown.load() && self->m_pipeline) {
                if (lvl) {
                    gst_bin_add_many(GST_BIN(self->m_pipeline), conv, res, lvl, sink, nullptr);
                    gst_element_link_many(conv, res, lvl, sink, nullptr);
                } else {
                    gst_bin_add_many(GST_BIN(self->m_pipeline), conv, res, sink, nullptr);
                    gst_element_link_many(conv, res, sink, nullptr);
                }
                attached = true;
            }
        }
        if (!attached) {   // teardown won the race — free the elements we made
            gst_object_unref(conv); gst_object_unref(res); gst_object_unref(sink);
            if (lvl) gst_object_unref(lvl);
            return;
        }
        if (lvl) gst_element_sync_state_with_parent(lvl);
        gst_element_sync_state_with_parent(conv);
        gst_element_sync_state_with_parent(res);
        gst_element_sync_state_with_parent(sink);

        // Link the decoded audio pad into the playback chain (conv → res →
        // [level] → wasapi2sink, built above). The AEC far-end reference is fed
        // separately by the single-pipeline path (setFarEndAppsrc →
        // onAecPlayoutSample) when echo cancellation is on; there is no
        // per-subscriber tap any more.
        GstPad *headSink = gst_element_get_static_pad(conv, "sink");
        gst_pad_link(pad, headSink);
        gst_object_unref(headSink);
        qInfo() << "SubscribeWebrtcSrc: audio pad linked (decoded)";
    }
}

GstFlowReturn SubscribeWebrtcSrc::onVideoNewSample(GstAppSink *sink, gpointer ud)
{
    auto *self = static_cast<SubscribeWebrtcSrc*>(ud);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    self->m_decodedAnyFrame.store(true, std::memory_order_relaxed);  // frame decoded -> keyframe watchdog can stop

    // --- RX video-rate probe (#111) — streaming thread only ---
    // Counts decoded frames actually delivered by webrtcsrc and the mean
    // gap between their buffer PTS. This is the decisive measurement for
    // "is the sender even producing 30 fps": fps≈1 & ptsΔ≈1000 ms ⇒ the
    // peer encodes ~1 fps (bytes can still look healthy — few huge frames);
    // fps≈1 & ptsΔ≈33 ms ⇒ frames are dropped on this receive leg.
    {
        const qint64 nowUs = (qint64)g_get_monotonic_time();
        if (self->m_rxWinStartUs == 0) self->m_rxWinStartUs = nowUs;
        if (GstBuffer *buf = gst_sample_get_buffer(sample)) {
            const quint64 pts = GST_BUFFER_PTS(buf);
            if (pts != GST_CLOCK_TIME_NONE) {
                if (self->m_rxLastPtsNs && pts > self->m_rxLastPtsNs)
                    self->m_rxDtSumNs += (pts - self->m_rxLastPtsNs);
                self->m_rxLastPtsNs = pts;
            }
            // Sparse FNV-1a fingerprint of the decoded frame. A decoder
            // concealing a lost reference repeats the previous picture
            // byte-for-byte, so an identical fingerprint ⇒ not a new
            // frame. ~256 strided samples: O(1), no full-buffer scan.
            GstMapInfo map;
            if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
                quint64 h = 1469598103934665603ull;
                const gsize stride = map.size > 256 ? map.size / 256 : 1;
                for (gsize i = 0; i < map.size; i += stride)
                    h = (h ^ map.data[i]) * 1099511628211ull;
                h ^= (quint64)map.size;
                if (h != self->m_rxLastHash) ++self->m_rxDistinct;
                self->m_rxLastHash = h;
                gst_buffer_unmap(buf, &map);
            }
        }
        ++self->m_rxFrames;
        const qint64 elapsedUs = nowUs - self->m_rxWinStartUs;
        if (elapsedUs >= 1000000) {
            const int n   = self->m_rxFrames;
            const int fps = (int)((qint64)n * 1000000 / elapsedUs);
            const int dst = (int)((qint64)self->m_rxDistinct * 1000000 / elapsedUs);
            const int gap = n > 1
                ? (int)(self->m_rxDtSumNs / (quint64)(n - 1) / 1000000ull)
                : 0;
            self->m_rxFps.store(fps, std::memory_order_relaxed);
            self->m_rxPtsGapMs.store(gap, std::memory_order_relaxed);
            self->m_rxDistinctFps.store(dst, std::memory_order_relaxed);
            // Decoded frame resolution (from the sample caps) — surfaces the
            // active simulcast substream the SFU is forwarding to us, for the
            // call-screen resolution bullet and for debugging selectStream.
            int rw = 0, rh = 0;
            if (GstCaps *caps = gst_sample_get_caps(sample)) {
                if (GstStructure *st = gst_caps_get_structure(caps, 0)) {
                    gst_structure_get_int(st, "width", &rw);
                    gst_structure_get_int(st, "height", &rh);
                }
            }
            if (rw > 0 && rh > 0) {
                self->m_rxWidth.store(rw, std::memory_order_relaxed);
                self->m_rxHeight.store(rh, std::memory_order_relaxed);
            }
            qInfo().nospace() << "SubscribeWebrtcSrc["
                              << self->m_remoteSessionId.left(8)
                              << "]: RX video " << fps << " fps (" << dst
                              << " distinct), " << rw << "x" << rh
                              << ", mean ptsΔ " << gap << " ms ("
                              << n << " frames / " << (elapsedUs / 1000)
                              << " ms)";
            self->m_rxWinStartUs = nowUs;
            self->m_rxFrames     = 0;
            self->m_rxDistinct   = 0;
            self->m_rxDtSumNs    = 0;
        }
    }

    QPointer<SubscribeWebrtcSrc> g(self);
    talq::leak::subPosted.fetch_add(1, std::memory_order_relaxed);
    QMetaObject::invokeMethod(self, [g, sample]() {
        if (g && g->m_videoProvider) g->m_videoProvider->feedFrame(sample);
        gst_sample_unref(sample);
        talq::leak::subDelivered.fetch_add(1, std::memory_order_relaxed);
    }, Qt::QueuedConnection);
    return GST_FLOW_OK;
}

void SubscribeWebrtcSrc::onVideoSample(GstSample *) {}
