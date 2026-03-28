#include "core/SubscribePipeline.h"
#include <QDebug>
#include <QPointer>
#include <QRegularExpression>
#include <QUrl>

SubscribePipeline::SubscribePipeline(const QString &remoteSessionId, QObject *parent)
    : QObject(parent)
    , m_remoteSessionId(remoteSessionId)
{
    m_videoProvider = new VideoFrameProvider(this);
}

SubscribePipeline::~SubscribePipeline()
{
    stop();
}

bool SubscribePipeline::start(const QString &stunServer, const QList<TurnServer> &turnServers,
                              const QString &audioOutputDeviceId)
{
    if (m_running) return false;
    m_audioOutputDeviceId = audioOutputDeviceId;

    m_pipeline = gst_pipeline_new("subscribe-pipeline");
    m_webrtcbin = gst_element_factory_make("webrtcbin", "sub-webrtcbin");

    if (!m_pipeline || !m_webrtcbin) {
        emit error("Failed to create subscribe pipeline elements");
        cleanup();
        return false;
    }

    if (!stunServer.isEmpty()) {
        // Nextcloud returns "stun:host:port" but GStreamer needs "stun://host:port"
        QString gstStun = stunServer;
        if (gstStun.startsWith("stun:") && !gstStun.startsWith("stun://"))
            gstStun = "stun://" + gstStun.mid(5);
        qDebug() << "SubscribePipeline: STUN server:" << gstStun;
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
            qDebug() << "SubscribePipeline: adding TURN server" << gstUrl;
            gboolean ret = FALSE;
            g_signal_emit_by_name(m_webrtcbin, "add-turn-server", gstUrl.toUtf8().constData(), &ret);
        }
    }

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

    // Log SDP content for debugging
    qDebug() << "SubscribePipeline: remote offer SDP:\n" << sdp.left(2000);
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

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    if (!caps) return;

    GstStructure *s = gst_caps_get_structure(caps, 0);
    const gchar *media = gst_structure_get_string(s, "media");
    const gchar *encoding = gst_structure_get_string(s, "encoding-name");

    qDebug() << "SubscribePipeline: pad added, media=" << media << "encoding=" << encoding;

    bool isAudio = (media && g_strcmp0(media, "audio") == 0)
                || (encoding && g_ascii_strcasecmp(encoding, "OPUS") == 0);
    bool isVideo = (media && g_strcmp0(media, "video") == 0)
                || (encoding && (g_ascii_strcasecmp(encoding, "VP8") == 0
                              || g_ascii_strcasecmp(encoding, "H264") == 0));

    // Copy encoding string before unreffing caps (encoding points into caps memory)
    QByteArray encodingCopy = encoding ? QByteArray(encoding) : QByteArray();
    gst_caps_unref(caps);

    if (isAudio) {
        self->createAudioChain(pad);
    } else if (isVideo) {
        self->createVideoChain(pad, encodingCopy.constData());
    } else {
        qDebug() << "SubscribePipeline: skipping unknown pad type";
    }
}

void SubscribePipeline::createAudioChain(GstPad *pad)
{
    GstElement *depay = gst_element_factory_make("rtpopusdepay", nullptr);
    GstElement *dec = gst_element_factory_make("opusdec", nullptr);
    GstElement *convert = gst_element_factory_make("audioconvert", nullptr);
    GstElement *resample = gst_element_factory_make("audioresample", nullptr);

    // Try WASAPI2 sink first (best Windows audio), then wasapisink, then autoaudiosink
    GstElement *sink = nullptr;
    sink = gst_element_factory_make("wasapi2sink", nullptr);
    if (sink) {
        qDebug() << "SubscribePipeline: audio sink: wasapi2sink";
        if (!m_audioOutputDeviceId.isEmpty())
            g_object_set(sink, "device", m_audioOutputDeviceId.toUtf8().constData(), nullptr);
    }
    if (!sink) {
        sink = gst_element_factory_make("wasapisink", nullptr);
        if (sink) {
            qDebug() << "SubscribePipeline: audio sink: wasapisink";
            g_object_set(sink, "low-latency", FALSE, nullptr);
            if (!m_audioOutputDeviceId.isEmpty())
                g_object_set(sink, "device", m_audioOutputDeviceId.toUtf8().constData(), nullptr);
        }
    }
    if (!sink) {
        sink = gst_element_factory_make("directsoundsink", nullptr);
        if (sink) {
            qDebug() << "SubscribePipeline: audio sink: directsoundsink";
            if (!m_audioOutputDeviceId.isEmpty())
                g_object_set(sink, "device", m_audioOutputDeviceId.toUtf8().constData(), nullptr);
        }
    }
    if (!sink) {
        sink = gst_element_factory_make("autoaudiosink", nullptr);
        if (sink) qDebug() << "SubscribePipeline: audio sink: autoaudiosink";
    }

    if (!depay || !dec || !convert || !resample || !sink) {
        qWarning() << "SubscribePipeline: failed to create audio receive chain";
        return;
    }

    gst_bin_add_many(GST_BIN(m_pipeline), depay, dec, convert, resample, sink, nullptr);
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

void SubscribePipeline::createVideoChain(GstPad *pad, const gchar *encoding)
{
    qDebug() << "SubscribePipeline: creating video chain for" << encoding;

    GstElement *depay = nullptr;
    GstElement *decoder = nullptr;

    if (encoding && g_ascii_strcasecmp(encoding, "VP8") == 0) {
        depay = gst_element_factory_make("rtpvp8depay", nullptr);
        decoder = gst_element_factory_make("vp8dec", nullptr);
    } else {
        depay = gst_element_factory_make("rtph264depay", nullptr);
        decoder = gst_element_factory_make("openh264dec", nullptr);
    }

    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *appsink = gst_element_factory_make("appsink", nullptr);

    if (!depay || !decoder || !convert || !appsink) {
        qWarning() << "SubscribePipeline: failed to create video elements";
        return;
    }

    GstCaps *sinkCaps = gst_caps_from_string("video/x-raw,format=I420");
    g_object_set(appsink,
        "emit-signals", TRUE,
        "caps", sinkCaps,
        "drop", TRUE,
        "max-buffers", 1,
        nullptr);
    gst_caps_unref(sinkCaps);

    g_signal_connect(appsink, "new-sample",
        G_CALLBACK(onNewVideoSample), this);
    m_videoAppsink = appsink;

    gst_bin_add_many(GST_BIN(m_pipeline), depay, decoder, convert, appsink, nullptr);
    gst_element_link_many(depay, decoder, convert, appsink, nullptr);

    gst_element_sync_state_with_parent(depay);
    gst_element_sync_state_with_parent(decoder);
    gst_element_sync_state_with_parent(convert);
    gst_element_sync_state_with_parent(appsink);

    GstPad *sinkPad = gst_element_get_static_pad(depay, "sink");
    GstPadLinkReturn ret = gst_pad_link(pad, sinkPad);
    gst_object_unref(sinkPad);

    if (ret != GST_PAD_LINK_OK) {
        qWarning() << "SubscribePipeline: video pad link failed:" << ret;
    } else {
        qDebug() << "SubscribePipeline: video chain linked successfully";

        // Request keyframes aggressively — send PLI at 0s, 1s, 2s, 4s after link.
        // webrtcbin translates ForceKeyUnit to RTCP PLI → MCU → phone sends keyframe.
        // Multiple requests needed because MCU/Janus may not forward the first one.
        m_videoDepay = depay;  // save for PLI requests
        auto sendPLI = [this]() {
            if (!m_videoDepay || !m_pipeline) return;
            // Send ForceKeyUnit on the depay's src pad — upstream events travel from src to sink
            // This propagates through webrtcbin which translates it to RTCP PLI
            GstPad *srcPad = gst_element_get_static_pad(m_videoDepay, "src");
            if (srcPad) {
                gst_pad_send_event(srcPad,
                    gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM,
                        gst_structure_new("GstForceKeyUnit",
                            "all-headers", G_TYPE_BOOLEAN, TRUE, nullptr)));
                gst_object_unref(srcPad);
                qDebug() << "SubscribePipeline: sent PLI for keyframe";
            }
        };
        sendPLI();
        QTimer::singleShot(500, this, sendPLI);
        QTimer::singleShot(1500, this, sendPLI);
        QTimer::singleShot(3000, this, sendPLI);

        // Periodic PLI every 5 seconds to recover from packet loss mid-call.
        // VP8 without keyframes scrambles on any lost packet and never recovers
        // until the next keyframe. MCU/Janus doesn't send keyframes automatically.
        m_pliTimer = new QTimer(this);
        m_pliTimer->setInterval(5000);
        connect(m_pliTimer, &QTimer::timeout, this, sendPLI);
        m_pliTimer->start();
    }
}

GstFlowReturn SubscribePipeline::onNewVideoSample(GstAppSink *sink, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;

    QPointer<SubscribePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, sample]() {
        if (guard && guard->m_videoProvider)
            guard->m_videoProvider->feedFrame(sample);
        gst_sample_unref(sample);
    }, Qt::QueuedConnection);

    return GST_FLOW_OK;
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

    qDebug() << "SubscribePipeline: answer SDP:\n" << sdp.left(2000);
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
