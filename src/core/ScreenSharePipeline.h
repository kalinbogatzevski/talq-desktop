#pragma once

#include <atomic>
#include <memory>
#include <QObject>
#include <QString>
#include <QTimer>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include "SignalingClient.h"
#include "VideoFrameProvider.h"
typedef struct _GstAppSink GstAppSink;

/**
 * Send-only screen capture pipeline for MCU screen sharing.
 * Captures the screen via d3d11screencapturesrc at full native resolution
 * and encodes with hardware H264 (preferred) so a shared screen stays
 * sharp at the high screen bitrate the HPB allows. Sends via RTP to the
 * MCU on a separate webrtcbin (roomType "screen"). No audio. No data
 * channels needed beyond the required "status" channel.
 */
class ScreenSharePipeline : public QObject
{
    Q_OBJECT

public:
    explicit ScreenSharePipeline(QObject *parent = nullptr);
    ~ScreenSharePipeline() override;

    bool start(const QString &stunServer, const QList<TurnServer> &turnServers = {},
                int monitorIndex = 0, quintptr windowHandle = 0);
    void stop();
    void setRemoteAnswer(const QString &sdp);
    void addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    bool isRunning() const { return m_running; }
    // e.g. "H264 · nvh264enc · hw" — for the call codec/quality telemetry.
    QString encoderDescription() const { return m_encoderDesc; }
    // 0.41.1-beta — local self-preview of the screen being shared. Fed by
    // an appsink tee taken AFTER scaleCaps so the user sees the exact
    // post-downscale content the peer receives, not the raw monitor.
    // Lifecycle: provider created in ctor, frames flow only while
    // start() succeeds; nullptr otherwise.
    VideoFrameProvider *previewProvider() const { return m_previewProvider; }
    // Screen-capture downscale cap applied before encode. Default 1080p:
    // a native 4K raw frame is ~38 MB and an unbounded native-res pool
    // ballooned RAM ~400 MB on share start. The quality switch sets a
    // higher cap then stop()->start(). Must be called before start().
    // Sets the screen-capture downscale cap. Before start(): just stored. While
    // RUNNING: applied LIVE by re-setting the downscale capsfilter, so a mid-
    // share quality change reconfigures the encoder in place (resolution is not
    // in the SDP -> no renegotiation, no new MCU offer/handle). This replaces
    // the old stop()->start() re-share, which re-offered on the same session and
    // the MCU's stale screen handle never confirmed the new publish (churn/drop).
    void setQualityCap(int maxW, int maxH);

signals:
    void localOfferReady(const QString &sdp);
    void iceCandidateReady(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void iceStateChanged(const QString &state);
    void iceGatheringComplete();           // ICE gathering done → endOfCandidates
    void error(const QString &message);
    // Outbound RTP is actually climbing — the only publisher-observable proof
    // the screen publish is live on the wire (the SDP answer is NOT). Emitted
    // once, after two consecutive positive packets-sent deltas. Together with
    // ICE-connected this is what marks the share confirmed (ShareStartPolicy).
    void mediaFlowing();
    // The async GStreamer teardown (detached NULL transition) has completed and
    // the capture device is released. Lets the owner safely start a new share
    // without colliding with a still-held d3d11 capture device (the back-to-
    // back "wait several seconds between shares" fix). Always main-thread.
    void released();

public slots:
    void pollBus();

private:
    void cleanup();

    GstElement *m_pipeline = nullptr;
    GstElement *m_webrtcbin = nullptr;
    GstElement *m_videoEncoder = nullptr;  // kept for proper adaptive (todo)
    GstElement *m_scaleCaps = nullptr;     // downscale capsfilter; re-set LIVE by setQualityCap() for mid-share quality changes
    GstElement *m_videoParser = nullptr;   // h264parse, present iff H264
    bool m_useH264 = false;
    bool m_running = false;
    bool m_remoteDescSet = false;
    QList<QPair<int, QString>> m_pendingCandidates;
    // Screen content is mostly static, so a VBR encoder costs little when
    // nothing changes and spends bits (up to the server ~12 Mbps screen
    // cap) only on detail/motion — content-adaptive without TWCC.
    int m_initBitrate = 8000000;
    // Server screen ceiling (HPB signaling [mcu] maxscreenbitrate). GCC is
    // clamped here; it drives the VBR average up/down with the content.
    int m_maxBitrate = 12000000;
    int m_capW = 1920;   // screen-capture downscale cap (default 1080p,
    int m_capH = 1080;   // ~4.7x less per-frame RAM than native 4K)
    GstElement *m_gccbwe = nullptr;  // rtpgccbwe, owned by webrtcbin once returned
    // Set before pipeline→NULL in cleanup(); aux-sender/GCC callbacks
    // (streaming thread) bail on it to avoid a teardown-race UAF.
    std::atomic<bool> m_shuttingDown{false};
    // Last bitrate pushed to the encoder — GCC notifies far more often
    // than a hardware encoder can honor a live reconfigure; deadband it.
    int m_lastAppliedBitrate = 0;
    QString m_encoderDesc;   // human codec/encoder/hw-sw, for telemetry/pill
    // "Share didn't start" watchdog — set in start(), cleared when ICE
    // reaches connected. If it fires before then, the pipeline silently
    // failed to negotiate (#134 "stream doesn't always start") and the
    // user sees no feedback. We emit error() so the UI surfaces it.
    QTimer m_startWatchdog;
    bool m_iceReachedConnected = false;
    // "Capture produced no frames" watchdog (#2). d3d11screencapturesrc in
    // WGC mode can silently fail to attach to a window (target minimized,
    // protected, or a stale capture session left by a fast stop→start with
    // a different window) — ICE connects fine but ZERO frames flow, so the
    // receiver shows "Starting remote screen share…" forever. A pad probe
    // on the capture src counts buffers; if none arrive within the window,
    // emit error() so the user gets a clear failure + retry instead of a
    // dead share. Distinct from m_startWatchdog (which only covers ICE).
    QTimer m_frameWatchdog;
    std::atomic<bool> m_firstFrameSeen{false};
    // Outbound-RTP confirmation (the real "publish is live" ack). On a short
    // timer we ask webrtcbin for stats and read the outbound-rtp packets-sent
    // counter; two consecutive positive deltas → emit mediaFlowing() once.
    QTimer m_statsTimer;
    guint64 m_lastPacketsSent = 0;
    int m_packetsRisingStreak = 0;
    bool m_mediaFlowingEmitted = false;
    void pollOutboundRtp();
    static void onStatsReady(GstPromise *promise, gpointer userData);
    // Lifetime token for the async get-stats promise. The promise callback
    // fires on a GStreamer thread and must NOT touch `this` if we've been
    // destroyed meanwhile. The callback copies this shared_ptr out, parses the
    // reply (self-independent), then hops to the main thread and only touches
    // members if the token is still true. Set false in the destructor (main
    // thread), so the check and the destruction are serialized on one thread.
    std::shared_ptr<std::atomic<bool>> m_alive {
        std::make_shared<std::atomic<bool>>(true) };

    static void onNegotiationNeeded(GstElement *webrtc, gpointer userData);
    static GstPadProbeReturn onCaptureBuffer(GstPad *pad, GstPadProbeInfo *info, gpointer userData);
    static GstFlowReturn onPreviewSample(GstAppSink *sink, gpointer userData);

    VideoFrameProvider *m_previewProvider = nullptr;
    GstElement         *m_previewAppsink  = nullptr;
    static void onIceCandidate(GstElement *webrtc, guint mlineIndex, gchar *candidate, gpointer userData);
    static void onOfferCreated(GstPromise *promise, gpointer userData);
    static void onIceStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    static void onIceGatheringStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    // Send-side adaptive bitrate (rtpgccbwe). Screen needs this most:
    // mostly-static content lets GCC idle near-zero and burst on motion.
    static GstElement *onRequestAuxSender(GstElement *webrtc, GObject *transport,
                                          gpointer userData);
    static void onGccBitrate(GObject *gcc, GParamSpec *pspec, gpointer userData);
};
