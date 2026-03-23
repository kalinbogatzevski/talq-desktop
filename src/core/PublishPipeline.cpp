#include "core/PublishPipeline.h"
#include <QDebug>
#include <QPointer>
#include <QRegularExpression>
#include <QUrl>
#include <gst/app/gstappsink.h>

PublishPipeline::PublishPipeline(QObject *parent)
    : QObject(parent)
{
    m_localVideoProvider = new VideoFrameProvider(this);
}

PublishPipeline::~PublishPipeline()
{
    stop();
}

bool PublishPipeline::start(const QString &stunServer, const QList<TurnServer> &turnServers,
                           const QString &audioDeviceId)
{
    if (m_running) return false;

    m_pipeline = gst_pipeline_new("publish-pipeline");
    m_webrtcbin = gst_element_factory_make("webrtcbin", "pub-webrtcbin");

    if (!m_pipeline || !m_webrtcbin) {
        emit error("Failed to create publish pipeline elements");
        cleanup();
        return false;
    }

    if (!stunServer.isEmpty())
        g_object_set(m_webrtcbin, "stun-server", stunServer.toUtf8().constData(), nullptr);
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
            qDebug() << "PublishPipeline: adding TURN server" << gstUrl;
            gboolean ret = FALSE;
            g_signal_emit_by_name(m_webrtcbin, "add-turn-server", gstUrl.toUtf8().constData(), &ret);
        }
    }

    // Audio capture chain
    GstElement *audiosrc = gst_element_factory_make("wasapi2src", "pub-audiosrc");
    GstElement *audioconvert = gst_element_factory_make("audioconvert", nullptr);
    GstElement *audioresample = gst_element_factory_make("audioresample", nullptr);
    GstElement *level = gst_element_factory_make("level", "pub-level");
    GstElement *opusenc = gst_element_factory_make("opusenc", nullptr);
    GstElement *rtpopuspay = gst_element_factory_make("rtpopuspay", nullptr);

    if (!audiosrc || !audioconvert || !audioresample || !level || !opusenc || !rtpopuspay) {
        emit error("Failed to create audio capture elements");
        cleanup();
        return false;
    }

    if (!audioDeviceId.isEmpty()) {
        g_object_set(audiosrc, "device", audioDeviceId.toUtf8().constData(), nullptr);
        qDebug() << "PublishPipeline: using audio input device" << audioDeviceId;
    }

    // Configure level element: report every 100ms
    g_object_set(level, "post-messages", TRUE, "interval", (guint64)100000000, nullptr);

    gst_bin_add_many(GST_BIN(m_pipeline), audiosrc, audioconvert, audioresample,
                     level, opusenc, rtpopuspay, m_webrtcbin, nullptr);

    if (!gst_element_link_many(audiosrc, audioconvert, audioresample,
                               level, opusenc, rtpopuspay, nullptr)) {
        emit error("Failed to link audio capture chain");
        cleanup();
        return false;
    }

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

    // No bus watch — pollBus() handles all bus messages via manual polling

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
    disableCamera();
    cleanup();
    m_running = false;
    qDebug() << "PublishPipeline: stopped";
}

void PublishPipeline::cleanup()
{
    // Disconnect GStreamer signals to prevent callbacks with stale userData
    if (m_webrtcbin)
        g_signal_handlers_disconnect_by_data(m_webrtcbin, this);
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

    QByteArray sdpUtf8 = sdp.toUtf8();
    GstSDPMessage *sdpMsg;
    gst_sdp_message_new(&sdpMsg);
    gst_sdp_message_parse_buffer((const guint8 *)sdpUtf8.constData(),
                                  sdpUtf8.size(), sdpMsg);

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

void PublishPipeline::enableCamera(int deviceIndex, bool hd1080)
{
    if (m_cameraEnabled || !m_pipeline) return;

    qDebug() << "PublishPipeline: enabling camera, device" << deviceIndex << (hd1080 ? "1080p" : "720p");

    m_cameraSrc = gst_element_factory_make("ksvideosrc", nullptr);
    if (!m_cameraSrc) {
        emit cameraError("Camera capture plugin (ksvideosrc) not available");
        return;
    }
    g_object_set(m_cameraSrc, "device-index", deviceIndex, nullptr);

    m_videoConvert = gst_element_factory_make("videoconvert", nullptr);
    m_videoCapsFilter = gst_element_factory_make("capsfilter", nullptr);
    m_videoEncoder = gst_element_factory_make("openh264enc", nullptr);
    m_videoPayloader = gst_element_factory_make("rtph264pay", nullptr);

    if (!m_videoConvert || !m_videoCapsFilter || !m_videoEncoder || !m_videoPayloader) {
        emit cameraError("Failed to create video encoding elements");
        if (m_cameraSrc) { gst_object_unref(m_cameraSrc); m_cameraSrc = nullptr; }
        if (m_videoConvert) { gst_object_unref(m_videoConvert); m_videoConvert = nullptr; }
        if (m_videoCapsFilter) { gst_object_unref(m_videoCapsFilter); m_videoCapsFilter = nullptr; }
        if (m_videoEncoder) { gst_object_unref(m_videoEncoder); m_videoEncoder = nullptr; }
        if (m_videoPayloader) { gst_object_unref(m_videoPayloader); m_videoPayloader = nullptr; }
        return;
    }

    int w = hd1080 ? 1920 : 1280;
    int h = hd1080 ? 1080 : 720;
    int bitrate = hd1080 ? 3000000 : 1500000;

    // Camera outputs JPEG at high res, raw only at 640x480.
    // Use JPEG capture + jpegdec for full resolution.
    m_jpegDec = gst_element_factory_make("jpegdec", nullptr);

    // Capture caps: request JPEG at desired resolution
    QString capsStr = QString("image/jpeg,width=%1,height=%2,framerate=30/1").arg(w).arg(h);
    GstCaps *caps = gst_caps_from_string(capsStr.toUtf8().constData());
    g_object_set(m_videoCapsFilter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    g_object_set(m_videoEncoder, "bitrate", bitrate, "rate-control", 1, "complexity", 1, nullptr);

    // Create tee + preview branch elements
    m_tee = gst_element_factory_make("tee", "camera-tee");
    m_encQueue = gst_element_factory_make("queue", "enc-queue");
    m_previewQueue = gst_element_factory_make("queue", "preview-queue");
    m_previewConvert = gst_element_factory_make("videoconvert", "preview-convert");
    m_previewAppsink = gst_element_factory_make("appsink", "preview-sink");

    if (!m_tee || !m_encQueue || !m_previewQueue || !m_previewConvert || !m_previewAppsink) {
        qWarning() << "PublishPipeline: failed to create preview elements, continuing without preview";
        if (m_tee) { gst_object_unref(m_tee); m_tee = nullptr; }
        if (m_encQueue) { gst_object_unref(m_encQueue); m_encQueue = nullptr; }
        if (m_previewQueue) { gst_object_unref(m_previewQueue); m_previewQueue = nullptr; }
        if (m_previewConvert) { gst_object_unref(m_previewConvert); m_previewConvert = nullptr; }
        if (m_previewAppsink) { gst_object_unref(m_previewAppsink); m_previewAppsink = nullptr; }
    }

    if (m_previewAppsink) {
        GstCaps *previewCaps = gst_caps_from_string("video/x-raw,format=I420");
        g_object_set(m_previewAppsink,
            "emit-signals", TRUE,
            "caps", previewCaps,
            "drop", TRUE,
            "max-buffers", 1,
            nullptr);
        gst_caps_unref(previewCaps);
        g_signal_connect(m_previewAppsink, "new-sample",
            G_CALLBACK(onPreviewSample), this);
    }

    if (!m_jpegDec) {
        qWarning() << "PublishPipeline: jpegdec not available, trying raw capture";
        // Fallback: raw capture (640x480 max)
        GstCaps *rawCaps = gst_caps_from_string("video/x-raw,framerate=30/1");
        g_object_set(m_videoCapsFilter, "caps", rawCaps, nullptr);
        gst_caps_unref(rawCaps);

        if (m_tee) {
            gst_bin_add_many(GST_BIN(m_pipeline), m_cameraSrc, m_videoConvert, m_videoCapsFilter,
                m_tee, m_encQueue, m_videoEncoder, m_videoPayloader,
                m_previewQueue, m_previewConvert, m_previewAppsink, nullptr);
        } else {
            gst_bin_add_many(GST_BIN(m_pipeline), m_cameraSrc, m_videoConvert, m_videoCapsFilter,
                m_videoEncoder, m_videoPayloader, nullptr);
        }
    } else {
        if (m_tee) {
            gst_bin_add_many(GST_BIN(m_pipeline), m_cameraSrc, m_videoCapsFilter, m_jpegDec, m_videoConvert,
                m_tee, m_encQueue, m_videoEncoder, m_videoPayloader,
                m_previewQueue, m_previewConvert, m_previewAppsink, nullptr);
        } else {
            gst_bin_add_many(GST_BIN(m_pipeline), m_cameraSrc, m_videoCapsFilter, m_jpegDec, m_videoConvert,
                m_videoEncoder, m_videoPayloader, nullptr);
        }
    }

    gboolean linked;
    if (m_tee) {
        // Link capture chain up to tee
        if (m_jpegDec) {
            linked = gst_element_link_many(m_cameraSrc, m_videoCapsFilter, m_jpegDec, m_videoConvert, m_tee, nullptr);
        } else {
            linked = gst_element_link_many(m_cameraSrc, m_videoConvert, m_videoCapsFilter, m_tee, nullptr);
        }
        // Link encoder branch: encQueue -> encoder -> payloader
        linked = linked && gst_element_link_many(m_encQueue, m_videoEncoder, m_videoPayloader, nullptr);
        // Link preview branch: previewQueue -> previewConvert -> previewAppsink
        linked = linked && gst_element_link_many(m_previewQueue, m_previewConvert, m_previewAppsink, nullptr);

        if (linked) {
            // Request tee src pads and link to each branch
            GstPad *teeSrcEnc = gst_element_request_pad_simple(m_tee, "src_%u");
            GstPad *encQueueSink = gst_element_get_static_pad(m_encQueue, "sink");
            GstPadLinkReturn r1 = gst_pad_link(teeSrcEnc, encQueueSink);
            gst_object_unref(teeSrcEnc);
            gst_object_unref(encQueueSink);

            GstPad *teeSrcPreview = gst_element_request_pad_simple(m_tee, "src_%u");
            GstPad *previewQueueSink = gst_element_get_static_pad(m_previewQueue, "sink");
            GstPadLinkReturn r2 = gst_pad_link(teeSrcPreview, previewQueueSink);
            gst_object_unref(teeSrcPreview);
            gst_object_unref(previewQueueSink);

            if (r1 != GST_PAD_LINK_OK || r2 != GST_PAD_LINK_OK) {
                qWarning() << "PublishPipeline: tee pad link failed:" << r1 << r2;
                linked = false;
            }
        }
    } else {
        // No tee fallback: original direct path
        if (m_jpegDec) {
            linked = gst_element_link_many(m_cameraSrc, m_videoCapsFilter, m_jpegDec, m_videoConvert, m_videoEncoder, m_videoPayloader, nullptr);
        } else {
            linked = gst_element_link_many(m_cameraSrc, m_videoConvert, m_videoCapsFilter, m_videoEncoder, m_videoPayloader, nullptr);
        }
    }

    if (!linked) {
        qWarning() << "PublishPipeline: failed to link video chain";
        emit cameraError("Failed to link video pipeline");
        disableCamera();
        return;
    }

    m_videoSinkPad = gst_element_request_pad_simple(m_webrtcbin, "sink_%u");
    GstPad *payloaderSrc = gst_element_get_static_pad(m_videoPayloader, "src");
    GstPadLinkReturn ret = gst_pad_link(payloaderSrc, m_videoSinkPad);
    gst_object_unref(payloaderSrc);

    if (ret != GST_PAD_LINK_OK) {
        qWarning() << "PublishPipeline: video pad link failed:" << ret;
        emit cameraError("Failed to connect video to WebRTC");
        disableCamera();
        return;
    }

    gst_element_sync_state_with_parent(m_cameraSrc);
    gst_element_sync_state_with_parent(m_videoConvert);
    gst_element_sync_state_with_parent(m_videoCapsFilter);
    if (m_jpegDec) gst_element_sync_state_with_parent(m_jpegDec);
    if (m_tee) {
        gst_element_sync_state_with_parent(m_tee);
        gst_element_sync_state_with_parent(m_encQueue);
        gst_element_sync_state_with_parent(m_previewQueue);
        gst_element_sync_state_with_parent(m_previewConvert);
        gst_element_sync_state_with_parent(m_previewAppsink);
    }
    gst_element_sync_state_with_parent(m_videoEncoder);
    gst_element_sync_state_with_parent(m_videoPayloader);

    m_cameraEnabled = true;
    qDebug() << "PublishPipeline: camera enabled successfully";

    // Manually trigger renegotiation — webrtcbin doesn't always fire
    // on-negotiation-needed when pads are added to a running pipeline
    qDebug() << "PublishPipeline: triggering renegotiation for video";
    GstPromise *promise = gst_promise_new_with_change_func(onOfferCreated, this, nullptr);
    g_signal_emit_by_name(m_webrtcbin, "create-offer", nullptr, promise);
}

void PublishPipeline::disableCamera()
{
    if (!m_cameraEnabled && !m_cameraSrc) return;

    qDebug() << "PublishPipeline: disabling camera";

    auto removeElement = [this](GstElement *&el) {
        if (el) {
            gst_element_set_state(el, GST_STATE_NULL);
            gst_bin_remove(GST_BIN(m_pipeline), el);
            el = nullptr;
        }
    };

    if (m_videoPayloader && m_videoSinkPad) {
        GstPad *src = gst_element_get_static_pad(m_videoPayloader, "src");
        if (src) {
            gst_pad_unlink(src, m_videoSinkPad);
            gst_object_unref(src);
        }
    }
    if (m_videoSinkPad) {
        gst_element_release_request_pad(m_webrtcbin, m_videoSinkPad);
        gst_object_unref(m_videoSinkPad);
        m_videoSinkPad = nullptr;
    }

    if (m_previewAppsink)
        g_signal_handlers_disconnect_by_data(m_previewAppsink, this);
    removeElement(m_previewAppsink);
    removeElement(m_previewConvert);
    removeElement(m_previewQueue);
    removeElement(m_tee);
    removeElement(m_encQueue);
    removeElement(m_videoPayloader);
    removeElement(m_videoEncoder);
    removeElement(m_videoCapsFilter);
    removeElement(m_jpegDec);
    removeElement(m_videoConvert);
    removeElement(m_cameraSrc);

    m_cameraEnabled = false;
}

void PublishPipeline::pollBus()
{
    if (!m_pipeline) return;
    GstBus *bus = gst_element_get_bus(m_pipeline);
    GstMessage *msg;
    while ((msg = gst_bus_pop(bus)) != nullptr) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ELEMENT) {
            const GstStructure *s = gst_message_get_structure(msg);
            const gchar *name = gst_structure_get_name(s);
            if (g_strcmp0(name, "level") == 0) {
                static int lvlDbg = 0;
                if (++lvlDbg <= 2) {
                    gchar *str = gst_structure_to_string(s);
                    qDebug() << "PublishPipeline: level raw:" << QString::fromUtf8(str).left(300);
                    g_free(str);
                }
                // Extract peak level from GValueArray
                // Range: -100dB (silence) to 0dB (max) → map to 0.0-1.0
                GValueArray *arr = nullptr;
                gst_structure_get(s, "peak", G_TYPE_VALUE_ARRAY, &arr, nullptr);
                if (arr && arr->n_values > 0) {
                    gdouble db = g_value_get_double(arr->values);
                    // Use wider range: -100 to 0
                    double lvl = qBound(0.0, (db + 100.0) / 100.0, 1.0);
                    // Apply curve for better visual response
                    lvl = lvl * lvl;  // square for more dynamic range visibility
                    emit audioLevelUpdated(lvl);
                }
                if (arr) g_value_array_free(arr);
            }
        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = nullptr; gchar *dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            QString errMsg = QString::fromUtf8(err->message);
            qWarning() << "PublishPipeline ERROR:" << errMsg << (dbg ? dbg : "");
            g_clear_error(&err); g_free(dbg);
            if (m_cameraEnabled) {
                disableCamera();
                emit cameraError(errMsg);
            }
        }
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

// --- GStreamer callbacks (marshal to Qt thread) ---

void PublishPipeline::onNegotiationNeeded(GstElement *, gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);
    QMetaObject::invokeMethod(self, [self]() {
        if (!self->m_webrtcbin) return;
        qDebug() << "PublishPipeline: negotiation needed, creating offer";
        GstPromise *promise = gst_promise_new_with_change_func(onOfferCreated, self, nullptr);
        g_signal_emit_by_name(self->m_webrtcbin, "create-offer", nullptr, promise);
    }, Qt::QueuedConnection);
}

void PublishPipeline::onIceCandidate(GstElement *, guint mlineIndex, gchar *candidate, gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);
    QString c = QString::fromUtf8(candidate);
    int ml = static_cast<int>(mlineIndex);
    QMetaObject::invokeMethod(self, [self, c, ml]() {
        emit self->iceCandidateReady(c, ml, QString("0"));
    }, Qt::QueuedConnection);
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
    QMetaObject::invokeMethod(self, [self, sdp]() {
        emit self->localOfferReady(sdp);
    }, Qt::QueuedConnection);
}

void PublishPipeline::onIceStateChanged(GObject *obj, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);
    GstWebRTCICEConnectionState state;
    g_object_get(obj, "ice-connection-state", &state, nullptr);
    const char *names[] = {"new", "checking", "connected", "completed", "failed", "disconnected", "closed"};
    int idx = static_cast<int>(state);
    QString stateName = (idx < 7) ? names[idx] : "unknown";
    QMetaObject::invokeMethod(self, [self, stateName]() {
        qDebug() << "PublishPipeline: ICE ->" << stateName;
        emit self->iceStateChanged(stateName);
    }, Qt::QueuedConnection);
}

GstFlowReturn PublishPipeline::onPreviewSample(GstAppSink *sink, gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;

    QPointer<PublishPipeline> guard(self);
    QMetaObject::invokeMethod(self->m_localVideoProvider, [guard, sample]() {
        if (guard && guard->m_localVideoProvider)
            guard->m_localVideoProvider->feedFrame(sample);
        gst_sample_unref(sample);
    }, Qt::QueuedConnection);

    return GST_FLOW_OK;
}
