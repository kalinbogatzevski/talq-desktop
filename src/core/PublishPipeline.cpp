#include "core/PublishPipeline.h"
#include <QDebug>

PublishPipeline::PublishPipeline(QObject *parent)
    : QObject(parent)
{
}

PublishPipeline::~PublishPipeline()
{
    stop();
}

bool PublishPipeline::start(const QString &stunServer)
{
    if (m_running) return false;

    m_pipeline = gst_pipeline_new("publish-pipeline");
    m_webrtcbin = gst_element_factory_make("webrtcbin", "pub-webrtcbin");

    if (!m_pipeline || !m_webrtcbin) {
        emit error("Failed to create publish pipeline elements");
        cleanup();
        return false;
    }

    // STUN
    if (!stunServer.isEmpty())
        g_object_set(m_webrtcbin, "stun-server", stunServer.toUtf8().constData(), nullptr);

    // Bundle policy (match browser default)
    g_object_set(m_webrtcbin, "bundle-policy", 3 /* max-bundle */, nullptr);

    // Audio capture chain: wasapi2src → audioconvert → audioresample → opusenc → rtpopuspay
    GstElement *audiosrc = gst_element_factory_make("wasapi2src", "pub-audiosrc");
    GstElement *audioconvert = gst_element_factory_make("audioconvert", nullptr);
    GstElement *audioresample = gst_element_factory_make("audioresample", nullptr);
    GstElement *opusenc = gst_element_factory_make("opusenc", nullptr);
    GstElement *rtpopuspay = gst_element_factory_make("rtpopuspay", nullptr);

    if (!audiosrc || !audioconvert || !audioresample || !opusenc || !rtpopuspay) {
        emit error("Failed to create audio capture elements");
        cleanup();
        return false;
    }

    gst_bin_add_many(GST_BIN(m_pipeline), audiosrc, audioconvert, audioresample,
                     opusenc, rtpopuspay, m_webrtcbin, nullptr);

    if (!gst_element_link_many(audiosrc, audioconvert, audioresample,
                               opusenc, rtpopuspay, nullptr)) {
        emit error("Failed to link audio capture chain");
        cleanup();
        return false;
    }

    // Link RTP payloader to webrtcbin
    GstPad *rtpSrcPad = gst_element_get_static_pad(rtpopuspay, "src");
    GstPad *sinkPad = gst_element_request_pad_simple(m_webrtcbin, "sink_%u");
    if (gst_pad_link(rtpSrcPad, sinkPad) != GST_PAD_LINK_OK) {
        emit error("Failed to link RTP to webrtcbin");
        gst_object_unref(rtpSrcPad);
        gst_object_unref(sinkPad);
        cleanup();
        return false;
    }
    gst_object_unref(rtpSrcPad);
    gst_object_unref(sinkPad);

    // Signals — no pad-added (send-only)
    g_signal_connect(m_webrtcbin, "on-negotiation-needed",
                     G_CALLBACK(onNegotiationNeeded), this);
    g_signal_connect(m_webrtcbin, "on-ice-candidate",
                     G_CALLBACK(onIceCandidate), this);
    g_signal_connect(m_webrtcbin, "notify::ice-connection-state",
                     G_CALLBACK(onIceStateChanged), this);

    // Bus watch for errors
    GstBus *bus = gst_element_get_bus(m_pipeline);
    gst_bus_add_watch(bus, [](GstBus *, GstMessage *msg, gpointer) -> gboolean {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = nullptr; gchar *dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            qWarning() << "PublishPipeline ERROR:" << err->message << (dbg ? dbg : "");
            g_clear_error(&err); g_free(dbg);
        }
        return TRUE;
    }, nullptr);
    gst_object_unref(bus);

    GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        emit error("Failed to start publish pipeline");
        cleanup();
        return false;
    }

    m_running = true;
    qDebug() << "PublishPipeline: started (send-only)";
    return true;
}

void PublishPipeline::stop()
{
    if (!m_running) return;
    cleanup();
    m_running = false;
    qDebug() << "PublishPipeline: stopped";
}

void PublishPipeline::cleanup()
{
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
    m_webrtcbin = nullptr;
}

void PublishPipeline::setRemoteAnswer(const QString &sdp)
{
    if (!m_webrtcbin) return;

    GstSDPMessage *sdpMsg;
    gst_sdp_message_new(&sdpMsg);
    gst_sdp_message_parse_buffer((const guint8 *)sdp.toUtf8().constData(),
                                  sdp.toUtf8().size(), sdpMsg);

    GstWebRTCSessionDescription *desc = gst_webrtc_session_description_new(
        GST_WEBRTC_SDP_TYPE_ANSWER, sdpMsg);

    g_signal_emit_by_name(m_webrtcbin, "set-remote-description", desc, nullptr);
    gst_webrtc_session_description_free(desc);

    qDebug() << "PublishPipeline: set remote answer";
}

void PublishPipeline::addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid)
{
    if (!m_webrtcbin) return;
    Q_UNUSED(sdpMid)
    g_signal_emit_by_name(m_webrtcbin, "add-ice-candidate",
                          sdpMLineIndex, candidate.toUtf8().constData());
}

void PublishPipeline::setMuted(bool muted)
{
    if (!m_pipeline) return;
    GstElement *src = gst_bin_get_by_name(GST_BIN(m_pipeline), "pub-audiosrc");
    if (src) {
        g_object_set(src, "mute", muted, nullptr);
        gst_object_unref(src);
    }
}

// --- GStreamer callbacks ---

void PublishPipeline::onNegotiationNeeded(GstElement *, gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);
    qDebug() << "PublishPipeline: negotiation needed, creating offer";
    GstPromise *promise = gst_promise_new_with_change_func(onOfferCreated, self, nullptr);
    g_signal_emit_by_name(self->m_webrtcbin, "create-offer", nullptr, promise);
}

void PublishPipeline::onIceCandidate(GstElement *, guint mlineIndex, gchar *candidate, gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);
    emit self->iceCandidateReady(
        QString::fromUtf8(candidate),
        static_cast<int>(mlineIndex),
        QString("0"));
}

void PublishPipeline::onOfferCreated(GstPromise *promise, gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);

    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);

    if (!offer) {
        qWarning() << "PublishPipeline: failed to create offer";
        gst_promise_unref(promise);
        return;
    }

    g_signal_emit_by_name(self->m_webrtcbin, "set-local-description", offer, nullptr);

    gchar *sdpText = gst_sdp_message_as_text(offer->sdp);
    QString sdp = QString::fromUtf8(sdpText);
    g_free(sdpText);

    gst_webrtc_session_description_free(offer);
    gst_promise_unref(promise);

    qDebug() << "PublishPipeline: offer created, SDP length=" << sdp.length();
    emit self->localOfferReady(sdp);
}

void PublishPipeline::onIceStateChanged(GObject *obj, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);
    GstWebRTCICEConnectionState state;
    g_object_get(obj, "ice-connection-state", &state, nullptr);
    const char *names[] = {"new", "checking", "connected", "completed", "failed", "disconnected", "closed"};
    int idx = static_cast<int>(state);
    QString stateName = (idx < 7) ? names[idx] : "unknown";
    qDebug() << "PublishPipeline: ICE ->" << stateName;
    emit self->iceStateChanged(stateName);
}
