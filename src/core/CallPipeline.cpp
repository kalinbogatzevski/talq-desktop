#include "core/CallPipeline.h"
#include <QDebug>

CallPipeline::CallPipeline(QObject *parent)
    : QObject(parent)
{
}

CallPipeline::~CallPipeline()
{
    stop();
}

bool CallPipeline::startCall(const QString &stunServer, const QString &turnServer)
{
    if (m_running) {
        qWarning() << "CallPipeline: already running";
        return false;
    }

    // Build pipeline: audio capture → webrtcbin (audio playback handled by pad-added)
    m_pipeline = gst_pipeline_new("call-pipeline");
    m_webrtcbin = gst_element_factory_make("webrtcbin", "webrtcbin");

    if (!m_pipeline || !m_webrtcbin) {
        emit error("Failed to create WebRTC pipeline elements");
        cleanup();
        return false;
    }

    // Configure STUN/TURN
    if (!stunServer.isEmpty())
        g_object_set(m_webrtcbin, "stun-server", stunServer.toUtf8().constData(), nullptr);
    if (!turnServer.isEmpty())
        g_object_set(m_webrtcbin, "turn-server", turnServer.toUtf8().constData(), nullptr);

    // Audio capture: wasapi2src → audioconvert → audioresample → opusenc → rtpopuspay
    GstElement *audiosrc = gst_element_factory_make("wasapi2src", "audiosrc");
    GstElement *audioconvert = gst_element_factory_make("audioconvert", "audioconvert");
    GstElement *audioresample = gst_element_factory_make("audioresample", "audioresample");
    GstElement *opusenc = gst_element_factory_make("opusenc", "opusenc");
    GstElement *rtpopuspay = gst_element_factory_make("rtpopuspay", "rtpopuspay");

    if (!audiosrc || !audioconvert || !audioresample || !opusenc || !rtpopuspay) {
        emit error("Failed to create audio pipeline elements");
        cleanup();
        return false;
    }

    // Add all elements to pipeline
    gst_bin_add_many(GST_BIN(m_pipeline), audiosrc, audioconvert, audioresample,
                     opusenc, rtpopuspay, m_webrtcbin, nullptr);

    // Link audio chain → webrtcbin
    if (!gst_element_link_many(audiosrc, audioconvert, audioresample,
                               opusenc, rtpopuspay, nullptr)) {
        emit error("Failed to link audio capture chain");
        cleanup();
        return false;
    }

    // Link RTP payloader to webrtcbin (request pad)
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

    // Connect webrtcbin signals
    g_signal_connect(m_webrtcbin, "on-negotiation-needed",
                     G_CALLBACK(onNegotiationNeeded), this);
    g_signal_connect(m_webrtcbin, "on-ice-candidate",
                     G_CALLBACK(onIceCandidate), this);
    g_signal_connect(m_webrtcbin, "pad-added",
                     G_CALLBACK(onPadAdded), this);

    // Start pipeline
    GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        emit error("Failed to start call pipeline");
        cleanup();
        return false;
    }

    m_running = true;
    qDebug() << "CallPipeline: WebRTC call started";
    return true;
}

void CallPipeline::stop()
{
    if (!m_running) return;
    cleanup();
    m_running = false;
    qDebug() << "CallPipeline: stopped";
}

void CallPipeline::cleanup()
{
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
    m_webrtcbin = nullptr;  // owned by pipeline, already freed
}

void CallPipeline::setRemoteDescription(const QString &type, const QString &sdp)
{
    if (!m_webrtcbin) return;

    GstSDPMessage *sdpMsg;
    gst_sdp_message_new(&sdpMsg);
    gst_sdp_message_parse_buffer((const guint8 *)sdp.toUtf8().constData(),
                                  sdp.toUtf8().size(), sdpMsg);

    GstWebRTCSessionDescription *desc = gst_webrtc_session_description_new(
        type == "offer" ? GST_WEBRTC_SDP_TYPE_OFFER : GST_WEBRTC_SDP_TYPE_ANSWER,
        sdpMsg);

    GstPromise *promise = gst_promise_new();
    g_signal_emit_by_name(m_webrtcbin, "set-remote-description", desc, promise);
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);
    gst_webrtc_session_description_free(desc);

    qDebug() << "CallPipeline: set remote description type=" << type;

    // If we received an offer, create an answer
    if (type == "offer") {
        GstPromise *answerPromise = gst_promise_new_with_change_func(
            onAnswerCreated, this, nullptr);
        g_signal_emit_by_name(m_webrtcbin, "create-answer", nullptr, answerPromise);
    }
}

void CallPipeline::addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid)
{
    if (!m_webrtcbin) return;
    g_signal_emit_by_name(m_webrtcbin, "add-ice-candidate",
                          sdpMLineIndex, candidate.toUtf8().constData());
}

void CallPipeline::setMuted(bool muted)
{
    if (!m_pipeline) return;
    GstElement *src = gst_bin_get_by_name(GST_BIN(m_pipeline), "audiosrc");
    if (src) {
        g_object_set(src, "mute", muted, nullptr);
        gst_object_unref(src);
    }
}

// --- Static GStreamer callbacks ---

void CallPipeline::onNegotiationNeeded(GstElement *webrtc, gpointer userData)
{
    Q_UNUSED(webrtc)
    auto *self = static_cast<CallPipeline *>(userData);
    qDebug() << "CallPipeline: negotiation needed, creating offer";

    GstPromise *promise = gst_promise_new_with_change_func(
        onOfferCreated, self, nullptr);
    g_signal_emit_by_name(self->m_webrtcbin, "create-offer", nullptr, promise);
}

void CallPipeline::onIceCandidate(GstElement *webrtc, guint mlineIndex, gchar *candidate, gpointer userData)
{
    Q_UNUSED(webrtc)
    auto *self = static_cast<CallPipeline *>(userData);
    emit self->iceCandidateReady(
        QString::fromUtf8(candidate),
        static_cast<int>(mlineIndex),
        QString());
}

void CallPipeline::onPadAdded(GstElement *webrtc, GstPad *pad, gpointer userData)
{
    Q_UNUSED(webrtc)
    auto *self = static_cast<CallPipeline *>(userData);

    // Only handle src pads (incoming media from remote peer)
    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC)
        return;

    qDebug() << "CallPipeline: new pad from webrtcbin:" << GST_PAD_NAME(pad);

    // Build receive chain: depayload → decode → output
    GstElement *depay = gst_element_factory_make("rtpopusdepay", nullptr);
    GstElement *dec = gst_element_factory_make("opusdec", nullptr);
    GstElement *convert = gst_element_factory_make("audioconvert", nullptr);
    GstElement *resample = gst_element_factory_make("audioresample", nullptr);
    GstElement *sink = gst_element_factory_make("wasapi2sink", nullptr);

    if (!depay || !dec || !convert || !resample || !sink) {
        qWarning() << "CallPipeline: failed to create receive chain";
        return;
    }

    gst_bin_add_many(GST_BIN(self->m_pipeline), depay, dec, convert, resample, sink, nullptr);
    gst_element_link_many(depay, dec, convert, resample, sink, nullptr);
    gst_element_sync_state_with_parent(depay);
    gst_element_sync_state_with_parent(dec);
    gst_element_sync_state_with_parent(convert);
    gst_element_sync_state_with_parent(resample);
    gst_element_sync_state_with_parent(sink);

    GstPad *sinkPad = gst_element_get_static_pad(depay, "sink");
    gst_pad_link(pad, sinkPad);
    gst_object_unref(sinkPad);
}

void CallPipeline::onOfferCreated(GstPromise *promise, gpointer userData)
{
    auto *self = static_cast<CallPipeline *>(userData);

    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);

    if (!offer) {
        qWarning() << "CallPipeline: failed to create offer";
        gst_promise_unref(promise);
        return;
    }

    // Set as local description
    GstPromise *localPromise = gst_promise_new();
    g_signal_emit_by_name(self->m_webrtcbin, "set-local-description", offer, localPromise);
    gst_promise_interrupt(localPromise);
    gst_promise_unref(localPromise);

    // Emit SDP to send via signaling
    gchar *sdpText = gst_sdp_message_as_text(offer->sdp);
    emit self->localSdpReady("offer", QString::fromUtf8(sdpText));
    g_free(sdpText);

    gst_webrtc_session_description_free(offer);
    gst_promise_unref(promise);
    qDebug() << "CallPipeline: offer created and set as local description";
}

void CallPipeline::onAnswerCreated(GstPromise *promise, gpointer userData)
{
    auto *self = static_cast<CallPipeline *>(userData);

    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *answer = nullptr;
    gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, nullptr);

    if (!answer) {
        qWarning() << "CallPipeline: failed to create answer";
        gst_promise_unref(promise);
        return;
    }

    // Set as local description
    GstPromise *localPromise = gst_promise_new();
    g_signal_emit_by_name(self->m_webrtcbin, "set-local-description", answer, localPromise);
    gst_promise_interrupt(localPromise);
    gst_promise_unref(localPromise);

    // Emit SDP to send via signaling
    gchar *sdpText = gst_sdp_message_as_text(answer->sdp);
    emit self->localSdpReady("answer", QString::fromUtf8(sdpText));
    g_free(sdpText);

    gst_webrtc_session_description_free(answer);
    gst_promise_unref(promise);
    qDebug() << "CallPipeline: answer created and set as local description";
}
