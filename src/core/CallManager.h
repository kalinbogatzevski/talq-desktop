#pragma once

#include <QObject>
#include <QTimer>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QPair>
#include <QByteArray>
#include <QPointer>
#include <functional>
#include "core/ApiClient.h"
#include "core/SignalingClient.h"
#include "core/PublishPipeline.h"
#include <gst/app/gstappsink.h>   // GstAppSink (for the #13 preview new-sample callback)
#include "core/SubscribePipeline.h"
#include "core/SubscribeWebrtcSrc.h"
#include "core/PeerPipeline.h"
#include "core/MediaDeviceManager.h"
#include "core/VideoFrameProvider.h"
#include "core/ScreenSharePipeline.h"
#include "core/ShareStartPolicy.h"
#include "core/EncodeTier.h"
#include "core/SubscriberStallPolicy.h"
#include "core/PublisherStallPolicy.h"
#include "core/CallParticipant.h"
#include "core/MediaLoadController.h"   // 0.51.x dynamic encode/decode load controller

class BackgroundEngine;   // #20 — owned by CallManager; lives across calls.
class ConversationListModel;
class ShareOverlay;       // #72 — coloured monitor border while screen-sharing.

class CallManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(CallState state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool isMuted READ isMuted NOTIFY muteChanged)
    Q_PROPERTY(bool isCameraOn READ isCameraOn NOTIFY cameraChanged)
    Q_PROPERTY(bool isCameraUnavailable READ isCameraUnavailable NOTIFY cameraChanged)
    Q_PROPERTY(int callDuration READ callDuration NOTIFY durationChanged)
    Q_PROPERTY(QString remotePeerName READ remotePeerName NOTIFY callInfoChanged)
    Q_PROPERTY(QString remotePeerId READ remotePeerId NOTIFY callInfoChanged)
    Q_PROPERTY(double audioLevel READ audioLevel NOTIFY audioLevelChanged)
    Q_PROPERTY(QString callStats READ callStats NOTIFY callStatsChanged)
    Q_PROPERTY(VideoFrameProvider* remoteVideoProvider READ remoteVideoProvider NOTIFY remoteVideoProviderChanged)
    Q_PROPERTY(VideoFrameProvider* localVideoProvider READ localVideoProvider NOTIFY localVideoProviderChanged)
    Q_PROPERTY(QString statusDetail READ statusDetail NOTIFY statusDetailChanged)
    Q_PROPERTY(bool callsAvailable READ callsAvailable CONSTANT)
    Q_PROPERTY(QString callsUnavailableReason READ callsUnavailableReason CONSTANT)
    Q_PROPERTY(bool remoteVideoMuted READ remoteVideoMuted NOTIFY remoteMediaChanged)
    Q_PROPERTY(bool remoteAudioMuted READ remoteAudioMuted NOTIFY remoteMediaChanged)
    Q_PROPERTY(QString remotePeerClient READ remotePeerClient NOTIFY callInfoChanged)

public:
    // Reconnecting sits between Active and Ending: the call is logically
    // still up but the publisher media path is being actively rebuilt after
    // an ICE failure (Zoom-style — we never auto-drop). Appended before
    // Ending so Idle..Active keep their existing int values; consumers
    // compare by enum name, not literal.
    enum CallState { Idle, Outgoing, Incoming, Connecting, Active, Reconnecting, Ending };
    Q_ENUM(CallState)

    // True only once a call has fully torn down (Idle) or is tearing down
    // (Ending). Used to reject signaling — subscriber/screen OFFERS and ICE
    // CANDIDATES — that the MCU emits a beat after we hang up: without this an
    // in-flight offer rebuilds a pipeline post-teardown and plays the peer's
    // audio with no call screen (orphan-audio-after-hangup, seen in the
    // dual-call/glare case). It deliberately does NOT cover Outgoing/Incoming:
    // during an outgoing MCU ring the publisher legitimately exchanges trickle
    // ICE with Janus (remote candidates arrive from our OWN session and it needs
    // them to connect), and a peer already in an open room can offer before we
    // reach Connecting. Mirrors the established m_state!=Idle&&!=Ending guard.
    bool callTornDown() const {
        return m_state == Idle || m_state == Ending;
    }

    explicit CallManager(ApiClient *api, SignalingClient *signaling, MediaDeviceManager *deviceMgr, QObject *parent = nullptr);

    CallState state() const { return m_state; }
    bool isMuted() const { return m_muted; }
    bool isCameraOn() const { return m_cameraOn; }
    // True when the camera device failed to open during this call (missing,
    // in use by another app, or blocked by OS privacy). Distinct from
    // "camera off" (a deliberate user toggle): the call surface uses this to
    // show a "Camera unavailable" notice instead of a silent black tile.
    bool isCameraUnavailable() const { return m_cameraUnavailable; }
    // True when the microphone could not be opened during this call (missing,
    // in use by another app, OS-blocked, or a device id wasapi2 couldn't
    // resolve) and the publisher fell back to a SILENT source. The call still
    // runs (video + receive intact); the surface shows a "microphone
    // unavailable" banner instead of pretending the mic is live.
    bool isMicUnavailable() const { return m_micUnavailable; }
    int callDuration() const { return m_callDuration; }
    QString remotePeerName() const { return m_remotePeerName; }
    QString remotePeerId() const { return m_remotePeerId; }
    double audioLevel() const { return m_audioLevel; }
    QString callStats() const { return m_callStats; }
    VideoFrameProvider *remoteVideoProvider() const { return m_remoteVideoProvider; }
    VideoFrameProvider *localVideoProvider() const { return m_localVideoProvider; }
    // 0.41.1-beta — local self-preview of the screen being shared.
    // Returns nullptr unless a screen-share pipeline exists and the
    // BGRx appsink tee is wired (which depends on gst-plugins-base).
    VideoFrameProvider *localScreenPreviewProvider() const;
    QString statusDetail() const { return m_statusDetail; }
    bool callsAvailable() const { return m_callsAvailable; }
    QString callsUnavailableReason() const { return m_callsUnavailableReason; }
    bool remoteVideoMuted() const { return m_remoteVideoMuted; }
    bool remoteAudioMuted() const { return m_remoteAudioMuted; }
    QString remotePeerClient() const { return m_remotePeerClient; }
    QString gpuAccelStatus() const { return m_gpuAccelStatus; }
    talq::GpuClass gpuClass() const { return m_gpuClass; }
    QString activeVideoCodec() const;
    QString activeVideoDecoder() const;
    // Publish (or screen-share) encoder, e.g. "H264 · nvh264enc · hw".
    QString activeVideoEncoder() const;
    // Whether the active video encoder is hardware-accelerated (pill tint).
    bool activeVideoEncoderIsHw() const;
    // Decoded resolution of the incoming stream (active simulcast layer),
    // e.g. "1280×720"; empty until the first frame decodes.
    QString activeRxResolution() const;
    // Peak decoded height observed from the remote this call (across substream
    // switches) — the honest basis for the receive-quality dropdown's HIGH
    // label, instead of guessing from our OWN send setting. 0 until a frame
    // decodes. Reset on call teardown.
    int peerPeakRxHeight() const { return m_peerPeakRxHeight; }
    // Send resolution + live bitrate, e.g. "1280×720 · 2.5 Mbps".
    QString videoTxLabel() const;
    // Current outbound stream bandwidth (GCC-applied video + Opus audio),
    // e.g. "↑ 2.45 Mbps". Empty when nothing is being sent.
    QString streamBandwidthLabel() const;
    // Numeric outbound bitrate (Mbps) for the telemetry bandwidth gauge.
    double txBitrateMbps() const;

    Q_INVOKABLE void startCall(const QString &token, bool withVideo);
    Q_INVOKABLE void setRemotePeerInfo(const QString &name, const QString &peerId);
    Q_INVOKABLE void acceptCall(bool withVideo);
    // 0.41.5-beta — inject the conversation list so the call mode
    // decision (m_useP2P) can read the room type and force P2P for
    // 1:1 calls even when the HPB advertises an MCU. Group calls
    // (type != 1) still use the MCU.
    void setConversations(ConversationListModel *c) { m_conversations = c; }
    // 0.41.5-beta — telemetry-pill readout. True = direct WebRTC P2P,
    // false = MCU/SFU forward. Decided once per call in setState→
    // ensureSignalingRoomJoined; stable for the call's lifetime.
    bool isUsingP2P() const { return m_useP2P; }
    Q_INVOKABLE void declineCall();
    // True when the current incoming/active call was offered with video, so
    // the incoming UI can offer an "answer with video" choice.
    bool callHasVideo() const { return m_withVideo; }
    Q_INVOKABLE void setUserActionReady();
    Q_INVOKABLE void hangUp();
    // Best-effort: free the server-side call participant on a clean exit
    // (window close / quit / logout) WITHOUT tearing pipelines, then
    // briefly flush the DELETE so it lands before the process exits.
    // Idempotent — safe to call when not in a call.
    void leaveCallBeacon();
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void toggleCamera();
    Q_INVOKABLE void startScreenShare(int monitorIndex = 0, quintptr windowHandle = 0);
    Q_INVOKABLE void stopScreenShare();
    bool isScreenSharing() const { return m_screenSharing; }
    // Runtime screen-share quality. Levels: 0=720p 1=1080p(default)
    // 2=1440p 3=Native. Changing it while sharing does a quick managed
    // re-share at the new cap, reusing the already-picked target.
    Q_INVOKABLE void setScreenShareQuality(int level);
    int screenShareQuality() const { return m_ssQuality; }

    // #132 simulcast: ask the SFU to forward substream `substream`
    // (0=180p / 1=360p / 2=720p) for our subscription to `sessionId`.
    // Driven by CallStage from the on-screen tile size (stage→2, gallery
    // →1, strip→0). Deduped per peer; re-sent when the subscriber (re)
    // connects. Janus adapts DOWN on its own if the link can't sustain
    // the requested layer, so this is an upper bound, not a guarantee.
    Q_INVOKABLE void requestPeerVideoQuality(const QString &sessionId, int substream);

    // 0.51.x dynamic load controller — receive actuator. Cap every peer's
    // received substream at `substreamCap` (min with the tile-size want), EXEMPT
    // the focused/largest tile unless `capFocused`. Re-sends selectStream only
    // where the effective substream changed. Idempotent. See MediaLoadController.h.
    void applyReceiveLoadCaps(int substreamCap, bool capFocused);

    // Incoming-call ringtone roster (id, label) for the Settings dropdown
    // + persistence under Calls/incomingRingtone. "default" = synthesized
    // TalQ ring; classic/bright/soft = bundled CC0 rings; "none" = silent.
    static QVector<QPair<QString, QString>> ringtones();
    // Play a ringtone once (no loop) for the Settings preview. Static so the
    // Settings dialog can audition without a live CallManager instance.
    static void auditionRingtone(const QString &id);
    VideoFrameProvider *remoteScreenProvider() const { return m_remoteScreenProvider; }
    void onIncomingCallDetected(const QString &callerName, const QString &token, int callFlag);

    // Multi-party model. Stable order: self first, then join order. The
    // legacy 1:1 getters above keep working (P2P = self + one remote).
    QList<CallParticipant*> participants() const { return m_participantOrder; }
    CallParticipant *selfParticipant() const { return m_selfParticipant; }

    // #20 — long-lived BackgroundEngine. Owned here so it persists across
    // calls (saves the GL context creation cost on every call). The
    // current PublishPipeline reads it at construction. Public for the
    // PublishPipeline wire-up + the Settings live-apply path.
    BackgroundEngine *backgroundEngine() const { return m_backgroundEngine; }

public slots:
    // #20 — re-read Talk/Backgrounds/* from QSettings and push to the
    // BackgroundEngine. Hooked to SettingsDialog::backgroundSettingsChanged
    // so changes apply live without rebuilding the publisher pipeline.
    void applyBackgroundSettings();

signals:
    void stateChanged();
    void muteChanged();
    void cameraChanged();
    void durationChanged();
    void callInfoChanged();
    void incomingCall(const QString &callerName, const QString &token, bool withVideo);
    void callEnded(const QString &reason);
    // A call we tried to place was rejected by the SERVER (e.g. HTTP 5xx on
    // POST call/{token}) and could not be established. Distinct from callEnded
    // (a normal terminal teardown): this carries a plain-language title +
    // message the UI shows as a prominent, must-dismiss dialog, so a server
    // outage never looks like a silent, unexplained call drop.
    void callFailed(const QString &title, const QString &message);
    // Fires when the leaveCall DELETE actually returns from the server.
    // Distinct from callEnded (which fires synchronously in teardown so
    // the UI never blocks on the network). Consumers that need server-
    // side state to be settled — currently the UserStatus call-status
    // revert, which would 404 if it ran before the server processed the
    // leave — should listen here. If the network is dead, this signal
    // never fires; the server's own session timeout handles cleanup.
    void callServerLeaveAcked();
    void audioLevelChanged();
    void callStatsChanged();
    void remoteVideoProviderChanged();
    void localVideoProviderChanged();
    void statusDetailChanged();
    void remoteMediaChanged();
    void screenShareChanged();
    void screenShareRetrying();            // share didn't confirm; auto-retrying
    void screenShareFailed(const QString &reason);  // gave up after retries
    void screenShareQualityChanged();
    void remoteScreenProviderChanged();
    void participantAdded(CallParticipant *p);
    void participantRemoved(const QString &sessionId);
    void participantsChanged();

private slots:
    void onParticipantJoinedCall(const QString &sessionId, int flags, const QString &displayName);
    void onParticipantLeftCall(const QString &sessionId);
    void onOfferReceived(const QString &fromSessionId, const QString &sdp, const QString &sid);
    void onAnswerReceived(const QString &fromSessionId, const QString &sdp);
    void onAudioLevelUpdated(double level);

private:
    void setState(CallState newState);

    // 0.51.x load controller internals.
    int     effectiveSubstreamFor(const QString &sessionId, int want) const; // apply recv cap + focus exemption
    QString focusedPeer() const;                       // peer with the highest tile-size want
    void    sendDesiredSubstream(const QString &sessionId, int substream);   // dedupe + selectStream
    void    startLoadController();   // arm the ~1 s tick for a live call (kill-switch aware)
    void    stopLoadController();    // disarm + reset caps to full on call end
    void    onLoadTick();            // one controller tick → apply send + receive caps
    void joinCallOnServer(bool withVideo);
    // Asynchronous: invokes `onDone` (if provided) when the DELETE /call
    // response comes back from the server, or immediately if there's no
    // call to leave. Qt's network reply callback fires unconditionally
    // (success, HTTP error, transport timeout) — no safety timer needed.
    // teardown() uses this to gate callEnded so the UserStatusManager
    // revert hits a server already in the post-call state.
    // token + wasJoined are snapshotted by teardown() before it clears
    // m_callToken / m_joinedCall, so the DELETE /call still fires (and the
    // other party gets the "left call" event). Async; onDone runs on the
    // server ACK (or immediately if there was no joined call).
    void leaveCallOnServer(const QString &token, bool wasJoined,
                           std::function<void()> onDone = {});
    void teardown(const QString &reason);
    void stopAllPipelines();
    void startRingtone();
    void stopRingtone();
    int videoDeviceIndex() const;
    bool preferHd1080() const;
    void broadcastMediaState(const QString &media, bool enabled);
    void updateCallFlags();

    // Upstream Talk requires the signaling room (session + WS room) to be
    // joined before POST call/{token}, or participant/offer events are
    // missed. Runs `next` once that is guaranteed (immediately if already
    // in the room). Used by both the outgoing and incoming paths so they
    // share one ordering.
    void ensureSignalingRoomJoined(std::function<void()> next);

    // Subscribe to a remote peer's stream. Upstream retries `requestoffer`
    // until the MCU delivers an offer; a single send races MCU readiness
    // and yields a connected call with no audio from that peer.
    void requestPeerStream(const QString &sessionId);
    // #bug3 -- if `sessionId` is the 1:1 peer returning from a grace hold (same
    // NC userId), re-adopt + re-subscribe and cancel grace. Returns true if so.
    bool tryAdoptReturningPeer(const QString &sessionId);
    // A subscriber feed died mid-call (SFU end-session / its webrtcbin ICE
    // went "failed" — both happen on a normal publisher renegotiation).
    // Tear down just that subscriber and re-subscribe; never kill the call.
    void recoverSubscriber(const QString &sessionId, const QString &reason);

    // Publisher (our send leg) reconnect — the Zoom-style "never drop"
    // counterpart to recoverSubscriber. recoverPublisher() enters/keeps the
    // Reconnecting state and arms m_pubRetryTimer with backoff (never tears
    // the call down). rebuildPublisherAndReoffer() runs on the timer: it
    // deleteLater()s the dead publisher and rebuilds via buildAndStartPublisher().
    // buildAndStartPublisher() creates the PublishPipeline, wires its signals,
    // starts it on the CACHED m_stunServer/m_turnServers and re-offers; it is
    // also the first-join path (factored out of joinCallOnServer). Returns
    // false if the pipeline failed to start (caller decides retry vs teardown).
    void recoverPublisher(const QString &reason);
    void rebuildPublisherAndReoffer();
    bool buildAndStartPublisher();

    // --- Participant registry (additive; mirrors signaling/pipeline state) ---
    CallParticipant *ensureParticipant(const QString &sessionId, const QString &name);
    CallParticipant *ensureSelfParticipant();
    void removeParticipant(const QString &sessionId);
    void syncSelfParticipant();
    void clearParticipants();
    QHash<QString, CallParticipant*> m_participants;   // sessionId -> participant
    QList<CallParticipant*> m_participantOrder;         // self first, then join order
    CallParticipant *m_selfParticipant = nullptr;

    ApiClient *m_api;
    SignalingClient *m_signaling;
    MediaDeviceManager *m_deviceManager = nullptr;
    // MCU dual pipelines
    PublishPipeline *m_publishPipeline = nullptr;

    // #20 background engine — long-lived, parented to CallManager so it
    // outlives individual calls. Constructed in CallManager's ctor.
    BackgroundEngine *m_backgroundEngine = nullptr;
    QHash<QString, SubscribeWebrtcSrc*> m_subscribePipelines;
    QHash<QString, QString> m_subscriberSids;  // sessionId -> current MCU sid
    // #132 simulcast: per-peer desired receive substream (0/1/2). Default
    // 2 (highest) so a fresh subscription / 1:1 call gets 720p; CallStage
    // refines it down per tile size. Sent via selectStream on connect +
    // on change.
    QHash<QString, int> m_desiredSubstream;
    // 0.51.x load controller — receive side. m_peerSubstreamWant is the RAW
    // tile-size want per peer (before the load cap), so a load-cap or focus
    // change can be re-applied without the UI re-deciding. m_recvLoadSubstreamCap
    // (2 = no cap) + m_recvLoadCapFocused are set by the controller tick.
    QHash<QString, int> m_peerSubstreamWant;
    int  m_recvLoadSubstreamCap = 2;
    bool m_recvLoadCapFocused   = false;
    // The controller itself (pure logic) + its ~1 s tick. Owned here because
    // load is a property of THIS machine, not of any one peer. The synthetic
    // fields feed it on the dev box (NVENC won't overload) until the encode/
    // decode latency probes land; TALQ_DISABLE_LOAD_CONTROLLER is the kill-switch.
    talq::MediaLoadController m_loadController;
    QTimer m_loadTimer;
    bool   m_loadControllerEnabled = true;
    // Verbose [MEDIA]/[LEAK] call-diagnostic heartbeats — OFF by default. The
    // talq::leak counters always count (cheap), but the per-second qInfo lines
    // + their forced disk sync only fire when this is on. Resolved per call in
    // startLoadController() from TALQ_MEDIA_DIAG (env override) or the
    // persisted Debug/mediaDiagnostics setting. See LeakStats.h.
    bool   m_mediaDiag = false;
    double m_synthEncodeUsage = 0.0;   // TALQ_TEST_ENCODE_USAGE
    double m_synthDecodeUsage = 0.0;   // TALQ_TEST_DECODE_USAGE
    QHash<QString, int> m_subscriberRecoveries;  // sessionId -> mid-call re-subscribe count (bounded; reset on connect)
    // #bug2 -- per-peer frame-stall watchdog. updateCallStats() ticks each
    // subscriber's SubscriberStallPolicy every 2 s; a feed that delivered frames
    // then froze (its publisher reconnected under new SSRCs, with no ICE-failed
    // to trip recovery) fires recoverSubscriber. Pruned on recover/re-offer/left.
    QHash<QString, SubscriberStallPolicy> m_subStall;
    // Publisher outbound-RTP stall watchdog. updateCallStats() ticks it every
    // 2 s with the summed packets-sent; when the local publisher stops sending
    // while it should be (consent revoked but publisher ICE stuck "completed",
    // e.g. the peer left without a clean leave), it fires recoverPublisher --
    // stopping the "consent revoked" nicesink hot-loop that froze a peer's
    // laptop. Reset on each publisher (re)build + full call teardown.
    PublisherStallPolicy m_pubStall;
    QByteArray m_ringtoneData;  // backing buffer for the selected ring (SND_ASYNC reads from it)

    // #13: pre-answer self-preview pipeline. Standalone camera→appsink
    // pipeline that runs while an incoming VIDEO call rings, so the callee
    // sees themselves before answering. STRICTLY teardown-synchronous on
    // accept/decline (camera must be released before PublishPipeline
    // grabs it — otherwise Windows MF device contention breaks the call).
    GstElement *m_previewPipeline = nullptr;
    GstElement *m_previewAppsink  = nullptr;
    VideoFrameProvider *m_previewProvider = nullptr;
    void startIncomingCameraPreview();
    void stopIncomingCameraPreview();
    static GstFlowReturn onPreviewSample(GstAppSink *sink, gpointer userData);
    // P2P single pipeline
    PeerPipeline *m_peerPipeline = nullptr;
    bool m_useP2P = false;
    // 0.41.5-beta — set by MainWindow at construction. Used at the
    // m_useP2P decision point to look up the current call's room type
    // (1 = one-to-one). When the user prefers P2P for 1:1, we force
    // P2P even on instances where the HPB advertises an MCU.
    ConversationListModel *m_conversations = nullptr;
    QTimer m_glibTimer;  // shared GLib main context pump

    CallState m_state = Idle;
    QString m_callToken;
    QString m_stunServer;
    QList<TurnServer> m_turnServers;
    QString m_remoteSessionId;
    QString m_remotePeerName;
    QString m_remotePeerId;
    QString m_remotePeerClient;  // "TalQ/X.Y.Z" or empty for non-TalQ peers
    bool m_muted = false;
    bool m_cameraOn = false;
    // Set when the camera device fails to start mid-call; drives the
    // "Camera unavailable" UI. Reset at the start of every call and whenever
    // the user toggles the camera back on (a retry).
    bool m_cameraUnavailable = false;
    // Set when the mic can't be opened and the publisher fell back to silent
    // audio; drives the "microphone unavailable" banner. Reset at every call.
    bool m_micUnavailable = false;
    bool m_speaking = false;
    QTimer m_speakingGrace;
    bool m_cameraFallbackTried = false;
    bool m_withVideo = false;
    int m_callDuration = 0;
    double m_audioLevel = 0.0;
    QString m_callStats;
    QString m_statusDetail;
    bool m_callsAvailable = true;
    QString m_callsUnavailableReason;
    QString m_gpuAccelStatus;
    // Encode-capability class (drives the camera/screen caps) — from HW-encoder
    // presence + GPU model name + the user override, NOT the decode tier above.
    talq::GpuClass m_gpuClass = talq::GpuClass::Software;
    // Peak remote decoded height this call (see peerPeakRxHeight()). mutable:
    // accumulated lazily inside the const activeRxResolution() read path.
    mutable int m_peerPeakRxHeight = 0;
    void checkGStreamerPlugins();
    void detectGpuClass();   // (re)classify encode capability; re-run per call
    void updateCallStats();
    void setStatusDetail(const QString &detail) {
        if (m_statusDetail != detail) { m_statusDetail = detail; emit statusDetailChanged(); }
    }

    QTimer m_durationTimer;
    QTimer m_ringTimeout;
    QTimer m_statsTimer;
    // Publisher ICE "failed" means our send path to the MCU died (e.g. a
    // long-haul link revoking ICE consent). Zoom-style recovery: NEVER
    // auto-drop. On "failed" we enter the Reconnecting state and arm this
    // single-shot timer with exponential backoff; each fire rebuilds the
    // publisher pipeline and re-offers to the MCU (reusing the cached
    // STUN/TURN so it works even while the network is still down). Only a
    // user Cancel/Leave, the peer leaving, or the room closing ends the
    // call. Any publisher ICE "connected"/"completed" cancels recovery and
    // returns us to Active.
    QTimer m_pubRetryTimer;
    // REST participant-poll backup (mobile/internal-signaling fallback). Owned
    // as a member + killed in stopAllPipelines so a re-entered call setup can't
    // leak the prior one (1.0 audit).
    QTimer *m_callPollTimer = nullptr;
    int    m_pubRetryAttempts   = 0;     // resets to 0 on ICE connected/completed
    bool   m_pubRebuildInFlight = false; // serialize rebuilds (one at a time)
    // #bug3 -- peer-grace: a transient remote-1:1-peer inCall=0 (WiFi blip) or a
    // full session drop+rejoin under a NEW NC session id must NOT end the call.
    // Separate from m_pubRetryTimer -- both drive Reconnecting and can be live at
    // once (a link flap downs our publisher AND the peer). MCU path only.
    QTimer  m_peerGraceTimer;            // single-shot, armed on remote-peer-left
    bool    m_peerGraceActive = false;   // true while waiting for the peer to return
    QString m_remotePeerUserId;          // NC userId of the 1:1 peer (correlation key)
    QString m_graceLeftSid;              // the (now dead) session id the peer left under
    // 0.40.15 — sticky flag: publisher ICE has been seen at "connected"
    // or "completed" at some point. The iceStateChanged handler only
    // promotes Connecting→Active when the event fires WHILE m_state is
    // already Connecting; if publisher ICE finishes BEFORE the
    // participant-joined signal flips us into Connecting (the common
    // case when the SFU is fast and the callee is slow to accept), the
    // promotion is missed and we stick in Connecting until some later
    // event nudges us. setState() now consults this flag whenever it
    // transitions INTO Connecting and promotes immediately if it's
    // already been "connected". Reset on every fresh call attempt.
    bool m_pubIceConnectedSeen = false;
    // #66 — P2P twin of the above. A 1:1 direct call's ICE can reach
    // "connected" before participant discovery flips us to Connecting; remember
    // it so setState(Connecting) promotes straight to Active instead of waiting
    // on the 12 s MCU-fallback timer. Reset on every fresh call attempt.
    bool m_p2pIceConnectedSeen = false;

    bool m_joinedCall = false;
    bool m_userActionReady = false;

    QString m_lastDeclinedToken;
    QDateTime m_lastDeclinedTime;
    // A call we just TRIED to place that the server rejected (e.g. 5xx). Used
    // to (a) bound auto-retry of the call-join POST and (b) suppress the
    // phantom incoming-call re-ring the signaling echo would otherwise bounce
    // our own failed outgoing into. See joinCallOnServer / onIncomingCallDetected.
    QString m_lastOutgoingToken;
    QDateTime m_lastOutgoingTime;
    int m_callJoinAttempts = 0;
    static constexpr int kMaxCallJoinAttempts = 2;   // 2 quick retries, then inform
    QDateTime m_incomingTime;  // when incoming call was detected
    bool m_remoteVideoMuted = true;
    bool m_remoteAudioMuted = true;

    VideoFrameProvider *m_remoteVideoProvider = nullptr;
    VideoFrameProvider *m_localVideoProvider = nullptr;

    // Screen sharing
    ScreenSharePipeline *m_screenSharePipeline = nullptr;
    QPointer<ShareOverlay> m_shareOverlay;   // #72 coloured monitor border (monitor shares)
    bool m_screenSharing = false;
    QString m_screenShareSid;
    // Set during stopScreenShare()'s 50-iteration GLib flush. Used by
    // publisher-ICE-failed handler to suppress recovery-counter bumps
    // and hangUp() while the screen pipeline teardown perturbs the
    // shared signaling agent. A non-main-stream failure must never drop
    // the call (#138 / user-stated policy).
    bool m_screenShareTearingDown = false;
    // Remembered share target so a quality change can re-share the SAME
    // screen/window without re-prompting.
    int m_ssMonitorIndex = 0;
    quintptr m_ssWindowHandle = 0;
    int m_ssQuality = 1;   // 0=720p 1=1080p 2=1440p 3=Native (persisted)
    // #share-reliability — start/confirm/retry. A share is "confirmed" only
    // once ICE is connected AND outbound RTP is flowing (the SDP answer is not
    // proof); if that doesn't arrive within m_shareConfirmTimer the policy tears
    // down and retries a fresh pipeline (bounded), and a start requested while a
    // prior share is still releasing its capture device is queued until
    // ScreenSharePipeline::released(). Pure logic lives in ShareStartPolicy.
    ShareStartPolicy m_sharePolicy;
    QTimer m_shareConfirmTimer;
    bool m_shareConfirmArmed = false;
    bool m_shareRetryTeardown = false;   // a stop() in flight is a retry, not a user stop
    void buildAndStartSharePipeline(int monitorIndex, quintptr windowHandle);
    // Clamp a screen-share capture size DOWN to the GPU tier's ceiling (720p
    // iGPU / 540p software; no clamp on a discrete GPU), preserving aspect.
    void clampScreenToGpuTier(int &cw, int &ch) const;
    // Drop OUR camera to a single LOW layer whenever ANY screen share is active
    // in the call — ours OR a remote peer's. While the room is focused on a
    // shared screen the cameras are just small PIPs, so this frees encode load
    // on every sender AND decode load on every receiver (the sharer, already
    // maxed encoding the screen, then only decodes a 180p peer camera). Driven
    // by (m_screenSharing || !m_screenSubscribers.isEmpty()); recomputed
    // explicitly at every point the active-share set changes (local share
    // start/stop, remote add via the screen-offer handler, remote remove via
    // removeScreenSubscriber, publisher rebuild).
    void updateCameraSuppression();
    // Tear down the screen subscriber for `sessionId` (no-op if none): stop +
    // delete its pipeline, drop it from m_screenSubscribers, unbind the rendered
    // screen provider if it was this peer's, and re-evaluate camera suppression
    // so a vanished/zombie sharer can't pin our camera to a single LOW layer.
    void removeScreenSubscriber(const QString &sessionId);
    void onShareConfirmed();
    void onShareConfirmTimeout();
    void onSharePipelineReleased();
    VideoFrameProvider *m_remoteScreenProvider = nullptr;
    QHash<QString, SubscribePipeline*> m_screenSubscribers;

    // Offers received before ICE servers are available (P3 race guard)
    struct PendingOffer { QString fromSessionId; QString sdp; QString sid; };
    QList<PendingOffer> m_pendingOffers;
    void processPendingOffers();
    // Trickle-ICE early candidates: the MCU sends a subscriber's remote
    // candidates together with (or just before) its offer, which can land
    // ~100ms before onOfferReceived has built the SubscribeWebrtcSrc. Queue
    // them per remote session and flush when the subscriber is created --
    // otherwise the subscriber starts with ZERO remote candidates and ICE
    // never leaves "new" ("waiting for video"). The official client queues.
    struct PendingIceCandidate { QString candidate; int mline; QString mid; };
    QHash<QString, QVector<PendingIceCandidate>> m_pendingSubCandidates;

    // requestoffer retry (upstream resends ~every 8s until the offer lands)
    QSet<QString> m_pendingRequestOffers;
    QHash<QString, int> m_requestOfferAttempts;
    QTimer m_requestOfferRetry;
};
