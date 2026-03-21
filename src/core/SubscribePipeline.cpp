#include "core/SubscribePipeline.h"
#include <QDebug>

SubscribePipeline::SubscribePipeline(const QString &remoteSessionId, QObject *parent)
    : QObject(parent)
    , m_remoteSessionId(remoteSessionId)
{
}

SubscribePipeline::~SubscribePipeline()
{
    stop();
}

bool SubscribePipeline::start(const QString &stunServer)
{
    if (m_running) return false;

    m_pipeline = gst_pipeline_new("subscribe-pipeline");
    m_webrtcbin = gst_element_factory_make("webrtcbin", "sub-webrtcbin");

    if (!m_pipeline || !m_webrtcbin) {
        emit error("Failed to create subscribe pipeline elements");
        cleanup();
        return false;
    }

    if (!stunServer.isEmpty())
        g_object_set(m_webrtcbin, "stun-server", stunServer.toUtf8().constData(), nullptr);
    g_object_set(m_webrtcbin, "bundle-policy",
                 GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, nullptr);

    gst_bin_add(GST_BIN(m_pipeline), m_webrtcbin);

    // No negotiation-needed (we don't create offers)
    g_signal_connect(m_webrtcbin, "on-ice-candidate",
                     G_CALLBACK(onIceCandidate), this);
    g_signal_connect(m_webrtcbin, "pad-added",
                     G_CALLBACK(onPadAdded), this);
    g_signal_connect(m_webrtcbin, "notify::ice-connection-state",
                     G_CALLBACK(onIceStateChanged), this);

    // Bus watch — track ID for cleanup
    GstBus *bus = gst_element_get_bus(m_pipeline);
    m_busWatchId = gst_bus_add_watch(bus, [](GstBus *, GstMessage *msg, gpointer) -> gboolean {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = nullptr; gchar *dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            qWarning() << "SubscribePipeline ERROR:" << err->message << (dbg ? dbg : "");
            g_clear_error(&err); g_free(dbg);
        }
        return TRUE;
    }, nullptr);
    gst_object_unref(bus);

    GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        emit error("Failed to start subscribe pipeline");
        cleanup();
        return false;
    }

    m_running = true;
    qDebug() << "SubscribePipeline: started (receive-only) for" << m_remoteSessionId.left(20);
    return true;
}

void SubscribePipeline::stop()
{
    if (!m_running) return;
    cleanup();
    m_running = false;
    qDebug() << "SubscribePipeline: stopped";
}

void SubscribePipeline::cleanup()
{
    if (m_busWatchId > 0) {
        g_source_remove(m_busWatchId);
        m_busWatchId = 0;
    }
    if (m_webrtcbin)
        g_signal_handlers_disconnect_by_data(m_webrtcbin, this);
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
    m_webrtcbin = nullptr;
}

void SubscribePipeline::setRemoteOffer(const QString &sdp)
{
    if (!m_webrtcbin) return;

    QByteArray sdpUtf8 = sdp.toUtf8();
    GstSDPMessage *sdpMsg;
    gst_sdp_message_new(&sdpMsg);
    gst_sdp_message_parse_buffer((const guint8 *)sdpUtf8.constData(),
                                  sdpUtf8.size(), sdpMsg);

    GstWebRTCSessionDescription *desc = gst_webrtc_session_description_new(
        GST_WEBRTC_SDP_TYPE_OFFER, sdpMsg);

    g_signal_emit_by_name(m_webrtcbin, "set-remote-description", desc, nullptr);
    gst_webrtc_session_description_free(desc);

    qDebug() << "SubscribePipeline: set remote offer, creating answer...";

    GstPromise *answerPromise = gst_promise_new_with_change_func(
        onAnswerCreated, this, nullptr);
    g_signal_emit_by_name(m_webrtcbin, "create-answer", nullptr, answerPromise);
}

void SubscribePipeline::addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid)
{
    if (!m_webrtcbin) return;
    Q_UNUSED(sdpMid)
    g_signal_emit_by_name(m_webrtcbin, "add-ice-candidate",
                          sdpMLineIndex, candidate.toUtf8().constData());
}

// --- GStreamer callbacks (marshalled to Qt thread) ---

void SubscribePipeline::onIceCandidate(GstElement *, guint mlineIndex, gchar *candidate, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);
    QString c = QString::fromUtf8(candidate);
    int ml = static_cast<int>(mlineIndex);
    QMetaObject::invokeMethod(self, [self, c, ml]() {
        emit self->iceCandidateReady(c, ml, QString("0"));
    }, Qt::QueuedConnection);
}

void SubscribePipeline::onPadAdded(GstElement *, GstPad *pad, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);

    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC)
        return;

    // Must build receive chain on the streaming thread (GStreamer requirement for pad linking)
    // but the pipeline pointer is safe because we disconnect signals before cleanup
    qDebug() << "SubscribePipeline: new pad from webrtcbin:" << GST_PAD_NAME(pad);

    GstElement *depay = gst_element_factory_make("rtpopusdepay", nullptr);
    GstElement *dec = gst_element_factory_make("opusdec", nullptr);
    GstElement *convert = gst_element_factory_make("audioconvert", nullptr);
    GstElement *resample = gst_element_factory_make("audioresample", nullptr);
    GstElement *sink = gst_element_factory_make("wasapi2sink", nullptr);

    if (!depay || !dec || !convert || !resample || !sink) {
        qWarning() << "SubscribePipeline: failed to create receive chain";
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

    qDebug() << "SubscribePipeline: audio receive chain linked";
}

void SubscribePipeline::onAnswerCreated(GstPromise *promise, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);

    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *answer = nullptr;
    gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, nullptr);

    if (!answer) {
        qWarning() << "SubscribePipeline: failed to create answer";
        gst_promise_unref(promise);
        return;
    }

    g_signal_emit_by_name(self->m_webrtcbin, "set-local-description", answer, nullptr);

    gchar *sdpText = gst_sdp_message_as_text(answer->sdp);
    QString sdp = QString::fromUtf8(sdpText);
    g_free(sdpText);
    gst_webrtc_session_description_free(answer);
    gst_promise_unref(promise);

    qDebug() << "SubscribePipeline: answer created, SDP length=" << sdp.length();
    QMetaObject::invokeMethod(self, [self, sdp]() {
        emit self->localAnswerReady(sdp);
    }, Qt::QueuedConnection);
}

void SubscribePipeline::onIceStateChanged(GObject *obj, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);
    GstWebRTCICEConnectionState state;
    g_object_get(obj, "ice-connection-state", &state, nullptr);
    const char *names[] = {"new", "checking", "connected", "completed", "failed", "disconnected", "closed"};
    int idx = static_cast<int>(state);
    QString stateName = (idx < 7) ? names[idx] : "unknown";
    QMetaObject::invokeMethod(self, [self, stateName]() {
        qDebug() << "SubscribePipeline: ICE ->" << stateName;
        emit self->iceStateChanged(stateName);
    }, Qt::QueuedConnection);
}
