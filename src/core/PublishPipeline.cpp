#include "core/PublishPipeline.h"
#include <QDebug>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QUrl>
#include <gst/app/gstappsink.h>
#include <gst/rtp/rtp.h>
#include <gst/sdp/sdp.h>
#include <thread>

#include "core/VideoEncoderUtil.h"

PublishPipeline::PublishPipeline(QObject *parent)
    : QObject(parent)
{
    m_localVideoProvider = new VideoFrameProvider(this);

    // No-frames watchdog for self-healing forced camera picks: if Settings
    // pinned an exact caps that the actually-opened mfvideosrc instance
    // can't deliver, no preview frames arrive — fire 3 s after enable to
    // reset the pick to Auto and signal CallManager to re-arm.
    m_camStartWatchdog.setSingleShot(true);
    m_camStartWatchdog.setInterval(3000);
    connect(&m_camStartWatchdog, &QTimer::timeout, this, [this]() {
        if (m_camFirstFrameSeen.load(std::memory_order_relaxed)) return;
        if (!m_camForcedCapsActive) return;
        qWarning() << "PublishPipeline: forced camera mode produced no "
                      "frames within 3 s — falling back to Auto / permissive";
        {
            QSettings s("TalQ", "TalQ");
            s.beginGroup("Video");
            s.setValue("cameraQuality", QStringLiteral("auto"));
            s.setValue("cameraSrcCaps", QString());
            s.endGroup();
        }
        m_camForcedCapsActive = false;
        emit cameraNegotiationFailed();
    });

    // Upstream Talk's BlackVideoEnforcer paints the canvas for 5 s after
    // every transition to "video muted" and then disables the track so
    // RTP halts. We mirror that timing: 5 s after the pipeline enters
    // "camera off", close the dummy valve. With both valves closed no
    // frames reach the funnel → no RTP — the wire goes silent.
    m_dummyHaltTimer.setSingleShot(true);
    m_dummyHaltTimer.setInterval(5000);
    connect(&m_dummyHaltTimer, &QTimer::timeout, this, [this]() {
        if (m_cameraEnabled) return;     // camera came up during the grace window
        if (!m_dummyValve) return;       // pipeline torn down already
        g_object_set(m_dummyValve, "drop", TRUE, nullptr);
        qDebug() << "PublishPipeline: dummy halted after 5 s grace — "
                    "RTP silent until camera enable (upstream conformance)";
    });
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

    // Optional WebRTC noise suppression (webrtcdsp). Enabled by default; the
    // setting and the plugin's presence in the deployed GStreamer both gate
    // it. If the plugin is missing we fall back to the plain chain so calls
    // still work on installs without libgstwebrtcdsp.
    GstElement *webrtcdsp = nullptr;
    {
        QSettings s("TalQ", "TalQ");
        s.beginGroup("Audio");
        const bool nsEnabled = s.value("noiseSuppression", true).toBool();
        s.endGroup();
        // echo-cancel is intentionally OFF. webrtcdsp's echo-cancel mode
        // requires a webrtcechoprobe to exist AT pipeline-start, but the
        // only place to tap the far-end audio is the SubscribeWebrtcSrc
        // playback pipeline, which starts AFTER the publisher (and not at
        // all until a subscriber connects). echo-cancel=TRUE therefore made
        // gst_webrtc_dsp_start fail ("No echo probe ... found") and the
        // whole publish pipeline — hence EVERY call, audio-only included —
        // dropped immediately. Cross-pipeline AEC needs a different design
        // (an early/shared probe on a mixed playback bus); tracked separately.
        if (nsEnabled) {
            webrtcdsp = gst_element_factory_make("webrtcdsp", "pub-webrtcdsp");
            if (webrtcdsp) {
                g_object_set(webrtcdsp,
                             "echo-cancel", FALSE,
                             "noise-suppression", TRUE,
                             "noise-suppression-level", 2,   // high
                             "high-pass-filter", TRUE,
                             "gain-control", FALSE,
                             "voice-detection", FALSE,
                             nullptr);
                qDebug() << "PublishPipeline: noise suppression ON (webrtcdsp)";
            } else {
                qWarning() << "PublishPipeline: webrtcdsp unavailable; "
                              "continuing without noise suppression";
            }
        }
    }

    if (webrtcdsp) {
        gst_bin_add_many(GST_BIN(m_pipeline), audiosrc, audioconvert, audioresample,
                         webrtcdsp, level, opusenc, rtpopuspay, m_webrtcbin, nullptr);
        if (!gst_element_link_many(audiosrc, audioconvert, audioresample,
                                   webrtcdsp, level, opusenc, rtpopuspay, nullptr)) {
            emit error("Failed to link audio capture chain");
            cleanup();
            return false;
        }
    } else {
        gst_bin_add_many(GST_BIN(m_pipeline), audiosrc, audioconvert, audioresample,
                         level, opusenc, rtpopuspay, m_webrtcbin, nullptr);
        if (!gst_element_link_many(audiosrc, audioconvert, audioresample,
                                   level, opusenc, rtpopuspay, nullptr)) {
            emit error("Failed to link audio capture chain");
            cleanup();
            return false;
        }
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
    // --- Simulcast architecture (#132): a single shared chain feeds an
    // outputTee, which fans out into N=3 parallel encoder branches —
    // one per simulcast layer (rid l/m/h at 180p/360p/720p). Each branch
    // links to its own webrtcbin sink_%u pad; per-transceiver
    // codec-preferences carry `rid`, and webrtcbin emits a consolidated
    // a=simulcast: send l;m;h block in the SDP offer.

    // Funnel + shared input (unchanged from single-stream): dummy & camera
    // multiplex into one constant 1280x720@30 source feed.
    m_funnel = gst_element_factory_make("funnel", "pub-funnel");
    if (!m_funnel) { emit error("funnel"); cleanup(); return false; }

    // --- Dummy branch (unchanged) ---
    m_dummySrc   = gst_element_factory_make("videotestsrc", "pub-dummyvideo");
    m_dummyCaps  = gst_element_factory_make("capsfilter", "pub-dummycaps");
    m_dummyConv  = gst_element_factory_make("videoconvert", "pub-dummyconv");
    m_dummyValve = gst_element_factory_make("valve", "pub-dummyvalve");
    if (!m_dummySrc || !m_dummyCaps || !m_dummyConv || !m_dummyValve) {
        emit error("Failed to create dummy video source"); cleanup(); return false;
    }
    g_object_set(m_dummySrc, "pattern", 2 /* black */, "is-live", TRUE, nullptr);
    {
        GstCaps *lowCaps = gst_caps_from_string(
            "video/x-raw,width=16,height=16,framerate=10/1");
        g_object_set(m_dummyCaps, "caps", lowCaps, nullptr);
        gst_caps_unref(lowCaps);
    }
    g_object_set(m_dummyValve, "drop", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(m_pipeline), m_funnel,
                     m_dummySrc, m_dummyCaps, m_dummyConv, m_dummyValve, nullptr);
    gst_element_link_many(m_dummySrc, m_dummyCaps, m_dummyConv, m_dummyValve, nullptr);

    {
        GstPad *dummyValveSrc = gst_element_get_static_pad(m_dummyValve, "src");
        GstPad *funnelSink = gst_element_request_pad_simple(m_funnel, "sink_%u");
        gst_pad_link(dummyValveSrc, funnelSink);
        gst_object_unref(dummyValveSrc);
        gst_object_unref(funnelSink);
    }

    // --- Shared convert/scale → constant 1280x720@30 caps → outputTee ---
    GstElement *sharedConvert = gst_element_factory_make("videoconvert", "pub-shared-conv");
    m_sharedScale = gst_element_factory_make("videoscale", "pub-shared-scale");
    m_sharedCaps  = gst_element_factory_make("capsfilter", "pub-shared-caps");
    GstElement *sharedRate = gst_element_factory_make("videorate", "pub-shared-rate");
    m_outputTee   = gst_element_factory_make("tee", "pub-output-tee");
    if (!sharedConvert || !m_sharedScale || !m_sharedCaps || !sharedRate || !m_outputTee) {
        emit error("Failed to create shared chain / outputTee"); cleanup(); return false;
    }
    {
        GstCaps *sc = gst_caps_from_string(
            "video/x-raw,width=1280,height=720,pixel-aspect-ratio=1/1,framerate=30/1");
        g_object_set(m_sharedCaps, "caps", sc, nullptr);
        gst_caps_unref(sc);
    }
    gst_bin_add_many(GST_BIN(m_pipeline),
                     sharedConvert, m_sharedScale, sharedRate, m_sharedCaps,
                     m_outputTee, nullptr);
    if (!gst_element_link_many(m_funnel, sharedConvert, m_sharedScale,
                                sharedRate, m_sharedCaps, m_outputTee, nullptr)) {
        emit error("Failed to link shared chain"); cleanup(); return false;
    }

    // Simulcast only in pre-release builds; stable sends a single 720p
    // stream (no downgrade vs 0.32.0 while substream selection is proven).
#ifdef TALQ_PRERELEASE
    m_simulcast = true;
#else
    m_simulcast = false;
    // Collapse the layer set to one branch carrying the 720p params.
    m_layers[0] = SimulcastLayer{ "h", 1280, 720, 2'500'000 };
#endif
    const size_t nBranches = m_simulcast ? m_layers.size() : 1;

    // --- Encoder branches: each tees off m_outputTee (3 for simulcast, 1 otherwise) ---
    bool firstBranchUsesH264 = false;
    for (size_t i = 0; i < nBranches; ++i) {
        auto &L = m_layers[i];
        QString tag = QString::fromUtf8(L.rid);

        L.valve      = gst_element_factory_make("valve",       ("pub-valve-"      + tag).toUtf8().constData());
        L.scale      = gst_element_factory_make("videoscale",  ("pub-scale-"      + tag).toUtf8().constData());
        L.caps       = gst_element_factory_make("capsfilter",  ("pub-caps-"       + tag).toUtf8().constData());
        L.ssrcFilter = gst_element_factory_make("capsfilter",  ("pub-ssrcfilter-" + tag).toUtf8().constData());
        // Per-layer encoder + parser. Codec selection MUST agree across layers:
        // makeWebrtcVideoEncoder is deterministic for the same `screen` flag,
        // so all three layers pick the same factory. We pin the codec from
        // layer 0's decision and assert it on subsequent layers.
        bool layerUsesH264 = false;
        L.encoder = makeWebrtcVideoEncoder(/*screen=*/false, L.nominalBitrate,
                                            &layerUsesH264, &L.parser,
                                            (i == 0) ? &m_encoderDesc : nullptr);
        // The branch elements are created here but only bin-added below.
        // On any early-exit before gst_bin_add_many, they're floating refs
        // the bin never adopts, so cleanup() (which only nulls pointers +
        // unrefs the pipeline) would leak them. Sink+drop them on each
        // error path before cleanup().
        auto dropFloating = [&L]() {
            for (GstElement *e : { L.valve, L.scale, L.caps, L.ssrcFilter,
                                   L.encoder, L.parser, L.payloader })
                if (e) gst_object_unref(e);
            L.valve = L.scale = L.caps = L.ssrcFilter = nullptr;
            L.encoder = L.parser = L.payloader = nullptr;
        };
        if (i == 0) {
            firstBranchUsesH264 = layerUsesH264;
            m_useH264 = layerUsesH264;
        } else if (layerUsesH264 != firstBranchUsesH264) {
            emit error("Simulcast branch codec mismatch");
            dropFloating();
            cleanup();
            return false;
        }
        L.payloader = gst_element_factory_make(
            L.encoder && m_useH264 ? "rtph264pay" : "rtpvp8pay",
            ("pub-rtppay-" + tag).toUtf8().constData());

        if (!L.valve || !L.scale || !L.caps || !L.ssrcFilter
            || !L.encoder || !L.payloader) {
            emit error(QString("Failed to create simulcast branch %1").arg(tag));
            dropFloating();
            cleanup();
            return false;
        }

        // Per-layer raw caps (after scale, before encoder)
        {
            GstCaps *sc = gst_caps_from_string(
                QString("video/x-raw,width=%1,height=%2,framerate=30/1")
                    .arg(L.targetW).arg(L.targetH).toUtf8().constData());
            g_object_set(L.caps, "caps", sc, nullptr);
            gst_caps_unref(sc);
        }

        // valve open by default — BWE may close 'h' / 'm' later
        g_object_set(L.valve, "drop", FALSE, nullptr);

        // H264 niceties (mirrors the single-stream path)
        if (m_useH264 && L.parser)
            g_object_set(L.parser, "config-interval", -1, nullptr);
        if (m_useH264)
            g_object_set(L.payloader,
                         "aggregate-mode", 1 /* zero-latency */,
                         "config-interval", -1, nullptr);

        L.ssrc = g_random_int();
        g_object_set(L.payloader, "ssrc", L.ssrc, "pt", 96, nullptr);

        // TWCC on every layer (shared id; aggregate feedback)
        if (GstRTPHeaderExtension *twcc =
                gst_rtp_header_extension_create_from_uri(kTwccUri)) {
            gst_rtp_header_extension_set_id(twcc, kTwccExtId);
            g_signal_emit_by_name(L.payloader, "add-extension", twcc);
            gst_object_unref(twcc);
        }
        // MID hdr ext (id=1) — required by RFC 8843 for BUNDLE.
        if (GstRTPHeaderExtension *midExt =
                gst_rtp_header_extension_create_from_uri(
                    "urn:ietf:params:rtp-hdrext:sdes:mid")) {
            gst_rtp_header_extension_set_id(midExt, 1);
            g_object_set(midExt, "mid", "video0", nullptr);
            g_signal_emit_by_name(L.payloader, "add-extension", midExt);
            gst_object_unref(midExt);
        }
        // RID hdr ext (id=2) — RFC 8852 simulcast identifier per RTP packet.
        // This is the on-wire mechanism the SFU uses to know which substream
        // a given packet belongs to. Only meaningful for simulcast; a single
        // stream must NOT carry a rid (it would imply a simulcast envelope
        // the SDP doesn't declare).
        if (m_simulcast) {
            if (GstRTPHeaderExtension *ridExt =
                    gst_rtp_header_extension_create_from_uri(
                        "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id")) {
                gst_rtp_header_extension_set_id(ridExt, 2);
                g_object_set(ridExt, "rid", L.rid, nullptr);
                g_signal_emit_by_name(L.payloader, "add-extension", ridExt);
                gst_object_unref(ridExt);
            }
        }

        // SSRC pinning per layer — distinct SSRC per simulcast layer is the
        // canonical RFC 8853 model the SFU uses to split substreams.
        {
            GstCaps *sc = gst_caps_from_string("application/x-rtp");
            gst_caps_set_simple(sc, "ssrc", G_TYPE_UINT, L.ssrc, nullptr);
            g_object_set(L.ssrcFilter, "caps", sc, nullptr);
            gst_caps_unref(sc);
        }

        // Bin-add the branch elements
        gst_bin_add_many(GST_BIN(m_pipeline),
                         L.valve, L.scale, L.caps, L.encoder,
                         L.payloader, L.ssrcFilter, nullptr);
        if (L.parser) gst_bin_add(GST_BIN(m_pipeline), L.parser);

        // outputTee → valve → scale → caps → encoder → (parser →) payloader → ssrcFilter → rtpfunnel
        {
            GstPad *teeSrc = gst_element_request_pad_simple(m_outputTee, "src_%u");
            GstPad *valveSink = gst_element_get_static_pad(L.valve, "sink");
            if (gst_pad_link(teeSrc, valveSink) != GST_PAD_LINK_OK) {
                emit error(QString("Failed to link outputTee to branch %1").arg(tag));
                gst_object_unref(teeSrc);
                gst_object_unref(valveSink);
                cleanup();
                return false;
            }
            gst_object_unref(teeSrc);
            gst_object_unref(valveSink);
        }
        if (!gst_element_link_many(L.valve, L.scale, L.caps, L.encoder, nullptr)) {
            emit error(QString("Failed to link branch %1 pre-encoder").arg(tag));
            cleanup();
            return false;
        }
        if (L.parser) {
            if (!gst_element_link_many(L.encoder, L.parser, L.payloader, L.ssrcFilter, nullptr)) {
                emit error(QString("Failed to link branch %1 post-encoder (H264)").arg(tag));
                cleanup();
                return false;
            }
        } else {
            if (!gst_element_link_many(L.encoder, L.payloader, L.ssrcFilter, nullptr)) {
                emit error(QString("Failed to link branch %1 post-encoder (VP8)").arg(tag));
                cleanup();
                return false;
            }
        }

        qInfo().nospace() << "PublishPipeline: simulcast branch '" << tag
                          << "' built (" << L.targetW << "x" << L.targetH
                          << " @ " << (L.nominalBitrate/1000) << " kbps, SSRC "
                          << L.ssrc << ")";
    }

    // --- ONE rtpfunnel muxes the 3 ssrcFilters → ONE webrtcbin sink_%u pad ---
    // This is the canonical C-webrtcbin simulcast topology per upstream test
    // tests/check/elements/webrtcbin.c::test_simulcast (1.28). Three separate
    // webrtcbin sink_%u pads would emit 3 m=video lines — wrong; we need ONE
    // m-line with multi-rid + a=simulcast.
    GstElement *rtpFunnel = gst_element_factory_make("rtpfunnel", "pub-rtpfunnel");
    if (!rtpFunnel) {
        emit error("Failed to create rtpfunnel — simulcast unavailable");
        cleanup();
        return false;
    }
    gst_bin_add(GST_BIN(m_pipeline), rtpFunnel);
    for (size_t i = 0; i < nBranches; ++i) {
        auto &L = m_layers[i];
        GstPad *src = gst_element_get_static_pad(L.ssrcFilter, "src");
        GstPad *funnelSink = gst_element_request_pad_simple(rtpFunnel, "sink_%u");
        if (gst_pad_link(src, funnelSink) != GST_PAD_LINK_OK) {
            emit error(QString("Failed to link branch %1 to rtpfunnel")
                       .arg(QString::fromUtf8(L.rid)));
            gst_object_unref(src);
            gst_object_unref(funnelSink);
            cleanup();
            return false;
        }
        gst_object_unref(src);
        gst_object_unref(funnelSink);
    }

    // Capsfilter between rtpfunnel and webrtcbin's sink_%u pad. THIS is the
    // caps webrtcbin reads when constructing the transceiver and the SDP
    // m=video block — per upstream test
    // tests/check/elements/webrtcbin.c::test_simulcast. Without this filter
    // webrtcbin sees the 3 distinct upstream branches (each ssrcFilter has
    // its own SSRC + rid in fmtp) and creates 3 transceivers / 3 m=video
    // lines. WITH this filter providing a unified caps that includes all
    // rid-X + a-simulcast fields, webrtcbin emits ONE m=video with
    // a=simulcast + 3 a=rid lines.
    GstElement *simulcastCaps = gst_element_factory_make("capsfilter", "pub-simulcastcaps");
    if (!simulcastCaps) {
        emit error("Failed to create simulcast capsfilter");
        cleanup();
        return false;
    }
    {
        GstCaps *vc = gst_caps_from_string(
            m_useH264
              ? "application/x-rtp,media=video,encoding-name=H264,clock-rate=90000,payload=96"
              : "application/x-rtp,media=video,encoding-name=VP8,clock-rate=90000,payload=96");
        gst_caps_set_simple(vc,
            "a-mid",        G_TYPE_STRING, "video0",
            "extmap-1",     G_TYPE_STRING, "urn:ietf:params:rtp-hdrext:sdes:mid",
            nullptr);
        char extField[16];
        g_snprintf(extField, sizeof(extField), "extmap-%d", kTwccExtId);
        gst_caps_set_simple(vc, extField, G_TYPE_STRING, kTwccUri, nullptr);
        // Multi-rid + a=simulcast directive ONLY for simulcast builds. A
        // single-stream build emits a plain m=video (no a=rid/a=simulcast),
        // so it must not declare the rid extmap or the rid-* fields.
        if (m_simulcast) {
            gst_caps_set_simple(vc,
                "extmap-2",     G_TYPE_STRING, "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id",
                "rid-l",        G_TYPE_STRING, "send",
                "rid-m",        G_TYPE_STRING, "send",
                "rid-h",        G_TYPE_STRING, "send",
                "a-simulcast",  G_TYPE_STRING, "send l;m;h",
                nullptr);
        }
        g_object_set(simulcastCaps, "caps", vc, nullptr);
        gst_caps_unref(vc);
    }
    gst_bin_add(GST_BIN(m_pipeline), simulcastCaps);

    // --- The single webrtcbin video sink pad ---
    GstPad *videoSinkPad = gst_element_request_pad_simple(m_webrtcbin, "sink_%u");
    if (!videoSinkPad) {
        emit error("Failed to request webrtcbin video sink pad");
        cleanup();
        return false;
    }
    // Stash the pad in layer 0's sinkPad slot for cleanup ownership tracking.
    m_layers[0].sinkPad = videoSinkPad;

    // rtpfunnel → simulcastCaps → webrtcbin sink_%u
    if (!gst_element_link(rtpFunnel, simulcastCaps)) {
        emit error("Failed to link rtpfunnel to simulcast capsfilter");
        cleanup();
        return false;
    }
    {
        GstPad *capsSrc = gst_element_get_static_pad(simulcastCaps, "src");
        if (gst_pad_link(capsSrc, videoSinkPad) != GST_PAD_LINK_OK) {
            emit error("Failed to link simulcast capsfilter to webrtcbin sink");
            gst_object_unref(capsSrc);
            cleanup();
            return false;
        }
        gst_object_unref(capsSrc);
    }

    // Set transceiver direction (codec-preferences inherited from caps).
    {
        GstWebRTCRTPTransceiver *vt = nullptr;
        g_object_get(videoSinkPad, "transceiver", &vt, nullptr);
        if (vt) {
            g_object_set(vt, "direction",
                         GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, nullptr);
            gst_object_unref(vt);
        }
    }

    qDebug().nospace() << "PublishPipeline: " << (uint)nBranches
                       << (m_simulcast ? " simulcast branches" : " branch (single 720p)")
                       << " built, codec=" << (m_useH264 ? "H264" : "VP8");

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
    g_signal_connect(m_webrtcbin, "notify::ice-gathering-state",
                     G_CALLBACK(onIceGatheringStateChanged), this);
    // Send-side congestion control: webrtcbin asks for an aux sender once
    // the transport is up; we return rtpgccbwe and ride its estimate onto
    // the encoder. Now that the offer carries the TWCC extmap (above),
    // Janus sends transport-wide RTCP feedback so the estimate is real.
    g_signal_connect(m_webrtcbin, "request-aux-sender",
                     G_CALLBACK(onRequestAuxSender), this);

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
    // Defence in depth: cleanup() of a prior instance sets this true,
    // and instances are normally re-created per call rather than reused
    // — but if an instance is ever reused, the shutdown guard would
    // silently no-op GCC / timers without this reset.
    m_shuttingDown.store(false);

    // Dummy feeds through dummyValve → funnel → shared encoder for the
    // first 5 s only (m_dummyHaltTimer below). After that the dummy valve
    // closes and RTP halts on the video m-line until enableCamera() flips
    // the camera valve open — mirrors upstream Talk's
    // BlackVideoEnforcer(5 s) → track.enabled=false pattern.
    m_dummyHaltTimer.start();
    qDebug() << "PublishPipeline: started (send-only), dummy feeding encoder "
                "for 5 s grace then RTP halts until camera enable";

    // Don't enable camera during start() — it blocks the UI thread.
    // CallManager will call enableCamera() after the call connects.
    // For "start with video" calls, CallManager enables camera via toggleCamera.
    Q_UNUSED(withVideo)

    return true;
}

void PublishPipeline::stop()
{
    if (!m_running) return;
    // Flag the shutdown BEFORE disableCamera so its dummy-halt-timer
    // restart short-circuits (otherwise the timer is re-armed seconds
    // before cleanup() stops it again, and on a stale Qt thread the
    // GStreamer streaming thread can trip "QObject::startTimer: Timers
    // can only be used with threads started with QThread").
    m_shuttingDown.store(true);
    disableCamera();
    cleanup();
    m_running = false;
    qDebug() << "PublishPipeline: stopped";
}

void PublishPipeline::cleanup()
{
    qDebug() << "PublishPipeline::cleanup() — begin, pipeline=" << (void*)m_pipeline << "webrtcbin=" << (void*)m_webrtcbin;
    // Block any aux-sender/GCC callback that races this teardown (they run
    // on a GStreamer streaming thread; cleanup() runs on the Qt thread).
    m_shuttingDown.store(true);
    // Stop the dummy-halt timer so it can't fire on a torn-down pipeline.
    m_dummyHaltTimer.stop();
    // Disconnect GStreamer signals to prevent callbacks with stale userData
    if (m_webrtcbin) {
        qDebug() << "PublishPipeline::cleanup() — disconnecting signals";
        g_signal_handlers_disconnect_by_data(m_webrtcbin, this);
    }
    if (m_gccbwe)  // notify::estimated-bitrate is on the gcc element, not webrtcbin
        g_signal_handlers_disconnect_by_data(m_gccbwe, this);
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
    m_sharedScale = nullptr;
    m_sharedCaps = nullptr;
    m_gccbwe = nullptr;
    m_outputTee = nullptr;
    for (auto &L : m_layers) {
        L.valve = L.scale = L.caps = L.encoder = L.parser
              = L.payloader = L.ssrcFilter = nullptr;
        L.sinkPad = nullptr;
        L.ssrc = 0;
        L.lastAppliedBitrate = 0;
        L.active = true;
    }
    m_cameraEnabled = false;
    m_dummySrc = nullptr;
    m_dummyCaps = nullptr;
    m_dummyConv = nullptr;
    m_dummyValve = nullptr;
    m_cameraSrc = nullptr;
    m_camSrcCaps = nullptr;
    m_camDecode = nullptr;
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
    // TALQ_PUB_TESTSRC (test/CI only — UNSET in production, where this
    // path is byte-identical to before): publish a synthetic high-motion
    // 720p feed through the REAL encoder + rtpgccbwe chain so
    // talq-call-test can validate the #111 GCC-floor fix headlessly (no
    // camera, no human). decodebin below passes the raw videotestsrc
    // buffer straight through unchanged.
    const bool kPubTestSrc = qEnvironmentVariableIsSet("TALQ_PUB_TESTSRC");
    if (kPubTestSrc) {
        m_cameraSrc = gst_element_factory_make("videotestsrc", nullptr);
        if (m_cameraSrc)
            // pattern=snow: full-frame random noise — EVERY pixel of EVERY
            // frame changes, so (a) the sparse-hash RX distinct counter
            // detects every frame unambiguously and (b) it is maximal
            // entropy, the hardest case for the encoder = the exact #111
            // stress condition. (ball = near-static frame, defeats the
            // distinct heuristic — invalid for this proxy.)
            g_object_set(m_cameraSrc, "is-live", TRUE,
                         "pattern", 1 /* snow */, nullptr);
        qDebug() << "PublishPipeline: TEST source (videotestsrc snow) — "
                    "#111 harness mode";
    } else {
        m_cameraSrc = gst_element_factory_make("mfvideosrc", nullptr);
        if (m_cameraSrc) {
            qDebug() << "PublishPipeline: camera source: mfvideosrc";
        } else {
            m_cameraSrc = gst_element_factory_make("ksvideosrc", nullptr);
            if (m_cameraSrc)
                qDebug() << "PublishPipeline: camera source: ksvideosrc (fallback)";
        }
    }
    if (!m_cameraSrc) {
        qWarning() << "PublishPipeline: no camera capture plugin available";
        return false;
    }
    if (!kPubTestSrc)
        g_object_set(m_cameraSrc, "device-index", deviceIndex, nullptr);

    // Create camera branch elements
    m_camSrcCaps      = gst_element_factory_make("capsfilter", "cam-src-caps");
    m_camDecode       = gst_element_factory_make("decodebin", "cam-decode");
    m_videoConvert    = gst_element_factory_make("videoconvert", nullptr);
    m_videoCapsFilter = gst_element_factory_make("capsfilter", nullptr);
    m_tee             = gst_element_factory_make("tee", "camera-tee");
    m_encQueue        = gst_element_factory_make("queue", "enc-queue");
    m_cameraValve     = gst_element_factory_make("valve", "camera-valve");
    m_previewQueue    = gst_element_factory_make("queue", "preview-queue");
    m_previewConvert  = gst_element_factory_make("videoconvert", "preview-convert");
    m_previewAppsink  = gst_element_factory_make("appsink", "preview-sink");

    if (!m_camSrcCaps || !m_camDecode ||
        !m_videoConvert || !m_videoCapsFilter || !m_tee || !m_encQueue ||
        !m_cameraValve || !m_previewQueue || !m_previewConvert || !m_previewAppsink) {
        qWarning() << "PublishPipeline: failed to create camera branch elements";
        // Clean up any elements that were created (not yet added to pipeline)
        auto freeIf = [](GstElement *&el) { if (el) { gst_object_unref(el); el = nullptr; } };
        freeIf(m_cameraSrc);
        freeIf(m_camSrcCaps);
        freeIf(m_camDecode);
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
    // Camera SOURCE caps (on mfvideosrc, BEFORE decodebin). The mode is
    // resolved in MediaDeviceManager from the device's real advertised
    // caps (Settings → Camera Quality; "Auto" = absolute best) and stored
    // as ONE exact structure in Video/cameraSrcCaps. A single fixed
    // structure is what actually forces the mode — a permissive multi-
    // structure menu lets mfvideosrc fall back to its native raw format
    // (raw 1280x720 over USB negotiates 30/1 but delivers ~10 real fps;
    // MJPEG is ~10:1 compressed so it fits USB and yields a true 30 fps,
    // which is why Zoom/Telegram are smooth on the same camera).
    // decodebin (below) auto-plugs jpegdec for MJPEG or passes raw
    // through. If the setting is empty (device exposed no parseable
    // modes / pre-population), fall back to the permissive ≤720p menu.
    {
        GstCaps *caps = nullptr;
        QString srcCapsStr;
        bool userForced = false;   // true ⇒ a user Settings pick is in
                                    // effect; arms the no-frames watchdog
                                    // so a bad pick can never strand the
                                    // camera (auto-recover to Auto).
        if (kPubTestSrc) {
            // Exercise exactly the pinned 720p30 path #111 is about.
            srcCapsStr = QStringLiteral(
                "video/x-raw,width=1280,height=720,framerate=30/1");
            caps = gst_caps_from_string(srcCapsStr.toUtf8().constData());
        } else {
            QSettings s("TalQ", "TalQ");
            s.beginGroup("Video");
            srcCapsStr = s.value("cameraSrcCaps").toString();
            s.endGroup();
            if (!srcCapsStr.isEmpty()) {
                caps = gst_caps_from_string(srcCapsStr.toUtf8().constData());
                if (caps) userForced = true;
            }
        }
        m_camForcedCapsActive = userForced;
        if (caps) {
            qDebug() << "PublishPipeline: forcing camera mode:" << srcCapsStr;
        } else {
            if (!srcCapsStr.isEmpty())
                qWarning() << "PublishPipeline: bad cameraSrcCaps, using menu:"
                           << srcCapsStr;
            caps = gst_caps_from_string(
                "image/jpeg,width=(int)[1,1280],height=(int)[1,720],"
                  "framerate=(fraction)30/1;"
                "video/x-raw,width=(int)[1,1280],height=(int)[1,720],"
                  "framerate=(fraction)30/1;"
                "image/jpeg,width=(int)[1,1280],height=(int)[1,720],"
                  "framerate=(fraction)[15/1,60/1];"
                "video/x-raw,width=(int)[1,1280],height=(int)[1,720],"
                  "framerate=(fraction)[15/1,60/1];"
                "video/x-raw,width=(int)[1,1280],height=(int)[1,720],"
                  "framerate=(fraction)[1/1,60/1]");
        }
        g_object_set(m_camSrcCaps, "caps", caps, nullptr);
        gst_caps_unref(caps);
    }
    // Post-decode capsfilter: just normalize to raw. Do NOT pin a
    // framerate here — the shared chain's videorate (sharedRate) does
    // CFR→30 for the encoder, and a genuinely slow raw-only webcam must
    // not fail negotiation by being forced to 30.
    {
        GstCaps *raw = gst_caps_from_string("video/x-raw");
        g_object_set(m_videoCapsFilter, "caps", raw, nullptr);
        gst_caps_unref(raw);
        qDebug() << "PublishPipeline: camera caps: MJPEG|raw ≤720p, "
                    "prefer 30fps (jpegdec via decodebin)";
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
        m_cameraSrc, m_camSrcCaps, m_camDecode,
        m_videoConvert, m_videoCapsFilter, m_tee,
        m_encQueue, m_cameraValve,
        m_previewQueue, m_previewConvert, m_previewAppsink, nullptr);

    // Capture chain: cameraSrc → camSrcCaps → decodebin →(dynamic pad)→
    // videoConvert → videoCapsFilter → tee. decodebin's src pad appears
    // only once it sees data, so link it to videoConvert in pad-added.
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

    gboolean linked = gst_element_link_many(m_cameraSrc, m_camSrcCaps, m_camDecode, nullptr);
    linked = linked && gst_element_link_many(m_videoConvert, m_videoCapsFilter, m_tee, nullptr);

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
    gst_element_sync_state_with_parent(m_camSrcCaps);
    gst_element_sync_state_with_parent(m_camDecode);
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

    // 3. Flip valves — camera frames flow, dummy frames stop. Also stop
    //    the 5-s dummy-halt timer (if it hadn't fired yet) so it cannot
    //    fire after we've intentionally taken over with the camera valve.
    m_dummyHaltTimer.stop();
    g_object_set(m_cameraValve, "drop", FALSE, nullptr);
    g_object_set(m_dummyValve, "drop", TRUE, nullptr);

    // 4. Pause dummy source to save CPU (stays linked to funnel)
    gst_element_set_state(m_dummySrc, GST_STATE_PAUSED);

    m_cameraEnabled = true;
    m_camFirstFrameSeen.store(false, std::memory_order_relaxed);
    if (m_camForcedCapsActive) {
        qDebug() << "PublishPipeline: arming camera-start watchdog (3 s)";
        m_camStartWatchdog.start();
    }

    // Force an immediate I-frame so the receiver gets a clean baseline
    // of real camera content. Without this, the encoder continues
    // emitting P-frames referenced against the just-replaced dummy
    // (black 16×16 upscaled), and the receiver decodes them against
    // that wrong baseline — blocky/choppy video until the next periodic
    // keyframe (~1 s at GOP=30, ~2 s at the old GOP=60). The standard
    // WebRTC mechanism: GstForceKeyUnit as a CUSTOM_UPSTREAM event
    // pushed at the pipeline; webrtcbin/encoder honor it and the very
    // next encoded frame becomes an I-frame. Send it twice — once now
    // (in case the first camera frame arrives before the event clears
    // the pipeline) and again ~200 ms later, after mfvideosrc's COM
    // init has settled and real frames are reliably flowing.
    auto sendForceKeyUnit = [this]() {
        if (!m_pipeline) return;
        GstStructure *s = gst_structure_new("GstForceKeyUnit",
            "all-headers", G_TYPE_BOOLEAN, TRUE, nullptr);
        GstEvent *ev = gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM, s);
        gst_element_send_event(m_pipeline, ev);
    };
    sendForceKeyUnit();
    QTimer::singleShot(200, this, sendForceKeyUnit);
    qDebug() << "PublishPipeline: requested immediate keyframe (camera enable)";
    // Undo the camera-off idle clamp: re-seed every active layer's encoder
    // to its nominal bitrate and clear per-layer deadband so the next GCC
    // notify re-applies live targets (onGccBitrate compares against
    // L.lastAppliedBitrate per-layer).
    for (auto &L : m_layers) {
        L.lastAppliedBitrate = 0;
        if (L.active && L.encoder)
            setWebrtcVideoEncoderBitrate(L.encoder, m_useH264, (guint)L.nominalBitrate);
    }
    qDebug() << "PublishPipeline: camera enabled (valve flip, no relink, "
                "encoders re-seeded, GCC re-armed)";
}

void PublishPipeline::disableCamera()
{
    if (!m_pipeline || !m_cameraEnabled) return;
    m_camStartWatchdog.stop();

    // 1. Flip valves — dummy frames flow, camera frames stop
    if (m_cameraValve) g_object_set(m_cameraValve, "drop", TRUE, nullptr);
    if (m_dummyValve) g_object_set(m_dummyValve, "drop", FALSE, nullptr);

    // 2. Resume dummy source + convert, pause camera (save CPU)
    gst_element_set_state(m_dummySrc, GST_STATE_PLAYING);
    gst_element_set_state(m_dummyConv, GST_STATE_PLAYING);
    if (m_cameraSrc) gst_element_set_state(m_cameraSrc, GST_STATE_PAUSED);

    m_cameraEnabled = false;
    // Re-arm the 5-s dummy-halt timer: the wire stays "alive" through the
    // grace window (so peers/Janus see continuity through the mute), then
    // RTP goes silent. Matches upstream's BlackVideoEnforcer mute timing.
    // Skip during shutdown: cleanup() is about to stop the timer anyway,
    // and starting it from a teardown call site can run on a non-Qt thread.
    if (!m_shuttingDown.load())
        m_dummyHaltTimer.start();
    // Collapse every layer's encoder to a trickle while the camera is off.
    // The transceivers stay alive (black dummy → no renegotiation), but we
    // no longer ship GCC-padded black on any layer. Matches the official
    // NC Talk client / libwebrtc. onGccBitrate() is gated on
    // m_cameraEnabled so GCC cannot push its floor back onto encoders
    // until the camera returns; enableCamera() re-seeds and re-arms GCC.
    for (auto &L : m_layers) {
        if (L.encoder)
            setWebrtcVideoEncoderBitrate(L.encoder, m_useH264, 50000u);
    }
    qDebug() << "PublishPipeline: camera disabled (valve flip, no relink, "
                "dummy resumed, all layers collapsed to ~50 kbps)";
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
            // Identify whether the error originated in one of the simulcast
            // branches' encoder/parser/payloader; if so, close only that
            // branch's valve and keep the other layers + the call alive.
            // Mirrors #138 policy: non-main-stream failure must not drop
            // the call.
            GstObject *src = GST_MESSAGE_SRC(msg);
            int branchIdx = -1;
            for (int i = 0; i < (int)m_layers.size(); ++i) {
                const auto &L = m_layers[i];
                if (src == GST_OBJECT(L.encoder) ||
                    src == GST_OBJECT(L.parser)  ||
                    src == GST_OBJECT(L.payloader)) {
                    branchIdx = i;
                    break;
                }
            }
            if (branchIdx >= 0) {
                qWarning().nospace()
                    << "PublishPipeline: simulcast layer '"
                    << m_layers[branchIdx].rid
                    << "' encoder ERROR (" << errMsg
                    << ") — closing branch valve, call stays up";
                setLayerActive(branchIdx, false);
                bool anyAlive = false;
                for (const auto &L : m_layers)
                    if (L.active) { anyAlive = true; break; }
                if (!anyAlive) {
                    qWarning() << "PublishPipeline: ALL simulcast layers "
                                  "dead — propagating fatal";
                    emit error(errMsg);
                }
            } else {
                qWarning() << "PublishPipeline ERROR:" << errMsg << dbgStr;
                // Non-branch error (e.g., webrtcbin transport): log only,
                // legacy behavior. Camera stays alive.
            }
            g_clear_error(&err); g_free(dbg);
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

// #132 simulcast fix (evidence-grounded via tcpdump on Janus 2026-05-22):
// the 3 layer SSRCs DO reach Janus on the wire (PT 96, distinct SSRCs,
// packet counts ∝ bitrate), but webrtcbin's m=video SDP declares only ONE
// PHANTOM a=ssrc that isn't even on the wire and NO ssrc-group. Janus
// 1.1.4 videoroom detects simulcast from the SSRC-group (not from a=rid —
// the HPB never sends the JSEP simulcast object), so it can't associate
// the 3 received SSRCs and forwards just one (stuck at 180p).
// Fix: replace webrtcbin's phantom a=ssrc lines with one set per REAL wire
// SSRC (low→mid→high, the payloader SSRCs), replicating the FULL attribute
// block webrtcbin emitted (cname + msid — bare a=ssrc broke negotiation on
// the first attempt), and add `a=ssrc-group:SIM l m h`. The rid lines are
// preserved. See memory project_talq_simulcast_janus.
[[maybe_unused]] static QString injectSimulcastSsrcGroup(
        const QString &sdp, quint32 ssrcL, quint32 ssrcM, quint32 ssrcH)
{
    const QStringList lines = sdp.split('\n');
    // The rtpfunnel single-m-line video section has NO a=ssrc of its own
    // (webrtcbin doesn't declare one), so there's nothing to capture/replace
    // there — we ADD the SIM group + a=ssrc lines for the 3 real wire SSRCs
    // at the end of the video section, using the session cname (the same
    // CNAME webrtcbin puts in RTCP for every stream; the audio section's
    // a=ssrc cname is that value). cname is what ties the SIM group together
    // for Janus; msid is reused if the video section declares one.
    QString cname, videoMsid;
    bool seenVideo = false;
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        if (t.startsWith("m=video")) seenVideo = true;
        if (cname.isEmpty() && t.startsWith("a=ssrc:")) {
            const int ci = t.indexOf("cname:");
            if (ci >= 0) cname = t.mid(ci + 6).section(' ', 0, 0).trimmed();
        }
        if (seenVideo && videoMsid.isEmpty() && t.startsWith("a=msid:"))
            videoMsid = t.mid(7).trimmed();
    }
    if (cname.isEmpty()) cname = QStringLiteral("talqsim");

    QStringList block;
    block << QStringLiteral("a=ssrc-group:SIM %1 %2 %3\r")
                 .arg(ssrcL).arg(ssrcM).arg(ssrcH);
    for (quint32 s : { ssrcL, ssrcM, ssrcH }) {
        block << QStringLiteral("a=ssrc:%1 cname:%2\r").arg(s).arg(cname);
        if (!videoMsid.isEmpty())
            block << QStringLiteral("a=ssrc:%1 msid:%2\r").arg(s).arg(videoMsid);
    }

    QStringList out;
    out.reserve(lines.size() + block.size());
    bool inVideo = false, inserted = false;
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        if (t.startsWith("m=")) {
            // Entering a new m-section: if we were in video and haven't
            // inserted yet, append the SIM block at the video section's end.
            if (inVideo && !inserted) { out << block; inserted = true; }
            inVideo = t.startsWith("m=video");
        }
        // Drop any stray existing a=ssrc in the video section (phantom).
        if (inVideo && t.startsWith("a=ssrc")) continue;
        out << line;
    }
    if (inVideo && !inserted) out << block;  // video was the last section
    return out.join('\n');
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

    // #132 simulcast SIM-group munge — DISABLED. Evidence (tcpdump on
    // Janus 2026-05-22): the 3 layer SSRCs DO reach Janus, but injecting
    // an a=ssrc-group:SIM into the offer makes the HPB/Janus reject the
    // publisher entirely ("No MCU client found to send message to") — tried
    // sent-only AND as the local description, both break MCU registration.
    // So SDP munging is a dead end here; the real fix needs webrtcbin to
    // emit the SIM group natively (it doesn't for the rtpfunnel topology in
    // 1.28), a publish-topology change, or an HPB patch to inject the JSEP
    // `simulcast` object. injectSimulcastSsrcGroup() is kept for reference.
    // Simulcast still works as a single forwarded layer (call connects).
    Q_UNUSED(self->m_simulcast);

    qDebug() << "PublishPipeline: offer created, SDP length=" << sdp.length() << "\n" << sdp;

    // Validate SDP has at least one media line — empty offers get rejected by MCU
    if (!sdp.contains("m=audio") && !sdp.contains("m=video")) {
        qWarning() << "PublishPipeline: offer has no media lines, not sending (audio source may have failed)";
        return;
    }

    // #132 simulcast diag — log just the a=simulcast line so the field log
    // confirms the publisher is offering simulcast (compact one-liner; not
    // a full SDP dump).
    for (const QString &line : sdp.split('\n')) {
        QString t = line.trimmed();
        if (t.startsWith("a=simulcast:")) {
            qInfo() << "PublishPipeline:" << t;
            break;
        }
    }

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

void PublishPipeline::onIceGatheringStateChanged(GObject *obj, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);
    GstWebRTCICEGatheringState state;
    g_object_get(obj, "ice-gathering-state", &state, nullptr);
    if (state != GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) return;
    QPointer<PublishPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard]() {
        if (!guard) return;
        qDebug() << "PublishPipeline: ICE gathering complete";
        emit guard->iceGatheringComplete();
    }, Qt::QueuedConnection);
}

GstFlowReturn PublishPipeline::onPreviewSample(GstAppSink *sink, gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;

    // First preview frame after enableCamera ⇒ the forced caps (if any)
    // negotiated successfully. Cancel the self-heal watchdog from the
    // Qt thread (QTimer.stop is not safe from this streaming thread).
    if (!self->m_camFirstFrameSeen.exchange(true, std::memory_order_relaxed)) {
        QPointer<PublishPipeline> gd(self);
        QMetaObject::invokeMethod(self, [gd]() {
            if (gd) gd->m_camStartWatchdog.stop();
        }, Qt::QueuedConnection);
    }

    QPointer<PublishPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, sample]() {
        if (guard && guard->m_localVideoProvider)
            guard->m_localVideoProvider->feedFrame(sample);
        gst_sample_unref(sample);
    }, Qt::QueuedConnection);

    return GST_FLOW_OK;
}

GstElement *PublishPipeline::onRequestAuxSender(GstElement *, GObject *,
                                                gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);
    if (self->m_shuttingDown.load()) return nullptr;
    GstElement *gcc = gst_element_factory_make("rtpgccbwe", nullptr);
    if (!gcc) {
        qWarning() << "PublishPipeline: rtpgccbwe unavailable — encoder "
                      "stays at fixed bitrate";
        return nullptr;
    }
    // GCC floor for the camera publish path. History:
    //   0.30.x      300k — too low, gave the *hypothesis* that encoder
    //                       starvation caused Ilko's 30/10 dup-pad chop.
    //   0.31.0/0.31.1 1.2M — shipped on that hypothesis; snow-proxy passed
    //                       at this floor, but field evidence later showed
    //                       Ilko's chop was actually camera-mode drift
    //                       (cleared on TalQ restart), not the floor —
    //                       and 1.2M is too high as a minimum for
    //                       moderate uplinks: it clamps GCC above what
    //                       the wire can carry, so excess bytes drop and
    //                       the receiver sees decoder artifacts even
    //                       though distinct ≈ delivered (Kalin's case).
    //   0.31.2+      600k — libwebrtc / Zoom-ish 720p30 floor: enough
    //                       to encode moving content at acceptable
    //                       quality, low enough that a marginal uplink
    //                       doesn't sustain constant loss. GCC remains
    //                       free to ramp up to m_maxBitrate when the
    //                       link allows.
    // Ceiling = server video cap. Seed the estimate with our start bitrate
    // (treated as the target until feedback arrives). guint props.
    g_object_set(gcc,
                 "min-bitrate", (guint)600000,
                 "max-bitrate", (guint)self->m_maxBitrate,
                 "estimated-bitrate", (guint)self->m_initBitrate,
                 nullptr);
    g_signal_connect(gcc, "notify::estimated-bitrate",
                     G_CALLBACK(onGccBitrate), self);
    self->m_gccbwe = gcc;
    qInfo().nospace() << "PublishPipeline: rtpgccbwe attached (min 600k, max "
                      << self->m_maxBitrate << ", start "
                      << self->m_initBitrate << ")";
    return gcc;  // webrtcbin takes ownership
}

void PublishPipeline::setLayerActive(int i, bool on)
{
    if (i < 0 || i >= (int)m_layers.size()) return;
    auto &L = m_layers[i];
    if (L.active == on) return;
    if (L.valve) g_object_set(L.valve, "drop", on ? FALSE : TRUE, nullptr);
    L.active = on;
    qInfo().nospace() << "PublishPipeline: simulcast layer '" << L.rid
                      << "' -> " << (on ? "ACTIVE" : "MUTED")
                      << " (BWE gate)";
}

void PublishPipeline::applyBweToLayers(int estimateBps)
{
    // Threshold rationale: each layer's effective wire cost exceeds its
    // nominal target by ~25% (RTP/RTCP overhead, FEC pacing slack).
    // Sums-of-active-layers we want to fit under: l alone ~200k,
    // l+m ~800k, l+m+h ~3 700k. Close-thresholds chosen below the next
    // sum-up; reopen with +200k hysteresis to avoid flapping.
    constexpr int kCloseH      = 1'800'000;
    constexpr int kCloseM      =   600'000;
    constexpr int kHysteresis  =   200'000;

    auto threshold = [&](int closeT, bool currentlyActive) {
        return currentlyActive ? closeT : (closeT + kHysteresis);
    };

    bool wantH = estimateBps > threshold(kCloseH, m_layers[2].active);
    bool wantM = estimateBps > threshold(kCloseM, m_layers[1].active);
    // 'l' always stays on — our minimum-viable channel.
    setLayerActive(2, wantH);
    setLayerActive(1, wantM);
}

void PublishPipeline::onGccBitrate(GObject *gcc, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);
    if (!self || self->m_shuttingDown.load()) return;
    // Camera off: disableCamera() has clamped all encoders to an idle
    // trickle for the black dummy (NC Talk parity). Don't let GCC re-raise
    // them; enableCamera() clears per-layer deadband so live targets
    // re-apply immediately.
    if (!self->m_cameraEnabled) return;
    guint est = 0;
    g_object_get(gcc, "estimated-bitrate", &est, nullptr);
    if (est == 0) return;
    if (est > (guint)self->m_maxBitrate) est = (guint)self->m_maxBitrate;

    // Test hook (#132 TALQ_TEST_SIMULCAST_DROP): when this env var is
    // present, the value replaces the real estimate so the harness can
    // deterministically step through BWE thresholds and watch the
    // layer-gate close h then m. Production has it unset.
    {
        QByteArray ov = qgetenv("TALQ_TEST_BWE_OVERRIDE_KBPS");
        if (!ov.isEmpty()) {
            bool okI = false;
            int kb = ov.toInt(&okI);
            if (okI && kb > 0) est = (guint)(kb * 1000);
        }
    }

    QPointer<PublishPipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, estBps = (int)est]() {
        if (!guard) return;

        // Single-stream (stable) build: GCC drives the lone encoder's
        // bitrate directly, exactly like 0.32.0 — the layer gate + nominal
        // pinning below are simulcast-only behaviors. Deadband avoids
        // hammering the HW encoder with tiny updates.
        if (!guard->m_simulcast) {
            auto &L = guard->m_layers[0];
            if (L.encoder && qAbs(L.lastAppliedBitrate - estBps) >= 50'000) {
                setWebrtcVideoEncoderBitrate(L.encoder, guard->m_useH264,
                                              (guint)estBps);
                L.lastAppliedBitrate = estBps;
            }
            return;
        }

        // Drive the layer gate first — opening/closing layers changes
        // the aggregate target before we set per-encoder bitrates.
        guard->applyBweToLayers(estBps);

        // Per-layer encoder bitrate stays at nominal target (not GCC-divided).
        // The deadband prevents the HW encoder from getting hammered with
        // tiny bitrate updates that fail with MFX_ERR_INCOMPATIBLE_VIDEO_PARAM.
        for (auto &L : guard->m_layers) {
            if (!L.active || !L.encoder) continue;
            if (qAbs(L.lastAppliedBitrate - L.nominalBitrate) < 50'000) continue;
            setWebrtcVideoEncoderBitrate(L.encoder, guard->m_useH264,
                                          (guint)L.nominalBitrate);
            L.lastAppliedBitrate = L.nominalBitrate;
        }
    }, Qt::QueuedConnection);
}
