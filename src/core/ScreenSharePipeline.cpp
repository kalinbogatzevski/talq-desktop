#include "core/ScreenSharePipeline.h"
#include <QDebug>
#include <QPointer>
#include <QRegularExpression>
#include <QUrl>

ScreenSharePipeline::ScreenSharePipeline(QObject *parent)
    : QObject(parent)
{
}

ScreenSharePipeline::~ScreenSharePipeline()
{
    stop();
}

bool ScreenSharePipeline::start(const QString &stunServer, const QList<TurnServer> &turnServers,
                                int monitorIndex, quintptr windowHandle)
{
    if (m_running) return false;

    m_pipeline = gst_pipeline_new(nullptr);
    m_webrtcbin = gst_element_factory_make("webrtcbin", nullptr);

    if (!m_pipeline || !m_webrtcbin) {
        emit error("Failed to create screen share pipeline elements");
        cleanup();
        return false;
    }

    if (!stunServer.isEmpty()) {
        QString gstStun = stunServer;
        if (gstStun.startsWith("stun:") && !gstStun.startsWith("stun://"))
            gstStun.replace("stun:", "stun://");
        g_object_set(m_webrtcbin, "stun-server", gstStun.toUtf8().constData(), nullptr);
    }
    g_object_set(m_webrtcbin, "bundle-policy",
                 GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, nullptr);

    for (const auto &turn : turnServers) {
        for (const auto &url : turn.urls) {
            QString gstUrl = url;
            gstUrl.remove(QRegularExpression("\\?transport=.*$"));
            if (gstUrl.startsWith("turn:") && !gstUrl.startsWith("turn://"))
                gstUrl.replace("turn:", "turn://");
            if (gstUrl.startsWith("turns:") && !gstUrl.startsWith("turns://"))
                gstUrl.replace("turns:", "turns://");
            QString escapedUser = QString(QUrl::toPercentEncoding(turn.username));
            QString escapedCred = QString(QUrl::toPercentEncoding(turn.credential));
            gstUrl.replace("://", QString("://%1:%2@").arg(escapedUser, escapedCred));
            gboolean ret = FALSE;
            g_signal_emit_by_name(m_webrtcbin, "add-turn-server", gstUrl.toUtf8().constData(), &ret);
        }
    }

    // Screen capture source
    GstElement *screenSrc = nullptr;

    if (windowHandle != 0) {
        // Window capture — try d3d11screencapturesrc with window-handle
        // (Windows Graphics Capture API works even on discrete GPU for windows)
        screenSrc = gst_element_factory_make("d3d11screencapturesrc", nullptr);
        if (screenSrc) {
            g_object_set(screenSrc, "window-handle", (guint64)windowHandle,
                         "show-cursor", TRUE, nullptr);
            qDebug() << "ScreenSharePipeline: window capture via d3d11screencapturesrc, hwnd=" << windowHandle;
        }
    }

    if (!screenSrc) {
        // Monitor capture — dx9screencapsrc is most reliable across GPU configs
        screenSrc = gst_element_factory_make("dx9screencapsrc", nullptr);
        if (screenSrc) {
            g_object_set(screenSrc, "monitor", monitorIndex, "cursor", TRUE, nullptr);
            qDebug() << "ScreenSharePipeline: monitor" << monitorIndex << "via dx9screencapsrc";
        }
    }
    if (!screenSrc) {
        screenSrc = gst_element_factory_make("gdiscreencapsrc", nullptr);
        if (screenSrc) {
            g_object_set(screenSrc, "monitor", monitorIndex, "cursor", TRUE, nullptr);
            qDebug() << "ScreenSharePipeline: monitor" << monitorIndex << "via gdiscreencapsrc";
        }
    }
    if (!screenSrc) {
        emit error("No screen capture plugin available");
        cleanup();
        return false;
    }

    // Simple chain: screenSrc → videoconvert → vp8enc → rtpvp8pay → capsfilter(ssrc)
    // No videorate/videoscale — let encoder handle what it gets
    GstElement *videoConvert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *vp8enc = gst_element_factory_make("vp8enc", nullptr);
    GstElement *rtpvp8pay = gst_element_factory_make("rtpvp8pay", nullptr);
    GstElement *ssrcFilter = gst_element_factory_make("capsfilter", nullptr);

    if (!videoConvert || !vp8enc || !rtpvp8pay || !ssrcFilter) {
        emit error("Failed to create screen share encoding elements");
        cleanup();
        return false;
    }

    qDebug() << "ScreenSharePipeline: elements created";

    // Encoder: realtime, low bitrate for screen content
    g_object_set(vp8enc, "deadline", (gint64)1, "threads", 4,
                 "target-bitrate", 1500000, nullptr);

    // SSRC capsfilter
    guint32 videoSsrc = g_random_int();
    g_object_set(rtpvp8pay, "ssrc", videoSsrc, nullptr);
    {
        GstCaps *ssrcCaps = gst_caps_from_string("application/x-rtp");
        gst_caps_set_simple(ssrcCaps, "ssrc", G_TYPE_UINT, videoSsrc, nullptr);
        g_object_set(ssrcFilter, "caps", ssrcCaps, nullptr);
        gst_caps_unref(ssrcCaps);
    }

    gst_bin_add_many(GST_BIN(m_pipeline), screenSrc, videoConvert,
                     vp8enc, rtpvp8pay, ssrcFilter, m_webrtcbin, nullptr);

    qDebug() << "ScreenSharePipeline: elements added to pipeline";

    if (!gst_element_link_many(screenSrc, videoConvert,
                               vp8enc, rtpvp8pay, ssrcFilter, nullptr)) {
        emit error("Failed to link screen share chain");
        cleanup();
        return false;
    }

    qDebug() << "ScreenSharePipeline: chain linked";

    // Link to webrtcbin
    GstPad *ssrcSrcPad = gst_element_get_static_pad(ssrcFilter, "src");
    GstPad *sinkPad = gst_element_request_pad_simple(m_webrtcbin, "sink_%u");
    gst_pad_link(ssrcSrcPad, sinkPad);

    // Set transceiver to sendonly VP8
    GstWebRTCRTPTransceiver *vt = nullptr;
    g_object_get(sinkPad, "transceiver", &vt, nullptr);
    if (vt) {
        GstCaps *vc = gst_caps_from_string(
            "application/x-rtp,media=video,encoding-name=VP8,clock-rate=90000,payload=96");
        g_object_set(vt, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY,
                     "codec-preferences", vc, nullptr);
        gst_caps_unref(vc);
        gst_object_unref(vt);
    }
    gst_object_unref(ssrcSrcPad);
    gst_object_unref(sinkPad);

    // Data channel — Janus requires at least one for publisher registration
    {
        GstWebRTCDataChannel *dc = nullptr;
        g_signal_emit_by_name(m_webrtcbin, "create-data-channel", "status", nullptr, &dc);
        if (dc) {
            qDebug() << "ScreenSharePipeline: created data channel 'status'";
            g_object_unref(dc);
        }
    }

    // Signals
    g_signal_connect(m_webrtcbin, "on-negotiation-needed",
                     G_CALLBACK(onNegotiationNeeded), this);
    g_signal_connect(m_webrtcbin, "on-ice-candidate",
                     G_CALLBACK(onIceCandidate), this);
    g_signal_connect(m_webrtcbin, "notify::ice-connection-state",
                     G_CALLBACK(onIceStateChanged), this);
    g_signal_connect(m_webrtcbin, "notify::ice-gathering-state",
                     G_CALLBACK(onIceGatheringStateChanged), this);

    qDebug() << "ScreenSharePipeline: setting pipeline to PLAYING...";
    GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    qDebug() << "ScreenSharePipeline: set_state returned" << ret;
    if (ret == GST_STATE_CHANGE_FAILURE) {
        emit error("Failed to start screen share pipeline");
        cleanup();
        return false;
    }

    m_running = true;
    qDebug() << "ScreenSharePipeline: started, capturing primary monitor";
    return true;
}

void ScreenSharePipeline::stop()
{
    if (!m_running) return;
    cleanup();
    m_running = false;
    qDebug() << "ScreenSharePipeline: stopped";
}

void ScreenSharePipeline::cleanup()
{
    if (m_webrtcbin)
        g_signal_handlers_disconnect_by_data(m_webrtcbin, this);
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
    m_webrtcbin = nullptr;
    m_remoteDescSet = false;
    m_pendingCandidates.clear();
}

void ScreenSharePipeline::setRemoteAnswer(const QString &sdp)
{
    if (!m_webrtcbin) return;

    QByteArray sdpUtf8 = sdp.toUtf8();
    GstSDPMessage *sdpMsg;
    gst_sdp_message_new(&sdpMsg);
    gst_sdp_message_parse_buffer((const guint8 *)sdpUtf8.constData(), sdpUtf8.size(), sdpMsg);

    GstWebRTCSessionDescription *desc = gst_webrtc_session_description_new(
        GST_WEBRTC_SDP_TYPE_ANSWER, sdpMsg);
    g_signal_emit_by_name(m_webrtcbin, "set-remote-description", desc, nullptr);
    gst_webrtc_session_description_free(desc);

    m_remoteDescSet = true;
    for (const auto &c : m_pendingCandidates)
        g_signal_emit_by_name(m_webrtcbin, "add-ice-candidate", c.first, c.second.toUtf8().constData());
    m_pendingCandidates.clear();
    qDebug() << "ScreenSharePipeline: set remote answer";
}

void ScreenSharePipeline::addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid)
{
    Q_UNUSED(sdpMid)
    if (!m_webrtcbin) return;
    if (!m_remoteDescSet) {
        m_pendingCandidates.append({sdpMLineIndex, candidate});
        return;
    }
    g_signal_emit_by_name(m_webrtcbin, "add-ice-candidate",
                          sdpMLineIndex, candidate.toUtf8().constData());
}

void ScreenSharePipeline::pollBus()
{
    if (!m_pipeline) return;
    GstBus *bus = gst_element_get_bus(m_pipeline);
    GstMessage *msg;
    while ((msg = gst_bus_pop(bus)) != nullptr) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = nullptr; gchar *dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            qWarning() << "ScreenSharePipeline ERROR:" << err->message;
            g_clear_error(&err); g_free(dbg);
        }
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

// --- Static callbacks ---

void ScreenSharePipeline::onNegotiationNeeded(GstElement *webrtc, gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    GstPromise *promise = gst_promise_new_with_change_func(onOfferCreated, self, nullptr);
    g_signal_emit_by_name(webrtc, "create-offer", nullptr, promise);
}

void ScreenSharePipeline::onOfferCreated(GstPromise *promise, gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);

    if (!offer || !self->m_webrtcbin) {
        if (offer) gst_webrtc_session_description_free(offer);
        gst_promise_unref(promise);
        return;
    }

    g_signal_emit_by_name(self->m_webrtcbin, "set-local-description", offer, nullptr);

    gchar *sdpText = gst_sdp_message_as_text(offer->sdp);
    QString sdp = QString::fromUtf8(sdpText);
    g_free(sdpText);
    gst_webrtc_session_description_free(offer);
    gst_promise_unref(promise);

    QPointer<ScreenSharePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, sdp]() {
        if (!guard) return;
        emit guard->localOfferReady(sdp);
    }, Qt::QueuedConnection);
}

void ScreenSharePipeline::onIceCandidate(GstElement *, guint mlineIndex, gchar *candidate, gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    QString c = QString::fromUtf8(candidate);
    int ml = static_cast<int>(mlineIndex);
    QPointer<ScreenSharePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, c, ml]() {
        if (!guard) return;
        emit guard->iceCandidateReady(c, ml, QString("0"));
    }, Qt::QueuedConnection);
}

void ScreenSharePipeline::onIceStateChanged(GObject *obj, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    GstWebRTCICEConnectionState state;
    g_object_get(obj, "ice-connection-state", &state, nullptr);
    const char *names[] = {"new", "checking", "connected", "completed", "failed", "disconnected", "closed"};
    int idx = static_cast<int>(state);
    QString stateName = (idx < 7) ? names[idx] : "unknown";
    QPointer<ScreenSharePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, stateName]() {
        if (!guard) return;
        emit guard->iceStateChanged(stateName);
    }, Qt::QueuedConnection);
}

void ScreenSharePipeline::onIceGatheringStateChanged(GObject *obj, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    GstWebRTCICEGatheringState state;
    g_object_get(obj, "ice-gathering-state", &state, nullptr);
    if (state != GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) return;
    QPointer<ScreenSharePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard]() {
        if (!guard) return;
        qDebug() << "ScreenSharePipeline: ICE gathering complete";
        emit guard->iceGatheringComplete();
    }, Qt::QueuedConnection);
}
