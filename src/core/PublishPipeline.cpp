#include "core/PublishPipeline.h"
#include "core/BackgroundEngine.h"
#include "core/LeakStats.h"
#include <QDebug>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QUrl>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtp/rtp.h>
#include <gst/sdp/sdp.h>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "core/VideoEncoderUtil.h"
#include "core/CaptureDsp.h"
#include "core/EncodeTier.h"

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

    // 0.41.0-beta — read the user's "Maximum send resolution" choice
    // (QSettings("TalQ","TalQ")/Video/maxSendHeight) and resize the HIGH
    // simulcast layer + the shared output caps + the GCC ceiling to
    // match. Default 720 = TalQ 0.40 behaviour. Allowed: 720, 1080,
    // 1440, 2160. Out-of-range values fall back to 720.
    {
        QSettings s("TalQ", "TalQ");
        s.beginGroup("Video");
        int h = s.value("maxSendHeight", 720).toInt();
        s.endGroup();
        if (h != 720 && h != 1080 && h != 1440 && h != 2160) h = 720;
        // Encode-load mitigation (field freeze 2026-06-05): cap the camera SEND
        // by GPU tier so a publisher can't saturate a weak/iGPU/software encoder
        // (it also has to decode the peer). Single source of truth = EncodeTier.h
        // (the Home screen shows the SAME cap). "When in doubt" (unknown tier) is
        // treated as the WEAKEST — if we can't confirm HW accel, assume none. The
        // res cap only bites users who chose >cap (Petia's log shows 1080p);
        // default-720 users are unchanged. The full CPU-load controller (later)
        // lifts this dynamically when headroom exists.
        const talq::EncodeTierCap tier = talq::encodeTierCap(m_gpuClass);
        if (tier.maxSendHeight > 0 && h > tier.maxSendHeight) {
            qInfo().nospace() << "PublishPipeline: GPU class "
                << talq::gpuClassName(m_gpuClass)
                << " -- capping camera send resolution " << h << "p -> "
                << tier.maxSendHeight << "p";
            h = tier.maxSendHeight;
        }
        if (tier.shedHighLayer)
            m_loadCapMaxLayer = 1;   // no HW encode -> send l+m only
        const int w = (h * 16) / 9 & ~1;   // 16:9, even width
        m_layers[2].targetW = w;
        m_layers[2].targetH = h;
        // Encoder bits-per-pixel-per-frame ≈ 0.10 at 30 fps for camera
        // content is the "Telegram-class HQ" rule of thumb; we round to
        // documented buckets so the bitrate set is predictable.
        switch (h) {
            case 480:  m_layers[2].nominalBitrate = 1'200'000; m_initBitrate = 1'200'000; m_maxBitrate =  2'000'000; break;
            case 720:  m_layers[2].nominalBitrate = 3'500'000; m_initBitrate = 3'500'000; m_maxBitrate =  6'000'000; break;
            case 1080: m_layers[2].nominalBitrate = 6'000'000; m_initBitrate = 6'000'000; m_maxBitrate =  9'000'000; break;
            case 1440: m_layers[2].nominalBitrate = 12'000'000; m_initBitrate = 12'000'000; m_maxBitrate = 18'000'000; break;
            case 2160: m_layers[2].nominalBitrate = 22'000'000; m_initBitrate = 22'000'000; m_maxBitrate = 30'000'000; break;
        }
        qInfo().nospace() << "PublishPipeline: max send resolution "
            << w << "x" << h << ", HIGH nominal=" << (m_layers[2].nominalBitrate/1000)
            << " kbps, GCC ceiling=" << (m_maxBitrate/1000) << " kbps";
        // 0.41.6-beta — capture the chosen ceiling so the sustained-low
        // BWE auto-cap (onGccBitrate) can both clamp aggressively AND
        // restore to the original when the link recovers.
        m_originalMaxBitrate = m_maxBitrate;
        m_autoCapActive      = false;
        m_lowBweTimer.invalidate();
    }

    // Audio-source resilience (0.48.x): a microphone that won't OPEN must
    // never drop the whole call. We build the pipeline and try to PLAY it; if
    // the audio source is the reason PLAYING fails, we tear down and rebuild
    // with the next, more-defensive audio tier:
    //   tier 0 — the user's SELECTED capture device (if any)
    //   tier 1 — the SYSTEM-DEFAULT device (a stale / wrong-provider /
    //            friendly-name device id that wasapi2src can't resolve into an
    //            IMMDevice is the most common open failure — 0x80070057)
    //   tier 2 — a SILENT source, so the call still connects (peer hears
    //            silence) and CallManager shows a "microphone unavailable"
    //            banner, instead of the entire call dropping.
    int audioTier = 0;
    for (;;) {
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
        // TALQ_TEST_AUDIO_VOL lets the AGC harness feed a known-quiet tone
        // (e.g. 0.1 ≈ -20 dBFS peak) so the gain boost is observable. Default
        // 0.8 matches audiotestsrc's own default (loud) for other scenarios.
        double testVol = 0.8;
        if (qEnvironmentVariableIsSet("TALQ_TEST_AUDIO_VOL"))
            testVol = qEnvironmentVariable("TALQ_TEST_AUDIO_VOL").toDouble();
        g_object_set(audiosrc, "is-live", TRUE, "wave", 0, "freq", 440.0,
                     "volume", testVol, nullptr);
        qDebug() << "PublishPipeline: audio source: audiotestsrc (TEST MODE) vol=" << testVol;
    } else if (audioTier >= 2) {
        // Tier 2 — every real capture device failed to OPEN. Substitute a
        // SILENT live source so the audio m-line still negotiates and the call
        // CONNECTS (peer hears silence) instead of dropping. CallManager shows
        // the "microphone unavailable" banner (audioError, emitted below).
        audiosrc = gst_element_factory_make("audiotestsrc", "pub-audiosrc");
        if (audiosrc)
            g_object_set(audiosrc, "is-live", TRUE, "wave", 4 /* silence */,
                         "volume", 0.0, nullptr);
        qWarning() << "PublishPipeline: audio tier 2 — SILENT source "
                      "(microphone could not be opened; call stays up)";
    } else {
        // Tier 0 = the user's selected device (if one is set). Tier 1 = system
        // default (drop the explicit device — the usual culprit is a device id
        // wasapi2src can't resolve into an IMMDevice).
        const bool useSelected = (audioTier == 0) && !audioDeviceId.isEmpty();
        audiosrc = gst_element_factory_make("wasapi2src", "pub-audiosrc");
        if (audiosrc) {
            qDebug() << "PublishPipeline: audio source: wasapi2src"
                     << (useSelected ? "(selected device)" : "(system default)");
            if (useSelected)
                g_object_set(audiosrc, "device", audioDeviceId.toUtf8().constData(), nullptr);
        } else {
            audiosrc = gst_element_factory_make("wasapisrc", "pub-audiosrc");
            if (audiosrc) {
                g_object_set(audiosrc, "low-latency", FALSE, nullptr);
                qDebug() << "PublishPipeline: audio source: wasapisrc"
                         << (useSelected ? "(selected device)" : "(system default)");
                if (useSelected)
                    g_object_set(audiosrc, "device", audioDeviceId.toUtf8().constData(), nullptr);
            } else {
                audiosrc = gst_element_factory_make("autoaudiosrc", "pub-audiosrc");
                qDebug() << "PublishPipeline: audio source: autoaudiosrc";
            }
        }
        if (useSelected)
            qDebug() << "PublishPipeline: using audio input device" << audioDeviceId;
    }

    GstElement *audioconvert = gst_element_factory_make("audioconvert", nullptr);
    GstElement *audioresample = gst_element_factory_make("audioresample", nullptr);
    GstElement *level = gst_element_factory_make("level", "pub-level");
    GstElement *opusenc = gst_element_factory_make("opusenc", nullptr);
    GstElement *rtpopuspay = gst_element_factory_make("rtpopuspay", "pub-rtpopuspay");

    // 0.41.0-beta — explicit Opus voice-mode + FEC, the tgcalls recipe.
    // GStreamer default is bitrate=64000, audio-type=generic, inband-fec=false.
    // Voice mode picks the SILK-hybrid path that Telegram / Discord / Meet
    // use for live calls; FEC restores quietly-attenuated frames on 1-2%
    // packet loss; dtx=FALSE keeps the silence-suppression OFF so peers
    // don't read inter-sentence gaps as drop-outs. complexity=10 is the
    // best-quality setting at negligible CPU cost on modern hardware.
    if (opusenc) {
        g_object_set(opusenc,
                     "bitrate",       48000,
                     "inband-fec",    TRUE,
                     "dtx",           FALSE,
                     "complexity",    10,
                     nullptr);
        gst_util_set_object_arg(G_OBJECT(opusenc), "audio-type",   "voice");
        gst_util_set_object_arg(G_OBJECT(opusenc), "bitrate-type", "cbr");
    }

    if (!audiosrc || !audioconvert || !audioresample || !level || !opusenc || !rtpopuspay) {
        emit error("Failed to create audio capture elements");
        cleanup();
        return false;
    }

    // Configure level element: report every 100ms
    g_object_set(level, "post-messages", TRUE, "interval", (guint64)100000000, nullptr);

    // 0.51.x AEC single-pipeline fix: build the inline playout tail (probe +
    // shared real wasapi2sink) in THIS pipeline BEFORE webrtcdsp is configured,
    // so the webrtcechoprobe is present for the dsp's probe lookup (the single
    // pipeline's two-phase NULL→READY→PAUSED registers the probe before the dsp
    // acquires it) and a tail-build failure degrades to AEC-off instead of a
    // "No echo probe" call drop.
    if (m_aecEnabled && !buildFarEndTail()) {
        qWarning() << "PublishPipeline: AEC playout tail failed — disabling AEC for this call";
        m_aecEnabled = false;
    }

    // Optional WebRTC DSP (webrtcdsp): noise suppression and/or automatic gain
    // control on the capture leg. Each is independently gated by its own
    // setting (both default ON); the element is created if EITHER is enabled
    // and the plugin is present. If the plugin is missing we fall back to the
    // plain chain so calls still work on installs without libgstwebrtcdsp.
    GstElement *webrtcdsp = nullptr;
    GstElement *aecCaps = nullptr;   // 48k/S16LE/mono pin before webrtcdsp (AEC only)
    {
        QSettings s("TalQ", "TalQ");
        s.beginGroup("Audio");
        const bool nsEnabled = s.value("noiseSuppression", true).toBool();
        bool agcEnabled      = s.value("audioAutoGainControl", true).toBool();
        s.endGroup();
        // Deterministic override for the TALQ_TEST_AUDIO_AGC harness: force AGC
        // on ("1") or off ("0") regardless of the persisted setting so the test
        // doesn't depend on whatever the dev machine last saved.
        if (qEnvironmentVariableIsSet("TALQ_FORCE_AGC"))
            agcEnabled = (qgetenv("TALQ_FORCE_AGC") != "0");
        // echo-cancel (AEC) is driven by CallManager, NOT this setting block:
        // m_aecEnabled is only true once the shared webrtcechoprobe bus is
        // already PLAYING (SharedFarEndBus, brought up before this publisher).
        // That ordering is what makes gst_webrtc_dsp_start succeed instead of
        // failing "No echo probe found" and dropping the call. See
        // docs/aec-design.md. If m_aecEnabled is false, echo-cancel stays off.
        const bool aecEnabled = m_aecEnabled;
        if (nsEnabled || agcEnabled || aecEnabled) {
            webrtcdsp = gst_element_factory_make("webrtcdsp", "pub-webrtcdsp");
            if (webrtcdsp) {
                // Same config helper the AGC harness (talq-agc-test) exercises.
                talq::configureCaptureDsp(webrtcdsp, nsEnabled, agcEnabled,
                                          aecEnabled, "talq-aec-probe");
                qInfo() << "PublishPipeline: webrtcdsp NS=" << nsEnabled
                        << "AGC=" << agcEnabled << "AEC=" << aecEnabled;
            } else {
                qWarning() << "PublishPipeline: webrtcdsp unavailable; "
                              "continuing without NS/AGC/AEC";
            }
        }

        // AEC needs the capture leg pinned to the probe's exact format
        // (48k/S16LE/MONO — mono also avoids the wap-1.x stereo-collapse bug).
        // Inserted between audioresample and webrtcdsp only when AEC is on.
        if (aecEnabled && webrtcdsp) {
            aecCaps = gst_element_factory_make("capsfilter", "pub-aec-caps");
            if (aecCaps) {
                GstCaps *ac = gst_caps_from_string(
                    "audio/x-raw,rate=48000,format=S16LE,channels=1,layout=interleaved");
                g_object_set(aecCaps, "caps", ac, nullptr);
                gst_caps_unref(ac);
            }
        }
    }

    if (webrtcdsp && aecCaps) {
        // AEC chain: …audioresample → aecCaps(48k/S16LE/mono) → webrtcdsp → level…
        gst_bin_add_many(GST_BIN(m_pipeline), audiosrc, audioconvert, audioresample,
                         aecCaps, webrtcdsp, level, opusenc, rtpopuspay, m_webrtcbin, nullptr);
        if (!gst_element_link_many(audiosrc, audioconvert, audioresample,
                                   aecCaps, webrtcdsp, level, opusenc, rtpopuspay, nullptr)) {
            emit error("Failed to link audio capture chain");
            cleanup();
            return false;
        }
    } else if (webrtcdsp) {
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
        // Camera-off "mute" dummy. Upstream NC Talk (spreed v23.0.4 +
        // libwebrtc BlackVideoEnforcer) feeds the sender a CAMERA-
        // RESOLUTION black canvas at ~10 fps for the first 5 s of mute,
        // then halts. We don't know the actual camera resolution at
        // start() time (enableCamera selects it later), so we use
        // upstream's documented fallback: 640x480 @ 10 fps. The 5 s
        // halt timer below closes the valve so RTP genuinely goes
        // silent after the grace window — the wire-state portion of
        // the upstream conformance fix. Previously 16x16 @ 1 fps was
        // shipped, which let GCC + jitterbuffer converge to dummy-
        // stream parameters and produced the deterministic asymmetric
        // callee-mid-call-camera-on chop (#111 / #135).
        GstCaps *lowCaps = gst_caps_from_string(
            "video/x-raw,width=640,height=480,framerate=10/1");
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
        // 0.41.0-beta — sharedCaps tracks the HIGH layer resolution so
        // the source's full pixel count survives into the encoder branch
        // instead of being downscaled to 720 unconditionally.
        const int sw = m_layers[2].targetW;
        const int sh = m_layers[2].targetH;
        const QString sc = QStringLiteral(
            "video/x-raw,width=%1,height=%2,pixel-aspect-ratio=1/1,framerate=30/1")
            .arg(sw).arg(sh);
        GstCaps *caps = gst_caps_from_string(sc.toUtf8().constData());
        g_object_set(m_sharedCaps, "caps", caps, nullptr);
        gst_caps_unref(caps);
        qDebug() << "PublishPipeline: sharedCaps =" << sc;
    }
    gst_bin_add_many(GST_BIN(m_pipeline),
                     sharedConvert, m_sharedScale, sharedRate, m_sharedCaps,
                     m_outputTee, nullptr);
    if (!gst_element_link_many(m_funnel, sharedConvert, m_sharedScale,
                                sharedRate, m_sharedCaps, m_outputTee, nullptr)) {
        emit error("Failed to link shared chain"); cleanup(); return false;
    }

    // Simulcast in BOTH stable and beta as of 0.38.0 — the SIM-group SDP
    // munge (#9) is now harness-verified end-to-end across all three
    // substream levels (180p / 360p / 720p), the rid header extension is
    // confirmed on the wire (#15 probe), and substream switching propagates
    // through HPB+Janus correctly (TALQ_TEST_SELECT_SUBSTREAM scenarios
    // PASS). The earlier TALQ_PRERELEASE gate was a pre-test-coverage
    // safety margin; the tests have done its job.
    // Test bisection (TALQ_TEST_SINGLE_STREAM=1): publish ONE stream (no
    // simulcast, no SIM ssrc-group, no per-layer keyframe routing) to isolate
    // whether the simulcast layering is what a strict receiver (official NC
    // Talk Android/web) chokes on -> "old TV" static. #android-static 2026-06-02.
    m_simulcast = !qEnvironmentVariableIsSet("TALQ_TEST_SINGLE_STREAM");
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
                                   L.encoder, L.parser, L.payloader, L.profileCaps })
                if (e) gst_object_unref(e);
            L.valve = L.scale = L.caps = L.ssrcFilter = nullptr;
            L.encoder = L.parser = L.payloader = L.profileCaps = nullptr;
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

        // H264: pin the encoder output to Constrained Baseline -- the profile the
        // official NC Talk / libwebrtc client uses (profile-level-id 42e01f). The
        // HW encoders (qsv/nv/mf) default to High/Main, which strict receivers
        // can't decode -> black video on the official client. `profile` is a caps
        // field (not a settable property), so constrain it with a capsfilter on
        // the encoder src. (#android-black-video, 2026-06-01.)
        if (m_useH264) {
            L.profileCaps = gst_element_factory_make(
                "capsfilter", ("pub-profilecaps-" + tag).toUtf8().constData());
            if (L.profileCaps) {
                GstCaps *pc = gst_caps_from_string(
                    "video/x-h264,profile=constrained-baseline");
                g_object_set(L.profileCaps, "caps", pc, nullptr);
                gst_caps_unref(pc);
            }
        }

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

        // valve open by default — BWE may close 'h' / 'm' later. On a weak
        // encode tier the HIGH layer (index 2) is shed up front via
        // m_loadCapMaxLayer; start a capped layer's valve dropped so no frames
        // ever reach its encoder (saves the iGPU the per-frame encode work).
        const bool layerAllowed = ((int)i <= effectiveLayerCeiling());
        g_object_set(L.valve, "drop", layerAllowed ? FALSE : TRUE, nullptr);
        L.active = layerAllowed;

        // H264 niceties (mirrors the single-stream path)
        if (m_useH264 && L.parser)
            g_object_set(L.parser, "config-interval", -1, nullptr);
        if (m_useH264)
            // aggregate-mode=0 (none): emit ONE NAL per RTP packet, no STAP-A
            // aggregation. zero-latency(1) still STAP-A-aggregates SPS+PPS+slice
            // into one packet, which strict libwebrtc depacketizers (official NC
            // Talk Android/web) snow on while GStreamer tolerates it. config-
            // interval=-1 keeps SPS/PPS on every IDR. #android-static 2026-06-02.
            g_object_set(L.payloader,
                         "aggregate-mode", 0 /* none -- max interop */,
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
        if (L.profileCaps) gst_bin_add(GST_BIN(m_pipeline), L.profileCaps);

        // outputTee → valve → scale → caps → encoder → (profileCaps →)(parser →) payloader → ssrcFilter → rtpfunnel
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
            // encoder -> [profileCaps=constrained-baseline] -> parser -> payloader
            const bool linked = L.profileCaps
                ? gst_element_link_many(L.encoder, L.profileCaps, L.parser,
                                        L.payloader, L.ssrcFilter, nullptr)
                : gst_element_link_many(L.encoder, L.parser,
                                        L.payloader, L.ssrcFilter, nullptr);
            if (!linked) {
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

        // 0.51.x encode-load probe: time each frame's encode (sink→src) into the
        // shared cell. Each probe carries its own heap ctx (freed by the pad's
        // destroy-notify) holding a shared_ptr to the cell — never `this`.
        if (L.encoder) {
            if (GstPad *esink = gst_element_get_static_pad(L.encoder, "sink")) {
                gst_pad_add_probe(esink, GST_PAD_PROBE_TYPE_BUFFER, onEncodeProbe,
                                  new EncProbeCtx{ m_encodeLoad, (int)i, false },
                                  [](gpointer p){ delete static_cast<EncProbeCtx *>(p); });
                gst_object_unref(esink);
            }
            if (GstPad *esrc = gst_element_get_static_pad(L.encoder, "src")) {
                gst_pad_add_probe(esrc, GST_PAD_PROBE_TYPE_BUFFER, onEncodeProbe,
                                  new EncProbeCtx{ m_encodeLoad, (int)i, true },
                                  [](gpointer p){ delete static_cast<EncProbeCtx *>(p); });
                gst_object_unref(esrc);
            }
        }
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

    // #15 diagnostic — print the rid RTP header extension (id=2) on the
    // first 30 packets leaving rtpfunnel.src. Janus's runtime
    // rid->substream map is populated from this extension; if it's
    // missing on the wire no SDP munge or upstream patch can rescue
    // substream switching. Auto-silences after 30 packets so it's safe
    // to leave in builds. Simulcast only.
    if (m_simulcast) {
        if (GstPad *funnelSrc = gst_element_get_static_pad(rtpFunnel, "src")) {
            gst_pad_add_probe(funnelSrc, GST_PAD_PROBE_TYPE_BUFFER,
                [](GstPad *, GstPadProbeInfo *info, gpointer) -> GstPadProbeReturn {
                    static std::atomic<int> logged{0};
                    if (logged.load(std::memory_order_relaxed) >= 30)
                        return GST_PAD_PROBE_OK;
                    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
                    if (!buf) return GST_PAD_PROBE_OK;
                    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
                    if (!gst_rtp_buffer_map(buf, GST_MAP_READ, &rtp))
                        return GST_PAD_PROBE_OK;
                    if (logged.fetch_add(1, std::memory_order_relaxed) < 30) {
                        guint32 ssrc = gst_rtp_buffer_get_ssrc(&rtp);
                        gpointer ridData = nullptr;
                        guint ridSize = 0;
                        gboolean hasRid = gst_rtp_buffer_get_extension_onebyte_header(
                            &rtp, 2, 0, &ridData, &ridSize);
                        gpointer midData = nullptr;
                        guint midSize = 0;
                        gboolean hasMid = gst_rtp_buffer_get_extension_onebyte_header(
                            &rtp, 1, 0, &midData, &midSize);
                        QByteArray ridVal;
                        if (hasRid && ridData && ridSize > 0)
                            ridVal = QByteArray(static_cast<char*>(ridData), ridSize);
                        QByteArray midVal;
                        if (hasMid && midData && midSize > 0)
                            midVal = QByteArray(static_cast<char*>(midData), midSize);
                        qInfo().nospace()
                            << "PublishPipeline #15: rtpfunnel.src ssrc=0x"
                            << QString::number(ssrc, 16)
                            << " ext1(mid)=" << (hasMid ? midVal : QByteArray("MISSING"))
                            << " ext2(rid)=" << (hasRid ? ridVal : QByteArray("MISSING"));
                    }
                    gst_rtp_buffer_unmap(&rtp);
                    return GST_PAD_PROBE_OK;
                }, nullptr, nullptr);
            gst_object_unref(funnelSrc);
        }
    }

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
        if (m_useH264) {
            // WebRTC H264 interop (#android-black-video, 2026-06-01): advertise
            // packetization-mode=1 + profile-level-id=42e01f (Constrained Baseline
            // 3.1) so STRICT libwebrtc receivers (official NC Talk Android/web)
            // can depacketize AND decode our stream. Without the fmtp, webrtcbin
            // emits NO a=fmtp:96; the official client assumes mode 0 + (per RFC
            // 6184) baseline, but our HW encoder emitted FU-A mode-1 High/Main
            // -> can't depacketize/decode -> black VIDEO TILE. The encoder is now
            // pinned to constrained-baseline (above) so the stream MATCHES this
            // advertised profile. GStreamer receivers are lenient (auto-detect),
            // which is why TalQ<->TalQ always worked and this stayed invisible
            // until the first TalQ<->Android test.
            // Browser/libwebrtc H264 standard fmtp (researched 2026-06-02):
            // profile-level-id=42e01f (Constrained Baseline, the ONLY profile
            // browsers publicly claim) + packetization-mode=1 +
            // level-asymmetry-allowed=1 (lets the receiver accept a higher
            // transmit level than 3.1, covering our level-4.0 SPS). The encoder
            // must produce a CONFORMANT constrained-baseline bitstream to match.
            gst_caps_set_simple(vc,
                "packetization-mode",      G_TYPE_STRING, "1",
                "profile-level-id",        G_TYPE_STRING, "42e01f",
                "level-asymmetry-allowed", G_TYPE_STRING, "1",
                nullptr);
        }
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
        QString detail = QStringLiteral("Failed to start publish pipeline");
        bool audioCulprit = false;
        bool farSinkCulprit = false;   // 0.51.x AEC: the shared playout wasapi2sink
        if (errMsg) {
            GError *err = nullptr;
            gchar *dbg = nullptr;
            gst_message_parse_error(errMsg, &err, &dbg);
            detail = QString("%1 (%2)").arg(err->message, dbg ? dbg : "no details");
            // Was the AUDIO SOURCE the reason PLAYING failed? (mic busy,
            // OS-blocked, unplugged, or a device id wasapi2 can't resolve).
            // If so this is NOT fatal — fall the mic back a tier and rebuild,
            // so a bad microphone never drops the whole call.
            GstObject *src = GST_MESSAGE_SRC(errMsg);
            if (src) {
                if (audiosrc && src == GST_OBJECT(audiosrc)) {
                    audioCulprit = true;
                } else if (gchar *sn = gst_object_get_name(src)) {
                    if (g_strcmp0(sn, "pub-far-sink") == 0)  farSinkCulprit = true; // AEC playout sink
                    else audioCulprit = (g_strcmp0(sn, "pub-audiosrc") == 0);
                    g_free(sn);
                }
            }
            qWarning() << "PublishPipeline: GStreamer error:" << detail
                       << (audioCulprit ? "[audio source]" : "");
            g_clear_error(&err);
            g_free(dbg);
            gst_message_unref(errMsg);
        }
        gst_object_unref(bus);
        cleanup();
        if (farSinkCulprit) {
            // 0.51.x AEC: the shared playout sink (real wasapi2sink) failed to open
            // — must NOT drop the call (mirrors the old SharedFarEndBus "AEC never
            // drops the call" guarantee). Disable AEC and rebuild; buildFarEndTail()
            // is gated on m_aecEnabled, so the retry has no far sink/probe and the
            // publisher comes up cleanly AEC-off.
            m_aecEnabled = false;
            qWarning() << "PublishPipeline: AEC playout sink failed to open — "
                          "rebuilding without echo cancellation (call stays up)";
            continue;
        }
        if (audioCulprit && audioTier < 2) {
            const int prev = audioTier;
            // Skip the redundant default-device retry when no device was
            // selected (tier 0 was already the default): jump straight to
            // the silent fallback.
            audioTier = (audioTier == 0 && audioDeviceId.isEmpty()) ? 2
                                                                    : audioTier + 1;
            qWarning().nospace()
                << "PublishPipeline: microphone open failed at tier " << prev
                << " — rebuilding at tier " << audioTier << " (call stays up)";
            continue;   // rebuild the publisher with the next audio tier
        }
        emit error(detail);
        return false;
    }

    // The microphone could not be opened on ANY real device; we connected with
    // a SILENT source so the call survives. Tell CallManager so it can show the
    // "microphone unavailable" banner (non-fatal — mirrors cameraError). A
    // successful tier-1 fall-back to the default device is NOT surfaced: the
    // mic works, it's just a different device than the one selected.
    if (audioTier >= 2) {
        qWarning() << "PublishPipeline: connected with SILENT audio — "
                      "microphone unavailable, call continues";
        emit audioError(QStringLiteral("Microphone unavailable"));
    } else if (audioTier == 1) {
        qInfo() << "PublishPipeline: connected on the system-default microphone "
                   "(the selected capture device could not be opened)";
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
    }   // for (;;) audio-tier retry — only the audio-open failure path loops
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
    if (m_bgAppsink)
        g_signal_handlers_disconnect_by_data(m_bgAppsink, this);

    if (m_statusDataChannel) {
        g_object_unref(m_statusDataChannel);
        m_statusDataChannel = nullptr;
    }

    if (m_pipeline) {
        // 0.40.9 — detach + NULL the pipeline on a worker thread. The
        // synchronous set_state(NULL) waits on every pad's stream lock,
        // and when BG-blur is on the bg-bridge appsink can hold one of
        // those locks for the duration of an in-flight engine round-trip
        // (GL + ORT). Qt main blocking on that = UI non-responding +
        // black PiP. The pointer is nulled BEFORE the thread starts so
        // re-entrant callers see no pipeline; the thread owns the local
        // ref and unrefs after the state transition.
        GstElement *pipe = m_pipeline;
        m_pipeline = nullptr;
        qDebug() << "PublishPipeline::cleanup() — scheduling pipeline NULL+unref on worker";
        std::thread([pipe]() {
            gst_element_set_state(pipe, GST_STATE_NULL);
            gst_object_unref(pipe);
        }).detach();
    }
    // Pipeline owns all elements — just null out the pointers
    m_webrtcbin = nullptr;
    m_funnel = nullptr;
    m_sharedScale = nullptr;
    m_sharedCaps = nullptr;
    m_gccbwe = nullptr;
    m_outputTee = nullptr;
    // 0.51.x AEC playout tail (owned by m_pipeline — just null the handles).
    m_farMixer = m_farProbe = m_farSink = nullptr;
    m_farPeerAppsrcs.clear();
    for (auto &L : m_layers) {
        L.valve = L.scale = L.caps = L.encoder = L.parser
              = L.profileCaps = L.payloader = L.ssrcFilter = nullptr;
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
    m_bgAppsink = nullptr;     // #20 Phase 3.3b — must null with the rest
    m_bgAppsrc  = nullptr;     // or a second start() dereferences garbage
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
    // 1.0 audit — reject a malformed answer instead of handing a partially-parsed
    // SDP to webrtcbin (mirrors SubscribePipeline::setRemoteOffer). Free the
    // message on failure so it isn't leaked either.
    if (gst_sdp_message_parse_buffer((const guint8 *)sdpUtf8.constData(),
                                     sdpUtf8.size(), sdpMsg) != GST_SDP_OK) {
        qWarning() << "PublishPipeline: failed to parse remote answer SDP — ignoring";
        gst_sdp_message_free(sdpMsg);
        return;
    }

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
    m_bgAppsink       = gst_element_factory_make("appsink", "bg-appsink");
    m_bgAppsrc        = gst_element_factory_make("appsrc",  "bg-appsrc");
    m_tee             = gst_element_factory_make("tee", "camera-tee");
    m_encQueue        = gst_element_factory_make("queue", "enc-queue");
    m_cameraValve     = gst_element_factory_make("valve", "camera-valve");
    m_previewQueue    = gst_element_factory_make("queue", "preview-queue");
    m_previewConvert  = gst_element_factory_make("videoconvert", "preview-convert");
    m_previewAppsink  = gst_element_factory_make("appsink", "preview-sink");

    if (!m_camSrcCaps || !m_camDecode ||
        !m_videoConvert || !m_videoCapsFilter || !m_bgAppsink || !m_bgAppsrc ||
        !m_tee || !m_encQueue ||
        !m_cameraValve || !m_previewQueue || !m_previewConvert || !m_previewAppsink) {
        qWarning() << "PublishPipeline: failed to create camera branch elements";
        // Clean up any elements that were created (not yet added to pipeline)
        auto freeIf = [](GstElement *&el) { if (el) { gst_object_unref(el); el = nullptr; } };
        freeIf(m_cameraSrc);
        freeIf(m_camSrcCaps);
        freeIf(m_camDecode);
        freeIf(m_videoConvert);
        freeIf(m_videoCapsFilter);
        freeIf(m_bgAppsink);
        freeIf(m_bgAppsrc);
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
    // Post-decode capsfilter — Phase 3.3b pins BGRx so the BG bridge has
    // a consistent input format to map without per-frame caps checks.
    // The shared chain downstream (sharedConvert) re-formats to whatever
    // the encoder wants (NV12/I420); BGRx → encoder via convert is the
    // same hop the pre-3.3b chain used implicitly.
    {
        GstCaps *raw = gst_caps_from_string("video/x-raw,format=BGRx");
        g_object_set(m_videoCapsFilter, "caps", raw, nullptr);
        gst_caps_unref(raw);
        qDebug() << "PublishPipeline: camera caps: BGRx (BG bridge needs "
                    "consistent input); sharedConvert handles encoder format";
    }
    // #20 Phase 3.3b — BG appsink + appsrc. The sink pulls BGRx samples;
    // the callback decides per-frame whether to engine-process (mode !=
    // None) or zero-copy push-through (mode == None). The src receives
    // BGRx buffers with the same PTS+duration. emit-signals on the sink
    // so the C++ callback fires; is-live + format=time + block=false on
    // the src so it integrates with the live camera clock.
    {
        GstCaps *bgrxCaps = gst_caps_from_string("video/x-raw,format=BGRx");
        g_object_set(m_bgAppsink,
            "emit-signals", TRUE,
            "caps",  bgrxCaps,
            "drop",  TRUE,
            "max-buffers", 2,
            "sync",  FALSE,    // streaming-thread already paced by the camera
            "async", FALSE,    // 0.40.13 — RCA: see preview-appsink below
            nullptr);
        g_object_set(m_bgAppsrc,
            "caps",   bgrxCaps,
            "is-live", TRUE,
            "format", GST_FORMAT_TIME,
            "block",   FALSE,
            "stream-type", 0,   // GST_APP_STREAM_TYPE_STREAM
            // 0.51.x OOM/FREEZE FIX. This was max-bytes=0 (UNBOUNDED) on the
            // assumption that downstream always drains it. The 0.51.0 encode-load
            // controller broke that assumption: when it throttles the encoders
            // (sheds layers / drops fps), the encoder branch stops consuming and
            // every captured camera frame piled into this appsrc FOREVER — the
            // [LEAK] heartbeat showed bgQ growing ~108MB/s (one 720p frame per
            // camera frame) → native memory exhaustion → the machine froze. Bound
            // the queue to a few frames and DROP THE OLDEST when full (a stalled
            // encoder must cost dropped frames, never unbounded memory). The
            // healthy path never hits the bound (downstream keeps it near-empty).
            "max-bytes", gint64(32 * 1024 * 1024),   // ~4×1080p / ~8×720p BGRx
            "leaky-type", 2 /* GST_APP_LEAKY_TYPE_DOWNSTREAM — drop OLDEST when full */,
            "do-timestamp", FALSE,    // we copy PTS from the input sample
            nullptr);
        gst_caps_unref(bgrxCaps);
        g_signal_connect(m_bgAppsink, "new-sample",
            G_CALLBACK(onBgSample), this);
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
            // 0.40.13 — async=FALSE. BaseSink defaults to async=TRUE,
            // meaning the sink REQUIRES a preroll buffer in PAUSED
            // before allowing the pipeline to reach PLAYING. With the
            // BG bridge in the chain (appsink → callback → appsrc),
            // preroll creates a circular dependency: bg-appsink emits
            // "new-preroll" (not "new-sample") in PAUSED, our
            // onBgSample callback hooks ONLY "new-sample", so no
            // buffer ever gets pushed to bg-appsrc, so preview-appsink
            // never receives its preroll buffer, so the pipeline stays
            // pending=PLAYING forever and preview-appsink stays in
            // READY → no PiP, no remote video.
            // Setting async=FALSE makes the sink transition directly
            // to PLAYING without waiting for a preroll buffer. RCA'd
            // 2026-05-27 via TALQ_DEBUG_PIPELINE state dumps at t+200ms,
            // t+1s, t+3s — preview-appsink stayed READY across all.
            "async", FALSE,
            nullptr);
        gst_caps_unref(previewCaps);
        g_signal_connect(m_previewAppsink, "new-sample",
            G_CALLBACK(onPreviewSample), this);
    }

    // Add all camera elements to pipeline
    gst_bin_add_many(GST_BIN(m_pipeline),
        m_cameraSrc, m_camSrcCaps, m_camDecode,
        m_videoConvert, m_videoCapsFilter,
        m_bgAppsink, m_bgAppsrc,    // #20 Phase 3.3b bridge
        m_tee,
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
    // #20 Phase 3.3b — break the capsfilter→tee link and insert the BG
    // bridge: capsfilter→bgAppsink (consumer in onBgSample) … bgAppsrc→tee.
    // Frames cross through the C++ callback whether or not the engine is
    // active; with Off-mode the callback push-throughs the same buffer so
    // there's a one-shot extra memcpy across the bridge boundary, which
    // is negligible at 720p30 (≤ 70 MB/s, well under the existing convert
    // throughput on the same thread).
    linked = linked && gst_element_link_many(m_videoConvert, m_videoCapsFilter, m_bgAppsink, nullptr);
    linked = linked && gst_element_link_many(m_bgAppsrc, m_tee, nullptr);

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
    // #20 Phase 3.3b — the BG bridge sits between capsfilter and tee.
    // Without these two syncs, the appsink/appsrc stay in NULL state
    // (we bin-add them during enableCamera, well after start() has
    // already promoted the pipeline to PLAYING) and the camera feed
    // never reaches the encoder branch. Shipped broken in 0.39.1-0.39.3
    // — caught in the post-ship code review.
    gst_element_sync_state_with_parent(m_bgAppsink);
    gst_element_sync_state_with_parent(m_bgAppsrc);
    gst_element_sync_state_with_parent(m_tee);
    gst_element_sync_state_with_parent(m_encQueue);
    gst_element_sync_state_with_parent(m_cameraValve);
    gst_element_sync_state_with_parent(m_previewQueue);
    gst_element_sync_state_with_parent(m_previewConvert);
    gst_element_sync_state_with_parent(m_previewAppsink);
    // 0.40.13 — RCA showed the pipeline was stuck in PAUSED at this
    // point (instrumented dump after enableCamera+200ms: m_pipeline
    // cur=PAUSED pending=PAUSED, every downstream element PAUSED).
    // start()'s initial set_state(PLAYING) had returned NO_PREROLL /
    // ASYNC and the pipeline never made it to PLAYING — so every
    // child added later inherited PAUSED via sync_state_with_parent,
    // and PAUSED appsinks don't emit `new-sample`. Re-issuing
    // set_state(pipeline, PLAYING) here forces the cascade through
    // the just-added camera chain. Cheap when already PLAYING.
    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    // Start camera source LAST and async — mfvideosrc COM init blocks ~1s
    gst_element_set_state(m_cameraSrc, GST_STATE_PLAYING);

    // Pipeline-state RCA dump. Runtime-gated on env var TALQ_DEBUG_PIPELINE
    // (e.g. `set TALQ_DEBUG_PIPELINE=1 & talq.exe`). When off (the
    // default in shipped builds) the std::getenv check costs nothing
    // meaningful and the timers are never armed. Dumps states at
    // 200/1000/3000 ms so we can see whether the pipeline converges.
    // Kept in source permanently as a reusable diagnostic.
    if (const char *e = std::getenv("TALQ_DEBUG_PIPELINE"); e && *e == '1') {
        auto schedule = [this](int delayMs, const char *label) {
            QTimer::singleShot(delayMs, this, [this, label]() {
                FILE *f = std::fopen("C:/temp/talq-pipe.log", "a");
                if (!f) return;
                auto dump = [f](const char *name, GstElement *el) {
                    if (!el) { std::fprintf(f, "  %-18s = NULL pointer\n", name); return; }
                    GstState cur = GST_STATE_VOID_PENDING, pending = GST_STATE_VOID_PENDING;
                    gst_element_get_state(el, &cur, &pending, 0);
                    static const char *names[] = {"VOID","NULL","READY","PAUSED","PLAYING"};
                    int ci = (cur     >= 0 && cur     < 5) ? (int)cur     : 0;
                    int pi = (pending >= 0 && pending < 5) ? (int)pending : 0;
                    std::fprintf(f, "  %-18s cur=%-7s pending=%s\n", name, names[ci], names[pi]);
                };
                std::fprintf(f, "=== enableCamera+%s states ===\n", label);
                dump("m_pipeline",       m_pipeline);
                dump("m_cameraSrc",      m_cameraSrc);
                dump("m_camSrcCaps",     m_camSrcCaps);
                dump("m_camDecode",      m_camDecode);
                dump("m_videoConvert",   m_videoConvert);
                dump("m_videoCapsFilter",m_videoCapsFilter);
                dump("m_bgAppsink",      m_bgAppsink);
                dump("m_bgAppsrc",       m_bgAppsrc);
                dump("m_tee",            m_tee);
                dump("m_encQueue",       m_encQueue);
                dump("m_cameraValve",    m_cameraValve);
                dump("m_previewQueue",   m_previewQueue);
                dump("m_previewConvert", m_previewConvert);
                dump("m_previewAppsink", m_previewAppsink);
                std::fclose(f);
            });
        };
        schedule(200,  "200ms");
        schedule(1000, "1s");
        schedule(3000, "3s");
    }

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

    // 2. Resume dummy source + convert, RELEASE the camera device. We
    //    drop the camera source all the way to NULL so mfvideosrc's
    //    IMFMediaSource handle is released and the hardware camera LED
    //    goes off. PAUSED would only stop streaming, leaving the device
    //    open and the LED on - which users read as "they can still see
    //    me" even though TalQ has muted video. The COM-init cost
    //    (~1 s) is paid again on the next enableCamera, but mute-toggle
    //    isn't frame-rate-critical and the visible LED-off feedback is
    //    worth the latency.
    gst_element_set_state(m_dummySrc, GST_STATE_PLAYING);
    gst_element_set_state(m_dummyConv, GST_STATE_PLAYING);
    // 0.40.9 — schedule the camera-source NULL transition on a worker.
    // mfvideosrc -> NULL can park for seconds waiting on the source pad's
    // stream lock, which a streaming thread holds while it's inside
    // onBgSample → BackgroundEngine::processFrame (BG-blur ON path). Qt
    // main waiting on that lock = UI non-responding and PiP black.
    // gst_element_call_async runs our state change on GStreamer's
    // element-pool thread; the camera LED still goes off, just a beat
    // after the user clicks hang-up.
    if (m_cameraSrc) {
        gst_element_call_async(m_cameraSrc,
            [](GstElement *elem, gpointer) {
                gst_element_set_state(elem, GST_STATE_NULL);
            },
            nullptr, nullptr);
    }

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
            } else if (m_cameraSrc && src == GST_OBJECT(m_cameraSrc)) {
                // The camera SOURCE failed to start (e.g. MediaFoundation
                // "No available video capture device" -- camera missing,
                // busy in another app, or blocked by Windows privacy).
                // NOT fatal to the call: audio + receive-video keep working.
                // But it must NOT stay silent -- route it to cameraError so
                // CallManager can surface "Camera unavailable" to the user
                // (otherwise the self-tile sits on "Starting camera..."
                // forever). cameraError is non-fatal (continues audio-only).
                qWarning() << "PublishPipeline: camera source ERROR --" << errMsg
                           << dbgStr << "(camera unavailable; call stays up)";
                emit cameraError(errMsg.isEmpty()
                                 ? QStringLiteral("Camera unavailable") : errMsg);
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

// #9 simulcast SDP munge — rewritten 2026-05-23 against authoritative source
// (nextcloud/spreed simplewebrtc/peer.js::mungeSdpForSimulcasting, lifted from
// janus.js, MIT). HPB has consumed this exact format from Chrome publishers
// for years, so Janus's core SDP parser will populate the SIM substream map
// and Talk JS's selectStream {substream:N} will switch layers.
//
// Why the May 22 attempts failed ("No MCU client found"):
//   * missing per-SSRC msid / mslabel / label (HPB's parser drops the publisher
//     when an a=ssrc-group:SIM block lacks the full attribute set)
//   * left a=rid / a=simulcast / extmap-2 in place, producing a rid+SIM hybrid
//     that no Talk client ever emits and HPB doesn't validate
//   * attempted to also replace the local description (webrtcbin rejects a
//     post-hoc SDP whose ssrc-group lines don't match its internal sender state)
//
// New approach: SIGNALING-ONLY munge — webrtcbin's local description stays
// exactly as webrtcbin generated it (3 rid-style RTP senders, internal state
// pristine). We rewrite ONLY the SDP string sent to HPB. The 3 layer SSRCs
// already on the wire are the ones we declare in the SIM group, so Janus's
// substream map points to real streams. No RTX/FID groups — our publisher
// doesn't emit RTX (verified — no rtprtxsend in PublishPipeline).
// 0.41.0-beta — ensure the Opus media block carries the voice-mode +
// FEC fmtp parameters so the PEER also encodes that way (our local
// `opusenc` settings only affect what WE transmit, not what arrives).
// Telegram's tgcalls sets useinbandfec=1 + usedtx=0 + maxaveragebitrate
// explicitly for the same reason. Looks for the existing `a=fmtp:<pt>`
// line for the Opus payload type and rewrites it; if missing, inserts
// one after the matching `a=rtpmap:<pt> opus/...` line.
static QString mungeOpusFmtp(const QString &sdp)
{
    const QStringList lines = sdp.split('\n');
    int opusPt = -1;
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        // a=rtpmap:111 opus/48000/2
        if (t.startsWith(QStringLiteral("a=rtpmap:"))
            && t.contains(QStringLiteral(" opus/"), Qt::CaseInsensitive)) {
            const int colon = t.indexOf(':');
            const int space = t.indexOf(' ', colon + 1);
            if (colon > 0 && space > colon) {
                opusPt = t.mid(colon + 1, space - colon - 1).toInt();
                break;
            }
        }
    }
    if (opusPt < 0) return sdp;          // no Opus → nothing to munge

    const QString fmtpKey = QStringLiteral("a=fmtp:%1 ").arg(opusPt);
    const QString rtpmapKey = QStringLiteral("a=rtpmap:%1 ").arg(opusPt);
    const QString opusParams = QStringLiteral(
        "useinbandfec=1;usedtx=0;maxaveragebitrate=48000;minptime=10;stereo=0");

    QStringList out;
    out.reserve(lines.size() + 1);
    bool replaced = false, insertedAfter = false;
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        if (!replaced && t.startsWith(fmtpKey)) {
            // Replace the params after "a=fmtp:<pt> " entirely. Keep
            // identical line endings.
            const int suffix = line.size() - t.size();   // \r\n etc.
            QString rewritten = fmtpKey + opusParams;
            if (suffix > 0) rewritten += line.right(suffix);
            out << rewritten;
            replaced = true;
            continue;
        }
        out << line;
        if (!replaced && !insertedAfter && t.startsWith(rtpmapKey)) {
            // Insert a new fmtp line directly after the rtpmap. CRLF if
            // we see one anywhere; LF otherwise.
            const bool crlf = line.endsWith('\r');
            out << QString(fmtpKey + opusParams + (crlf ? "\r" : ""));
            insertedAfter = true;
        }
    }
    return out.join('\n');
}

static QString mungeOfferForSimulcast(
        const QString &sdp, quint32 ssrcL, quint32 ssrcM, quint32 ssrcH)
{
    const QStringList lines = sdp.split('\n');
    QString cname, videoMsid;
    int ridExtmapId = -1;

    bool seenVideo = false;
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        if (t.startsWith("m=")) seenVideo = t.startsWith("m=video");
        if (cname.isEmpty() && t.startsWith("a=ssrc:")) {
            const int ci = t.indexOf("cname:");
            if (ci >= 0)
                cname = t.mid(ci + 6).section(' ', 0, 0).trimmed();
        }
        if (seenVideo && videoMsid.isEmpty() && t.startsWith("a=msid:"))
            videoMsid = t.mid(7).trimmed();
        if (seenVideo && t.contains(
                QLatin1String("urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"))) {
            // a=extmap:<id> urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id
            const QRegularExpression rx(QStringLiteral("a=extmap:(\\d+)"));
            const auto m = rx.match(t);
            if (m.hasMatch()) ridExtmapId = m.captured(1).toInt();
        }
    }
    if (cname.isEmpty()) cname = QStringLiteral("talqsim");
    if (videoMsid.isEmpty())
        videoMsid = QStringLiteral("talq-stream talq-track");
    // msid format: "<mslabel> <label>" (RFC 5576 / draft-ietf-mmusic-msid).
    const QStringList msidParts = videoMsid.split(' ');
    const QString mslabel = msidParts.value(0);
    const QString label   = msidParts.size() > 1 ? msidParts.value(1) : mslabel;

    QStringList block;
    block << QStringLiteral("a=ssrc-group:SIM %1 %2 %3\r")
                 .arg(ssrcL).arg(ssrcM).arg(ssrcH);
    for (quint32 s : { ssrcL, ssrcM, ssrcH }) {
        block << QStringLiteral("a=ssrc:%1 cname:%2\r").arg(s).arg(cname);
        block << QStringLiteral("a=ssrc:%1 msid:%2\r").arg(s).arg(videoMsid);
        block << QStringLiteral("a=ssrc:%1 mslabel:%2\r").arg(s).arg(mslabel);
        block << QStringLiteral("a=ssrc:%1 label:%2\r").arg(s).arg(label);
    }

    const QString ridExtmapLine = ridExtmapId >= 0
        ? QStringLiteral("a=extmap:%1 urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id")
              .arg(ridExtmapId)
        : QString();

    QStringList out;
    out.reserve(lines.size() + block.size());
    bool inVideo = false, inserted = false;
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        if (t.startsWith("m=")) {
            if (inVideo && !inserted) { out << block; inserted = true; }
            inVideo = t.startsWith("m=video");
        }
        if (inVideo) {
            if (t.startsWith("a=rid:") || t.startsWith("a=simulcast:"))
                continue;
            if (!ridExtmapLine.isEmpty() && t == ridExtmapLine)
                continue;
            // Drop any phantom a=ssrc / a=ssrc-group webrtcbin emitted —
            // we control the SSRC-group declaration via `block`.
            if (t.startsWith("a=ssrc:") || t.startsWith("a=ssrc-group:"))
                continue;
        }
        out << line;
    }
    if (inVideo && !inserted) {
        // Video is the last m-section. webrtcbin's SDP ends with the SDP
        // terminator (trailing blank line); appending the SIM block AFTER
        // that blank line breaks HPB's parser (RFC 4566: no blank lines
        // mid-SDP). Strip trailing empties first.
        while (!out.isEmpty() && out.last().trimmed().isEmpty())
            out.removeLast();
        out << block;
    }
    QString result = out.join('\n');
    // Ensure proper SDP terminator (final CRLF).
    if (!result.endsWith('\n')) result.append('\n');
    return result;
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

    // #9 simulcast SIM-group munge (signaling-only). Janus's videoroom needs
    // a=ssrc-group:SIM in the offer to build the substream map; rid-only
    // offers leave the map empty and selectStream{substream:N} no-ops.
    // Local description stays untouched (webrtcbin's internal RTP sender
    // state must match what it generated); we rewrite only the wire SDP.
    if (self->m_simulcast && self->m_layers.size() >= 3) {
        const quint32 sL = self->m_layers[0].ssrc;
        const quint32 sM = self->m_layers[1].ssrc;
        const quint32 sH = self->m_layers[2].ssrc;
        const QString mungedSdp = mungeOfferForSimulcast(sdp, sL, sM, sH);
        if (mungedSdp.isEmpty() || !mungedSdp.contains("a=ssrc-group:SIM")) {
            // Silently shipping the un-munged offer here would put the
            // publisher back into the exact pre-fix state: rid-only offer,
            // empty substream map at Janus, selectStream no-op, every
            // receiver stuck at 180p — but with no visible error so the
            // user wouldn't know why their quality is degraded. Surface it.
            const QString why = QStringLiteral(
                "Simulcast SDP munge produced %1 — refusing to publish "
                "rid-only offer (would silently break substream switching). "
                "Please report this build.")
                .arg(mungedSdp.isEmpty() ? "empty output" : "no SIM line");
            qWarning().noquote() << "PublishPipeline #9 FATAL:" << why;
            // Emit error on the Qt thread and bail before localOfferReady
            // fires — better to fail the call than degrade silently.
            QPointer<PublishPipeline> gd(self);
            QMetaObject::invokeMethod(self, [gd, why]() {
                if (gd) emit gd->error(why);
            }, Qt::QueuedConnection);
            return;
        }
        qInfo().nospace() << "PublishPipeline #9: signaling munged offer "
                          << "with SIM(" << sL << "," << sM << "," << sH
                          << ") — length " << sdp.length() << " -> "
                          << mungedSdp.length();
        sdp = mungedSdp;
    }

    // 0.41.0-beta — Opus voice-mode fmtp munge (applies whether or not
    // simulcast is on). Idempotent and harmless if Opus isn't offered.
    sdp = mungeOpusFmtp(sdp);

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

namespace {
// Heap context for the publisher get-stats promise: ONLY a copy of the
// shared-ptr packets-sent cell — no `self` pointer. The async reply callback
// writes the cell and never touches the PublishPipeline, so a late reply that
// races teardown is harmless (the cell outlives the pipeline). Freed by the
// promise's GDestroyNotify.
struct PubStatsCtx {
    std::shared_ptr<std::atomic<quint64>> cell;
};
void pubStatsCtxFree(gpointer p) { delete static_cast<PubStatsCtx *>(p); }
} // namespace

void PublishPipeline::pollOutboundRtp()
{
    if (!m_webrtcbin || m_shuttingDown.load()) return;
    // webrtcbin "get-stats" is async: it replies via a GstPromise carrying a
    // GstStructure of all RTC stats. onStatsReady parses it on a GStreamer
    // thread, sums every outbound-rtp leg's packets-sent, and writes the shared
    // cell that CallManager's PublisherStallPolicy reads on the next 2 s tick.
    auto *ctx = new PubStatsCtx{ m_outboundPacketsSent };
    GstPromise *promise =
        gst_promise_new_with_change_func(onStatsReady, ctx, pubStatsCtxFree);
    g_signal_emit_by_name(m_webrtcbin, "get-stats", nullptr, promise);
}

void PublishPipeline::onStatsReady(GstPromise *promise, gpointer userData)
{
    // Runs on a GStreamer thread. Touches ONLY the shared cell (which outlives
    // the pipeline) — never `this` — so a late reply during teardown is safe.
    auto *ctx = static_cast<PubStatsCtx *>(userData);
    std::shared_ptr<std::atomic<quint64>> cell = ctx->cell;

    const GstStructure *reply = gst_promise_get_reply(promise);
    guint64 sumPacketsSent = 0;
    bool found = false;
    bool sawOutboundRtp = false;
    if (reply) {
        // The report is a flat structure whose fields are themselves
        // GstStructures, one per stat object. SUM packets-sent across EVERY
        // outbound-rtp entry (audio + each simulcast layer): a single layer's
        // BWE valve close must NOT read as a stall, only ALL legs going quiet.
        const int n = gst_structure_n_fields(reply);
        for (int i = 0; i < n; ++i) {
            const gchar *name = gst_structure_nth_field_name(reply, i);
            const GValue *val = gst_structure_get_value(reply, name);
            if (!val || !GST_VALUE_HOLDS_STRUCTURE(val)) continue;
            const GstStructure *s = static_cast<const GstStructure *>(
                g_value_get_boxed(val));
            if (!s) continue;
            GstWebRTCStatsType type;
            if (gst_structure_get(s, "type", GST_TYPE_WEBRTC_STATS_TYPE, &type, nullptr)
                && type == GST_WEBRTC_STATS_OUTBOUND_RTP) {
                sawOutboundRtp = true;
                guint64 ps = 0;
                if (gst_structure_get_uint64(s, "packets-sent", &ps)) {
                    sumPacketsSent += ps;
                    found = true;
                }
            }
        }
    }
    gst_promise_unref(promise);

    if (found) {
        cell->store(sumPacketsSent, std::memory_order_relaxed);
    } else if (sawOutboundRtp) {
        // An outbound-rtp object exists but its packets-sent could not be read:
        // a stat-shape mismatch on this libgstwebrtc would leave the watchdog
        // silently INERT (never sees packets rise -> never arms -> never fires).
        // Surface once, loudly, so it is caught rather than a silent no-op.
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            qWarning() << "PublishPipeline: outbound-rtp stat has no readable "
                          "packets-sent — publisher stall watchdog is INERT "
                          "(libgstwebrtc stat-shape mismatch)";
    }
}

GstFlowReturn PublishPipeline::onPreviewSample(GstAppSink *sink, gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);
    // Gated RCA logging — TALQ_DEBUG_PIPELINE=1. See note in enableCamera.
    if (const char *dbg = std::getenv("TALQ_DEBUG_PIPELINE"); dbg && *dbg == '1') {
        static std::atomic<int> count{0};
        const int n = ++count;
        if (n <= 5 || n % 100 == 0) {
            if (FILE *f = std::fopen("C:/temp/talq-pipe.log", "a")) {
                std::fprintf(f, "onPreviewSample n=%d self=%p\n", n, (void*)self);
                std::fclose(f);
            }
        }
    }
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

    // #20 Phase 3.3b — the BG engine now runs UPSTREAM of m_tee (in
    // onBgSample), so by the time pixels reach the preview branch they
    // are already the processed BGRx the receivers will see. Self-PiP
    // is therefore a straight feedFrame of whatever came off the tee:
    // identical to the legacy pre-3.3a path, no per-frame map / Qt-hop
    // / QImage conversion required.
    QPointer<PublishPipeline> guard(self);
    talq::leak::previewPosted.fetch_add(1, std::memory_order_relaxed);
    QMetaObject::invokeMethod(self, [guard, sample]() {
        if (guard && guard->m_localVideoProvider)
            guard->m_localVideoProvider->feedFrame(sample);
        gst_sample_unref(sample);
        talq::leak::previewDelivered.fetch_add(1, std::memory_order_relaxed);
    }, Qt::QueuedConnection);

    return GST_FLOW_OK;
}

// #20 Phase 3.3b — bridge appsink callback.
// Sits between m_videoCapsFilter and m_tee. Every captured camera frame
// passes here BEFORE the encoder/preview branches. Two regimes:
//
//   Off-mode (engine null or Mode::None): zero-cost push-through. We
//   keep the original GstBuffer and forward it to m_bgAppsrc — same
//   pixels, same PTS+duration, no engine, no Qt-thread hop.
//
//   On-mode  (Blur / Image): call BackgroundEngine::processFrame on this
//   streaming thread. As of 0.40.1 the engine internally routes the
//   work to its own worker QThread (BackgroundWorker) so we briefly
//   block on the worker — never on Qt main. The 0.40.0 stable hopped
//   to Qt main via BlockingQueuedConnection and could deadlock the UI
//   when Qt main needed the GST stream lock during call setup.
//
// PTS preservation is critical — both rtpgccbwe (BWE pacing) and the
// receiver-side jitter buffer use it to detect timing drift. Anything
// other than verbatim original timestamps will look like a wandering
// clock and confuse rate control.
GstFlowReturn PublishPipeline::onBgSample(GstAppSink *sink, gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);
    // Gated RCA logging — TALQ_DEBUG_PIPELINE=1. See note in enableCamera.
    if (const char *dbg = std::getenv("TALQ_DEBUG_PIPELINE"); dbg && *dbg == '1') {
        static std::atomic<int> count{0};
        const int n = ++count;
        if (n <= 5 || n % 100 == 0) {
            if (FILE *f = std::fopen("C:/temp/talq-pipe.log", "a")) {
                std::fprintf(f, "onBgSample n=%d self=%p\n", n, (void*)self);
                std::fclose(f);
            }
        }
    }
    if (!self) return GST_FLOW_ERROR;
    if (self->m_shuttingDown.load(std::memory_order_relaxed)) {
        // Drain so the streaming thread doesn't back up while the
        // pipeline is tearing down.
        if (GstSample *drop = gst_app_sink_try_pull_sample(sink, 0))
            gst_sample_unref(drop);
        return GST_FLOW_OK;
    }

    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    GstBuffer *inBuf = gst_sample_get_buffer(sample);
    if (!inBuf) { gst_sample_unref(sample); return GST_FLOW_OK; }

    const bool engineOn = self->m_backgroundEngine
        && self->m_backgroundEngine->mode() != BackgroundEngine::Mode::None;

    GstAppSrc *src = GST_APP_SRC(self->m_bgAppsrc);
    if (!src) { gst_sample_unref(sample); return GST_FLOW_ERROR; }

    // ---------- Off-mode: zero-allocation push-through ----------
    if (!engineOn) {
        // 0.40.13 — push_SAMPLE not push_buffer. The appsrc's static
        // caps property is "video/x-raw,format=BGRx" with NO width /
        // height / framerate; push_buffer leaves those caps in place
        // downstream, so the preview-convert and preview-appsink see
        // unbounded caps and either fixate to garbage or never get a
        // useful caps event at all (this is why onPreviewSample never
        // fires in release builds). push_sample forwards the sample's
        // full caps (the camera's actual BGRx + concrete dims +
        // framerate), letting downstream negotiate cleanly. Same
        // ownership model: push_sample is transfer-none, we unref
        // the sample after.
        GstFlowReturn fr = gst_app_src_push_sample(src, sample);
        gst_sample_unref(sample);
        self->m_bgBridgeFramesPassThrough.fetch_add(1, std::memory_order_relaxed);
        talq::leak::bgPassThrough.fetch_add(1, std::memory_order_relaxed);
        return fr;
    }

    // ---------- On-mode: GL compositor round-trip ----------
    // Snapshot caps + map BGRx pixels into a detached QImage. We do
    // this on the streaming thread (cheap memcpy, ~70 MB/s at 720p30
    // — the videoConvert upstream does an equal-cost convert anyway,
    // so the bridge adds one extra copy per frame).
    GstCaps *caps = gst_sample_get_caps(sample);
    GstStructure *s = caps ? gst_caps_get_structure(caps, 0) : nullptr;
    int width = 0, height = 0;
    const gchar *format = s ? gst_structure_get_string(s, "format") : nullptr;
    if (s) { gst_structure_get_int(s, "width", &width); gst_structure_get_int(s, "height", &height); }

    if (!format || g_strcmp0(format, "BGRx") != 0 || width <= 0 || height <= 0) {
        // Caps shifted off BGRx — push the original sample through so
        // downstream still sees concrete caps with dims (BG-blur OFF
        // path uses the same trick; see comment there). Surface once.
        if (!self->m_previewEngineDegraded.exchange(true, std::memory_order_relaxed)) {
            qWarning() << "PublishPipeline: BG bridge sees non-BGRx caps "
                          "(" << (format ? format : "<null>") << "); push-through fallback";
        }
        GstFlowReturn fr = gst_app_src_push_sample(src, sample);
        gst_sample_unref(sample);
        return fr;
    }

    GstClockTime pts = GST_BUFFER_PTS(inBuf);
    GstClockTime dts = GST_BUFFER_DTS(inBuf);
    GstClockTime dur = GST_BUFFER_DURATION(inBuf);

    QImage rgbaSnapshot;
    {
        GstMapInfo map;
        if (!gst_buffer_map(inBuf, &map, GST_MAP_READ)) {
            // Map failed — push the original sample through with caps.
            GstFlowReturn fr = gst_app_src_push_sample(src, sample);
            gst_sample_unref(sample);
            return fr;
        }
        if (qint64(map.size) < qint64(width) * height * 4) {
            gst_buffer_unmap(inBuf, &map);
            GstFlowReturn fr = gst_app_src_push_sample(src, sample);
            gst_sample_unref(sample);
            return fr;
        }
        QImage wrap(map.data, width, height, width * 4, QImage::Format_RGB32);
        rgbaSnapshot = wrap.copy();  // detach before unmap
        gst_buffer_unmap(inBuf, &map);
    }
    // Keep `caps` borrowed from sample around until after we build outSample.
    // gst_sample_get_caps returns a borrowed pointer that's valid only while
    // the sample is alive, so we must not unref `sample` until we've ref'd
    // `caps` for the new outSample below.
    GstCaps *outCaps = gst_caps_ref(caps);
    gst_sample_unref(sample);

    // 0.40.1 — BackgroundEngine::processFrame is now thread-safe and
    // routes the work to its own internal worker thread. We can call it
    // directly from this GStreamer streaming thread without any Qt-main
    // hop. The 0.40.0 stable used Qt::BlockingQueuedConnection to Qt
    // main here, which deadlocked the UI whenever Qt main needed the
    // GST stream lock during call setup. See task #32.
    QImage processed;
    if (BackgroundEngine *engine = self->m_backgroundEngine) {
        processed = engine->processFrame(rgbaSnapshot);
    } else {
        processed = rgbaSnapshot;
    }

    if (processed.isNull() || processed.width() != width || processed.height() != height) {
        // Engine returned garbage — push the unprocessed snapshot so the
        // receiver still sees a valid frame.
        processed = rgbaSnapshot;
    }
    if (processed.format() != QImage::Format_RGB32) {
        // Engine should already return BGRx-aliased RGB32, but be safe.
        processed.convertTo(QImage::Format_RGB32);
    }

    // Build a fresh GstBuffer wrapping a copy of the processed pixels.
    const gsize bytes = gsize(width) * height * 4;
    GstBuffer *outBuf = gst_buffer_new_allocate(nullptr, bytes, nullptr);
    if (!outBuf) { gst_caps_unref(outCaps); return GST_FLOW_ERROR; }  // 1.0 audit: was leaking outCaps
    {
        GstMapInfo om;
        if (!gst_buffer_map(outBuf, &om, GST_MAP_WRITE)) {
            gst_buffer_unref(outBuf);
            gst_caps_unref(outCaps);   // 1.0 audit: was leaking outCaps on this error path
            return GST_FLOW_ERROR;
        }
        // QImage::bits() may have row padding; copy line-by-line to
        // honour the appsrc's tight BGRx stride (width*4).
        const int srcStride = processed.bytesPerLine();
        const int rowBytes  = width * 4;
        const uchar *srcPtr = processed.constBits();
        guint8 *dstPtr      = om.data;
        for (int y = 0; y < height; ++y) {
            memcpy(dstPtr + y * rowBytes, srcPtr + y * srcStride, rowBytes);
        }
        gst_buffer_unmap(outBuf, &om);
    }
    GST_BUFFER_PTS(outBuf)      = pts;
    GST_BUFFER_DTS(outBuf)      = dts;
    GST_BUFFER_DURATION(outBuf) = dur;

    // 0.40.14 — push_SAMPLE not push_buffer. Same fix as the off-mode
    // path: bg-appsrc's static caps have no width/height, so push_buffer
    // leaves downstream negotiating against unbounded caps. Constructing
    // a sample with the actual input caps (BGRx + concrete dims) makes
    // downstream see proper caps and the preview branch fires. In the
    // field, PiP was black on 0.40.13 because BG-blur was ON, hitting this
    // path — the 0.40.13 fix only covered the off-mode path.
    GstSample *outSample = gst_sample_new(outBuf, outCaps, nullptr, nullptr);
    gst_buffer_unref(outBuf);     // sample owns its ref
    gst_caps_unref(outCaps);      // sample owns its ref
    GstFlowReturn fr = gst_app_src_push_sample(src, outSample);
    gst_sample_unref(outSample);
    self->m_bgBridgeFramesProcessed.fetch_add(1, std::memory_order_relaxed);
    return fr;
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
    //                       starvation caused the field 30/10 dup-pad chop.
    //   0.31.0/0.31.1 1.2M — shipped on that hypothesis; snow-proxy passed
    //                       at this floor, but field evidence later showed
    //                       the chop was actually camera-mode drift
    //                       (cleared on TalQ restart), not the floor —
    //                       and 1.2M is too high as a minimum for
    //                       moderate uplinks: it clamps GCC above what
    //                       the wire can carry, so excess bytes drop and
    //                       the receiver sees decoder artifacts even
    //                       though distinct ≈ delivered (a field case).
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

void PublishPipeline::setScreenShareSuppression(bool on)
{
    if (on == m_screenShareSuppress) return;
    m_screenShareSuppress = on;
    // Apply the new effective ceiling to the live valves NOW (don't wait for the
    // next BWE tick). On a not-yet-started pipeline the valves are null, so this
    // is a no-op and start()'s build-time gate (effectiveLayerCeiling) applies.
    const int ceil = effectiveLayerCeiling();
    setLayerActive(1, ceil >= 1);
    setLayerActive(2, ceil >= 2);
    qInfo().nospace() << "PublishPipeline: screen share "
                      << (on ? "active -> camera simulcast LOW only"
                             : "ended -> camera simulcast restored")
                      << " (ceiling " << ceil << ", tier cap "
                      << m_loadCapMaxLayer << ")";
    // When un-suppressing, the next onGccBitrate tick re-confirms m/h against
    // the live network estimate; the optimistic re-open above is corrected then.
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
    m_lastBweEstimateBps = estimateBps;   // remembered so a load-ceiling change re-gates
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

    // Effective cap = MIN(network, encode-load): a layer sends only if BOTH the
    // BWE estimate AND the encode-load cap (m_loadCapMaxLayer) allow it, so the
    // CPU veto can't be overridden by a healthy network estimate.
    const int ceil = effectiveLayerCeiling();   // tier cap AND screen-share overlay
    bool wantH = (ceil >= 2) && estimateBps > threshold(kCloseH, m_layers[2].active);
    bool wantM = (ceil >= 1) && estimateBps > threshold(kCloseM, m_layers[1].active);
    // 'l' always stays on — our minimum-viable channel.
    setLayerActive(2, wantH);
    setLayerActive(1, wantM);
}

void PublishPipeline::setLoadCaps(int layerCeiling, int fps)
{
    layerCeiling = qBound(0, layerCeiling, 2);
    fps          = qBound(10, fps, 60);

    if (layerCeiling != m_dynLoadCeiling) {
        m_dynLoadCeiling = layerCeiling;
        // Re-gate the valves under the new ceiling now (don't wait for the next
        // GCC tick). applyBweToLayers MINs the network estimate with
        // effectiveLayerCeiling(), so a tighter ceiling closes h/m immediately
        // and a looser one re-opens only as far as the last estimate allows.
        applyBweToLayers(m_lastBweEstimateBps);
    }
    if (fps != m_loadFps) {
        m_loadFps = fps;
        applySharedFramerate(fps);
    }
}

void PublishPipeline::applySharedFramerate(int fps)
{
    // DISABLED (0.51.5). The old body retargeted m_sharedCaps' framerate live on
    // the assumption that "framerate is timing metadata, the encoders won't
    // reconfigure." That assumption is FALSE on this pipeline: setting the shared
    // capsfilter to a new framerate forces a renegotiation the encoder branch
    // can't satisfy, and it threw NOT_NEGOTIATED (-4) on pub-dummyvideo /
    // bg-appsrc / enc-queue, WEDGING the whole video path → the camera froze on
    // BOTH ends (and the wedged-but-still-fed bg-appsrc was the memory leak fixed
    // in 0.51.4). Proven from Kalin+Ilko's log: "load controller set send
    // framerate 20 fps" was immediately followed by those three NOT_NEGOTIATED
    // errors. The encode-load controller now sheds via simulcast LAYERS only
    // (setLayerActive → valve drop, which renegotiates nothing). A real fps knob
    // would use videorate's `max-rate` property against a non-fixed-framerate
    // capsfilter (no caps renegotiation); deferred until it can be live-tested.
    Q_UNUSED(fps);
}

GstPadProbeReturn PublishPipeline::onEncodeProbe(GstPad *, GstPadProbeInfo *info,
                                                 gpointer user)
{
    // Runs on a GStreamer streaming thread. Touches ONLY the shared cell (which
    // outlives the pipeline) — never `this`. The constrained-baseline encoders
    // emit no B-frames, so a frame's sink→src delta ≈ its encode latency.
    auto *c = static_cast<EncProbeCtx *>(user);
    if (!c || !(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER))
        return GST_PAD_PROBE_OK;
    if (c->layer < 0 || c->layer >= (int)c->cell->sinkNs.size())
        return GST_PAD_PROBE_OK;
    const long long now = (long long)g_get_monotonic_time() * 1000;   // µs → ns
    if (!c->isSrc) {
        c->cell->sinkNs[c->layer].store(now, std::memory_order_relaxed);
    } else {
        const long long in = c->cell->sinkNs[c->layer].load(std::memory_order_relaxed);
        if (in > 0) {
            const long long d = now - in;
            // Ignore >500 ms deltas: those are queue stalls / first-frame warmup,
            // not encode cost, and would spike the usage falsely.
            if (d > 0 && d < 500000000LL)
                c->cell->busyNs.fetch_add(d, std::memory_order_relaxed);
        }
    }
    return GST_PAD_PROBE_OK;
}

double PublishPipeline::encodeUsage()
{
    const long long now  = (long long)g_get_monotonic_time() * 1000;
    const long long busy = m_encodeLoad->busyNs.exchange(0, std::memory_order_relaxed);
    const long long wall = (m_encodeLoadLastReadNs > 0) ? (now - m_encodeLoadLastReadNs) : 0;
    m_encodeLoadLastReadNs = now;
    if (wall <= 0) return 0.0;
    const double u = double(busy) / double(wall);
    return u < 0.0 ? 0.0 : u;
}

bool PublishPipeline::buildFarEndTail()
{
    // 0.51.x AEC fix. Mirrors the proven SharedFarEndBus element setup (silent
    // keepalive so the mixer always produces, even with zero peers; 48k/S16LE/
    // mono everywhere) but lives in THIS pipeline and ends in a REAL wasapi2sink
    // — so the probe and webrtcdsp share one clock and the probe records exactly
    // what hits the speaker, at its real latency.
    if (!m_pipeline) return false;
    GstElement *ka     = gst_element_factory_make("audiotestsrc", "pub-far-keepalive");
    GstElement *kaCaps = gst_element_factory_make("capsfilter",   "pub-far-ka-caps");
    m_farMixer         = gst_element_factory_make("audiomixer",   "pub-far-mix");
    GstElement *caps   = gst_element_factory_make("capsfilter",   "pub-far-caps");
    m_farProbe         = gst_element_factory_make("webrtcechoprobe", "talq-aec-probe");
    GstElement *q      = gst_element_factory_make("queue",        "pub-far-q");
    // The probe records mono/S16LE/48k (the AEC reference format webrtcdsp wants),
    // but the REAL wasapi2sink is a Windows render endpoint that negotiates the
    // device mix format (shared-mode is typically 48k float STEREO) and does NOT
    // convert internally. Without conv+res in front of it the sink returns
    // NOT_NEGOTIATED, which propagates back up through probe→mixer→keepalive
    // ("streaming stopped, reason not-negotiated (-4)") and collapses the whole
    // tail → AEC silently off AND the bus error churns the publisher into
    // Reconnecting. (The retired SharedFarEndBus ended in a fakesink, which
    // accepts anything, so this never surfaced until the real sink in 0.51.2.)
    // Mirror the PROVEN subscriber playout chain (SubscribeWebrtcSrc legacy path):
    // conv → res → wasapi2sink. Probe stays upstream so it still sees mono.
    GstElement *farConv = gst_element_factory_make("audioconvert",  "pub-far-conv");
    GstElement *farRes  = gst_element_factory_make("audioresample", "pub-far-res");
    m_farSink          = gst_element_factory_make("wasapi2sink",  "pub-far-sink");
    if (!ka || !kaCaps || !m_farMixer || !caps || !m_farProbe || !q || !farConv || !farRes || !m_farSink) {
        qWarning() << "PublishPipeline: AEC playout tail element unavailable — AEC off";
        for (GstElement *e : { ka, kaCaps, m_farMixer, caps, m_farProbe, q, farConv, farRes, m_farSink })
            if (e) gst_object_unref(e);
        m_farMixer = m_farProbe = m_farSink = nullptr;
        return false;
    }

    GstCaps *c = gst_caps_from_string(
        "audio/x-raw,rate=48000,format=S16LE,channels=1,layout=interleaved");
    g_object_set(caps,   "caps", c, nullptr);
    g_object_set(kaCaps, "caps", c, nullptr);
    gst_caps_unref(c);
    gst_util_set_object_arg(G_OBJECT(ka), "wave", "silence");
    g_object_set(ka, "is-live", TRUE, nullptr);
    // Keep the probe→speaker buffering small and stable: a default queue can grow
    // to 1s under jitter, which inflates the (delay-agnostic) AEC's far-end→mic
    // alignment and lets echo leak. Bound to ~100ms, leak old data downstream.
    g_object_set(q, "max-size-time", (guint64)100000000, "max-size-buffers", (guint)0,
                 "max-size-bytes", (guint)0, "leaky", 2 /*GST_QUEUE_LEAK_DOWNSTREAM*/, nullptr);
    // Don't let the playout sink gate the publisher's PLAYING transition with its
    // own async preroll (the live keepalive keeps it producing anyway), and don't
    // retain the last sample — mirrors the retired SharedFarEndBus fakesink intent.
    g_object_set(m_farSink, "async", FALSE, "enable-last-sample", FALSE, nullptr);
    if (!m_farOutputDeviceId.isEmpty())
        g_object_set(m_farSink, "device", m_farOutputDeviceId.toUtf8().constData(), nullptr);

    gst_bin_add_many(GST_BIN(m_pipeline),
                     ka, kaCaps, m_farMixer, caps, m_farProbe, q, farConv, farRes, m_farSink, nullptr);

    bool ok = gst_element_link(ka, kaCaps);
    if (ok) {   // keepalive → a mixer request pad
        GstPad *src = gst_element_get_static_pad(kaCaps, "src");
        GstPad *mp  = gst_element_request_pad_simple(m_farMixer, "sink_%u");
        ok = src && mp && (gst_pad_link(src, mp) == GST_PAD_LINK_OK);
        if (src) gst_object_unref(src);
        if (mp)  gst_object_unref(mp);
    }
    if (ok)     // mixer → caps(mono) → probe → queue → conv → res → real speaker
        ok = gst_element_link_many(m_farMixer, caps, m_farProbe, q, farConv, farRes, m_farSink, nullptr);

    if (!ok) {
        qWarning() << "PublishPipeline: AEC playout tail link failed — AEC off";
        gst_bin_remove_many(GST_BIN(m_pipeline),
                            ka, kaCaps, m_farMixer, caps, m_farProbe, q, farConv, farRes, m_farSink, nullptr);
        m_farMixer = m_farProbe = m_farSink = nullptr;
        return false;
    }
    qInfo() << "PublishPipeline: AEC playout tail built in publisher pipeline "
               "(probe inline + shared wasapi2sink) — single-clock AEC";
    return true;
}

GstElement *PublishPipeline::addFarEndPeer(const QString &peerId)
{
    if (!m_farMixer || !m_pipeline) return nullptr;           // AEC tail not up
    if (auto *ex = m_farPeerAppsrcs.value(peerId)) return ex; // idempotent per peer
    GstElement *src = gst_element_factory_make("appsrc", nullptr);
    if (!src) return nullptr;
    GstCaps *c = gst_caps_from_string(
        "audio/x-raw,rate=48000,format=S16LE,channels=1,layout=interleaved");
    g_object_set(src, "is-live", TRUE, "format", GST_FORMAT_TIME,
                 "do-timestamp", TRUE, "caps", c, nullptr);
    gst_caps_unref(c);
    gst_bin_add(GST_BIN(m_pipeline), src);
    GstPad *sp = gst_element_get_static_pad(src, "src");
    GstPad *mp = gst_element_request_pad_simple(m_farMixer, "sink_%u");
    const bool ok = sp && mp && (gst_pad_link(sp, mp) == GST_PAD_LINK_OK);
    if (sp) gst_object_unref(sp);
    if (mp) gst_object_unref(mp);
    if (!ok) {
        qWarning() << "PublishPipeline: far-end peer link failed" << peerId.left(12);
        gst_bin_remove(GST_BIN(m_pipeline), src);
        return nullptr;
    }
    gst_element_sync_state_with_parent(src);
    m_farPeerAppsrcs.insert(peerId, src);
    qInfo() << "PublishPipeline: far-end peer added to AEC reference mix" << peerId.left(12);
    return src;
}

void PublishPipeline::setFarEndOutputDevice(const QString &deviceId)
{
    m_farOutputDeviceId = deviceId;
    if (m_farSink && !deviceId.isEmpty())
        g_object_set(m_farSink, "device", deviceId.toUtf8().constData(), nullptr);
}

void PublishPipeline::removeFarEndPeer(const QString &peerId)
{
    GstElement *src = m_farPeerAppsrcs.take(peerId);
    if (!src) return;                 // unknown peer / AEC off
    if (!m_pipeline) return;          // pipeline gone → the appsrc died with it
    // NULL the appsrc first so any in-flight subscriber push short-circuits
    // (push_sample returns FLUSHING, no UAF), then release its audiomixer request
    // pad and remove it from the bin (gst_bin_remove drops the bin's ref).
    gst_element_set_state(src, GST_STATE_NULL);
    if (GstPad *sp = gst_element_get_static_pad(src, "src")) {
        if (GstPad *mp = gst_pad_get_peer(sp)) {
            gst_pad_unlink(sp, mp);
            if (m_farMixer) gst_element_release_request_pad(m_farMixer, mp);
            gst_object_unref(mp);
        }
        gst_object_unref(sp);
    }
    gst_bin_remove(GST_BIN(m_pipeline), src);
    qInfo() << "PublishPipeline: far-end peer removed from AEC mix" << peerId.left(12);
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

        // 0.41.6-beta — sustained-low BWE auto-cap. If the estimate
        // stays below half of the originally-chosen max for 15 s, we
        // assume the link can't sustain the configured ceiling and
        // clamp m_maxBitrate to (estimate × 1.2). When the estimate
        // recovers above 70 % of the original we restore. Protects
        // against the mid-call freeze where GCC keeps probing a
        // saturated uplink, packet loss climbs, both ends stall.
        constexpr int kAutoCapLowMs        = 15000;
        if (guard->m_originalMaxBitrate > 0) {
            const int loThreshold = guard->m_originalMaxBitrate / 2;
            const int hiThreshold = guard->m_originalMaxBitrate * 7 / 10;
            if (estBps < loThreshold) {
                if (!guard->m_lowBweTimer.isValid()) guard->m_lowBweTimer.start();
                if (!guard->m_autoCapActive
                    && guard->m_lowBweTimer.elapsed() > kAutoCapLowMs) {
                    guard->m_autoCapActive = true;
                    guard->m_maxBitrate    = estBps * 6 / 5;
                    qWarning().nospace()
                        << "PublishPipeline: BWE sustained low ("
                        << (estBps / 1000) << " kbps) — auto-capping max to "
                        << (guard->m_maxBitrate / 1000) << " kbps. "
                        << "Will restore on BWE recovery above "
                        << (hiThreshold / 1000) << " kbps.";
                }
            } else {
                guard->m_lowBweTimer.invalidate();
                if (guard->m_autoCapActive && estBps > hiThreshold) {
                    guard->m_maxBitrate    = guard->m_originalMaxBitrate;
                    guard->m_autoCapActive = false;
                    qInfo().nospace()
                        << "PublishPipeline: BWE recovered ("
                        << (estBps / 1000) << " kbps) — restored max to "
                        << (guard->m_maxBitrate / 1000) << " kbps.";
                }
            }
        }

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
