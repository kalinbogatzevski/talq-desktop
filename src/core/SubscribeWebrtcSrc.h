#pragma once

// Receive-only video/audio for the Talk MCU, built on gst-plugins-rs
// `webrtcsrc` instead of a hand-driven webrtcbin. webrtcsrc owns
// webrtcbin internally and gets the receive negotiation (RTX/FEC,
// transceivers, jitterbuffer, decode, threading) right — the exact
// things the old SubscribePipeline kept failing at. We feed it the
// Nextcloud-spreed↔Janus signalling through a custom GObject that
// implements gst-plugins-rs' `Signallable` interface; this class is the
// Qt bridge for that signaller and exposes the SAME surface CallManager
// already used for SubscribePipeline (drop-in).

#include <QObject>
#include <QTimer>
#include <QString>
#include <QList>
#include <QPair>
#include <QMutex>
#include <atomic>
#include <mutex>
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/app/gstappsink.h>
#include "SignalingClient.h"

class VideoFrameProvider;

class SubscribeWebrtcSrc : public QObject
{
    Q_OBJECT
public:
    explicit SubscribeWebrtcSrc(const QString &remoteSessionId, QObject *parent = nullptr);
    ~SubscribeWebrtcSrc() override;

    bool start(const QString &stunServer, const QList<TurnServer> &turnServers,
               const QString &audioOutputDeviceId = {});
    void stop();

    // 0.51.x AEC single-pipeline fix: when set BEFORE start(), the decoded audio
    // branch pushes 48k/S16LE/mono into THIS external appsrc (the publisher
    // pipeline's far-end mixer) and does NOT create its own wasapi2sink — so
    // playback runs through the one shared, probed sink that lives in the
    // publisher pipeline (probe + webrtcdsp on one clock = the actual fix). The
    // appsrc is owned by the publisher pipeline; we ref it for our streaming-
    // thread pushes and unref on teardown (after disconnecting the appsink).
    // Null (default) keeps the legacy per-subscriber wasapi2sink playback.
    void setFarEndAppsrc(GstElement *appsrc);
    void setRemoteOffer(const QString &sdp);
    void addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    bool isRunning() const { return m_running; }
    VideoFrameProvider *videoProvider() const { return m_videoProvider; }
    QString videoCodec() const { return m_videoCodec; }
    QString videoDecoder() const { return QStringLiteral("webrtcsrc"); }
    // Live receive-video diagnostics (#111/#112): decoded frames/sec
    // actually arriving from this peer, and the mean buffer-PTS gap
    // between them. fps≈1 with gap≈1000 ms ⇒ the sender is only emitting
    // ~1 fps; fps≈1 with gap≈33 ms ⇒ frames are dropped on receive.
    int rxVideoFps() const { return m_rxFps.load(std::memory_order_relaxed); }
    int rxPtsGapMs() const { return m_rxPtsGapMs.load(std::memory_order_relaxed); }
    // Distinct-content fps: how many of the delivered frames were actually
    // a new picture. delivered≈30 but distinct≈1 ⇒ the decoder is
    // concealing (repeating the last good frame) — the "perfect frozen
    // image" symptom, quantified. delivered≈distinct ⇒ decode is fine and
    // the fault is elsewhere (sender rate, render). THE decisive number.
    int rxDistinctVideoFps() const { return m_rxDistinctFps.load(std::memory_order_relaxed); }
    // Decoded frame resolution of the substream the SFU is currently
    // forwarding (changes live when the active simulcast layer switches).
    int rxWidth()  const { return m_rxWidth.load(std::memory_order_relaxed); }
    int rxHeight() const { return m_rxHeight.load(std::memory_order_relaxed); }

signals:
    void localAnswerReady(const QString &sdp);
    void iceCandidateReady(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void iceGatheringComplete();
    void iceStateChanged(const QString &state);
    void mediaStateReceived(const QString &type);     // unused via webrtcsrc (signaling drives mute)
    void peerClientInfo(const QString &client, const QString &version);
    // Decoded remote-mic peak level (0..1) from a `level` element on this
    // peer's playback chain — drives the per-peer VU meter in the call UI.
    // Without it the remote mic indicator never moved (only the self meter,
    // fed by the publisher's level element, did).
    void audioLevelUpdated(double level);
    void error(const QString &message);
    void sessionEnded();   // SFU ended this feed; CallManager re-subscribes

private:
    void cleanup();
    // GstRSWebRTCSignallableIface implementor (registered once at runtime).
    GObject *makeSignaller();
    void onVideoSample(GstSample *sample);
    // Emit session-started + session-description(offer) into the signaller
    // once BOTH webrtcsrc has emitted "start" and we hold the Janus offer.
    // Drives the answerer handshake; safe to call from either thread.
    void feedOfferToSignaller();

    // webrtcsrc→signaller signal handlers (it emits these ON the signaller;
    // we connect, run before the Rust no-op default, forward to spreed).
    static gboolean sigStart(GObject *signaller, gpointer self);
    static gboolean sigStop(GObject *signaller, gpointer self);
    static gboolean sigSendSdp(GObject *signaller, const gchar *sessionId,
                               GstWebRTCSessionDescription *desc, gpointer self);
    static gboolean sigSendIce(GObject *signaller, const gchar *sessionId,
                               const gchar *candidate, guint mlineIndex,
                               const gchar *mid, gpointer self);
    static gboolean sigEndSession(GObject *signaller, const gchar *sessionId, gpointer self);
    // Signaller emits this with webrtcsrc's internal webrtcbin. Lets us
    // drive ICE directly on webrtcbin if the send-ice signal path is
    // unreliable from C (it lacks .run_last() unlike send-sdp).
    static void onWebrtcbinReady(GObject *signaller, const gchar *peerId,
                                 GstElement *webrtcbin, gpointer self);
    static void onWebrtcbinLocalIce(GstElement *webrtcbin, guint mlineIndex,
                                    gchar *candidate, gpointer self);
    // webrtcsrc's internal webrtcbin owns the real ICE connection state.
    // Without this, iceStateChanged is never emitted and CallManager never
    // leaves "Connecting" even though media is flowing.
    static void onWebrtcbinIceConnState(GstElement *webrtcbin,
                                        GParamSpec *pspec, gpointer self);
    static void onPadAdded(GstElement *src, GstPad *pad, gpointer self);
    static GstFlowReturn onVideoNewSample(GstAppSink *sink, gpointer self);
    // 0.51.x AEC single-pipeline fix: pulls each decoded sample and pushes it to
    // m_farAppsrcExt (the publisher pipeline's far-end mixer) — this IS the
    // playout path now (no per-subscriber wasapi2sink). Streaming thread.
    static GstFlowReturn onAecPlayoutSample(GstAppSink *sink, gpointer self);

    QString m_remoteSessionId;
    QString m_sessionId = QStringLiteral("talq");  // our id within the signaller
    GstElement *m_pipeline = nullptr;
    GstElement *m_webrtcsrc = nullptr;
    GstElement *m_webrtcbin = nullptr;  // weak: webrtcsrc-owned, via webrtcbin-ready
    GObject *m_signaller = nullptr;
    GstElement *m_videoConvert = nullptr;
    // The decoded-video appsink. Created + stored on the GStreamer STREAMING thread
    // in onPadAdded; its "new-sample" handler is disconnected from the Qt MAIN thread
    // in cleanup(). 0.52.17: we now hold OUR OWN ref on it (taken in onPadAdded under
    // m_videoSinkMutex, dropped in cleanup after the disconnect) so the pipeline's
    // detached NULL/unref worker can't free it under cleanup's signal-disconnect —
    // that borrowed-pointer UAF faulted in libgobject on hangup (the 0.52.16
    // deleteLater() fix was downstream of this and didn't cover it).
    GstElement *m_videoAppsink = nullptr;
    // Serializes onPadAdded's pad-attach (the gst_bin_add_many into m_pipeline + the
    // m_videoAppsink ref-store, video AND audio) against cleanup's snapshot+null of
    // m_videoAppsink and m_pipeline. Held by both sides so a streaming-thread pad-add
    // can't attach into / ref past a pipeline the main thread is tearing down.
    std::mutex  m_videoSinkMutex;
    // Set true at the very top of cleanup(); onPadAdded bails immediately if set, so a
    // late streaming-thread pad event can't build/attach a new appsink into a pipeline
    // the main thread is already tearing down. Mirrors PublishPipeline::m_shuttingDown.
    std::atomic<bool> m_shuttingDown{false};
    VideoFrameProvider *m_videoProvider = nullptr;
    // Keyframe-stall watchdog: if no frame decodes shortly after ICE connects,
    // the first IDR was lost and the tile sits frozen on "Starting…" — re-request
    // a keyframe (PLI) every 2.5 s up to 5× until the first frame arrives.
    QTimer            *m_keyframeWatchdog = nullptr;   // child of this (main thread)
    std::atomic<bool>  m_decodedAnyFrame{false};       // set on the streaming thread
    int                m_keyframePliRetriesLeft = 0;
    // 0.51.x AEC single-pipeline fix: external appsrc (publisher pipeline's
    // far-end mixer) that decoded audio is pushed into INSTEAD of a per-sub
    // wasapi2sink. Refed in setFarEndAppsrc, unrefed in cleanup (after the
    // appsink is disconnected) so a late streaming-thread push can't UAF it.
    // The streaming-thread push (onAecPlayoutSample) reads this while the Qt
    // thread swaps it (setFarEndAppsrc re-points to a new publisher mixer on a
    // publisher rebuild). Guarded by m_farAppsrcMutex: the reader takes a ref
    // UNDER the lock so the writer's unref can't free it mid-push (an atomic
    // pointer can't fix this — load + ref aren't one atomic step).
    GstElement *m_farAppsrcExt   = nullptr;
    std::mutex  m_farAppsrcMutex;
    GstElement *m_aecPlayoutSink = nullptr;  // the appsink feeding m_farAppsrcExt
    // RX video-rate probe (#111): measures the cadence of decoded frames
    // actually arriving from webrtcsrc, with their buffer-PTS deltas.
    // Distinguishes "sender only emits ~1 fps" (ptsΔ ≈ frame interval, e.g.
    // ~1000 ms) from "receiver/decoder drops frames" (ptsΔ ≈ 33 ms but few
    // pulled). Logged once per ~1 s window.
    qint64  m_rxWinStartUs = 0;   // streaming-thread only
    int     m_rxFrames     = 0;   // streaming-thread only
    quint64 m_rxLastPtsNs  = 0;   // streaming-thread only
    quint64 m_rxDtSumNs    = 0;   // streaming-thread only
    quint64 m_rxLastHash   = 0;   // streaming-thread only — last frame fingerprint
    int     m_rxDistinct   = 0;   // streaming-thread only — distinct frames in window
    std::atomic<int> m_rxFps{0};         // delivered fps, published to UI thread
    std::atomic<int> m_rxPtsGapMs{0};    // mean PTS gap ms, published to UI thread
    std::atomic<int> m_rxDistinctFps{0}; // distinct-content fps, published to UI thread
    std::atomic<int> m_rxWidth{0};       // decoded frame width (active substream)
    std::atomic<int> m_rxHeight{0};      // decoded frame height (active substream)
    QString m_videoCodec;
    QString m_audioOutputDeviceId;
    bool m_running = false;
    bool m_offerDelivered = false;     // session-started+session-description emitted
    bool m_webrtcStarted = false;      // webrtcsrc emitted "start" on the signaller
    int m_lastIceConnState = -1;       // de-dup webrtcbin ice-connection-state notify
    QString m_pendingOffer;            // Janus offer SDP, held until both ready
    QList<QPair<int, QString>> m_pendingRemoteIce;  // mline, candidate
    QMutex m_feedMutex;                // serialises feedOfferToSignaller (2 threads)
    guint m_busWatchId = 0;
};
