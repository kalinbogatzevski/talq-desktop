#pragma once

#include <array>
#include <atomic>
#include <QObject>
#include <QString>
#include <QTimer>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include "SignalingClient.h"
#include "VideoFrameProvider.h"

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

    VideoFrameProvider *localVideoProvider() const { return m_localVideoProvider; }

    // #20 — when set (CallManager passes its long-lived engine), the
    // preview-appsink callback routes each BGRx frame through the engine
    // before handing it to m_localVideoProvider. Pure pass-through if
    // the engine is Mode::None or null. The encode path is integrated
    // separately in Phase 3.3b.
    void setBackgroundEngine(class BackgroundEngine *engine) { m_backgroundEngine = engine; }

    void sendStatusMessage(const QByteArray &json);

signals:
    void localOfferReady(const QString &sdp);
    void iceCandidateReady(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void iceStateChanged(const QString &state);
    void iceGatheringComplete();           // ICE gathering done → endOfCandidates
    void audioLevelUpdated(double level);  // 0.0 to 1.0
    void error(const QString &message);
    void cameraError(const QString &reason);
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

    GstElement *m_pipeline = nullptr;
    GstElement *m_webrtcbin = nullptr;
    bool m_running = false;
    bool m_remoteDescSet = false;
    QList<QPair<int, QString>> m_pendingCandidates;
    guint m_busWatchId = 0;

    // Shared encoder chain (built once, never torn down):
    // funnel → sharedConvert → sharedScale → sharedCaps → outputTee → N x simulcast branches → webrtcbin
    // Three branches (l/m/h) fan out from outputTee. Per-branch state in m_layers.
    GstElement *m_funnel = nullptr;
    GstElement *m_sharedScale = nullptr;  // funnel→...→sharedScale→sharedCaps→outputTee
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
        GstElement *payloader = nullptr;
        GstElement *ssrcFilter= nullptr;
        GstPad     *sinkPad   = nullptr;  // webrtcbin's sink_%u for this rid
        guint32     ssrc      = 0;
        bool        active    = true;     // BWE-gating state (false = valve drop)
        int         lastAppliedBitrate = 0;
    };
    std::array<SimulcastLayer, 3> m_layers = {{
        { "l",  320,  180,  150'000 },
        { "m",  640,  360,  500'000 },
        { "h", 1280,  720, 2'500'000 },
    }};
    // Simulcast on/off. PRE-RELEASE builds send the 3 layers above; stable
    // builds send a SINGLE 720p stream (collapse to one branch, plain SDP,
    // GCC-driven bitrate) so stable is never a downgrade from 0.32.0's
    // single-stream 720p while subscriber substream selection is still
    // being proven in beta. Set in start() from the TALQ_PRERELEASE define.
    bool m_simulcast = true;
    // Start bitrate (bits/s) handed to the encoder; rtpgccbwe drives the
    // live rate up to the server ceiling once TWCC feedback flows.
    int m_initBitrate = 2500000;
    // Server video ceiling (HPB signaling [mcu] maxstreambitrate). GCC is
    // clamped here so it never probes past what Janus will forward.
    int m_maxBitrate = 4000000;
    GstElement *m_gccbwe = nullptr;  // rtpgccbwe, owned by webrtcbin once returned
    // Set before the pipeline goes to NULL in cleanup(). webrtcbin can fire
    // request-aux-sender / notify::estimated-bitrate on a streaming thread
    // concurrently with Qt-thread teardown; these callbacks bail on it.
    std::atomic<bool> m_shuttingDown{false};
    bool m_cameraEnabled = false;
    // Self-heal state: a forced exact camera caps from Settings → if no
    // frames in m_camStartWatchdog seconds after enableCamera, the pick
    // is unrecoverably wrong (mfvideosrc can't deliver the mode);
    // reset to Auto and let CallManager re-arm via permissive.
    bool m_camForcedCapsActive = false;
    std::atomic<bool> m_camFirstFrameSeen{false};
    QTimer m_camStartWatchdog;
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

    static void onNegotiationNeeded(GstElement *webrtc, gpointer userData);
    static void onIceCandidate(GstElement *webrtc, guint mlineIndex, gchar *candidate, gpointer userData);
    static void onOfferCreated(GstPromise *promise, gpointer userData);
    static void onIceStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    static void onIceGatheringStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    static GstFlowReturn onPreviewSample(GstAppSink *sink, gpointer userData);
    // Send-side adaptive bitrate: webrtcbin asks for an aux sender; we hand
    // back rtpgccbwe and follow its estimate live onto the encoder.
    static GstElement *onRequestAuxSender(GstElement *webrtc, GObject *transport,
                                          gpointer userData);
    static void onGccBitrate(GObject *gcc, GParamSpec *pspec, gpointer userData);

    // BWE → layer state. Called from onGccBitrate after deadband.
    void applyBweToLayers(int estimateBps);
    // Open/close a layer's valve and update its `active` flag.
    void setLayerActive(int layerIndex, bool on);
};
