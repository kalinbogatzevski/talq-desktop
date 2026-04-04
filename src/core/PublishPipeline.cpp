#include "core/PublishPipeline.h"
#include <QDebug>
#include <QPointer>
#include <QRegularExpression>
#include <QUrl>
#include <gst/app/gstappsink.h>
#include <thread>

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

    qDebug() << "PublishPipeline::start() — creating pipeline...";
    m_pipeline = gst_pipeline_new(nullptr);
    qDebug() << "PublishPipeline::start() — pipeline created:" << (void*)m_pipeline;

    qDebug() << "PublishPipeline::start() — creating webrtcbin...";
    m_webrtcbin = gst_element_factory_make("webrtcbin", nullptr);
    qDebug() << "PublishPipeline::start() — webrtcbin created:" << (void*)m_webrtcbin;

    if (!m_pipeline || !m_webrtcbin) {
        emit error("Failed to create publish pipeline elements");
        cleanup();
        return false;
    }

    if (!stunServer.isEmpty()) {
        // Nextcloud returns "stun:host:port" but GStreamer needs "stun://host:port"
        QString gstStun = stunServer;
        if (gstStun.startsWith("stun:") && !gstStun.startsWith("stun://"))
            gstStun.replace("stun:", "stun://");
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
            // Mask credentials in log output
            QString logUrl = gstUrl;
            logUrl.replace(QRegularExpression("://[^@]+@"), "://***@");
            qDebug() << "PublishPipeline: adding TURN server" << logUrl;
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
        audiosrc = gst_element_factory_make("wasapi2src", "pub-audiosrc");
        if (audiosrc) {
            qDebug() << "PublishPipeline: audio source: wasapi2src";
            if (!audioDeviceId.isEmpty())
                g_object_set(audiosrc, "device", audioDeviceId.toUtf8().constData(), nullptr);
        } else {
            audiosrc = gst_element_factory_make("wasapisrc", "pub-audiosrc");
            if (audiosrc) {
                g_object_set(audiosrc, "low-latency", FALSE, nullptr);
                qDebug() << "PublishPipeline: audio source: wasapisrc";
                if (!audioDeviceId.isEmpty())
                    g_object_set(audiosrc, "device", audioDeviceId.toUtf8().constData(), nullptr);
            } else {
                audiosrc = gst_element_factory_make("autoaudiosrc", "pub-audiosrc");
                qDebug() << "PublishPipeline: audio source: autoaudiosrc";
            }
        }
        if (!audioDeviceId.isEmpty())
            qDebug() << "PublishPipeline: using audio input device" << audioDeviceId;
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

    // Force a known SSRC via capsfilter between payloader and webrtcbin.
    // This ensures webrtcbin's internal rtpbin uses the SAME SSRC in both
    // the SDP and on the wire. Without this, rtpbin generates its own SSRC
    // which doesn't match the SDP, causing Janus to drop packets.
    guint32 audioSsrc = g_random_int();
    g_object_set(rtpopuspay, "ssrc", audioSsrc, nullptr);
    GstElement *audioCapsFilter = gst_element_factory_make("capsfilter", "pub-audio-ssrc-filter");
    {
        GstCaps *ssrcCaps = gst_caps_from_string("application/x-rtp");
        gst_caps_set_simple(ssrcCaps, "ssrc", G_TYPE_UINT, audioSsrc, nullptr);
        g_object_set(audioCapsFilter, "caps", ssrcCaps, nullptr);
        gst_caps_unref(ssrcCaps);
    }
    gst_bin_add(GST_BIN(m_pipeline), audioCapsFilter);
    gst_element_link(rtpopuspay, audioCapsFilter);

    GstPad *rtpSrcPad = gst_element_get_static_pad(audioCapsFilter, "src");
    GstPad *sinkPad = gst_element_request_pad_simple(m_webrtcbin, "sink_%u");
    if (gst_pad_link(rtpSrcPad, sinkPad) != GST_PAD_LINK_OK) {
        emit error("Failed to link RTP to webrtcbin");
        gst_object_unref(rtpSrcPad);
        gst_object_unref(sinkPad);
        cleanup();
        return false;
    }
    qDebug() << "PublishPipeline: forced audio SSRC" << audioSsrc << "via capsfilter";

    // Force standard OPUS (not MULTIOPUS) — Janus doesn't support multichannel Opus
    GstWebRTCRTPTransceiver *audioTransceiver = nullptr;
    g_object_get(sinkPad, "transceiver", &audioTransceiver, nullptr);
    if (audioTransceiver) {
        GstCaps *audioCaps = gst_caps_from_string(
            "application/x-rtp,media=audio,encoding-name=OPUS,payload=111,clock-rate=48000,encoding-params=(string)2");
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
    // subscribers get "not sending yet for video" and the call never connects.
    //
    // Architecture (funnel + valve flips): both dummy and camera sources are
    // permanently linked to a funnel element via valves. Only one valve is
    // open at a time. Switching is just a pair of g_object_set("drop") calls.
    // No unlinking, no relinking, no pad swaps.
    m_videoSsrc = g_random_int();

    // --- Shared encoder chain: funnel → vp8enc → rtpvp8pay → videoSsrcFilter ---
    // VP8 with speed optimizations. H264 NVENC needs Janus SDP negotiation work.
    m_funnel          = gst_element_factory_make("funnel", "pub-funnel");
    m_videoEncoder    = gst_element_factory_make("vp8enc", "pub-vp8enc");
    m_videoPayloader  = gst_element_factory_make("rtpvp8pay", "pub-rtpvp8pay");
    m_videoSsrcFilter = gst_element_factory_make("capsfilter", "pub-video-ssrc-filter");
    m_videoParser = nullptr;
    m_useH264 = false;
    if (!m_funnel || !m_videoEncoder || !m_videoPayloader || !m_videoSsrcFilter) {
        emit error("Failed to create shared video encoder chain");
        cleanup();
        return false;
    }
    g_object_set(m_videoEncoder, "deadline", (gint64)1, "target-bitrate", 2000000,
                 "cpu-used", 8, "threads", 4, nullptr);
    g_object_set(m_videoPayloader, "ssrc", m_videoSsrc, nullptr);
    {
        GstCaps *sc = gst_caps_from_string("application/x-rtp");
        gst_caps_set_simple(sc, "ssrc", G_TYPE_UINT, m_videoSsrc, nullptr);
        g_object_set(m_videoSsrcFilter, "caps", sc, nullptr);
        gst_caps_unref(sc);
    }

    // --- Dummy branch: dummySrc → dummyCaps(16x16,1fps) → dummyConv → dummyValve ---
    m_dummySrc   = gst_element_factory_make("videotestsrc", "pub-dummyvideo");
    m_dummyCaps  = gst_element_factory_make("capsfilter", "pub-dummycaps");
    m_dummyConv  = gst_element_factory_make("videoconvert", "pub-dummyconv");
    m_dummyValve = gst_element_factory_make("valve", "pub-dummyvalve");
    if (!m_dummySrc || !m_dummyCaps || !m_dummyConv || !m_dummyValve) {
        emit error("Failed to create dummy video source");
        cleanup();
        return false;
    }
    g_object_set(m_dummySrc, "pattern", 2 /* black */, "is-live", TRUE, nullptr);
    {
        GstCaps *lowCaps = gst_caps_from_string("video/x-raw,width=16,height=16,framerate=1/1");
        g_object_set(m_dummyCaps, "caps", lowCaps, nullptr);
        gst_caps_unref(lowCaps);
    }
    g_object_set(m_dummyValve, "drop", FALSE, nullptr);  // dummy active at startup

    // Add shared chain + dummy branch to pipeline
    gst_bin_add_many(GST_BIN(m_pipeline), m_funnel, m_videoEncoder, m_videoPayloader,
                     m_videoSsrcFilter, m_dummySrc, m_dummyCaps, m_dummyConv,
                     m_dummyValve, nullptr);
    // (m_videoParser only used with H264 NVENC — not active currently)

    // Link dummy branch: dummySrc → dummyCaps → dummyConv → dummyValve
    gst_element_link_many(m_dummySrc, m_dummyCaps, m_dummyConv, m_dummyValve, nullptr);

    // Link dummyValve → funnel (request pad)
    {
        GstPad *dummyValveSrc = gst_element_get_static_pad(m_dummyValve, "src");
        GstPad *funnelSink = gst_element_request_pad_simple(m_funnel, "sink_%u");
        gst_pad_link(dummyValveSrc, funnelSink);
        gst_object_unref(dummyValveSrc);
        gst_object_unref(funnelSink);
    }

    // Shared converter+scaler between funnel and encoder — handles resolution
    // changes when switching between dummy (16x16) and camera (native resolution).
    GstElement *sharedConvert = gst_element_factory_make("videoconvert", "pub-shared-conv");
    GstElement *sharedScale = gst_element_factory_make("videoscale", "pub-shared-scale");
    gst_bin_add_many(GST_BIN(m_pipeline), sharedConvert, sharedScale, nullptr);

    // Link shared chain: funnel → videoconvert → videoscale → vp8enc → rtpvp8pay → ssrcFilter
    gst_element_link_many(m_funnel, sharedConvert, sharedScale,
                          m_videoEncoder, m_videoPayloader,
                          m_videoSsrcFilter, nullptr);

    qDebug() << "PublishPipeline: encoder chain built, codec=" << (m_useH264 ? "H264 NVENC" : "VP8")
             << "SSRC" << m_videoSsrc;

    // --- Link videoSsrcFilter to webrtcbin video sink pad ---
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

    GstPad *ssrcSrc = gst_element_get_static_pad(m_videoSsrcFilter, "src");
    gst_pad_link(ssrcSrc, m_videoSinkPad);
    gst_object_unref(ssrcSrc);

    qDebug() << "PublishPipeline: shared chain linked to webrtcbin video pad (funnel architecture)";

    // Add a data channel — Janus videoroom requires it for publisher registration.
    // The browser's Talk client creates "status" + "simplewebrtc" data channels.
    // Without at least one, Janus doesn't properly initialize the publisher
    // and drops all incoming RTP as "Unknown SSRC".
    {
        GstWebRTCDataChannel *dc = nullptr;
        g_signal_emit_by_name(m_webrtcbin, "create-data-channel", "status", nullptr, &dc);
        if (dc) {
            m_statusDataChannel = dc;  // takes ownership of the ref
            qDebug() << "PublishPipeline: created data channel 'status'";
        }
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

    // Dummy feeds through dummyValve → funnel → shared encoder continuously.
    // 16x16 black @ 1fps VP8 uses negligible bandwidth but maintains RTP
    // sequence number continuity. Camera valve is drop=TRUE (inactive).
    qDebug() << "PublishPipeline: started (send-only), dummy feeding encoder via funnel";

    // Don't enable camera during start() — it blocks the UI thread.
    // CallManager will call enableCamera() after the call connects.
    // For "start with video" calls, CallManager enables camera via toggleCamera.
    Q_UNUSED(withVideo)

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
    qDebug() << "PublishPipeline::cleanup() — begin, pipeline=" << (void*)m_pipeline << "webrtcbin=" << (void*)m_webrtcbin;
    // Disconnect GStreamer signals to prevent callbacks with stale userData
    if (m_webrtcbin) {
        qDebug() << "PublishPipeline::cleanup() — disconnecting signals";
        g_signal_handlers_disconnect_by_data(m_webrtcbin, this);
    }
    if (m_previewAppsink)
        g_signal_handlers_disconnect_by_data(m_previewAppsink, this);

    if (m_statusDataChannel) {
        g_object_unref(m_statusDataChannel);
        m_statusDataChannel = nullptr;
    }

    if (m_pipeline) {
        qDebug() << "PublishPipeline::cleanup() — setting NULL state";
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        qDebug() << "PublishPipeline::cleanup() — unrefing pipeline";
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
        qDebug() << "PublishPipeline::cleanup() — pipeline freed";
    }
    // Pipeline owns all elements — just null out the pointers
    m_webrtcbin = nullptr;
    m_funnel = nullptr;
    m_videoEncoder = nullptr;
    m_videoParser = nullptr;
    m_videoPayloader = nullptr;
    m_videoSsrcFilter = nullptr;
    m_videoSinkPad = nullptr;
    m_videoSsrc = 0;
    m_cameraEnabled = false;
    m_dummySrc = nullptr;
    m_dummyCaps = nullptr;
    m_dummyConv = nullptr;
    m_dummyValve = nullptr;
    m_cameraSrc = nullptr;
    m_videoConvert = nullptr;
    m_videoCapsFilter = nullptr;
    m_tee = nullptr;
    m_encQueue = nullptr;
    m_cameraValve = nullptr;
    m_previewQueue = nullptr;
    m_previewConvert = nullptr;
    m_previewAppsink = nullptr;
    m_remoteDescSet = false;
    m_lvlDbg = 0;
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
    if (!src) return;
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(src), "mute"))
        g_object_set(src, "mute", muted, nullptr);
    else
        qDebug() << "PublishPipeline: audio source does not support mute property";
    gst_object_unref(src);
}

bool PublishPipeline::buildCameraChain(int deviceIndex, bool hd1080)
{
    if (!m_pipeline || !m_funnel) return false;

    qDebug() << "PublishPipeline::buildCameraChain() — device" << deviceIndex << (hd1080 ? "1080p" : "720p");

    // --- Camera source: mfvideosrc preferred, ksvideosrc fallback ---
    m_cameraSrc = gst_element_factory_make("mfvideosrc", nullptr);
    if (m_cameraSrc) {
        qDebug() << "PublishPipeline: camera source: mfvideosrc";
    } else {
        m_cameraSrc = gst_element_factory_make("ksvideosrc", nullptr);
        if (m_cameraSrc)
            qDebug() << "PublishPipeline: camera source: ksvideosrc (fallback)";
    }
    if (!m_cameraSrc) {
        qWarning() << "PublishPipeline: no camera capture plugin available";
        return false;
    }
    g_object_set(m_cameraSrc, "device-index", deviceIndex, nullptr);

    // Create camera branch elements
    m_videoConvert    = gst_element_factory_make("videoconvert", nullptr);
    m_videoCapsFilter = gst_element_factory_make("capsfilter", nullptr);
    m_tee             = gst_element_factory_make("tee", "camera-tee");
    m_encQueue        = gst_element_factory_make("queue", "enc-queue");
    m_cameraValve     = gst_element_factory_make("valve", "camera-valve");
    m_previewQueue    = gst_element_factory_make("queue", "preview-queue");
    m_previewConvert  = gst_element_factory_make("videoconvert", "preview-convert");
    m_previewAppsink  = gst_element_factory_make("appsink", "preview-sink");

    if (!m_videoConvert || !m_videoCapsFilter || !m_tee || !m_encQueue ||
        !m_cameraValve || !m_previewQueue || !m_previewConvert || !m_previewAppsink) {
        qWarning() << "PublishPipeline: failed to create camera branch elements";
        // Clean up any elements that were created (not yet added to pipeline)
        auto freeIf = [](GstElement *&el) { if (el) { gst_object_unref(el); el = nullptr; } };
        freeIf(m_cameraSrc);
        freeIf(m_videoConvert);
        freeIf(m_videoCapsFilter);
        freeIf(m_tee);
        freeIf(m_encQueue);
        freeIf(m_cameraValve);
        freeIf(m_previewQueue);
        freeIf(m_previewConvert);
        freeIf(m_previewAppsink);
        return false;
    }

    // Configure elements
    // Camera capsfilter: auto-negotiate (let camera decide resolution)
    {
        GstCaps *caps = gst_caps_from_string("video/x-raw");
        g_object_set(m_videoCapsFilter, "caps", caps, nullptr);
        gst_caps_unref(caps);
        qDebug() << "PublishPipeline: camera caps: auto-negotiate";
    }
    // Queues: leaky downstream to prevent blocking
    g_object_set(m_encQueue, "leaky", 2 /* downstream */, "max-size-buffers", 3, nullptr);
    g_object_set(m_previewQueue, "leaky", 2, "max-size-buffers", 2, nullptr);
    // Valve starts closed — camera frames don't reach funnel until enableCamera
    g_object_set(m_cameraValve, "drop", TRUE, nullptr);
    // Preview appsink
    {
        GstCaps *previewCaps = gst_caps_from_string("video/x-raw,format=BGRx");
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

    // Add all camera elements to pipeline
    gst_bin_add_many(GST_BIN(m_pipeline),
        m_cameraSrc, m_videoConvert, m_videoCapsFilter, m_tee,
        m_encQueue, m_cameraValve,
        m_previewQueue, m_previewConvert, m_previewAppsink, nullptr);

    // Link capture chain: cameraSrc → videoConvert → videoCapsFilter → tee
    gboolean linked = gst_element_link_many(m_cameraSrc, m_videoConvert, m_videoCapsFilter, m_tee, nullptr);

    // Link encoder branch: tee → encQueue → cameraValve
    linked = linked && gst_element_link_many(m_encQueue, m_cameraValve, nullptr);

    // Link preview branch: previewQueue → previewConvert → previewAppsink
    linked = linked && gst_element_link_many(m_previewQueue, m_previewConvert, m_previewAppsink, nullptr);

    if (!linked) {
        qWarning() << "PublishPipeline: failed to link camera branch chains";
        return false;  // elements are in the pipeline already; cleanup() will free them
    }

    // Link tee src pads to each branch
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
        return false;
    }

    // Link cameraValve → funnel (permanent connection via request pad)
    GstPad *cameraValveSrc = gst_element_get_static_pad(m_cameraValve, "src");
    GstPad *funnelSink = gst_element_request_pad_simple(m_funnel, "sink_%u");
    GstPadLinkReturn r3 = gst_pad_link(cameraValveSrc, funnelSink);
    gst_object_unref(cameraValveSrc);
    gst_object_unref(funnelSink);

    if (r3 != GST_PAD_LINK_OK) {
        qWarning() << "PublishPipeline: cameraValve → funnel link failed:" << r3;
        return false;
    }

    // Camera chain is fully built and permanently linked to funnel.
    // enableCamera() just flips valves and syncs states.
    qDebug() << "PublishPipeline: camera chain built and linked to funnel (valve drop=TRUE)";
    return true;
}

void PublishPipeline::enableCamera(int deviceIndex, bool hd1080)
{
    if (m_cameraEnabled || !m_pipeline) return;

    // 1. Build camera chain if not yet built (links cameraValve → funnel permanently)
    if (!m_cameraSrc) {
        if (!buildCameraChain(deviceIndex, hd1080)) {
            emit cameraError("No camera available");
            return;
        }
    }

    // 2. Sync all camera elements to PLAYING
    gst_element_sync_state_with_parent(m_videoConvert);
    gst_element_sync_state_with_parent(m_videoCapsFilter);
    gst_element_sync_state_with_parent(m_tee);
    gst_element_sync_state_with_parent(m_encQueue);
    gst_element_sync_state_with_parent(m_cameraValve);
    gst_element_sync_state_with_parent(m_previewQueue);
    gst_element_sync_state_with_parent(m_previewConvert);
    gst_element_sync_state_with_parent(m_previewAppsink);
    // Start camera source LAST and async — mfvideosrc COM init blocks ~1s
    gst_element_set_state(m_cameraSrc, GST_STATE_PLAYING);

    // 3. Flip valves — camera frames flow, dummy frames stop
    g_object_set(m_cameraValve, "drop", FALSE, nullptr);
    g_object_set(m_dummyValve, "drop", TRUE, nullptr);

    // 4. Pause dummy source to save CPU (stays linked to funnel)
    gst_element_set_state(m_dummySrc, GST_STATE_PAUSED);

    m_cameraEnabled = true;
    qDebug() << "PublishPipeline: camera enabled (valve flip, no relink)";
}

void PublishPipeline::disableCamera()
{
    if (!m_pipeline || !m_cameraEnabled) return;

    // 1. Flip valves — dummy frames flow, camera frames stop
    if (m_cameraValve) g_object_set(m_cameraValve, "drop", TRUE, nullptr);
    if (m_dummyValve) g_object_set(m_dummyValve, "drop", FALSE, nullptr);

    // 2. Resume dummy source + convert, pause camera (save CPU)
    gst_element_set_state(m_dummySrc, GST_STATE_PLAYING);
    gst_element_set_state(m_dummyConv, GST_STATE_PLAYING);
    if (m_cameraSrc) gst_element_set_state(m_cameraSrc, GST_STATE_PAUSED);

    m_cameraEnabled = false;
    qDebug() << "PublishPipeline: camera disabled (valve flip, no relink, dummy resumed)";
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
                if (++m_lvlDbg <= 2) {
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
            QString dbgStr = dbg ? QString::fromUtf8(dbg) : QString();
            qWarning() << "PublishPipeline ERROR:" << errMsg << dbgStr;
            g_clear_error(&err); g_free(dbg);
            // Camera stays alive — just log the error.
        }
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

void PublishPipeline::sendStatusMessage(const QByteArray &json)
{
    if (!m_statusDataChannel || !m_running) return;
    gst_webrtc_data_channel_send_string(m_statusDataChannel, json.constData());
}

// --- GStreamer callbacks (marshal to Qt thread) ---

void PublishPipeline::onNegotiationNeeded(GstElement *, gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);
    QPointer<PublishPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard]() {
        if (!guard) return;
        auto *self = guard.data();
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
    QPointer<PublishPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, c, ml]() {
        if (!guard) return;
        emit guard->iceCandidateReady(c, ml, QString("0"));
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

    if (!self->m_webrtcbin) {
        gst_webrtc_session_description_free(offer);
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

    // Keep a=ssrc lines in the SDP — Janus requires them to map publisher SSRCs.
    // The capsfilter before webrtcbin forces a consistent SSRC that matches
    // both the SDP and the wire. (Browser keeps a=ssrc lines and it works.)

    QPointer<PublishPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, sdp]() {
        if (!guard) return;
        emit guard->localOfferReady(sdp);
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
    QPointer<PublishPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, stateName]() {
        if (!guard) return;
        qDebug() << "PublishPipeline: ICE ->" << stateName;
        emit guard->iceStateChanged(stateName);
    }, Qt::QueuedConnection);
}

GstFlowReturn PublishPipeline::onPreviewSample(GstAppSink *sink, gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;

    QPointer<PublishPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, sample]() {
        if (guard && guard->m_localVideoProvider)
            guard->m_localVideoProvider->feedFrame(sample);
        gst_sample_unref(sample);
    }, Qt::QueuedConnection);

    return GST_FLOW_OK;
}
