#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QHash>
#include <QMutex>
#include <QTimer>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include "SignalingClient.h"
#include "VideoFrameProvider.h"
#include "BweAutoCap.h"
#include "EncodeTier.h"
#include "BweLayerGate.h"
#include "SoloStreamPolicy.h"

/**
 * Send-only GStreamer webrtcbin pipeline for MCU publishing.
 * Captures local audio, encodes as Opus, sends via RTP to the MCU.
 * Creates an offer; MCU answers back.
 *
 * Video architecture (funnel + valve flips):
 * Both dummy (16x16 black) and camera sources are PERMANENTLY linked to a
 * funnel element via valves. Only one valve is open at a time. Switching
 * between dummy and camera is a pair of g_object_set("drop") calls — no
 * unlinking, no relinking, no pad swaps. The encoder chain downstream of
 * the funnel (vp8enc → rtpvp8pay → ssrcFilter → webrtcbin) is built once
 * and never torn down. RTP sequence number continuity is maintained across
 * all switches.
 *
 * Camera-off wire behavior (upstream conformance, 2026-05-21):
 * Upstream Nextcloud Talk's BlackVideoEnforcer paints a black canvas for
 * 5 seconds at mute-time and then sets the track's enabled=false so the
 * browser stops emitting RTP entirely. TalQ used to pump the 16x16 dummy
 * forever, which let the sender-side rtpgccbwe estimator and the
 * receiver-side jitterbuffer converge to a dummy-rate profile; on the
 * subsequent valve-flip-to-camera the receiver couldn't ramp cleanly and
 * the caller saw the deterministic "30 fps / ~10 distinct" callee-
 * mid-call chop. We now mirror upstream: keep the dummy active for 5 s
 * after every transition to "camera off" (initial start without camera,
 * or disableCamera() mid-call), then close the dummy valve. With both
 * valves closed, no frames reach the funnel → no RTP. enableCamera()
 * stops the halt timer and flips the valves as before.
 */
class PublishPipeline : public QObject
{
    Q_OBJECT

public:
    explicit PublishPipeline(QObject *parent = nullptr);
    ~PublishPipeline() override;

    bool start(const QString &stunServer, const QList<TurnServer> &turnServers = {},
               const QString &audioDeviceId = {}, bool withVideo = false,
               int videoDeviceIndex = 0, bool hd1080 = true);
    void stop();
    void setRemoteAnswer(const QString &sdp);
    void addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void setMuted(bool muted);
    bool isRunning() const { return m_running; }
    bool isSingleStream() const { return m_singleStream; }
    bool isCameraOn() const { return m_cameraEnabled; }
    bool usesH264() const { return m_useH264; }
    // e.g. "H264 · nvh264enc · hw" — for the call codec/quality telemetry.
    QString encoderDescription() const { return m_encoderDesc; }
    // Sum of nominal bitrates of currently-active layers — what the UI
    // telemetry chip reads. The actual wire rate matches this within the
    // RTP/RTCP overhead margin baked into the BWE thresholds.
    int currentVideoBitrate() const {
        int sum = 0;
        for (const auto &L : m_layers) if (L.active) sum += L.nominalBitrate;
        return sum > 0 ? sum : m_initBitrate;
    }

    void enableCamera(int deviceIndex, bool hd1080 = true);
    void disableCamera();

    // AEC: when set true BEFORE start(), the capture-leg webrtcdsp runs with
    // echo-cancel=true probe="talq-aec-probe". CallManager only enables this
    // once the SharedFarEndBus probe is already PLAYING (see docs/aec-design.md);
    // calling it has no effect after start().
    void setEchoCancellation(bool on) { m_aecEnabled = on; }

    // 0.51.x AEC single-pipeline fix. The publisher pipeline (which holds
    // webrtcdsp) now also hosts the playout tail: per-peer appsrc → audiomixer →
    // caps(48k/S16LE/mono) → webrtcechoprobe("talq-aec-probe") → queue →
    // wasapi2sink. Probe + dsp share ONE clock, and the reference IS the real
    // played audio at its real render latency — the fix for the cross-pipeline
    // defect that defeated cancellation. Built in start() when AEC is on (else
    // these no-op and subscribers keep their own sinks).
    // addFarEndPeer returns the appsrc a subscriber pushes its decoded
    // 48k/S16LE/mono audio into (idempotent per peerId; null if AEC off / failed).
    GstElement *addFarEndPeer(const QString &peerId);
    // Release a peer's far-end appsrc + its audiomixer request pad (on peer-leave),
    // so per-peer churn doesn't leak pads/elements. Idempotent; safe if AEC off.
    void        removeFarEndPeer(const QString &peerId);
    void        setFarEndOutputDevice(const QString &deviceId);
    bool        aecPlayoutActive() const { return m_farProbe != nullptr; }
    // Has the camera delivered at least one real frame since the last enable?
    // Diagnostics: "camera on (intent) but firstFrame=0" after a few seconds is
    // the silent mfvideosrc-failure signature (the "her camera stopped" symptom).
    bool        cameraFirstFrameSeen() const { return m_camFirstFrameSeen.load(std::memory_order_relaxed); }
    // Diagnostic (0.51.x leak hunt): bytes currently queued inside the BG-bridge
    // appsrc. It's max-bytes=0 (unbounded); if this climbs during a call the
    // camera frames are accumulating in the appsrc instead of draining downstream.
    qint64      bgAppsrcQueuedBytes() const {
        if (!m_bgAppsrc) return 0;
        guint64 cur = 0;
        g_object_get(m_bgAppsrc, "current-level-bytes", &cur, nullptr);
        return (qint64)cur;
    }

    // Encode-load mitigation: CallManager passes the detected GPU accel tier
    // ("NVIDIA NVDEC" / "Intel DXVA" / "DXVA (H264 only)" / "Software only")
    // BEFORE start(). On a non-discrete-GPU box, start() caps the camera send
    // resolution and sheds the HIGH simulcast layer so the publisher can't
    // saturate the iGPU it also decodes on (the field freeze 2026-06-05).
    void setGpuClass(talq::GpuClass cls) { m_gpuClass = cls; }

    // While the user is screen-sharing, stop the camera's simulcast: a share is
    // a second encode on top of the camera's layers, and on a weak/iGPU encoder
    // the two together stall the machine (field freeze 2026-06-08). Drops the
    // camera to the LOW layer only by clamping m_loadCapMaxLayer to 0 (which
    // applyBweToLayers already honours as a hard ceiling, so BWE can't re-open
    // m/h); restores the prior ceiling when the share ends. Idempotent.
    void setScreenShareSuppression(bool on);

    // 0.51.x dynamic encode-load controller (CallManager owns it, ticks ~1 s).
    // Applies the controller's SEND caps: layerCeiling 0..2 (folds into
    // effectiveLayerCeiling + re-gates the valves live) and fps 10..60 (drives
    // the shared videorate via m_sharedCaps). Idempotent — only re-gates /
    // renegotiates on a real change. See MediaLoadController.h.
    void setLoadCaps(int layerCeiling, int fps);

    VideoFrameProvider *localVideoProvider() const { return m_localVideoProvider; }

    // #20 — when set (CallManager passes its long-lived engine), the
    // preview-appsink callback routes each BGRx frame through the engine
    // before handing it to m_localVideoProvider. Pure pass-through if
    // the engine is Mode::None or null. The encode path is integrated
    // separately in Phase 3.3b.
    void setBackgroundEngine(class BackgroundEngine *engine) { m_backgroundEngine = engine; }

    // #20 Phase 5 — bridge throughput counters for the harness.
    // _passThrough bumps every frame onBgSample took the Off-mode
    // gst_buffer_ref+push path; _processed every frame the On-mode
    // engine round-trip ran end-to-end (mapped, engine returned a
    // same-size image, fresh GstBuffer pushed). The harness asserts
    // the right counter is non-zero per scenario.
    quint64 bgBridgeFramesPassThrough() const {
        return m_bgBridgeFramesPassThrough.load(std::memory_order_relaxed);
    }
    quint64 bgBridgeFramesProcessed() const {
        return m_bgBridgeFramesProcessed.load(std::memory_order_relaxed);
    }
    // 0.60.5 — frames the BG bridge DROPPED rather than push. Non-zero means
    // the engine handed back something unusable while a background was ON and
    // we refused to substitute the raw camera frame (a privacy leak) for it.
    // Should be 0 in the field; a resolution change mid-call can tick it once.
    // Distinct from BackgroundEngine::framesDropped(), which counts frames the
    // engine's drop-oldest input slot discarded because the worker was busy —
    // that one is healthy backpressure and is EXPECTED to be non-zero on a
    // loaded box.
    quint64 bgBridgeFramesDropped() const {
        return m_bgBridgeFramesDropped.load(std::memory_order_relaxed);
    }

    void sendStatusMessage(const QByteArray &json);

    // Outbound-RTP stall watchdog support (PublisherStallPolicy). CallManager
    // calls pollOutboundRtp() each 2 s stats tick (async get-stats) and reads
    // outboundPacketsSent(); a summed packets-sent that stops rising while the
    // publisher should be sending means the send leg is dead (consent revoked
    // while ice-connection-state stays "completed") -> recoverPublisher.
    void pollOutboundRtp();
    quint64 outboundPacketsSent() const {
        return m_outboundPacketsSent->load(std::memory_order_relaxed);
    }

    // 0.51.x encode-load probe: the fraction of wall-clock time the send encoders
    // spent encoding since the previous call (main thread; resets the accumulator
    // each call). Feeds the MediaLoadController via CallManager's ~1 s tick. Can
    // exceed 1.0 when several simulcast encoders run concurrently — that's the
    // intended "total encode demand" signal.
    double encodeUsage();

    // 0.61.0 send-fps badge: DRAINS the layer-0 encoded-frame counter against
    // wall time since the previous call and returns the measured (not
    // commanded) integer send fps. Mutating — call exactly ONCE per stats tick
    // (CallManager::updateCallStats caches the result); never from paint code.
    int sendFps();

signals:
    void localOfferReady(const QString &sdp);
    void iceCandidateReady(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void iceStateChanged(const QString &state);
    void iceGatheringComplete();           // ICE gathering done → endOfCandidates
    void audioLevelUpdated(double level);  // 0.0 to 1.0
    void error(const QString &message);
    void cameraError(const QString &reason);
    // The hardware video encoder (NVENC) could not open a session on this
    // machine — the publisher has latched talqAvoidNvenc() for the rest of the
    // session. CallManager persists the latch (QSettings Video/avoidNvenc) so
    // every future call/launch skips NVENC and uses Intel QSV / software from
    // the start, instead of failing at camera-on again.
    void hwVideoEncoderUnavailable();
    // The camera resolved to the SOFTWARE (x264) encoder — surfaced to the user
    // once per call so they know why CPU is higher (no usable HW encoder).
    void softwareVideoEncoderUsed();
    // 0.52.5 — the send is pinned at the capability floor under load/BWE pressure
    // (true) or has recovered above it (false). Only ever true on a NON-Capable /
    // software box (a Capable HW box floors at full quality, so it never fires).
    // Drives a persistent "limited to 480p" chip on the sender's own screen.
    void sendQualityFloored(bool atFloor);
    // Non-fatal mic failure. Emitted when the microphone could NOT be opened
    // and the publisher fell back to a SILENT audio source so the call still
    // connects (peer hears silence) instead of dropping the whole call.
    // Mirrors cameraError: CallManager surfaces a "microphone unavailable"
    // banner rather than tearing the call down. (A successful fall-back to the
    // SYSTEM-DEFAULT device — the common "selected device id won't resolve"
    // case — does NOT emit this; the mic works, it's just a different device.)
    void audioError(const QString &reason);
    // Self-healing: emitted when a forced exact camera caps fails to
    // produce any frames within the watchdog window (mfvideosrc couldn't
    // deliver that mode). The persisted choice is reset to Auto here;
    // CallManager handles the disable→enable re-arm so the camera comes
    // up via permissive negotiation (the guaranteed-working escape).
    void cameraNegotiationFailed();

public slots:
    void pollBus();  // called from CallManager's GLib timer

private:
    void cleanup();
    bool buildCameraChain(int deviceIndex, bool hd1080);
    void reArmCameraSource();   // 0.52.13 — stall recovery: NULL→PLAYING the MF source
    QByteArray framerateCapsFrag() const;   // range when m_fpsAdaptActive, else fixed 30/1

    GstElement *m_pipeline = nullptr;
    GstElement *m_webrtcbin = nullptr;
    bool m_running = false;
    bool m_muted = false;   // sender mute state; applied to pub-volume on (re)build
    bool m_remoteDescSet = false;
    QList<QPair<int, QString>> m_pendingCandidates;
    guint m_busWatchId = 0;

    // Shared encoder chain (built once, never torn down):
    // funnel → sharedConvert → sharedScale → sharedCaps → outputTee → N x simulcast branches → webrtcbin
    // Three branches (l/m/h) fan out from outputTee. Per-branch state in m_layers.
    GstElement *m_funnel = nullptr;
    GstElement *m_sharedScale = nullptr;  // funnel→...→sharedScale→sharedRate→sharedCaps→outputTee
    GstElement *m_sharedRate = nullptr;   // C2/#29 — shared videorate; its max-rate is the
                                          // live send-FPS degrade knob (active only when
                                          // TALQ_FPS_ADAPT=1, which also relaxes the framerate
                                          // caps below to a range so the cap can take effect)
    GstElement *m_sharedCaps = nullptr;   // pins a CONSTANT 1280x720@30 input so the
                                          // encoders never reconfigure on the
                                          // 16x16-dummy↔camera source switch
    GstElement *m_outputTee = nullptr;    // splits shared-caps output to the 3 layers

    struct SimulcastLayer {
        const char *rid;       // "l" / "m" / "h"
        int targetW;           // 320 / 640 / 1280
        int targetH;           // 180 / 360 / 720
        int nominalBitrate;    // 150_000 / 500_000 / 2_500_000 (bits/s)
        GstElement *valve     = nullptr;  // gates frames into this branch
        GstElement *scale     = nullptr;
        GstElement *caps      = nullptr;
        GstElement *encoder   = nullptr;
        GstElement *parser    = nullptr;  // h264parse when m_useH264, else null
        GstElement *profileCaps= nullptr; // H264: pins encoder to constrained-baseline
        GstElement *payloader = nullptr;
        GstElement *ssrcFilter= nullptr;
        GstPad     *sinkPad   = nullptr;  // webrtcbin's sink_%u for this rid
        guint32     ssrc      = 0;
        bool        active    = true;     // BWE-gating state (false = valve drop)
        int         lastAppliedBitrate = 0;
    };
    // 0.41.0-beta — Layer ladder. L/M stay at 180p/360p for compatibility
    // with constrained subscribers; HIGH scales with the user's
    // QSettings("TalQ","TalQ")/Video/maxSendHeight choice (default 720).
    // The HIGH targetW/targetH + nominalBitrate are RESET in start() from
    // the live setting before the encoder branches are built. Values here
    // are the 720p defaults a fresh install would see.
    std::array<SimulcastLayer, 3> m_layers = {{
        { "l",  320,  180,  150'000 },
        { "m",  640,  360,  500'000 },
        { "h", 1280,  720, 3'500'000 },
    }};
    // Simulcast on/off. PRE-RELEASE builds send the 3 layers above; stable
    // builds send a SINGLE 720p stream (collapse to one branch, plain SDP,
    // GCC-driven bitrate) so stable is never a downgrade from 0.32.0's
    // single-stream 720p while subscriber substream selection is still
    // being proven in beta. Set in start() from the TALQ_PRERELEASE define.
    bool m_simulcast = true;
    // 0.61.0 — weak-tier single-stream mode (one non-simulcast branch, decided in
    // start() from the GPU tier). When true, m_simulcast is forced false and the
    // fps-adapt path is active regardless of TALQ_FPS_ADAPT.
    bool m_singleStream   = false;
    bool m_fpsAdaptActive = false;
    // Start bitrate (bits/s) handed to the encoder; rtpgccbwe drives the
    // live rate up to the server ceiling once TWCC feedback flows.
    int m_initBitrate = 3500000;
    // Server video ceiling (HPB signaling [mcu] maxstreambitrate). GCC is
    // clamped here so it never probes past what Janus will forward.
    // 0.41.0-beta — bumped 4 → 6 Mbps to match Telegram's 720p30 target.
    int m_maxBitrate = 6000000;
    // 0.41.6-beta — auto-cap on sustained packet loss. m_originalMaxBitrate
    // is captured in start() after the resolution-based ceiling is picked.
    // When the BWE estimate stays below half of it for kAutoCapLowSeconds,
    // m_maxBitrate is clamped to (estimate × 1.2) for the rest of the
    // call (or until the BWE recovers above 70 % of the original). The
    // existing applyBweToLayers / setWebrtcVideoEncoderBitrate paths
    // respect the clamped m_maxBitrate, so this protects against runaway
    // probing on a sustainedly-saturated uplink (the mid-call freeze
    // class of bugs).
    // 0.52.3 — the latch/restore decision lives in talq::BweAutoCap (pure,
    // unit-tested). It evaluates the RAW (un-clamped) GCC estimate so a recovered
    // link is visible (the old inline version clamped first → cap latched low for
    // the whole call), with a restore dwell to prevent flap.
    int             m_originalMaxBitrate = 0;
    talq::BweAutoCap m_bweAutoCap;
    GstElement *m_gccbwe = nullptr;  // rtpgccbwe, owned by webrtcbin once returned
    // Set before the pipeline goes to NULL in cleanup(). webrtcbin can fire
    // request-aux-sender / notify::estimated-bitrate on a streaming thread
    // concurrently with Qt-thread teardown; these callbacks bail on it.
    std::atomic<bool> m_shuttingDown{false};
    // Outbound-RTP packets-sent cell for the stall watchdog (PublisherStallPolicy).
    // Written by the async get-stats callback on a GStreamer thread, read by
    // CallManager's 2 s tick. A SHARED-PTR CELL (not a raw member) so the
    // callback NEVER dereferences `this` — it writes the cell, which outlives
    // the pipeline if a late reply races teardown. Closes the get-stats UAF.
    std::shared_ptr<std::atomic<quint64>> m_outboundPacketsSent{
        std::make_shared<std::atomic<quint64>>(0)};
    // 0.51.x encode-load probe. The encoder sink→src pad probes write encode
    // busy-time into this SHARED cell (they capture a shared_ptr, never `this`,
    // so a late buffer racing teardown can't UAF — same discipline as the
    // packets-sent cell above). encodeUsage() drains it against wall time.
    struct EncodeLoadCell {
        std::atomic<long long> busyNs{0};                  // accumulated encode busy time (ns)
        std::array<std::atomic<long long>, 3> sinkNs{};    // per-layer last sink-entry time (ns)
        std::atomic<long long> framesEncoded{0};           // layer-0 encoded-frame count (send-fps badge)
    };
    std::shared_ptr<EncodeLoadCell> m_encodeLoad{std::make_shared<EncodeLoadCell>()};
    long long m_encodeLoadLastReadNs = 0;   // wall time of the last encodeUsage() read
    long long m_sendFpsLastReadNs = 0;      // wall time of the last sendFps() read
    struct EncProbeCtx { std::shared_ptr<EncodeLoadCell> cell; int layer; bool isSrc; };
    // GPU encode-capability class from CallManager (set before start()); drives
    // the encode-load cap in start().
    talq::GpuClass m_gpuClass = talq::GpuClass::Software;
    // Highest simulcast layer index allowed to send (2 = all l/m/h, 1 = l/m,
    // 0 = l only). Lowered up front on a weak encode tier (sheds HIGH); the BWE
    // gate in applyBweToLayers honors it so it can't re-open a capped layer.
    int m_loadCapMaxLayer = 2;
    // Screen-share camera-simulcast suppression — a separate boolean OVERLAY on
    // the tier ceiling (not a mutation of it), so build/BWE ordering can't lose
    // the natural ceiling. While true, the effective ceiling is forced to 0
    // (LOW only); when cleared the tier ceiling applies again.
    bool m_screenShareSuppress = false;
    // 0.51.x dynamic load controller: a fourth, runtime-MEASURED ceiling axis
    // (driven by MediaLoadController via setLoadCaps). 2 = no cap.
    // effectiveLayerCeiling is the MIN of the static device tier, this dynamic
    // axis, and the screen-share overlay — every axis is a ceiling, so the
    // controller can only ever pull BELOW what the tier/BWE already allow.
    int m_dynLoadCeiling     = 2;
    int m_loadFps            = 30;  // controller send-fps target (drives m_sharedCaps)
    int m_lastBweEstimateBps = 0;   // last GCC estimate, so a ceiling change re-gates
    // 0.52.5 capability send-floor: the LOWEST layer ceiling the dynamic load
    // controller may pull us down to. Set in start() from the GPU class + the
    // runtime software-fallback latch. A Capable box with a WORKING hardware
    // encoder gets 2 (the load controller can NEVER shed its send — field repro:
    // two capable HW boxes on a healthy 9 Mbps link both collapsed to 180p because
    // the load controller dropped the ceiling to 0; only real BWE congestion may
    // reduce a capable box now). A non-HW / software box gets 1 (floors at 'm' =
    // the "480p" tier, never the 320x180 bottom rung) and shows the notice chip.
    // NOTE: only floors the LOAD axis (clamped after the dyn-ceiling min, before
    // the BWE gate decides per-layer on the real estimate); screen-share
    // suppression still wins via the early return.
    int m_layerFloor         = 1;
    bool m_sendFlooredLatch  = false;  // edge-trigger for sendQualityFloored()
    // Anti-flap dwell for the asymmetric BWE layer gate (drop fast, re-add only
    // after a sustained estimate). Indexed by layer (1=m, 2=h; 0=l never gates).
    // See BweLayerGate.h + tests/bwe_layer_gate_test.cpp.
    talq::BweGateState m_bweGate[3];
    QElapsedTimer      m_bweClock;  // monotonic clock for the re-add dwell
    // Active layers gate on this, NOT m_loadCapMaxLayer directly.
    int effectiveLayerCeiling() const {
        if (m_screenShareSuppress) return 0;          // share suppression wins (unchanged)
        int c = m_loadCapMaxLayer;
        if (m_dynLoadCeiling < c) c = m_dynLoadCeiling;
        if (c < m_layerFloor)    c = m_layerFloor;    // capability floor (never below)
        return c;
    }
    bool m_cameraEnabled = false;
    // AEC on the capture leg (echo-cancel + probe). Set via setEchoCancellation
    // before start(); CallManager guarantees the probe exists first.
    bool m_aecEnabled = false;
    // 0.51.x AEC playout tail (lives in THIS pipeline → shared clock with
    // webrtcdsp). m_farMixer != null means the tail is up. Built before the
    // webrtcdsp config in start() so the probe exists for the dsp's lookup and a
    // tail-build failure degrades to AEC-off (never a call drop).
    GstElement *m_farMixer = nullptr;   // legacy (unused in the loopback design)
    GstElement *m_farProbe = nullptr;
    GstElement *m_farSink  = nullptr;   // now a fakesink (reference-only tail)
    GstElement *m_farSrc   = nullptr;   // 0.55.3 AEC: WASAPI render-loopback reference source
    QString     m_farOutputDeviceId;
    // 0.55.3 AEC ERLE proxy: last RMS (dB) seen on the loopback reference
    // (pub-far-ref-level) vs the post-webrtcdsp send leg (pub-level). In a
    // remote-talking / local-silent window, a working canceller keeps the send
    // RMS well below the reference RMS; if they track, echo is leaking. Logged
    // from pollBus so a live speaker test yields a number, not just "sounds off".
    double      m_aecFarRefRms = -100.0;
    double      m_aecSendRms   = -100.0;
    int         m_aecErleDbg   = 0;
    QHash<QString, GstElement*> m_farPeerAppsrcs;
    // Serialises far-end peer add/remove and tail teardown against each other:
    // the hash mutations PLUS the structural pipeline edits (audiomixer
    // request/release pad, bin add/remove) must be atomic, so a peer leaving
    // can't race a peer joining or a teardown clearing the map. The decoded-
    // audio PUSH into each appsrc is guarded separately on the subscriber side
    // (SubscribeWebrtcSrc::m_farAppsrcMutex + a ref + NULL-first short-circuit).
    QMutex m_farPeerMutex;
    bool buildFarEndTail();
    // Self-heal state: a forced exact camera caps from Settings → if no
    // frames in m_camStartWatchdog seconds after enableCamera, the pick
    // is unrecoverably wrong (mfvideosrc can't deliver the mode);
    // reset to Auto and let CallManager re-arm via permissive.
    bool m_camForcedCapsActive = false;
    std::atomic<bool> m_camFirstFrameSeen{false};
    QTimer m_camStartWatchdog;
    // 0.52.13 — STALL-AWARE self-heal. The MF source reader can leak ONE frame
    // then stall (Intel MF/D3D concurrency, esp. while a screen-share is using
    // the encoder), which the old "no first frame" watchdog couldn't recover
    // (the one frame cancelled it) and it never armed on Auto at all. Track frame
    // PROGRESS and re-negotiate at permissive ≤720p on a stall.
    std::atomic<quint64> m_camFrameCount{0};   // bumped per preview frame (streaming thread)
    quint64 m_camLastSeenCount = 0;            // last value the watchdog observed (main thread)
    int     m_camStallTicks = 0;               // consecutive no-progress ticks (debounce)
    int     m_camHealthyTicks = 0;             // consecutive progress ticks (resets attempts)
    int     m_camRecoveryAttempts = 0;         // capped auto-rebuilds before giving up
    bool    m_camRebuildInFlight = false;      // a re-arm is settling — don't stack another
    bool m_useH264 = false;
    QString m_encoderDesc;   // human codec/encoder/hw-sw, for telemetry/pill
    int m_lvlDbg = 0;
    GstWebRTCDataChannel *m_statusDataChannel = nullptr;

    // Dummy source (16x16 black @ 10 fps, on the wire ONLY for the
    // 5-second grace window after every "camera off" transition; then
    // m_dummyHaltTimer closes the dummy valve so the wire goes silent).
    GstElement *m_dummySrc = nullptr;
    GstElement *m_dummyCaps = nullptr;
    GstElement *m_dummyConv = nullptr;
    GstElement *m_dummyValve = nullptr;
    // 5 s after entering "camera off" → close the dummy valve so no RTP
    // packets reach webrtcbin. Mirrors upstream BlackVideoEnforcer's
    // canvas-for-5s → track.enabled=false pattern. Single-shot.
    QTimer m_dummyHaltTimer;

    // Camera branch elements (built lazily in buildCameraChain(), stay alive)
    GstElement *m_cameraSrc = nullptr;
    GstElement *m_camSrcCaps = nullptr;  // mfvideosrc out: jpeg|raw, <=720p
    GstElement *m_camDecode = nullptr;   // decodebin: MJPEG->raw or raw passthru
    GstElement *m_videoConvert = nullptr;
    GstElement *m_videoCapsFilter = nullptr;
    GstElement *m_tee = nullptr;
    GstElement *m_encQueue = nullptr;
    GstElement *m_cameraValve = nullptr;

    // #20 Phase 3.3b — appsink/appsrc bridge inserted between the
    // post-decode capsfilter and the encoder/preview tee, so the
    // BackgroundEngine sees every camera frame BEFORE it reaches the
    // encoder branch (in Phase 3.3a only the preview branch had it).
    // When mode is Off the bridge zero-copies the input buffer straight
    // to the appsrc; when mode is Blur/Image, frames cross to the Qt
    // thread, run through the engine, and are pushed back as a new
    // GstBuffer with the original PTS+duration. PTS preserved so the
    // encoder's rate control + gccbwe still see the upstream timing.
    GstElement *m_bgAppsink = nullptr;
    GstElement *m_bgAppsrc  = nullptr;

    // Local preview (tee branch off camera)
    GstElement *m_previewQueue = nullptr;
    GstElement *m_previewConvert = nullptr;
    GstElement *m_previewAppsink = nullptr;
    VideoFrameProvider *m_localVideoProvider = nullptr;
    class BackgroundEngine *m_backgroundEngine = nullptr;   // #20, not owned
    // Set true the first time the BG-engine preview path hits an
    // unrecoverable degradation (caps != BGRx, gst_buffer_map failure,
    // size mismatch). Triggers a one-shot qWarning, then suppresses
    // further warnings — matches the "surface once, then quiet" pattern
    // BackgroundEngine itself uses for engineDisabled. Atomic so the
    // streaming-thread branch and Qt-thread reads stay consistent.
    std::atomic<bool> m_previewEngineDegraded{false};

    // #20 Phase 5 — bridge counters, see public getters above.
    std::atomic<quint64> m_bgBridgeFramesPassThrough{0};
    std::atomic<quint64> m_bgBridgeFramesProcessed{0};
    std::atomic<quint64> m_bgBridgeFramesDropped{0};      // 0.60.5 — see bgBridgeFramesDropped()
    std::atomic<bool>    m_bgLeakGuardWarned{false};      // one-shot warn on the drop path

    static void onNegotiationNeeded(GstElement *webrtc, gpointer userData);
    static void onIceCandidate(GstElement *webrtc, guint mlineIndex, gchar *candidate, gpointer userData);
    static void onOfferCreated(GstPromise *promise, gpointer userData);
    static void onIceStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    static void onIceGatheringStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    // webrtcbin get-stats reply (async, GStreamer thread): sums outbound-rtp
    // packets-sent and stores it for the stall watchdog. Lifetime-guarded by
    // m_alive + a QPointer main-thread hop.
    static void onStatsReady(GstPromise *promise, gpointer userData);
    // 0.51.x encode-load probe callback (one per encoder sink+src pad). Times
    // each frame's encode and accumulates it into the shared EncodeLoadCell.
    static GstPadProbeReturn onEncodeProbe(GstPad *pad, GstPadProbeInfo *info, gpointer user);
    static GstFlowReturn onPreviewSample(GstAppSink *sink, gpointer userData);
    // #20 Phase 3.3b — bridge appsink that feeds the BG engine and
    // pushes the (processed or pass-through) buffer back via m_bgAppsrc.
    static GstFlowReturn onBgSample(GstAppSink *sink, gpointer userData);
    // Send-side adaptive bitrate: webrtcbin asks for an aux sender; we hand
    // back rtpgccbwe and follow its estimate live onto the encoder.
    static GstElement *onRequestAuxSender(GstElement *webrtc, GObject *transport,
                                          gpointer userData);
    static void onGccBitrate(GObject *gcc, GParamSpec *pspec, gpointer userData);

    // BWE → layer state. Called from onGccBitrate after deadband.
    void applyBweToLayers(int estimateBps);
    // Update the shared videorate's target framerate live (m_sharedCaps),
    // preserving the pinned width/height. The controller's fps lever.
    void applySharedFramerate(int fps);
    // Open/close a layer's valve and update its `active` flag.
    void setLayerActive(int layerIndex, bool on);
};
