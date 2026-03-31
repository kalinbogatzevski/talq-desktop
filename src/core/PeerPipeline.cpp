#include "core/PeerPipeline.h"
#include <QDebug>
#include <QPointer>
#include <QRegularExpression>
#include <QUrl>
#include <thread>

PeerPipeline::PeerPipeline(QObject *parent)
    : QObject(parent)
{
    m_localVideoProvider = new VideoFrameProvider(this);
    m_remoteVideoProvider = new VideoFrameProvider(this);
}

PeerPipeline::~PeerPipeline()
{
    stop();
}

bool PeerPipeline::start(const QString &stunServer, const QList<TurnServer> &turnServers,
                         const QString &audioInputDeviceId, const QString &audioOutputDeviceId)
{
    if (m_running) return false;
    m_audioOutputDeviceId = audioOutputDeviceId;

    m_pipeline = gst_pipeline_new("peer-pipeline");
    m_webrtcbin = gst_element_factory_make("webrtcbin", "peer-webrtcbin");

    if (!m_pipeline || !m_webrtcbin) {
        emit error("Failed to create peer pipeline elements");
        cleanup();
        return false;
    }

    if (!stunServer.isEmpty()) {
        // Nextcloud returns "stun:host:port" but GStreamer needs "stun://host:port"
        QString gstStun = stunServer;
        if (gstStun.startsWith("stun:") && !gstStun.startsWith("stun://"))
            gstStun.replace("stun:", "stun://");
        qDebug() << "PeerPipeline: STUN server:" << gstStun;
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
            qDebug() << "PeerPipeline: adding TURN server" << gstUrl;
            gboolean ret = FALSE;
            g_signal_emit_by_name(m_webrtcbin, "add-turn-server", gstUrl.toUtf8().constData(), &ret);
        }
    }

    // Audio capture — use audiotestsrc in test mode, else wasapisrc
    bool testAudio = !qEnvironmentVariableIsEmpty("TALQ_TEST_AUDIO");
    GstElement *audiosrc = nullptr;
    if (testAudio) {
        audiosrc = gst_element_factory_make("audiotestsrc", "peer-audiosrc");
        if (audiosrc) {
            g_object_set(audiosrc, "wave", 0 /* sine */, "freq", 440.0, "is-live", TRUE, nullptr);
            qDebug() << "PeerPipeline: using audiotestsrc (test mode)";
        }
    }
    if (!audiosrc) {
        audiosrc = gst_element_factory_make("wasapi2src", "peer-audiosrc");
        if (audiosrc) {
            qDebug() << "PeerPipeline: audio source: wasapi2src";
        } else {
            audiosrc = gst_element_factory_make("wasapisrc", "peer-audiosrc");
            if (audiosrc) {
                g_object_set(audiosrc, "low-latency", FALSE, nullptr);
                qDebug() << "PeerPipeline: audio source: wasapisrc";
            }
        }
    }
    if (!audiosrc) {
        audiosrc = gst_element_factory_make("directsoundsrc", "peer-audiosrc");
        if (audiosrc) qDebug() << "PeerPipeline: audio source: directsoundsrc";
    }
    if (!audiosrc) {
        audiosrc = gst_element_factory_make("autoaudiosrc", "peer-audiosrc");
        if (audiosrc) qDebug() << "PeerPipeline: audio source: autoaudiosrc";
    }
    GstElement *audioconvert = gst_element_factory_make("audioconvert", nullptr);
    GstElement *audioresample = gst_element_factory_make("audioresample", nullptr);
    GstElement *capsfilter = gst_element_factory_make("capsfilter", nullptr);
    GstElement *level = gst_element_factory_make("level", "peer-level");
    GstElement *opusenc = gst_element_factory_make("opusenc", nullptr);
    GstElement *rtpopuspay = gst_element_factory_make("rtpopuspay", "peer-rtpopuspay");

    if (!audiosrc || !audioconvert || !audioresample || !capsfilter || !level || !opusenc || !rtpopuspay) {
        emit error("Failed to create audio capture elements");
        cleanup();
        return false;
    }

    // Force S16LE right after capture — prevents format negotiation failures
    GstCaps *srcCaps = gst_caps_from_string("audio/x-raw,format=S16LE");
    g_object_set(capsfilter, "caps", srcCaps, nullptr);
    gst_caps_unref(srcCaps);

    if (!audioInputDeviceId.isEmpty()) {
        g_object_set(audiosrc, "device", audioInputDeviceId.toUtf8().constData(), nullptr);
        qDebug() << "PeerPipeline: using audio input device" << audioInputDeviceId;
    }

    // Configure level element: report every 100ms
    g_object_set(level, "post-messages", TRUE, "interval", (guint64)100000000, nullptr);

    gst_bin_add_many(GST_BIN(m_pipeline), audiosrc, capsfilter, audioconvert, audioresample,
                     level, opusenc, rtpopuspay, m_webrtcbin, nullptr);

    if (!gst_element_link_many(audiosrc, capsfilter, audioconvert, audioresample,
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

    // Connect signals — including pad-added for receiving remote media
    g_signal_connect(m_webrtcbin, "on-ice-candidate",
                     G_CALLBACK(onIceCandidate), this);
    g_signal_connect(m_webrtcbin, "pad-added",
                     G_CALLBACK(onPadAdded), this);
    g_signal_connect(m_webrtcbin, "notify::ice-connection-state",
                     G_CALLBACK(onIceStateChanged), this);
    // Do NOT connect on-negotiation-needed — CallManager controls when offers happen

    // No bus watch — pollBus() handles all bus messages via manual polling

    GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        emit error("Failed to start peer pipeline");
        cleanup();
        return false;
    }

    m_running = true;
    qDebug() << "PeerPipeline: started (send+receive)";
    return true;
}

void PeerPipeline::stop()
{
    if (!m_running) return;
    disableCamera();
    cleanup();
    m_running = false;
    qDebug() << "PeerPipeline: stopped";
}

void PeerPipeline::cleanup()
{
    // Disconnect GStreamer signals to prevent callbacks with stale userData
    if (m_webrtcbin)
        g_signal_handlers_disconnect_by_data(m_webrtcbin, this);
    if (m_pipeline) {
        // Move blocking GST_STATE_NULL off the UI thread
        GstElement *pipeline = m_pipeline;
        m_pipeline = nullptr;
        std::thread([pipeline]() {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
        }).detach();
    }
    m_webrtcbin = nullptr;
    m_remoteDescSet = false;
    m_pendingCandidates.clear();
}

void PeerPipeline::createOffer()
{
    if (!m_webrtcbin) return;
    qDebug() << "PeerPipeline: creating offer";
    GstPromise *promise = gst_promise_new_with_change_func(onOfferCreated, this, nullptr);
    g_signal_emit_by_name(m_webrtcbin, "create-offer", nullptr, promise);
}

void PeerPipeline::setRemoteOffer(const QString &sdp)
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

    m_remoteDescSet = true;
    qDebug() << "PeerPipeline: remote offer SDP:\n" << sdp.left(2000);
    qDebug() << "PeerPipeline: set remote offer, flushing" << m_pendingCandidates.size() << "queued candidates";
    for (const auto &c : m_pendingCandidates)
        g_signal_emit_by_name(m_webrtcbin, "add-ice-candidate", c.first, c.second.toUtf8().constData());
    m_pendingCandidates.clear();

    qDebug() << "PeerPipeline: creating answer...";
    GstPromise *answerPromise = gst_promise_new_with_change_func(
        onAnswerCreated, this, nullptr);
    g_signal_emit_by_name(m_webrtcbin, "create-answer", nullptr, answerPromise);
}

void PeerPipeline::setRemoteAnswer(const QString &sdp)
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

    m_remoteDescSet = true;
    qDebug() << "PeerPipeline: set remote answer, flushing" << m_pendingCandidates.size() << "queued candidates";
    for (const auto &c : m_pendingCandidates)
        g_signal_emit_by_name(m_webrtcbin, "add-ice-candidate", c.first, c.second.toUtf8().constData());
    m_pendingCandidates.clear();
}

void PeerPipeline::addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid)
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

void PeerPipeline::setMuted(bool muted)
{
    if (!m_pipeline) return;
    GstElement *src = gst_bin_get_by_name(GST_BIN(m_pipeline), "peer-audiosrc");
    if (src) {
        g_object_set(src, "mute", muted, nullptr);
        gst_object_unref(src);
    }
}

void PeerPipeline::enableCamera(int deviceIndex, bool hd1080)
{
    if (m_cameraEnabled || !m_pipeline) return;

    qDebug() << "PeerPipeline: enabling camera, device" << deviceIndex << (hd1080 ? "1080p" : "720p");

    bool testVideo = !qEnvironmentVariableIsEmpty("TALQ_TEST_AUDIO");

    if (testVideo) {
        m_cameraSrc = gst_element_factory_make("videotestsrc", nullptr);
        if (m_cameraSrc) {
            g_object_set(m_cameraSrc, "is-live", TRUE, "pattern", 0 /* SMPTE */, nullptr);
            qDebug() << "PeerPipeline: using videotestsrc (test mode)";
        }
    }
    if (!m_cameraSrc) {
        m_cameraSrc = gst_element_factory_make("mfvideosrc", nullptr);
        if (!m_cameraSrc)
            m_cameraSrc = gst_element_factory_make("ksvideosrc", nullptr);
    }
    if (!m_cameraSrc) {
        emit cameraError("No camera capture plugin available");
        return;
    }
    if (!testVideo)
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

    // In test mode, videotestsrc outputs raw video — no jpegdec needed.
    // In real mode, camera outputs JPEG at high res, raw only at 640x480.
    if (testVideo) {
        m_jpegDec = nullptr;
        QString capsStr = QString("video/x-raw,width=%1,height=%2,framerate=30/1").arg(w).arg(h);
        GstCaps *caps = gst_caps_from_string(capsStr.toUtf8().constData());
        g_object_set(m_videoCapsFilter, "caps", caps, nullptr);
        gst_caps_unref(caps);
    } else {
        m_jpegDec = gst_element_factory_make("jpegdec", nullptr);
        QString capsStr = QString("image/jpeg,width=%1,height=%2,framerate=30/1").arg(w).arg(h);
        GstCaps *caps = gst_caps_from_string(capsStr.toUtf8().constData());
        g_object_set(m_videoCapsFilter, "caps", caps, nullptr);
        gst_caps_unref(caps);
    }

    g_object_set(m_videoEncoder, "bitrate", bitrate, "rate-control", 1, "complexity", 1, nullptr);

    // Create tee + preview branch elements
    m_tee = gst_element_factory_make("tee", "camera-tee");
    m_encQueue = gst_element_factory_make("queue", "enc-queue");
    m_previewQueue = gst_element_factory_make("queue", "preview-queue");
    // Prevent tee from blocking: drop old frames if downstream is slow
    if (m_encQueue) g_object_set(m_encQueue, "leaky", 2 /* downstream */, "max-size-buffers", 3, nullptr);
    if (m_previewQueue) g_object_set(m_previewQueue, "leaky", 2, "max-size-buffers", 2, nullptr);
    m_previewConvert = gst_element_factory_make("videoconvert", "preview-convert");
    m_previewAppsink = gst_element_factory_make("appsink", "preview-sink");

    if (!m_tee || !m_encQueue || !m_previewQueue || !m_previewConvert || !m_previewAppsink) {
        qWarning() << "PeerPipeline: failed to create preview elements, continuing without preview";
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
        qWarning() << "PeerPipeline: jpegdec not available, trying raw capture";
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
                qWarning() << "PeerPipeline: tee pad link failed:" << r1 << r2;
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
        qWarning() << "PeerPipeline: failed to link video chain";
        emit cameraError("Failed to link video pipeline");
        disableCamera();
        return;
    }

    // Request a webrtcbin sink pad for video, then configure its transceiver
    // with H264 caps so renegotiation SDP gets an active m=video line (not port 0).
    m_videoSinkPad = gst_element_request_pad_simple(m_webrtcbin, "sink_%u");

    // Get the transceiver for this pad and set codec-preferences + direction
    GstWebRTCRTPTransceiver *transceiver = nullptr;
    g_object_get(m_videoSinkPad, "transceiver", &transceiver, nullptr);
    if (transceiver) {
        GstCaps *videoCaps = gst_caps_from_string(
            "application/x-rtp,media=video,encoding-name=H264,clock-rate=90000,payload=96");
        g_object_set(transceiver,
                     "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY,
                     "codec-preferences", videoCaps,
                     nullptr);
        gst_caps_unref(videoCaps);
        qDebug() << "PeerPipeline: configured video transceiver (sendonly, H264)";
        gst_object_unref(transceiver);
    } else {
        qWarning() << "PeerPipeline: could not get transceiver from video pad";
    }

    GstPad *payloaderSrc = gst_element_get_static_pad(m_videoPayloader, "src");
    GstPadLinkReturn ret = gst_pad_link(payloaderSrc, m_videoSinkPad);
    gst_object_unref(payloaderSrc);

    if (ret != GST_PAD_LINK_OK) {
        qWarning() << "PeerPipeline: video pad link failed:" << ret;
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
    qDebug() << "PeerPipeline: camera enabled successfully";

    // Renegotiation needed after adding video track
    qDebug() << "PeerPipeline: triggering renegotiation for video";
    createOffer();
}

void PeerPipeline::disableCamera()
{
    if (!m_cameraEnabled && !m_cameraSrc) return;

    qDebug() << "PeerPipeline: disabling camera";

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

void PeerPipeline::pollBus()
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
                    qDebug() << "PeerPipeline: level raw:" << QString::fromUtf8(str).left(300);
                    g_free(str);
                }
                // Extract peak level from GValueArray
                // Range: -100dB (silence) to 0dB (max) -> map to 0.0-1.0
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
            qWarning() << "PeerPipeline ERROR:" << errMsg << (dbg ? dbg : "");
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

// --- Receive chain builders (called from onPadAdded) ---

void PeerPipeline::createAudioReceiveChain(GstPad *pad)
{
    GstElement *depay = gst_element_factory_make("rtpopusdepay", nullptr);
    GstElement *dec = gst_element_factory_make("opusdec", nullptr);
    GstElement *convert = gst_element_factory_make("audioconvert", nullptr);
    GstElement *resample = gst_element_factory_make("audioresample", nullptr);

    bool testMode = !qEnvironmentVariableIsEmpty("TALQ_TEST_AUDIO");
    GstElement *sink = nullptr;
    if (testMode) {
        sink = gst_element_factory_make("fakesink", nullptr);
        if (sink) qDebug() << "PeerPipeline: using fakesink for audio receive (test mode)";
    }
    // Try WASAPI2 sink first (best Windows audio output), then wasapisink, then autoaudiosink
    if (!sink) {
        sink = gst_element_factory_make("wasapi2sink", nullptr);
        if (sink) {
            qDebug() << "PeerPipeline: audio receive sink: wasapi2sink";
            if (!m_audioOutputDeviceId.isEmpty())
                g_object_set(sink, "device", m_audioOutputDeviceId.toUtf8().constData(), nullptr);
        }
    }
    if (!sink) {
        sink = gst_element_factory_make("wasapisink", nullptr);
        if (sink) {
            qDebug() << "PeerPipeline: audio receive sink: wasapisink";
            g_object_set(sink, "low-latency", FALSE, nullptr);
            if (!m_audioOutputDeviceId.isEmpty())
                g_object_set(sink, "device", m_audioOutputDeviceId.toUtf8().constData(), nullptr);
        }
    }
    if (!sink) {
        sink = gst_element_factory_make("directsoundsink", nullptr);
        if (sink) {
            qDebug() << "PeerPipeline: audio receive sink: directsoundsink";
            if (!m_audioOutputDeviceId.isEmpty())
                g_object_set(sink, "device", m_audioOutputDeviceId.toUtf8().constData(), nullptr);
        }
    }
    if (!sink) {
        sink = gst_element_factory_make("autoaudiosink", nullptr);
        if (sink) qDebug() << "PeerPipeline: audio receive sink: autoaudiosink (device selection may not work)";
    }

    if (!depay || !dec || !convert || !resample || !sink) {
        qWarning() << "PeerPipeline: failed to create audio receive chain";
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
    GstPadLinkReturn ret = gst_pad_link(pad, sinkPad);
    gst_object_unref(sinkPad);

    if (ret != GST_PAD_LINK_OK)
        qWarning() << "PeerPipeline: audio receive pad link failed:" << ret;
    else
        qDebug() << "PeerPipeline: audio receive chain linked successfully";
}

void PeerPipeline::createVideoReceiveChain(GstPad *pad, const gchar *encoding)
{
    qDebug() << "PeerPipeline: creating video receive chain for" << encoding;

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
        qWarning() << "PeerPipeline: failed to create video receive elements";
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
        G_CALLBACK(onRemoteVideoSample), this);

    gst_bin_add_many(GST_BIN(m_pipeline), depay, decoder, convert, appsink, nullptr);
    gst_element_link_many(depay, decoder, convert, appsink, nullptr);

    gst_element_sync_state_with_parent(depay);
    gst_element_sync_state_with_parent(decoder);
    gst_element_sync_state_with_parent(convert);
    gst_element_sync_state_with_parent(appsink);

    GstPad *sinkPad = gst_element_get_static_pad(depay, "sink");
    GstPadLinkReturn ret = gst_pad_link(pad, sinkPad);
    gst_object_unref(sinkPad);

    if (ret != GST_PAD_LINK_OK)
        qWarning() << "PeerPipeline: video receive pad link failed:" << ret;
    else
        qDebug() << "PeerPipeline: video receive chain linked successfully";
}

// --- GStreamer callbacks (marshal to Qt thread) ---

void PeerPipeline::onNegotiationNeeded(GstElement *, gpointer userData)
{
    Q_UNUSED(userData)
    // Intentionally empty — CallManager controls when offers are created
    qDebug() << "PeerPipeline: on-negotiation-needed fired (ignored, CallManager controls offers)";
}

void PeerPipeline::onIceCandidate(GstElement *, guint mlineIndex, gchar *candidate, gpointer userData)
{
    auto *self = static_cast<PeerPipeline *>(userData);
    QString c = QString::fromUtf8(candidate);
    int ml = static_cast<int>(mlineIndex);
    QPointer<PeerPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, c, ml]() {
        if (!guard) return;
        emit guard->iceCandidateReady(c, ml, QString("0"));
    }, Qt::QueuedConnection);
}

void PeerPipeline::onOfferCreated(GstPromise *promise, gpointer userData)
{
    auto *self = static_cast<PeerPipeline *>(userData);

    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);

    if (!offer) {
        qWarning() << "PeerPipeline: failed to create offer";
        gst_promise_unref(promise);
        return;
    }

    g_signal_emit_by_name(self->m_webrtcbin, "set-local-description", offer, nullptr);

    gchar *sdpText = gst_sdp_message_as_text(offer->sdp);
    QString sdp = QString::fromUtf8(sdpText);
    g_free(sdpText);
    gst_webrtc_session_description_free(offer);
    gst_promise_unref(promise);

    qDebug() << "PeerPipeline: offer created, SDP length=" << sdp.length();

    // Strip a=ssrc/a=ssrc-group lines — see PublishPipeline::onOfferCreated
    // for full explanation. rtpbin rewrites wire SSRC; stripping forces the
    // remote end (browser or Janus) to learn SSRC from the first RTP packet.
    QString mungedSdp;
    int stripped = 0;
    const auto lines = sdp.split('\n');
    for (const QString &line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("a=ssrc:") || trimmed.startsWith("a=ssrc-group:")) {
            stripped++;
            continue;
        }
        mungedSdp += line + '\n';
    }
    while (mungedSdp.endsWith("\n\n"))
        mungedSdp.chop(1);
    if (!mungedSdp.endsWith('\n'))
        mungedSdp += '\n';

    if (stripped > 0)
        qDebug() << "PeerPipeline: stripped" << stripped << "a=ssrc lines from offer SDP";

    QPointer<PeerPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, mungedSdp]() {
        if (!guard) return;
        emit guard->localOfferReady(mungedSdp);
    }, Qt::QueuedConnection);
}

void PeerPipeline::onAnswerCreated(GstPromise *promise, gpointer userData)
{
    auto *self = static_cast<PeerPipeline *>(userData);

    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *answer = nullptr;
    gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, nullptr);

    if (!answer) {
        qWarning() << "PeerPipeline: failed to create answer";
        gst_promise_unref(promise);
        return;
    }

    g_signal_emit_by_name(self->m_webrtcbin, "set-local-description", answer, nullptr);

    gchar *sdpText = gst_sdp_message_as_text(answer->sdp);
    QString sdp = QString::fromUtf8(sdpText);
    g_free(sdpText);
    gst_webrtc_session_description_free(answer);
    gst_promise_unref(promise);

    qDebug() << "PeerPipeline: answer created, SDP length=" << sdp.length();

    // Strip a=ssrc/a=ssrc-group lines from answer too — same rtpbin SSRC
    // rewrite issue applies when we are the answerer in P2P calls.
    QString mungedSdp;
    int stripped = 0;
    const auto lines = sdp.split('\n');
    for (const QString &line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("a=ssrc:") || trimmed.startsWith("a=ssrc-group:")) {
            stripped++;
            continue;
        }
        mungedSdp += line + '\n';
    }
    while (mungedSdp.endsWith("\n\n"))
        mungedSdp.chop(1);
    if (!mungedSdp.endsWith('\n'))
        mungedSdp += '\n';

    if (stripped > 0)
        qDebug() << "PeerPipeline: stripped" << stripped << "a=ssrc lines from answer SDP";

    qDebug() << "PeerPipeline: answer SDP:\n" << mungedSdp.left(2000);

    QPointer<PeerPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, mungedSdp]() {
        if (!guard) return;
        emit guard->localAnswerReady(mungedSdp);
    }, Qt::QueuedConnection);
}

void PeerPipeline::onPadAdded(GstElement *, GstPad *pad, gpointer userData)
{
    auto *self = static_cast<PeerPipeline *>(userData);

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    if (!caps) return;

    GstStructure *s = gst_caps_get_structure(caps, 0);
    const gchar *media = gst_structure_get_string(s, "media");
    const gchar *encoding = gst_structure_get_string(s, "encoding-name");

    qDebug() << "PeerPipeline: pad added, media=" << media << "encoding=" << encoding;

    bool isAudio = (media && g_strcmp0(media, "audio") == 0)
                || (encoding && g_ascii_strcasecmp(encoding, "OPUS") == 0);
    bool isVideo = (media && g_strcmp0(media, "video") == 0)
                || (encoding && (g_ascii_strcasecmp(encoding, "VP8") == 0
                              || g_ascii_strcasecmp(encoding, "H264") == 0));

    // Copy encoding string before unreffing caps (encoding points into caps memory)
    QByteArray encodingCopy = encoding ? QByteArray(encoding) : QByteArray();
    gst_caps_unref(caps);

    if (!isAudio && !isVideo) {
        qDebug() << "PeerPipeline: skipping unknown pad type";
        return;
    }

    // Ref the pad so it survives until the Qt thread processes it
    gst_object_ref(pad);

    QPointer<PeerPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, pad, isAudio, isVideo, encodingCopy]() {
        if (!guard) { gst_object_unref(pad); return; }
        auto *self = guard.data();

        if (isAudio) {
            self->createAudioReceiveChain(pad);
        } else if (isVideo) {
            self->createVideoReceiveChain(pad, encodingCopy.constData());
        }
        gst_object_unref(pad);
    }, Qt::QueuedConnection);
}

void PeerPipeline::onIceStateChanged(GObject *obj, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<PeerPipeline *>(userData);
    GstWebRTCICEConnectionState state;
    g_object_get(obj, "ice-connection-state", &state, nullptr);
    const char *names[] = {"new", "checking", "connected", "completed", "failed", "disconnected", "closed"};
    int idx = static_cast<int>(state);
    QString stateName = (idx < 7) ? names[idx] : "unknown";
    QPointer<PeerPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, stateName]() {
        if (!guard) return;
        qDebug() << "PeerPipeline: ICE ->" << stateName;
        emit guard->iceStateChanged(stateName);
    }, Qt::QueuedConnection);
}

GstFlowReturn PeerPipeline::onPreviewSample(GstAppSink *sink, gpointer userData)
{
    auto *self = static_cast<PeerPipeline *>(userData);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;

    QPointer<PeerPipeline> guard(self);
    QMetaObject::invokeMethod(self->m_localVideoProvider, [guard, sample]() {
        if (guard && guard->m_localVideoProvider)
            guard->m_localVideoProvider->feedFrame(sample);
        gst_sample_unref(sample);
    }, Qt::QueuedConnection);

    return GST_FLOW_OK;
}

GstFlowReturn PeerPipeline::onRemoteVideoSample(GstAppSink *sink, gpointer userData)
{
    auto *self = static_cast<PeerPipeline *>(userData);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;

    QPointer<PeerPipeline> guard(self);
    QMetaObject::invokeMethod(self->m_remoteVideoProvider, [guard, sample]() {
        if (guard && guard->m_remoteVideoProvider)
            guard->m_remoteVideoProvider->feedFrame(sample);
        gst_sample_unref(sample);
    }, Qt::QueuedConnection);

    return GST_FLOW_OK;
}
