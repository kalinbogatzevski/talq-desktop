#include "core/PeerPipeline.h"
#include "core/VideoEncoderUtil.h"
#include <QDebug>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
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

    // Disable ICE-TCP gathering -- see PublishPipeline.cpp's ice-agent block
    // for the full rationale (Janus runs ICE-TCP disabled cluster-wide;
    // libnice unconditionally rejects TCP-transport remote candidates either
    // way, so gathering them here is guaranteed-useless dead weight).
    {
        GObject *iceAgent = nullptr;
        g_object_get(m_webrtcbin, "ice-agent", &iceAgent, nullptr);
        if (iceAgent) {
            g_object_set(iceAgent, "ice-tcp", FALSE, nullptr);
            g_object_unref(iceAgent);
        }
    }

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
            qDebug() << "PeerPipeline: adding TURN server" << logUrl;
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

    // Optional WebRTC noise suppression (webrtcdsp). Enabled by default; the
    // setting and the plugin's presence in the deployed GStreamer both gate
    // it. Falls back to the plain chain if the plugin is missing so calls
    // still work on installs without libgstwebrtcdsp.
    GstElement *webrtcdsp = nullptr;
    {
        QSettings s("TalQ", "TalQ");
        s.beginGroup("Audio");
        const bool nsEnabled = s.value("noiseSuppression", true).toBool();
        s.endGroup();
        if (nsEnabled) {
            webrtcdsp = gst_element_factory_make("webrtcdsp", "peer-webrtcdsp");
            if (webrtcdsp) {
                g_object_set(webrtcdsp,
                             "echo-cancel", FALSE,
                             "noise-suppression", TRUE,
                             "noise-suppression-level", 2,   // high
                             "high-pass-filter", TRUE,
                             "gain-control", FALSE,
                             "voice-detection", FALSE,
                             nullptr);
                qDebug() << "PeerPipeline: noise suppression ON (webrtcdsp)";
            } else {
                qWarning() << "PeerPipeline: webrtcdsp unavailable; "
                              "continuing without noise suppression";
            }
        }
    }

    // Force a known SSRC via a capsfilter between the payloader and
    // webrtcbin so rtpbin uses the SAME SSRC on the wire as in the SDP.
    // Without this, rtpbin picks its own SSRC, the SDP a=ssrc is wrong,
    // and Janus drops every RTP packet as "Unknown SSRC" (no media
    // forwarded). Mirrors PublishPipeline's proven approach.
    m_audioSsrc = g_random_int();
    g_object_set(rtpopuspay, "ssrc", m_audioSsrc, nullptr);
    m_audioSsrcFilter = gst_element_factory_make("capsfilter", "peer-audio-ssrc-filter");
    {
        GstCaps *sc = gst_caps_from_string("application/x-rtp");
        gst_caps_set_simple(sc, "ssrc", G_TYPE_UINT, m_audioSsrc, nullptr);
        g_object_set(m_audioSsrcFilter, "caps", sc, nullptr);
        gst_caps_unref(sc);
    }

    if (webrtcdsp) {
        gst_bin_add_many(GST_BIN(m_pipeline), audiosrc, capsfilter, audioconvert, audioresample,
                         webrtcdsp, level, opusenc, rtpopuspay, m_audioSsrcFilter, m_webrtcbin, nullptr);
        if (!gst_element_link_many(audiosrc, capsfilter, audioconvert, audioresample,
                                   webrtcdsp, level, opusenc, rtpopuspay, m_audioSsrcFilter, nullptr)) {
            emit error("Failed to link audio capture chain");
            cleanup();
            return false;
        }
    } else {
        gst_bin_add_many(GST_BIN(m_pipeline), audiosrc, capsfilter, audioconvert, audioresample,
                         level, opusenc, rtpopuspay, m_audioSsrcFilter, m_webrtcbin, nullptr);
        if (!gst_element_link_many(audiosrc, capsfilter, audioconvert, audioresample,
                                   level, opusenc, rtpopuspay, m_audioSsrcFilter, nullptr)) {
            emit error("Failed to link audio capture chain");
            cleanup();
            return false;
        }
    }

    GstPad *rtpSrcPad = gst_element_get_static_pad(m_audioSsrcFilter, "src");
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
    g_signal_connect(m_webrtcbin, "notify::ice-gathering-state",
                     G_CALLBACK(onIceGatheringStateChanged), this);
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
        // 0.43.0 — detach + NULL the pipeline on a worker thread, mirroring
        // PublishPipeline::cleanup() (0.40.9). The synchronous set_state(NULL)
        // waits on every pad's stream lock and tears down BOTH a HW encoder
        // (send leg) AND a HW decoder (receive leg) on this single P2P
        // pipeline; on some GPUs that wedges the driver and froze the whole
        // machine when the remote party hung up — the callee teardown ran this
        // synchronously on the Qt main thread (caller hangs up → onParticipant-
        // LeftCall → teardown → stop() → cleanup()). Null the pointer BEFORE
        // the thread starts so re-entrant callers / pollBus see no pipeline;
        // the worker owns the local ref and unrefs after the NULL transition.
        GstElement *pipe = m_pipeline;
        m_pipeline = nullptr;
        qDebug() << "PeerPipeline: cleanup — scheduling pipeline NULL+unref on worker";
        std::thread([pipe]() {
            gst_element_set_state(pipe, GST_STATE_NULL);
            gst_object_unref(pipe);
        }).detach();
    }
    m_webrtcbin = nullptr;
    m_remoteDescSet = false;
    m_audioChainCreated = false;
    m_videoChainCreated = false;
    m_lvlDbg = 0;
    // SSRC filters are children of the pipeline (freed above) — just drop
    // the stale pointers so the next start() recreates them cleanly.
    m_audioSsrcFilter = nullptr;
    m_videoSsrcFilter = nullptr;
    m_audioSsrc = 0;
    m_videoSsrc = 0;
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
    if (!src) return;
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(src), "mute"))
        g_object_set(src, "mute", muted, nullptr);
    else
        qDebug() << "PeerPipeline: audio source does not support mute property";
    gst_object_unref(src);
}

void PeerPipeline::enableCamera(int deviceIndex, bool hd1080,
                                bool forceTestSource, bool triggerOffer)
{
    if (m_cameraEnabled || !m_pipeline) return;

    qDebug() << "PeerPipeline: enabling camera, device" << deviceIndex << (hd1080 ? "1080p" : "720p");

    // Video test source is gated by its OWN env (TALQ_TEST_VIDEO), decoupled
    // from TALQ_TEST_AUDIO: the harness wants synthetic audio (no mic↔speaker
    // feedback — the subscriber plays remote audio on the same box) while
    // still exercising the REAL camera capture+encode path. The real app
    // sets neither env, so it always uses the real camera (unchanged).
    bool testVideo = forceTestSource ||
                     !qEnvironmentVariableIsEmpty("TALQ_TEST_VIDEO");

    if (testVideo) {
        m_cameraSrc = gst_element_factory_make("videotestsrc", nullptr);
        if (m_cameraSrc) {
            // TEST-ONLY source (gated on TALQ_TEST_VIDEO / forceTestSource;
            // production always uses the real mfvideosrc, which is a genuine
            // self-pacing live source and exhibits none of the quirks below).
            // is-live=FALSE: when a videotestsrc is added to an ALREADY-
            // PLAYING pipeline (enableCamera runs after start()), is-live=TRUE
            // wedges on base-time/clock pacing and emits ~0 buffers; is-live=
            // FALSE produces continuously (verified: 1125 buffers / run) so
            // the send+receive chain is actually exercised. It floods faster
            // than 30 fps, but the leaky enc/preview queues bound that.
            g_object_set(m_cameraSrc, "is-live", FALSE, "pattern", 0 /* SMPTE */, nullptr);
            qDebug() << "PeerPipeline: using videotestsrc (TALQ_TEST_VIDEO)";
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

    // 0.41.9-beta — robust capture chain ported from PublishPipeline.
    //   mfvideosrc → camSrcCaps → decodebin →(dynamic pad)→ videoConvert
    //   → videoCapsFilter(I420) → tee → {encoder, preview}
    // decodebin auto-handles MJPEG *or* raw, so we no longer hard-pin a
    // single image/jpeg structure (the cause of "not-negotiated (-4)" on
    // cameras lacking that exact mode).
    m_camSrcCaps   = gst_element_factory_make("capsfilter", "peer-cam-src-caps");
    m_camDecode    = gst_element_factory_make("decodebin",  "peer-cam-decode");
    m_videoConvert = gst_element_factory_make("videoconvert", nullptr);
    m_videoCapsFilter = gst_element_factory_make("capsfilter", nullptr);

    const int w = hd1080 ? 1920 : 1280;
    const int h = hd1080 ? 1080 : 720;
    const int bitrate = hd1080 ? 3000000 : 1500000;

    // TEST-ONLY (TALQ_TEST_BFRAMES): publish a B-frame H.264 stream to
    // reproduce the mfh264enc-on-some-hardware defect end-to-end. NEVER
    // set in production.
    bool useH264 = false;
    QString encDesc;
    if (!qEnvironmentVariableIsEmpty("TALQ_TEST_BFRAMES")) {
        m_videoEncoder = gst_element_factory_make("x264enc", nullptr);
        if (m_videoEncoder) {
            gst_util_set_object_arg(G_OBJECT(m_videoEncoder),
                                    "speed-preset", "veryfast");
            g_object_set(m_videoEncoder, "bframes", 3, "b-adapt", TRUE,
                         "key-int-max", 60, "bitrate", 1500, nullptr);
            m_videoParser = gst_element_factory_make("h264parse", nullptr);
            useH264 = true;
            qWarning() << "PeerPipeline: TEST B-FRAME mode — x264enc bframes=3";
        }
    } else {
        // Shared factory: hardware probe (nvh264enc/qsvh264enc/mfh264enc)
        // → x264enc fallback, with the 0.41.0 psy-tune / qp tuning. Sets
        // the bitrate + rate-control itself and returns an h264parse.
        m_videoEncoder = makeWebrtcVideoEncoder(/*screen=*/false, bitrate,
                                                &useH264, &m_videoParser,
                                                &encDesc);
        if (m_videoEncoder && !useH264) {
            // VP8 path isn't wired for P2P (codec-prefs are H264-pinned
            // below). Shouldn't happen on Windows — x264enc is the
            // guaranteed software H264 fallback. Drop + use openh264enc.
            if (m_videoParser) { gst_object_unref(m_videoParser); m_videoParser = nullptr; }
            gst_object_unref(m_videoEncoder);
            m_videoEncoder = gst_element_factory_make("openh264enc", nullptr);
            if (m_videoEncoder)
                g_object_set(m_videoEncoder, "bitrate", bitrate, nullptr);
            useH264 = true;
            qWarning() << "PeerPipeline: factory returned non-H264 — using openh264enc";
        }
        if (!encDesc.isEmpty())
            qDebug() << "PeerPipeline: video encoder =" << encDesc;
    }
    m_videoPayloader = gst_element_factory_make("rtph264pay", nullptr);

    // #69 — P2P-only H.264 High profile + CABAC. TalQ↔TalQ P2P decodes via
    // decodebin (which reads the profile from the in-band SPS), and our
    // codec-preferences pin no profile-level-id, so we can run High profile
    // here for ~10-15% better quality-per-bitrate WITHOUT the browser/Janus
    // interop constraint that keeps the MCU path on Constrained Baseline.
    // A downstream capsfilter forces High (verified: x264enc/nv/qsv/mf all
    // advertise High in their src caps); CABAC is the encoders' default and
    // High profile requires it. Gated to the factory H.264 encoders — the
    // openh264enc fallback is Baseline-only and leaves m_videoParser null, so
    // it's skipped. TALQ_P2P_HIGH_PROFILE=0 forces Baseline if it ever
    // misbehaves on some hardware.
    if (useH264 && m_videoParser
        && qEnvironmentVariable("TALQ_P2P_HIGH_PROFILE", QStringLiteral("1"))
               != QLatin1String("0")) {
        m_videoProfileCaps = gst_element_factory_make("capsfilter", nullptr);
        if (m_videoProfileCaps) {
            GstCaps *hp = gst_caps_from_string("video/x-h264,profile=high");
            g_object_set(m_videoProfileCaps, "caps", hp, nullptr);
            gst_caps_unref(hp);
            qInfo() << "PeerPipeline: P2P H.264 High profile + CABAC enabled";
        }
    }

    if (!m_camSrcCaps || !m_camDecode || !m_videoConvert || !m_videoCapsFilter
        || !m_videoEncoder || !m_videoPayloader) {
        emit cameraError("Failed to create video encoding elements");
        auto freeIf = [](GstElement *&el){ if (el){ gst_object_unref(el); el=nullptr; } };
        freeIf(m_cameraSrc); freeIf(m_camSrcCaps); freeIf(m_camDecode);
        freeIf(m_videoConvert); freeIf(m_videoCapsFilter);
        freeIf(m_videoEncoder); freeIf(m_videoParser); freeIf(m_videoPayloader);
        freeIf(m_videoProfileCaps);
        return;
    }

    // Pin the video wire SSRC to the SDP a=ssrc (mirrors PublishPipeline):
    // a capsfilter between the payloader and webrtcbin forces rtpbin to
    // keep this SSRC, otherwise the peer drops all video RTP as "Unknown
    // SSRC" and renders nothing.
    m_videoSsrc = g_random_int();
    // PT 97 must match the video codec-preferences below (audio OPUS = 96).
    g_object_set(m_videoPayloader, "ssrc", m_videoSsrc, "pt", 97, nullptr);
    if (useH264) {
        // Repeat SPS/PPS before every IDR so a peer that starts decoding
        // mid-stream (or after packet loss) recovers without waiting for
        // the next natural keyframe.
        if (m_videoParser)
            g_object_set(m_videoParser, "config-interval", -1, nullptr);
        g_object_set(m_videoPayloader, "config-interval", -1,
                     "aggregate-mode", 1 /* zero-latency */, nullptr);
    }
    m_videoSsrcFilter = gst_element_factory_make("capsfilter", "peer-video-ssrc-filter");
    if (m_videoSsrcFilter) {
        GstCaps *sc = gst_caps_from_string("application/x-rtp");
        gst_caps_set_simple(sc, "ssrc", G_TYPE_UINT, m_videoSsrc, nullptr);
        g_object_set(m_videoSsrcFilter, "caps", sc, nullptr);
        gst_caps_unref(sc);
    }

    // Camera SOURCE caps (on mfvideosrc, BEFORE decodebin).
    {
        GstCaps *caps = nullptr;
        if (testVideo) {
            // videotestsrc emits raw — pin a fixed mode.
            QString capsStr = QStringLiteral(
                "video/x-raw,width=%1,height=%2,framerate=30/1").arg(w).arg(h);
            caps = gst_caps_from_string(capsStr.toUtf8().constData());
        } else {
            // User-forced exact mode wins (Settings → Camera quality);
            // else a PERMISSIVE menu. MJPEG first (real 30 fps over USB),
            // raw fallbacks after. A single fixed structure is what made
            // mfvideosrc fail to negotiate on cameras lacking that mode.
            QSettings s("TalQ", "TalQ");
            s.beginGroup("Video");
            const QString srcCapsStr = s.value("cameraSrcCaps").toString();
            s.endGroup();
            if (!srcCapsStr.isEmpty())
                caps = gst_caps_from_string(srcCapsStr.toUtf8().constData());
            if (!caps) {
                const QString menu = QStringLiteral(
                    "image/jpeg,width=(int)[1,%1],height=(int)[1,%2],framerate=(fraction)30/1;"
                    "video/x-raw,width=(int)[1,%1],height=(int)[1,%2],framerate=(fraction)30/1;"
                    "image/jpeg,width=(int)[1,%1],height=(int)[1,%2],framerate=(fraction)[15/1,60/1];"
                    "video/x-raw,width=(int)[1,%1],height=(int)[1,%2],framerate=(fraction)[15/1,60/1];"
                    "video/x-raw,width=(int)[1,%1],height=(int)[1,%2],framerate=(fraction)[1/1,60/1]")
                    .arg(w).arg(h);
                caps = gst_caps_from_string(menu.toUtf8().constData());
            }
        }
        g_object_set(m_camSrcCaps, "caps", caps, nullptr);
        gst_caps_unref(caps);
    }
    // Post-decode caps: guarantee RAW only — do NOT pin a format. The
    // encoder negotiates its native format (e.g. NV12 for qsvh264enc)
    // back through videoConvert; the preview branch has its own convert.
    // Over-pinning format=I420 here both starved a NV12-only encoder and
    // narrowed the tee src caps enough to trip GST_PAD_LINK_NOFORMAT.
    {
        GstCaps *raw = gst_caps_from_string("video/x-raw");
        g_object_set(m_videoCapsFilter, "caps", raw, nullptr);
        gst_caps_unref(raw);
    }
    m_jpegDec = nullptr;   // legacy element no longer used

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
            // #63 — async=FALSE. This appsink is bin-added to an ALREADY-
            // PLAYING pipeline; with the BaseSink default (async=TRUE) it waits
            // for a preroll buffer in PAUSED, and a live mfvideosrc (camera
            // warmup latency) may never deliver one — so "new-sample" never
            // fires, onPreviewSample never runs, and the P2P self-view PiP
            // stays black on both ends. The proven MCU PublishPipeline sets
            // async=FALSE here for exactly this reason (0.40.13). videotestsrc
            // prerolls instantly, which is why the harness never caught it.
            "async", FALSE,
            nullptr);
        gst_caps_unref(previewCaps);
        g_signal_connect(m_previewAppsink, "new-sample",
            G_CALLBACK(onPreviewSample), this);
    }

    // 0.41.9-beta — decodebin's src pad is dynamic (appears once it sees
    // data); link it to videoConvert's sink when it shows up, only if
    // it's a video pad. Mirrors PublishPipeline's pad-added handler.
    // Only needed for the REAL camera (which may be MJPEG) — the synthetic
    // raw path (testVideo) bypasses decodebin entirely below.
    if (!testVideo) {
        g_signal_connect(m_camDecode, "pad-added",
            G_CALLBACK(+[](GstElement *, GstPad *pad, gpointer ud) {
                auto *conv = static_cast<GstElement *>(ud);
                GstPad *sink = gst_element_get_static_pad(conv, "sink");
                if (sink) {
                    if (!gst_pad_is_linked(sink)) {
                        GstCaps *c = gst_pad_get_current_caps(pad);
                        if (!c) c = gst_pad_query_caps(pad, nullptr);
                        const GstStructure *s = c ? gst_caps_get_structure(c, 0) : nullptr;
                        const gchar *n = s ? gst_structure_get_name(s) : nullptr;
                        if (n && g_str_has_prefix(n, "video"))
                            gst_pad_link(pad, sink);
                        if (c) gst_caps_unref(c);
                    }
                    gst_object_unref(sink);
                }
            }), m_videoConvert);
    }

    // encoder → [profileCaps] → [h264parse] → payloader. The factory hands
    // back an h264parse for hardware/x264 encoders; openh264enc fallback
    // returns none. m_videoProfileCaps (#69) forces H264 High profile on the
    // P2P path and is null on the MCU/openh264enc/baseline paths.
    auto linkEncoderTail = [this]() -> gboolean {
        GstElement *tail = m_videoEncoder;
        if (m_videoProfileCaps) {
            if (!gst_element_link(tail, m_videoProfileCaps)) return FALSE;
            tail = m_videoProfileCaps;
        }
        if (m_videoParser) {
            if (!gst_element_link(tail, m_videoParser)) return FALSE;
            tail = m_videoParser;
        }
        return gst_element_link(tail, m_videoPayloader);
    };
    auto step = [](const char *what, gboolean ok) -> gboolean {
        if (!ok) qWarning() << "PeerPipeline: link FAILED at" << what;
        return ok;
    };

    gboolean linked;
    if (m_tee) {
        // testVideo (videotestsrc, raw): KEEP camSrcCaps (it pins
        // video/x-raw,WxH,framerate=30/1 — without it the is-live source has
        // no framerate to pace against and emits a single frame then stalls),
        // but SKIP decodebin (m_camDecode errors on already-raw input —
        // "Internal data stream error"). videotestsrc → camSrcCaps →
        // videoConvert. The unused m_camDecode stays un-added; disableCamera's
        // parent-guard unrefs it. The real camera keeps the decodebin path.
        if (testVideo) {
            gst_bin_add_many(GST_BIN(m_pipeline),
                m_cameraSrc, m_camSrcCaps, m_videoConvert, m_videoCapsFilter,
                m_tee, m_encQueue, m_videoEncoder, m_videoPayloader,
                m_previewQueue, m_previewConvert, m_previewAppsink, nullptr);
            if (m_videoParser) gst_bin_add(GST_BIN(m_pipeline), m_videoParser);
            if (m_videoProfileCaps) gst_bin_add(GST_BIN(m_pipeline), m_videoProfileCaps);
            linked = step("cameraSrc→camSrcCaps→videoConvert (raw)",
                          gst_element_link_many(m_cameraSrc, m_camSrcCaps, m_videoConvert, nullptr));
        } else {
            gst_bin_add_many(GST_BIN(m_pipeline),
                m_cameraSrc, m_camSrcCaps, m_camDecode,
                m_videoConvert, m_videoCapsFilter,
                m_tee, m_encQueue, m_videoEncoder, m_videoPayloader,
                m_previewQueue, m_previewConvert, m_previewAppsink, nullptr);
            if (m_videoParser) gst_bin_add(GST_BIN(m_pipeline), m_videoParser);
            if (m_videoProfileCaps) gst_bin_add(GST_BIN(m_pipeline), m_videoProfileCaps);
            linked = step("cameraSrc→camSrcCaps→decodebin",
                          gst_element_link_many(m_cameraSrc, m_camSrcCaps, m_camDecode, nullptr));
        }
        linked = linked && step("videoConvert→capsFilter→tee",
                      gst_element_link_many(m_videoConvert, m_videoCapsFilter, m_tee, nullptr));
        linked = linked && step("encQueue→encoder",
                      gst_element_link(m_encQueue, m_videoEncoder));
        linked = linked && step("encoder→[parser]→payloader", linkEncoderTail());
        linked = linked && step("previewQueue→convert→appsink",
                      gst_element_link_many(m_previewQueue, m_previewConvert, m_previewAppsink, nullptr));

        if (linked) {
            // GST_PAD_LINK_CHECK_NOTHING: the tee sink's caps are not yet
            // resolved at link time because decodebin connects videoConvert
            // dynamically (pad-added). Deferring the caps check to runtime
            // negotiation is the idiomatic pattern here — without it the
            // tee→queue link fails GST_PAD_LINK_NOFORMAT (-4) even though
            // negotiation succeeds once frames flow.
            GstPad *teeSrcEnc = gst_element_request_pad_simple(m_tee, "src_%u");
            GstPad *encQueueSink = gst_element_get_static_pad(m_encQueue, "sink");
            GstPadLinkReturn r1 = gst_pad_link_full(teeSrcEnc, encQueueSink,
                                                    GST_PAD_LINK_CHECK_NOTHING);
            gst_object_unref(teeSrcEnc);
            gst_object_unref(encQueueSink);

            GstPad *teeSrcPreview = gst_element_request_pad_simple(m_tee, "src_%u");
            GstPad *previewQueueSink = gst_element_get_static_pad(m_previewQueue, "sink");
            GstPadLinkReturn r2 = gst_pad_link_full(teeSrcPreview, previewQueueSink,
                                                    GST_PAD_LINK_CHECK_NOTHING);
            gst_object_unref(teeSrcPreview);
            gst_object_unref(previewQueueSink);

            if (r1 != GST_PAD_LINK_OK || r2 != GST_PAD_LINK_OK) {
                qWarning() << "PeerPipeline: tee pad link failed:" << r1 << r2;
                linked = false;
            }
        }
    } else {
        // No-preview fallback: direct cameraSrc → … → encoder → payloader.
        gst_bin_add_many(GST_BIN(m_pipeline),
            m_cameraSrc, m_camSrcCaps, m_camDecode,
            m_videoConvert, m_videoCapsFilter,
            m_videoEncoder, m_videoPayloader, nullptr);
        if (m_videoParser) gst_bin_add(GST_BIN(m_pipeline), m_videoParser);
        if (m_videoProfileCaps) gst_bin_add(GST_BIN(m_pipeline), m_videoProfileCaps);
        linked = gst_element_link_many(m_cameraSrc, m_camSrcCaps, m_camDecode, nullptr);
        linked = linked && gst_element_link_many(m_videoConvert, m_videoCapsFilter, m_videoEncoder, nullptr);
        linked = linked && linkEncoderTail();
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
        // Video MUST use a different RTP payload type than audio (OPUS is
        // PT 96). Audio and video share one BUNDLE m-section group, so a
        // duplicate PT makes Janus keep the first mapping (OPUS/48000) and
        // skip the video rtpmap ("Payload type 96 already mapped..."),
        // dropping all video. Use 97 for H264.
        GstCaps *videoCaps = gst_caps_from_string(
            "application/x-rtp,media=video,encoding-name=H264,clock-rate=90000,payload=97");
        // 0.41.9 — SENDRECV in P2P so the one video m-line carries both
        // directions (each peer sends its camera + receives the other's).
        // SENDONLY only for the MCU-publisher path.
        const auto dir = m_videoSendRecv
            ? GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV
            : GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY;
        g_object_set(transceiver,
                     "direction", dir,
                     "codec-preferences", videoCaps,
                     nullptr);
        gst_caps_unref(videoCaps);
        qDebug() << "PeerPipeline: configured video transceiver ("
                 << (m_videoSendRecv ? "sendrecv" : "sendonly") << ", H264)";
        gst_object_unref(transceiver);
    } else {
        qWarning() << "PeerPipeline: could not get transceiver from video pad";
    }

    // Insert the SSRC capsfilter between payloader and webrtcbin so the
    // wire SSRC matches the SDP a=ssrc (see comment at filter creation).
    gst_bin_add(GST_BIN(m_pipeline), m_videoSsrcFilter);
    if (!gst_element_link(m_videoPayloader, m_videoSsrcFilter)) {
        qWarning() << "PeerPipeline: failed to link video payloader -> ssrc filter";
        emit cameraError("Failed to connect video to WebRTC");
        disableCamera();
        return;
    }
    GstPad *payloaderSrc = gst_element_get_static_pad(m_videoSsrcFilter, "src");
    GstPadLinkReturn ret = gst_pad_link(payloaderSrc, m_videoSinkPad);
    gst_object_unref(payloaderSrc);

    if (ret != GST_PAD_LINK_OK) {
        qWarning() << "PeerPipeline: video pad link failed:" << ret;
        emit cameraError("Failed to connect video to WebRTC");
        disableCamera();
        return;
    }

    // 0.41.x — bring the chain to PLAYING DOWNSTREAM-FIRST, source LAST.
    // We add these elements to an already-PLAYING pipeline; if the source
    // reaches PLAYING and starts pushing before the tee/queues/encoder have
    // finished their own state change, those not-yet-ready sink pads return
    // GST_FLOW_FLUSHING and gst_base_src_loop PAUSES the source task for
    // good — i.e. the camera emits ~1 frame then dies. A slow hardware
    // mfvideosrc happened to survive (warmup outlasts the downstream state
    // change); a synthetic videotestsrc pushes instantly and tripped it
    // every time. Syncing sink→source guarantees every consumer is PLAYING
    // before the producer runs.
    gst_element_sync_state_with_parent(m_videoSsrcFilter);
    gst_element_sync_state_with_parent(m_videoPayloader);
    if (m_videoParser) gst_element_sync_state_with_parent(m_videoParser);
    if (m_videoProfileCaps) gst_element_sync_state_with_parent(m_videoProfileCaps);
    gst_element_sync_state_with_parent(m_videoEncoder);
    if (m_tee) {
        gst_element_sync_state_with_parent(m_previewAppsink);
        gst_element_sync_state_with_parent(m_previewConvert);
        gst_element_sync_state_with_parent(m_previewQueue);
        gst_element_sync_state_with_parent(m_encQueue);
        gst_element_sync_state_with_parent(m_tee);
    }
    gst_element_sync_state_with_parent(m_videoCapsFilter);
    gst_element_sync_state_with_parent(m_videoConvert);
    gst_element_sync_state_with_parent(m_camDecode);
    gst_element_sync_state_with_parent(m_camSrcCaps);
    gst_element_sync_state_with_parent(m_cameraSrc);

    // #63 — force the cascade to PLAYING. enableCamera runs on an already-
    // PLAYING pipeline, but start()'s transition can return NO_PREROLL/ASYNC
    // with a live capture source, leaving the pipeline PAUSED; the children
    // synced above then inherit PAUSED, and a PAUSED appsink emits
    // "new-preroll" (not "new-sample"), so onPreviewSample never fires and the
    // self-view stays black. Re-issue PLAYING on the pipeline (idempotent when
    // already PLAYING; GStreamer transitions children sink→source internally)
    // and explicitly start the live capture source LAST — mirrors the proven
    // MCU PublishPipeline. This advances element states only; it does NOT
    // trigger an offer (negotiation-needed is intentionally not connected —
    // CallManager drives offers explicitly), so the deferred-offer contract
    // (triggerOffer=false) is preserved.
    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    gst_element_set_state(m_cameraSrc, GST_STATE_PLAYING);

    m_cameraEnabled = true;
    qDebug() << "PeerPipeline: camera enabled successfully";

    // Renegotiation after adding the video track — but only when we're
    // the offering side. The answerer (and the start-time auto-enable,
    // whose offer is driven by participant-joined) passes triggerOffer
    // =false to avoid offer glare in true P2P.
    if (triggerOffer) {
        qDebug() << "PeerPipeline: triggering renegotiation for video";
        createOffer();
    } else {
        qDebug() << "PeerPipeline: video chain attached (offer deferred)";
    }
}

void PeerPipeline::disableCamera()
{
    if (!m_pipeline) return;
    if (!m_cameraEnabled && !m_cameraSrc) return;

    qDebug() << "PeerPipeline: disabling camera";

    // 0.41.9 — guard against gst_bin_remove on an element that was created
    // but never added to the bin (a link-failure early-return can leave
    // e.g. m_videoSsrcFilter floating). Removing a non-child emits a
    // GLib-CRITICAL and leaks the float ref; unref it directly instead.
    auto removeElement = [this](GstElement *&el) {
        if (!el) return;
        gst_element_set_state(el, GST_STATE_NULL);
        if (GST_OBJECT_PARENT(el) == GST_OBJECT(m_pipeline))
            gst_bin_remove(GST_BIN(m_pipeline), el);
        else
            gst_object_unref(el);
        el = nullptr;
    };
    // 0.41.x — STOP THE SOURCE FIRST. Tearing the chain down sink→source
    // while the source is still actively pushing buffers deadlocks: the
    // source streaming thread blocks pushing into a downstream element we've
    // already set to NULL, while set_state(NULL) on that element waits for
    // the very same thread to finish. Driving the source to NULL up front
    // halts production cleanly, so the rest of the chain tears down with no
    // data in flight. (mfvideosrc tolerated the old order by luck — its
    // hardware-stop raced ahead; videotestsrc, pushing flat out, hung every
    // time.) NULL also releases mfvideosrc's device handle for the next call.
    if (m_cameraSrc)
        gst_element_set_state(m_cameraSrc, GST_STATE_NULL);

    // 0.41.9 — drop the decodebin pad-added handler (captures m_videoConvert
    // as raw user-data) BEFORE tearing the chain down, so a late pad-added
    // can't hand a removed element to gst_element_get_static_pad.
    if (m_camDecode)
        g_signal_handlers_disconnect_by_data(m_camDecode, m_videoConvert);

    GstElement *videoTail = m_videoSsrcFilter ? m_videoSsrcFilter : m_videoPayloader;
    if (videoTail && m_videoSinkPad) {
        GstPad *src = gst_element_get_static_pad(videoTail, "src");
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
    removeElement(m_videoSsrcFilter);
    removeElement(m_videoPayloader);
    removeElement(m_videoParser);     // 0.41.9 — h264parse (factory path)
    removeElement(m_videoProfileCaps); // #69 — High-profile capsfilter (P2P)
    removeElement(m_videoEncoder);
    removeElement(m_videoCapsFilter);
    removeElement(m_jpegDec);          // legacy, normally null
    removeElement(m_videoConvert);
    removeElement(m_camDecode);        // 0.41.9 — decodebin
    removeElement(m_camSrcCaps);       // 0.41.9 — source capsfilter
    // Release the camera device LAST + to NULL so mfvideosrc's
    // IMFMediaSource handle is freed before the next call's pipeline
    // tries to open the same device (the "camera wedged after a failed
    // P2P attempt" symptom). removeElement already drives it to NULL.
    removeElement(m_cameraSrc);

    m_cameraEnabled = false;
    qDebug() << "PeerPipeline: camera disabled (teardown of capture chain complete)";
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
                if (++m_lvlDbg <= 2) {
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
            // webrtcbin's internal transport (nicesrc/dtlssrtp/rtpbin) can
            // post a transient "Internal data stream error / not-linked"
            // while the RECEIVE flow is (re)wired during the SENDRECV
            // renegotiation handshake. That is a transport race, not a
            // capture failure — tearing the camera down on it kills a call
            // whose send path is healthy and whose media is already flowing.
            // Only errors from our OWN capture/encode elements (which live
            // OUTSIDE webrtcbin) are fatal to the camera.
            GstObject *src = GST_MESSAGE_SRC(msg);
            const bool fromWebrtc = src && m_webrtcbin &&
                (src == GST_OBJECT(m_webrtcbin) ||
                 gst_object_has_as_ancestor(src, GST_OBJECT(m_webrtcbin)));
            qWarning() << "PeerPipeline ERROR:" << errMsg
                       << (fromWebrtc ? "[webrtcbin-internal — non-fatal]" : "")
                       << (dbg ? dbg : "");
            g_clear_error(&err); g_free(dbg);
            if (!fromWebrtc && m_cameraEnabled) {
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
        if (depay) gst_object_unref(depay);
        if (dec) gst_object_unref(dec);
        if (convert) gst_object_unref(convert);
        if (resample) gst_object_unref(resample);
        if (sink) gst_object_unref(sink);
        return;
    }

    // 0.41.0-beta — receive-side loudness chain. Vanilla opusdec→sink ran
    // playback at whatever level Opus encoded, with no AGC and no limiter.
    // tgcalls / libwebrtc run AudioProcessing (AGC2 + soft-limiter) on
    // playout; we get most of the win with a GStreamer dynamics +
    // constant-gain + safety-limiter triplet between opusdec and the
    // resampler. The QSettings keys mirror Settings → Audio sliders
    // (added later in the same release cycle).
    QSettings audSettings("TalQ", "TalQ");
    audSettings.beginGroup("Audio");
    const double playbackGain = qBound(0.0,
        audSettings.value("playbackGain", 1.8).toDouble(), 4.0);
    audSettings.endGroup();
    GstElement *dyn   = gst_element_factory_make("audiodynamic", nullptr);
    GstElement *gain  = gst_element_factory_make("volume",       nullptr);
    GstElement *limit = gst_element_factory_make("rglimiter",    nullptr);
    if (dyn) {
        gst_util_set_object_arg(G_OBJECT(dyn), "mode",            "compressor");
        gst_util_set_object_arg(G_OBJECT(dyn), "characteristics", "soft-knee");
        g_object_set(dyn, "threshold", 0.25, "ratio", 0.4, nullptr);
    }
    if (gain) g_object_set(gain, "volume", playbackGain, nullptr);

    if (dyn && gain && limit) {
        gst_bin_add_many(GST_BIN(m_pipeline), depay, dec, convert, resample,
                         dyn, gain, limit, sink, nullptr);
        gst_element_link_many(depay, dec, dyn, gain, limit,
                              convert, resample, sink, nullptr);
    } else {
        // Either gst-plugins-good is incomplete or one element refused;
        // keep audio alive without the loudness boost.
        if (dyn)   gst_object_unref(dyn);
        if (gain)  gst_object_unref(gain);
        if (limit) gst_object_unref(limit);
        gst_bin_add_many(GST_BIN(m_pipeline), depay, dec, convert, resample, sink, nullptr);
        gst_element_link_many(depay, dec, convert, resample, sink, nullptr);
    }
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
    if (encoding && g_ascii_strcasecmp(encoding, "VP8") == 0)
        depay = gst_element_factory_make("rtpvp8depay", nullptr);
    else
        depay = gst_element_factory_make("rtph264depay", nullptr);

    // 0.41.x — decode via decodebin, which auto-selects the best available
    // decoder (d3d11/qsv hardware → software) AND inserts whatever caps/
    // memory-download steps that decoder needs. This mirrors the MCU receive
    // path (webrtcsrc → decodebin), which delivers frames reliably (verified:
    // 180 frames). The previous explicit depay→openh264dec→appsink chain
    // negotiated and linked "successfully" yet produced ZERO decoded frames
    // P2P — openh264dec alone couldn't drive the live RTP stream into the
    // appsink. Let decodebin own decoder selection.
    GstElement *decode = gst_element_factory_make("decodebin", nullptr);
    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *appsink = gst_element_factory_make("appsink", nullptr);

    if (!depay || !decode || !convert || !appsink) {
        qWarning() << "PeerPipeline: failed to create video receive elements";
        if (depay) gst_object_unref(depay);
        if (decode) gst_object_unref(decode);
        if (convert) gst_object_unref(convert);
        if (appsink) gst_object_unref(appsink);
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

    // decodebin's src pad is dynamic (appears once it identifies the stream);
    // link it to videoconvert when it shows up, video pads only.
    g_signal_connect(decode, "pad-added",
        G_CALLBACK(+[](GstElement *, GstPad *p, gpointer ud) {
            auto *cv = static_cast<GstElement *>(ud);
            GstPad *sink = gst_element_get_static_pad(cv, "sink");
            if (sink) {
                if (!gst_pad_is_linked(sink)) {
                    GstCaps *c = gst_pad_get_current_caps(p);
                    if (!c) c = gst_pad_query_caps(p, nullptr);
                    const GstStructure *s = c ? gst_caps_get_structure(c, 0) : nullptr;
                    const gchar *n = s ? gst_structure_get_name(s) : nullptr;
                    if (n && g_str_has_prefix(n, "video"))
                        gst_pad_link(p, sink);
                    if (c) gst_caps_unref(c);
                }
                gst_object_unref(sink);
            }
        }), convert);

    gst_bin_add_many(GST_BIN(m_pipeline), depay, decode, convert, appsink, nullptr);
    // depay → decodebin and convert → appsink are static; decodebin → convert
    // joins dynamically (pad-added above).
    gst_element_link(depay, decode);
    gst_element_link(convert, appsink);

    // Sink→source so every consumer is PLAYING before data flows.
    gst_element_sync_state_with_parent(appsink);
    gst_element_sync_state_with_parent(convert);
    gst_element_sync_state_with_parent(decode);
    gst_element_sync_state_with_parent(depay);

    GstPad *sinkPad = gst_element_get_static_pad(depay, "sink");
    GstPadLinkReturn ret = gst_pad_link(pad, sinkPad);
    gst_object_unref(sinkPad);

    if (ret != GST_PAD_LINK_OK)
        qWarning() << "PeerPipeline: video receive pad link failed:" << ret;
    else
        qDebug() << "PeerPipeline: video receive chain linked successfully (decodebin)";
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

    qDebug() << "PeerPipeline: offer created, SDP length=" << sdp.length();

    // Keep a=ssrc/a=ssrc-group lines: the SSRC capsfilters pin the wire
    // SSRC to the values webrtcbin advertised, so the SDP is now truthful.
    // Janus requires a=ssrc to map the publisher's RTP; stripping it made
    // Janus drop every packet as "Unknown SSRC". (Mirrors PublishPipeline.)
    QPointer<PeerPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, sdp]() {
        if (!guard) return;
        emit guard->localOfferReady(sdp);
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

    if (!self->m_webrtcbin) {
        gst_webrtc_session_description_free(answer);
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

    // Keep a=ssrc lines (the SSRC capsfilters make them truthful) so the
    // remote end can map our RTP. Mirrors the offer path / PublishPipeline.
    qDebug() << "PeerPipeline: answer SDP:\n" << sdp.left(2000);

    QPointer<PeerPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, sdp]() {
        if (!guard) return;
        emit guard->localAnswerReady(sdp);
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

    // 0.41.x-beta — link the receive chain SYNCHRONOUSLY, here on the
    // streaming thread, BEFORE returning from pad-added. webrtcbin begins
    // pushing RTP into this src pad the instant we return; deferring the
    // link to the Qt event loop (the old Qt::QueuedConnection path) left the
    // pad unlinked for the first buffers → GST_FLOW_NOT_LINKED, which
    // propagates up to the shared bundled-transport nicesrc and PERMANENTLY
    // stops streaming — taking the send direction down with it (the SENDRECV
    // transport is bundled). That race was deterministic (the queued lambda
    // always ran after pad-added returned), which is why P2P video never
    // delivered a frame. gst_bin_add/link/sync_state are all thread-safe;
    // remote frames still reach the UI because the appsink "new-sample"
    // handler marshals each sample to the Qt thread via invokeMethod.
    if (isAudio && !self->m_audioChainCreated) {
        self->createAudioReceiveChain(pad);
        self->m_audioChainCreated = true;
    } else if (isVideo && !self->m_videoChainCreated) {
        self->createVideoReceiveChain(pad, encodingCopy.constData());
        self->m_videoChainCreated = true;
    }
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

void PeerPipeline::onIceGatheringStateChanged(GObject *obj, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<PeerPipeline *>(userData);
    GstWebRTCICEGatheringState state;
    g_object_get(obj, "ice-gathering-state", &state, nullptr);
    if (state != GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) return;
    QPointer<PeerPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard]() {
        if (!guard) return;
        qDebug() << "PeerPipeline: ICE gathering complete";
        emit guard->iceGatheringComplete();
    }, Qt::QueuedConnection);
}

GstFlowReturn PeerPipeline::onPreviewSample(GstAppSink *sink, gpointer userData)
{
    auto *self = static_cast<PeerPipeline *>(userData);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;

    QPointer<PeerPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, sample]() {
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
    QMetaObject::invokeMethod(self, [guard, sample]() {
        if (guard && guard->m_remoteVideoProvider)
            guard->m_remoteVideoProvider->feedFrame(sample);
        gst_sample_unref(sample);
    }, Qt::QueuedConnection);

    return GST_FLOW_OK;
}
