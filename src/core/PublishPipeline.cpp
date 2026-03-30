#include "core/PublishPipeline.h"
#include <QDebug>
#include <QPointer>
#include <QRegularExpression>
#include <QUrl>
#include <thread>
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
                           const QString &audioDeviceId, bool withVideo,
                           int videoDeviceIndex, bool hd1080)
{
    if (m_running) return false;

    m_pipeline = gst_pipeline_new("publish-pipeline");
    m_webrtcbin = gst_element_factory_make("webrtcbin", "pub-webrtcbin");

    if (!m_pipeline || !m_webrtcbin) {
        emit error("Failed to create publish pipeline elements");
        cleanup();
        return false;
    }

    if (!stunServer.isEmpty()) {
        // Nextcloud returns "stun:host:port" but GStreamer needs "stun://host:port"
        QString gstStun = stunServer;
        if (gstStun.startsWith("stun:") && !gstStun.startsWith("stun://"))
            gstStun = "stun://" + gstStun.mid(5);
        qDebug() << "PublishPipeline: STUN server:" << gstStun;
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
            qDebug() << "PublishPipeline: adding TURN server" << gstUrl;
            gboolean ret = FALSE;
            g_signal_emit_by_name(m_webrtcbin, "add-turn-server", gstUrl.toUtf8().constData(), &ret);
        }
    }

    // Audio capture — wasapi2src (best), wasapisrc (fallback), autoaudiosrc (last resort)
    // DEBUG: set TALQ_TEST_AUDIO=1 env var to use audiotestsrc (440Hz tone) for testing
    GstElement *audiosrc = nullptr;
    if (qEnvironmentVariableIsSet("TALQ_TEST_AUDIO")) {
        audiosrc = gst_element_factory_make("audiotestsrc", "pub-audiosrc");
        g_object_set(audiosrc, "is-live", TRUE, "wave", 0, "freq", 440.0, nullptr);
        qDebug() << "PublishPipeline: audio source: audiotestsrc (TEST MODE)";
    } else {
        // Try sources in preference order
        const char *srcName = "autoaudiosrc";
        audiosrc = gst_element_factory_make("wasapi2src", "pub-audiosrc");
        if (audiosrc) {
            srcName = "wasapi2src";
        } else {
            audiosrc = gst_element_factory_make("wasapisrc", "pub-audiosrc");
            if (audiosrc) {
                srcName = "wasapisrc";
                g_object_set(audiosrc, "low-latency", FALSE, nullptr);
            } else {
                audiosrc = gst_element_factory_make("autoaudiosrc", "pub-audiosrc");
            }
        }
        qDebug() << "PublishPipeline: audio source:" << srcName;

        if (audiosrc && !audioDeviceId.isEmpty()) {
            g_object_set(audiosrc, "device", audioDeviceId.toUtf8().constData(), nullptr);
            qDebug() << "PublishPipeline: using audio input device" << audioDeviceId;
        }
    }

    GstElement *audioconvert = gst_element_factory_make("audioconvert", nullptr);
    GstElement *audioresample = gst_element_factory_make("audioresample", nullptr);
    GstElement *level = gst_element_factory_make("level", "pub-level");
    GstElement *opusenc = gst_element_factory_make("opusenc", nullptr);
    GstElement *rtpopuspay = gst_element_factory_make("rtpopuspay", "pub-rtpopuspay");

    if (!audiosrc || !audioconvert || !audioresample || !level || !opusenc || !rtpopuspay) {
        emit error("Failed to create audio capture elements");
        cleanup();
        return false;
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

    // Force standard OPUS (not MULTIOPUS) — Janus doesn't support multichannel Opus
    GstWebRTCRTPTransceiver *audioTransceiver = nullptr;
    g_object_get(sinkPad, "transceiver", &audioTransceiver, nullptr);
    if (audioTransceiver) {
        GstCaps *audioCaps = gst_caps_from_string(
            "application/x-rtp,media=audio,encoding-name=OPUS,payload=111,clock-rate=48000");
        g_object_set(audioTransceiver,
                     "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY,
                     "codec-preferences", audioCaps,
                     nullptr);
        gst_caps_unref(audioCaps);
        qDebug() << "PublishPipeline: configured audio transceiver (sendonly, OPUS)";
        gst_object_unref(audioTransceiver);
    }

    gst_object_unref(rtpSrcPad);
    gst_object_unref(sinkPad);

    // MCU mode: ALWAYS include a video track in the publisher SDP.
    // Janus videoroom expects video from all publishers — without it, remote
    // Always set up the video chain with dummy source.
    // enableCamera() will swap the source feeding the encoder without touching
    // the encoder/payloader/webrtcbin — same as browser's replaceTrack().
    {
        GstElement *testsrc = gst_element_factory_make("videotestsrc", "pub-dummyvideo");
        GstElement *vconv = gst_element_factory_make("videoconvert", "pub-dummyconv");
        m_videoEncoder = gst_element_factory_make("vp8enc", "pub-videoenc");
        m_videoPayloader = gst_element_factory_make("rtpvp8pay", "pub-videopay");
        if (testsrc && vconv && m_videoEncoder && m_videoPayloader) {
            g_object_set(testsrc, "pattern", 2 /* black */, "is-live", TRUE, nullptr);
            g_object_set(m_videoEncoder, "deadline", (gint64)1, "target-bitrate", 10000, nullptr);
            gst_bin_add_many(GST_BIN(m_pipeline), testsrc, vconv, m_videoEncoder, m_videoPayloader, nullptr);

            GstCaps *lowCaps = gst_caps_from_string("video/x-raw,width=16,height=16,framerate=1/1");
            gst_element_link(testsrc, vconv);
            gst_element_link_filtered(vconv, m_videoEncoder, lowCaps);
            gst_caps_unref(lowCaps);
            gst_element_link(m_videoEncoder, m_videoPayloader);

            GstPad *vpSrc = gst_element_get_static_pad(m_videoPayloader, "src");
            m_videoSinkPad = gst_element_request_pad_simple(m_webrtcbin, "sink_%u");

            GstWebRTCRTPTransceiver *vt = nullptr;
            g_object_get(m_videoSinkPad, "transceiver", &vt, nullptr);
            if (vt) {
                GstCaps *vc = gst_caps_from_string(
                    "application/x-rtp,media=video,encoding-name=VP8,clock-rate=90000,payload=96");
                g_object_set(vt, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY,
                             "codec-preferences", vc, nullptr);
                gst_caps_unref(vc);
                gst_object_unref(vt);
            }
            gst_pad_link(vpSrc, m_videoSinkPad);
            gst_object_unref(vpSrc);
            qDebug() << "PublishPipeline: video chain ready (dummy → enc → pay → webrtcbin)";
        }
    }

    // Try camera if requested — swaps dummy source for camera on the same encoder
    if (withVideo) {
        enableCamera(videoDeviceIndex, hd1080);
    }

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
        // Get the actual GStreamer error from the bus
        GstBus *bus = gst_element_get_bus(m_pipeline);
        GstMessage *errMsg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (errMsg) {
            GError *err = nullptr;
            gchar *dbg = nullptr;
            gst_message_parse_error(errMsg, &err, &dbg);
            QString detail = QString("%1 (%2)").arg(err->message, dbg ? dbg : "no details");
            qWarning() << "PublishPipeline: GStreamer error:" << detail;
            emit error(detail);
            g_clear_error(&err);
            g_free(dbg);
            gst_message_unref(errMsg);
        } else {
            emit error("Failed to start publish pipeline");
        }
        gst_object_unref(bus);
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
        GstElement *pipeline = m_pipeline;
        m_pipeline = nullptr;
        std::thread([pipeline]() {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
        }).detach();
    }
    m_webrtcbin = nullptr;
    m_videoEncoder = nullptr;
    m_videoPayloader = nullptr;
    m_videoSinkPad = nullptr;
    m_cameraSrc = nullptr;
    m_videoConvert = nullptr;
    m_videoCapsFilter = nullptr;
    m_tee = nullptr;
    m_encQueue = nullptr;
    m_previewQueue = nullptr;
    m_previewConvert = nullptr;
    m_previewAppsink = nullptr;
    m_cameraEnabled = false;
    m_remoteDescSet = false;
    m_pendingCandidates.clear();
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

    m_remoteDescSet = true;
    qDebug() << "PublishPipeline: set remote answer, flushing" << m_pendingCandidates.size() << "queued candidates";
    for (const auto &c : m_pendingCandidates)
        g_signal_emit_by_name(m_webrtcbin, "add-ice-candidate", c.first, c.second.toUtf8().constData());
    m_pendingCandidates.clear();
}

void PublishPipeline::addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid)
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
    if (m_cameraEnabled || !m_pipeline || !m_videoEncoder) return;

    qDebug() << "PublishPipeline: enabling camera, device" << deviceIndex;

    bool testVideo = qEnvironmentVariableIsSet("TALQ_TEST_VIDEO") || qEnvironmentVariableIsSet("TALQ_TEST_AUDIO");

    // Create camera source
    if (testVideo) {
        m_cameraSrc = gst_element_factory_make("videotestsrc", nullptr);
        if (m_cameraSrc)
            g_object_set(m_cameraSrc, "is-live", TRUE, "pattern", 0 /* SMPTE */, nullptr);
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

    if (!m_videoConvert || !m_videoCapsFilter) {
        emit cameraError("Failed to create video elements");
        if (m_cameraSrc) { gst_object_unref(m_cameraSrc); m_cameraSrc = nullptr; }
        if (m_videoConvert) { gst_object_unref(m_videoConvert); m_videoConvert = nullptr; }
        if (m_videoCapsFilter) { gst_object_unref(m_videoCapsFilter); m_videoCapsFilter = nullptr; }
        return;
    }

    if (testVideo) {
        int w = hd1080 ? 1920 : 1280, h = hd1080 ? 1080 : 720;
        GstCaps *caps = gst_caps_from_string(
            QString("video/x-raw,width=%1,height=%2,framerate=30/1").arg(w).arg(h).toUtf8().constData());
        g_object_set(m_videoCapsFilter, "caps", caps, nullptr);
        gst_caps_unref(caps);
    } else {
        GstCaps *caps = gst_caps_from_string("video/x-raw");
        g_object_set(m_videoCapsFilter, "caps", caps, nullptr);
        gst_caps_unref(caps);
    }

    // Tee + preview branch
    m_tee = gst_element_factory_make("tee", "camera-tee");
    m_encQueue = gst_element_factory_make("queue", "enc-queue");
    m_previewQueue = gst_element_factory_make("queue", "preview-queue");
    m_previewConvert = gst_element_factory_make("videoconvert", "preview-convert");
    m_previewAppsink = gst_element_factory_make("appsink", "preview-sink");

    if (m_encQueue) g_object_set(m_encQueue, "leaky", 2, "max-size-buffers", 3, nullptr);
    if (m_previewQueue) g_object_set(m_previewQueue, "leaky", 2, "max-size-buffers", 2, nullptr);

    if (!m_tee || !m_encQueue || !m_previewQueue || !m_previewConvert || !m_previewAppsink) {
        // Clean up partial elements — dummy is still intact
        auto freeEl = [](GstElement *&e) { if (e) { gst_object_unref(e); e = nullptr; } };
        freeEl(m_cameraSrc); freeEl(m_videoConvert); freeEl(m_videoCapsFilter);
        freeEl(m_tee); freeEl(m_encQueue); freeEl(m_previewQueue);
        freeEl(m_previewConvert); freeEl(m_previewAppsink);
        emit cameraError("Failed to create preview elements");
        return;
    }

    GstCaps *previewCaps = gst_caps_from_string("video/x-raw,format=I420");
    g_object_set(m_previewAppsink, "emit-signals", TRUE, "caps", previewCaps,
                 "drop", TRUE, "max-buffers", 1, nullptr);
    gst_caps_unref(previewCaps);
    g_signal_connect(m_previewAppsink, "new-sample", G_CALLBACK(onPreviewSample), this);

    // Step 1: Remove dummy source from the permanent encoder
    GstElement *dummySrc = gst_bin_get_by_name(GST_BIN(m_pipeline), "pub-dummyvideo");
    GstElement *dummyConv = gst_bin_get_by_name(GST_BIN(m_pipeline), "pub-dummyconv");
    if (dummySrc) {
        gst_element_set_state(dummySrc, GST_STATE_NULL);
        gst_element_unlink(dummySrc, dummyConv);
        gst_bin_remove(GST_BIN(m_pipeline), dummySrc);
        gst_object_unref(dummySrc);  // release ref from gst_bin_get_by_name
    }
    if (dummyConv) {
        gst_element_set_state(dummyConv, GST_STATE_NULL);
        gst_element_unlink(dummyConv, m_videoEncoder);
        gst_bin_remove(GST_BIN(m_pipeline), dummyConv);
        gst_object_unref(dummyConv);
    }

    // Step 2: Add camera elements and link to the EXISTING encoder
    gst_bin_add_many(GST_BIN(m_pipeline), m_cameraSrc, m_videoConvert, m_videoCapsFilter,
        m_tee, m_encQueue, m_previewQueue, m_previewConvert, m_previewAppsink, nullptr);

    gst_element_link_many(m_cameraSrc, m_videoConvert, m_videoCapsFilter, m_tee, nullptr);
    gst_element_link_many(m_encQueue, m_videoEncoder, nullptr);  // enc-queue → EXISTING encoder
    gst_element_link_many(m_previewQueue, m_previewConvert, m_previewAppsink, nullptr);

    // Tee src pads
    GstPad *teeSrcEnc = gst_element_request_pad_simple(m_tee, "src_%u");
    GstPad *encQueueSink = gst_element_get_static_pad(m_encQueue, "sink");
    gst_pad_link(teeSrcEnc, encQueueSink);
    gst_object_unref(teeSrcEnc);
    gst_object_unref(encQueueSink);

    GstPad *teeSrcPreview = gst_element_request_pad_simple(m_tee, "src_%u");
    GstPad *previewQueueSink = gst_element_get_static_pad(m_previewQueue, "sink");
    gst_pad_link(teeSrcPreview, previewQueueSink);
    gst_object_unref(teeSrcPreview);
    gst_object_unref(previewQueueSink);

    // Step 3: Sync new elements to PLAYING, reconfigure encoder bitrate
    auto syncEl = [](GstElement *el) { if (el) gst_element_sync_state_with_parent(el); };
    syncEl(m_cameraSrc);
    syncEl(m_videoConvert);
    syncEl(m_videoCapsFilter);
    syncEl(m_tee);
    syncEl(m_encQueue);
    syncEl(m_previewQueue);
    syncEl(m_previewConvert);
    syncEl(m_previewAppsink);

    int bitrate = hd1080 ? 3000000 : 1500000;
    g_object_set(m_videoEncoder, "target-bitrate", bitrate, nullptr);

    m_cameraEnabled = true;
    qDebug() << "PublishPipeline: camera enabled — swapped source on existing encoder, bitrate=" << bitrate;
    // No renegotiation needed — the webrtcbin pad/transceiver/SSRC are all permanent.
    // Only the source feeding the encoder changed, like browser's replaceTrack().
}

void PublishPipeline::disableCamera()
{
    if (!m_cameraEnabled) return;

    qDebug() << "PublishPipeline: disabling camera — swapping back to dummy";

    auto removeElement = [this](GstElement *&el) {
        if (!el) return;
        gst_element_set_state(el, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_pipeline), el);
        el = nullptr;
    };

    // Step 1: Unlink camera chain from the permanent encoder
    if (m_encQueue)
        gst_element_unlink(m_encQueue, m_videoEncoder);
    else if (m_videoCapsFilter)
        gst_element_unlink(m_videoCapsFilter, m_videoEncoder);

    // Step 2: Remove camera elements
    if (m_previewAppsink)
        g_signal_handlers_disconnect_by_data(m_previewAppsink, this);
    removeElement(m_previewAppsink);
    removeElement(m_previewConvert);
    removeElement(m_previewQueue);
    removeElement(m_tee);
    removeElement(m_encQueue);
    removeElement(m_videoCapsFilter);
    removeElement(m_videoConvert);
    removeElement(m_cameraSrc);

    // Step 3: Recreate and link dummy source to the EXISTING encoder
    GstElement *dummySrc = gst_element_factory_make("videotestsrc", "pub-dummyvideo");
    GstElement *dummyConv = gst_element_factory_make("videoconvert", "pub-dummyconv");
    if (dummySrc && dummyConv) {
        g_object_set(dummySrc, "pattern", 2 /* black */, "is-live", TRUE, nullptr);
        gst_bin_add_many(GST_BIN(m_pipeline), dummySrc, dummyConv, nullptr);

        GstCaps *lowCaps = gst_caps_from_string("video/x-raw,width=16,height=16,framerate=1/1");
        gst_element_link(dummySrc, dummyConv);
        gst_element_link_filtered(dummyConv, m_videoEncoder, lowCaps);
        gst_caps_unref(lowCaps);

        gst_element_sync_state_with_parent(dummySrc);
        gst_element_sync_state_with_parent(dummyConv);

        g_object_set(m_videoEncoder, "target-bitrate", 10000, nullptr);
    }

    m_cameraEnabled = false;
    if (m_localVideoProvider)
        m_localVideoProvider->feedFrame(nullptr);
    qDebug() << "PublishPipeline: camera disabled, dummy source restored";
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

    qDebug() << "PublishPipeline: offer created, SDP length=" << sdp.length() << "\n" << sdp;

    // Validate SDP has at least one media line — empty offers get rejected by MCU
    if (!sdp.contains("m=audio") && !sdp.contains("m=video")) {
        qWarning() << "PublishPipeline: offer has no media lines, not sending (audio source may have failed)";
        return;
    }

    // Sync payloader SSRCs with SDP — GStreamer webrtcbin may assign different
    // SSRCs in the payloader vs what it advertises in the SDP offer. Janus drops
    // RTP packets with unknown SSRCs, so we force the payloaders to match.
    {
        // Extract SSRC per media section
        auto extractSsrc = [](const QString &sdp, const QString &mediaType) -> guint32 {
            // Find the m=audio or m=video section, then the first a=ssrc in that section
            int mIdx = sdp.indexOf("m=" + mediaType);
            if (mIdx < 0) return 0;
            // Find next m= line (start of next section) or end
            int nextM = sdp.indexOf("\nm=", mIdx + 1);
            QString section = (nextM > 0) ? sdp.mid(mIdx, nextM - mIdx) : sdp.mid(mIdx);
            QRegularExpression ssrcRe("a=ssrc:(\\d+)\\s");
            auto match = ssrcRe.match(section);
            return match.hasMatch() ? match.captured(1).toUInt() : 0;
        };

        guint32 audioSsrc = extractSsrc(sdp, "audio");
        guint32 videoSsrc = extractSsrc(sdp, "video");

        if (audioSsrc) {
            GstElement *pay = gst_bin_get_by_name(GST_BIN(self->m_pipeline), "pub-rtpopuspay");
            if (pay) {
                g_object_set(pay, "ssrc", audioSsrc, nullptr);
                gst_object_unref(pay);
                qDebug() << "PublishPipeline: synced audio SSRC to" << audioSsrc;
            }
        }
        if (videoSsrc) {
            // Try camera payloader first, then dummy video payloader
            GstElement *pay = gst_bin_get_by_name(GST_BIN(self->m_pipeline), "pub-rtpvp8pay");
            if (!pay)
                pay = gst_bin_get_by_name(GST_BIN(self->m_pipeline), "pub-dummypay");
            if (pay) {
                g_object_set(pay, "ssrc", videoSsrc, nullptr);
                gst_object_unref(pay);
                qDebug() << "PublishPipeline: synced video SSRC to" << videoSsrc;
            }
        }
    }

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
