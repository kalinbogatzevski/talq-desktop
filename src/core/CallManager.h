#pragma once

#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
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
#include "core/MediaDeviceManager.h"
#include "core/VideoFrameProvider.h"
#include "core/ScreenSharePipeline.h"
#include "core/ShareStartPolicy.h"
#include "core/EncodeTier.h"
#include "core/SubscriberStallPolicy.h"
#include "core/PublisherStallPolicy.h"
#include "core/SignalQualityPolicy.h"
#include "core/CallParticipant.h"
#include "core/MediaLoadController.h"   // 0.51.x dynamic encode/decode load controller
#include "core/FpsRampPolicy.h"         // 0.61.0 weak-tier solo slow-start fps ramp
#include "core/MemShedPolicy.h"         // host-protection memory-shed decision (onLoadTick)
#include "core/ShareCapPolicy.h"        // screen-share quality level -> encode cap

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
    // The conversation token of the current (or last) call. Valid whenever
    // state() != Idle; used by MainWindow to keep the signaling session
    // pinned to the call's room while a call is in progress.
    QString callToken() const { return m_callToken; }
    bool isMuted() const { return m_muted; }
    bool isCameraOn() const { return m_cameraOn; }
    // True when the camera device failed to open during this call (missing,
    // in use by another app, or blocked by OS privacy). Distinct from
    // "camera off" (a deliberate user toggle): the call surface uses this to
    // show a "Camera unavailable" notice instead of a silent black tile.
    bool isCameraUnavailable() const { return m_cameraUnavailable; }
    // 0.52.12 — the call UI calls this when a real self-camera frame arrives, so a
    // stale "camera unavailable" notice clears the moment the device recovers
    // (it was otherwise only cleared on a fresh call or a manual re-toggle).
    void cameraFrameConfirmed();
    // True when the microphone could not be opened during this call (missing,
    // in use by another app, OS-blocked, or a device id wasapi2 couldn't
    // resolve) and the publisher fell back to a SILENT source. The call still
    // runs (video + receive intact); the surface shows a "microphone
    // unavailable" banner instead of pretending the mic is live.
    bool isMicUnavailable() const { return m_micUnavailable; }
    // 0.52.5 — non-empty while our OWN camera SEND quality is degraded for a
    // reason the user should see (software encoding, or shed to the 480p floor
    // under load). Drives a persistent amber chip on the call surface. A Capable
    // box with a working HW encoder stays empty (it sends full quality). Cleared
    // when full quality resumes / on teardown.
    QString videoQualityNotice() const { return m_videoQualityNotice; }
    // 0.52.10 — derive duration from an elapsed timer started once on the first
    // Active, NOT from a counter that the reconnect/re-Active path zeroes (which
    // showed 00:00 mid-call after a "waiting for others" resync).
    int callDuration() const { return m_callElapsed.isValid()
                                      ? int(m_callElapsed.elapsed() / 1000) : 0; }
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
    // #76 -- a remote peer is sharing their screen TO us; remoteScreenResolutionLabel()
    // returns the EXACT received share resolution ("W×H") so the stage can show the
    // SHARE size during a share instead of the sharer's camera substream (pinned to
    // LOW while they share). Exact size, not a bucketed tier: a shared window / 16:10
    // display height doesn't map to a display standard.
    bool hasRemoteScreen() const;
    QString remoteScreenResolutionLabel() const;
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
    // Measured (not commanded) send fps for the self-tile badge — cached ONCE
    // per ~2s stats tick in updateCallStats(); NEVER call the mutating
    // PublishPipeline::sendFps() drain from paint code (repaints at ~30 fps
    // would corrupt the measurement).
    int sendFps() const { return m_sendFps; }
    // Telemetry: the routing this call actually selected — the TURN relay host(s)
    // in use (after nearest-selection) and the signaling/HPB server host.
    QString selectedTurnLabel() const;
    QString selectedSignalingLabel() const;
    // Measured RTT (ms) to the selected TURN relay / HPB from the nearest-server
    // probes, or -1 if unknown. For the telemetry ROUTING readout.
    int selectedTurnRttMs() const;
    int selectedSignalingRttMs() const;

    Q_INVOKABLE void startCall(const QString &token, bool withVideo);
    // #78 (add-to-call) -- pull the peer who is calling us (from their 1:1
    // conversation callerToken) into our ACTIVE call. Group active call: add
    // them + ring them in. 1:1 active call: promote to a new group with self +
    // current peer + caller (NC won't add a 3rd to a one-to-one room), then
    // join that group call and ring both. Async; reports via addToCallResult.
    Q_INVOKABLE void addPeerToActiveCall(const QString &callerToken, const QString &callerName);
    Q_INVOKABLE void setRemotePeerInfo(const QString &name, const QString &peerId);
    Q_INVOKABLE void acceptCall(bool withVideo);
    // Inject the conversation list so call logic can read the room type
    // (1 = one-to-one) via isOneToOneCall().
    void setConversations(ConversationListModel *c) { m_conversations = c; }
    Q_INVOKABLE void declineCall();
    // True when the current incoming/active call was offered with video, so
    // the incoming UI can offer an "answer with video" choice.
    bool callHasVideo() const { return m_withVideo; }
    Q_INVOKABLE void setUserActionReady();
    Q_INVOKABLE void hangUp();
    // #80 -- the peer we're ringing auto-replied "busy" (they're on another
    // call). Called by the app layer when a chat message with the busy marker
    // arrives in the conversation we're placing a call to. Plays the busy tone
    // + surfaces peerBusy() for a popup. No-op unless we're still ringing out.
    void onPeerBusy(const QString &peerName);
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
    // 0.53.1 — true when the active share is a single WINDOW (not a full monitor).
    // The Zoom-style self-view stage is shown ONLY for window shares: a MONITOR
    // share usually contains the TalQ window, so promoting the live share to the
    // stage makes the capture grab TalQ-showing-the-share → a frozen hall-of-mirrors
    // (and the near-static content stalls the receiver into a rebuild). Window shares
    // capture only that app window, so there's no feedback.
    bool screenShareIsWindow() const { return m_ssWindowHandle != 0; }
    // Index into QApplication::screens() of the monitor being shared (the share
    // picker assigns it from that same list). Only meaningful for a MONITOR
    // share (screenShareIsWindow()==false) — lets the stage tell whether the
    // call window sits on the very monitor we're sharing (hall-of-mirrors) or a
    // different display (safe to show the live self-share).
    int shareMonitorIndex() const { return m_ssMonitorIndex; }
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
    Q_INVOKABLE void requestPeerVideoQuality(const QString &sessionId, int substream, bool manual = false);

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
    // #77 -- called from onIncomingCallDetected when a second call arrives while
    // we're already in a call: notify us + auto-reply "busy" to the caller
    // (once per call token) instead of silently dropping it.
    void maybeReplyBusy(const QString &callerName, const QString &token);
    void playBusyTone();   // #80 -- one-shot :/sounds/busy_soft.wav
    // #78 add-to-call helpers (async):
    void addAndRingIntoCall(const QString &token, const QString &userId, const QString &name);
    void promoteOneToOneToGroup(const QString &callerUserId, const QString &callerName, bool withVideo);

    // Multi-party model. Stable order: self first, then join order. The
    // legacy 1:1 getters above keep working (P2P = self + one remote).
    QList<CallParticipant*> participants() const { return m_participantOrder; }
    CallParticipant *selfParticipant() const { return m_selfParticipant; }

    // #20 — long-lived BackgroundEngine. Owned here so it persists across
    // calls (saves the GL context creation cost on every call). The
    // current PublishPipeline reads it at construction. Public for the
    // PublishPipeline wire-up + the Settings live-apply path.
    BackgroundEngine *backgroundEngine() const { return m_backgroundEngine; }

    // Published by CallStage on every relayout: the peers rendered on the STAGE (the
    // big tile(s)). The receive-load cap exempts exactly these and nothing else.
    void setStagePeers(const QSet<QString> &peers);

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
    // #77 -- a second call arrived while we're already in a call. busyIncomingCall
    // drives a "called while you were busy" desktop notification for us;
    // busyAutoReply asks the app layer to post a one-line "on another call" chat
    // reply to the caller (their busy signal — Talk has no native one).
    void busyIncomingCall(const QString &callerName, const QString &token);
    void busyAutoReply(const QString &token, const QString &text);
    // #80 -- the peer we were ringing is on another call (their TalQ told us via
    // the busy marker). App layer shows a "<peer> is on another call" popup.
    void peerBusy(const QString &peerName);
    // #78 -- add-to-call outcome for the UI (ok + a human-readable message).
    void addToCallResult(bool ok, const QString &message);
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
    void softwareVideoEncoderNotice();     // camera fell back to software (x264); notify once/call
    void videoQualityNoticeChanged();      // 0.52.5 — persistent degraded-send chip text changed
    void screenShareQualityChanged();
    void remoteScreenProviderChanged();
    void participantAdded(CallParticipant *p);
    void participantRemoved(const QString &sessionId);
    void participantsChanged();

private slots:
    void onParticipantJoinedCall(const QString &sessionId, int flags, const QString &displayName);
    void onParticipantLeftCall(const QString &sessionId);
    void onPeerHungUp(const QString &sessionId);
    void onOfferReceived(const QString &fromSessionId, const QString &sdp, const QString &sid);
    void onAnswerReceived(const QString &fromSessionId, const QString &sdp);
    void onAudioLevelUpdated(double level);

private:
    void setState(CallState newState);

    // 0.51.x load controller internals.
    int     effectiveSubstreamFor(const QString &sessionId, int want) const; // apply recv cap + focus exemption
    // Peers currently ON THE STAGE (the big tile(s)). Published by CallStage on every
    // relayout; the receive-load cap exempts exactly these. Replaces the old single
    // focusedPeer(), which could only ever exempt ONE peer — wrong once the
    // active-speaker layout can put two cameras side by side.
    bool isFocusedPeer(const QString &sessionId) const;
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
    // #bug4 — 1:1 multi-session peer helpers.
    bool isOneToOneCall() const;                          // room type == 1
    bool isPeerUserSession(const QString &sessionId) const;
    QString pickPeerSiblingSid() const;
    // A subscriber feed died mid-call (SFU end-session / its webrtcbin ICE
    // went "failed" — both happen on a normal publisher renegotiation).
    // Tear down just that subscriber and re-subscribe; never kill the call.
    void recoverSubscriber(const QString &sessionId, const QString &reason);
    // A3a — tear down a peer's subscriber WITHOUT re-requesting (the peer's sid
    // is definitively dead, e.g. room/leave): drop the zombie + purge all its
    // requestoffer bookkeeping so the retry tick stops chasing a dead sid.
    void dropSubscriber(const QString &sessionId);
    // D3 — apply the coalesced camera desired-state (m_cameraOn) to the live
    // pipeline; called by m_cameraApplyTimer after toggle mashing settles.
    void applyCameraState();

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
    // Peers CallStage is currently rendering on the stage (the big tile(s)). The only
    // peers the receive-load cap exempts. Empty in the even gallery — which is correct:
    // no tile is privileged there, so the cap must bite everyone.
    QSet<QString> m_stagePeers;
    // A manual receive-quality pick (UI dropdown) per peer: 0/1/2 = pinned,
    // absent = Auto. A pin bypasses the auto load controller in
    // effectiveSubstreamFor (fixes "selecting quality does nothing").
    QHash<QString, int> m_peerManualSubstreamOverride;
    int  m_recvLoadSubstreamCap = 2;
    bool m_recvLoadCapFocused   = false;
    // Host-protection memory watchdog (onLoadTick): hard-shed every peer to the
    // 180p layer when memory genuinely overloads the host, restore when it
    // eases. Decision + thresholds + the 2026-07-13 false-trip field rationale
    // live in core/MemShedPolicy.h; the side effects (applyReceiveLoadCaps +
    // the quality notice) stay in onLoadTick.
    talq::MemShedPolicy m_memShed;
    // The controller itself (pure logic) + its ~1 s tick. Owned here because
    // load is a property of THIS machine, not of any one peer. The synthetic
    // fields feed it on the dev box (NVENC won't overload) until the encode/
    // decode latency probes land; TALQ_DISABLE_LOAD_CONTROLLER is the kill-switch.
    talq::MediaLoadController m_loadController;
    talq::FpsRampPolicy m_fpsRamp;   // 0.61.0 weak-tier solo slow-start fps ramp
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
    // Per-tile signal-quality glyph. updateCallStats() ticks each subscriber's
    // SignalQualityPolicy every 2 s alongside m_subStall, from
    // SubscribeWebrtcSrc::pollInboundRtp()'s windowed loss/jitter, and
    // publishes the committed level onto the matching CallParticipant. Pruned
    // in lockstep with m_subStall (same sites — recover/re-offer/left/teardown).
    QHash<QString, SignalQualityPolicy> m_signalQuality;
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
    // Set by MainWindow at construction; used to look up the current call's
    // room type (1 = one-to-one) via isOneToOneCall().
    ConversationListModel *m_conversations = nullptr;
    QTimer m_glibTimer;  // shared GLib main context pump

    CallState m_state = Idle;
    QString m_callToken;
    QString m_stunServer;
    QList<TurnServer> m_turnServers;
    // Nearest-TURN preference (0.57.6): a client offered TURN servers in several
    // regions can get its publish media relayed cross-continent (field: Ivan in ZA
    // relayed via the BG TURN -> SA->BG->SA detour -> shredded outbound audio+video,
    // while his ping/speedtest/receive were all fine). probeNearestTurnAsync()
    // RTT-probes each offered TURN host on room-join; effectiveTurnServers() then
    // hands the call pipelines only the nearest host(s), so a relay (when ICE needs
    // one) is always local. Falls back to the full list until probed / if none answer.
    QList<TurnServer> m_nearestTurnServers;
    bool m_turnProbed = false;
    int  m_turnBestRttMs = -1;   // best measured TURN RTT (ms) from the probe; telemetry
    int  m_turnProbeGen = 0;   // bumped per probe so a stale timer can't clobber a newer one
    // updateCallStats() tick counter (2s/tick) gating the periodic TURN RTT
    // re-probe below -- see the comment at its use site for the interval choice.
    int  m_turnRttTick = 0;
    void probeNearestTurnAsync();
    QList<TurnServer> effectiveTurnServers() const;
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
    // Camera grace-period auto-retry (backlog: MF 0xc00d36e6 first-enable
    // failure). mfvideosrc can throw a transient error the FIRST time a
    // device is opened for this call (still settling after a previous
    // session freed it / racing the Windows camera-privacy handshake) even
    // though the device is otherwise fine. Counts retries attempted for the
    // CURRENT enable attempt so we can bound them (see kCameraGraceMaxRetries
    // in CallManager.cpp) instead of either masking a genuinely broken
    // camera (unbounded retry) or flashing "unavailable" on a one-off
    // transient (zero retry). Reset at the start of every call, on a manual
    // toggle-on retry, and on a device-hotplug auto-resume — each is a fresh
    // attempt that deserves its own budget. Not reset on a successful grace
    // retry itself: cameraFirstFrameSeen() already gates that correctly (once
    // a frame has flowed, a LATER failure is never eligible for grace-retry
    // regardless of this counter's value).
    int m_cameraGraceRetries = 0;
    bool m_withVideo = false;
    int m_callDuration = 0;                 // legacy counter (kept; getter now uses m_callElapsed)
    QElapsedTimer m_callElapsed;            // 0.52.10 — call duration source of truth; survives reconnects
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
    // Cached measured send fps for the self-tile badge — written exactly once
    // per stats tick in updateCallStats() from the mutating
    // PublishPipeline::sendFps() drain; sendFps() getter above only reads this.
    int m_sendFps = 0;
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
    // 2026-07-13 field incident — REST-evidence backstop (full RCA at the
    // noteRestPeerEvidence definition). An outgoing call's ONLY exit from
    // Outgoing was the HPB participants-update rising edge (inCall 0→N →
    // participantJoinedCall → adopt). On 3 consecutive field attempts the HPB
    // delivered updates containing ONLY OUR OWN session — the ANSWERED
    // callee never appeared in any signaling event — and the REST poll was
    // blind too (its NC→HPB translation maps are themselves HPB-fed), so the
    // caller rang out 60s → "No answer" against a callee stuck on
    // "connecting". The poll now hands unmapped in-call rows to
    // noteRestPeerEvidence(), which promotes Outgoing→Connecting on REST
    // evidence ALONE and defers the HPB sid until it finally resolves.
    // m_remoteSessionId stays HPB-only — the NC session id is a DIFFERENT id
    // space and must never be stored in it (everything downstream routes on
    // HPB sids); the pending NC sid lives here instead.
    void noteRestPeerEvidence(const QString &ncSid, const QString &displayName);
    QString m_pendingRestPeerNcSid;       // NC session id of the REST-proven peer (no HPB sid yet)
    qint64  m_restPeerEvidenceMs = 0;     // first REST proof of an in-call peer (ms epoch; 0 = none)
    qint64  m_restPeerLastSeenMs = 0;     // last poll tick that still saw a non-self in-call row
    bool    m_restPromoted = false;       // backstop promoted us out of Outgoing on REST alone
    bool    m_restBackstopWarned = false; // loud delivery-failure warning fired (once per call)
    // #bug3 -- peer-grace: a transient remote-1:1-peer inCall=0 (WiFi blip) or a
    // full session drop+rejoin under a NEW NC session id must NOT end the call.
    // Separate from m_pubRetryTimer -- both drive Reconnecting and can be live at
    // once (a link flap downs our publisher AND the peer). MCU path only.
    QTimer  m_peerGraceTimer;            // single-shot, armed on remote-peer-left
    bool    m_peerGraceActive = false;   // true while waiting for the peer to return
    QString m_remotePeerUserId;          // NC userId of the 1:1 peer (correlation key)
    QString m_graceLeftSid;              // the (now dead) session id the peer left under
    // #bug4 — multi-session 1:1 peer: the SAME user can hold several live
    // signaling sessions at once (desktop + phone + browser + a VPN half of a
    // reconnect). The 1:1 call is keyed on the peer USER; m_remoteSessionId is
    // merely the sibling we currently subscribe. Every in-call session id of
    // that user lives here so ONE device dropping does not end the call.
    QSet<QString> m_peerInCallSids;      // in-call sessions of m_remotePeerUserId
    // #bug4 — outgoing ring-out late-answer window: a multi-device callee often
    // answers seconds AFTER our 60s ring gave up (devices ring staggered). A
    // peer JOIN on this token inside the window re-joins our call as the answer
    // instead of ringing us as a fresh incoming call.
    QString   m_lastRingoutToken;
    QDateTime m_lastRingoutTime;
    bool      m_lastRingoutWithVideo = false;
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
    // The token of a call we just HUNG UP from. Suppresses the phantom incoming
    // re-ring caused by the peer's inCall flag flapping back to 7 during the
    // room-participant churn of teardown. See hangUp / onIncomingCallDetected.
    QString m_lastHangupToken;
    QDateTime m_lastHangupTime;
    // #79 -- the peer session(s) we were in a call with at hangup. A teardown
    // flap of any of these re-rings us AFTER we rejoin the viewed room, so it
    // arrives under a DIFFERENT room token than m_lastHangupToken and the
    // token-keyed cooldown above can't catch it. Guarded by session id instead.
    QSet<QString> m_lastHangupSids;
    // #77 -- last conversation we auto-replied "busy" to + when, so a caller who
    // keeps their call ringing doesn't get spammed with repeated replies.
    QString   m_lastBusyReplyToken;
    QDateTime m_lastBusyReplyTime;
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
    bool m_softwareEncoderNotified = false;  // once-per-call guard for softwareVideoEncoderNotice
    bool m_hwDecodeFallbackDone = false;     // B4 — one-shot guard for the d3d11 decode-fault fallback
    bool m_resubscribeOnActive = false;      // A2 fix — replay subscriber re-request on the next Active
    QHash<QString,int> m_neverDecodedRecoveries;  // D2 fix — bounded never-decoded rebuilds per sid
    QString m_videoQualityNotice;            // 0.52.5 — "" = full quality; else the sender chip text
    void setVideoQualityNotice(const QString &text);   // emits videoQualityNoticeChanged() on change
    QString m_screenShareSid;
    // Reset (restart()) whenever WE send an unshareScreen. A peer discovers an
    // ongoing screen share ONLY from the publisher's one-shot sendoffer (there
    // is no screen-share flag to poll) — so when a re-share starts within the
    // server's reap window of a prior unshare, that single sendoffer races the
    // still-closing publisher slot and is dropped, leaving the peer stuck on
    // "starting share…". Used to gate a few targeted sendoffer re-asserts after
    // a quick stop→re-share so the peer re-discovers the share once the reap
    // completes — without the user having to wait. See dispatchScreenSendoffer().
    QElapsedTimer m_lastUnshareTimer;
    // (Re)announce an active screen share to every current peer via sendoffer.
    void dispatchScreenSendoffer();
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
    int m_ssQuality = 2;   // 0=720p 1=1080p 2=1440p 3=Native (persisted; default 1440p —
                           // kept in lockstep with the QSettings fallbacks in
                           // buildAndStartSharePipeline and SharePickerDialog)
    // #reshare-hw-collision — build the NEXT share pipeline with the software
    // x264 encoder. Set by the confirm-timeout Retry (the attempt produced no
    // outbound RTP; on HW boxes the dominant cause is a collision with the
    // prior share's still-freeing qsv/mf encoder session); cleared on each
    // fresh user start. buildAndStartSharePipeline() additionally forces
    // software on its own when starting inside the HW-encoder release window
    // of a recent unshare, so the FIRST attempt succeeds — no retry, no
    // per-retry sid churn for the server to reject.
    bool m_ssForceSwEncoder = false;
    // #share-reliability — start/confirm/retry. A share is "confirmed" only
    // once ICE is connected AND outbound RTP is flowing (the SDP answer is not
    // proof); if that doesn't arrive within m_shareConfirmTimer the policy tears
    // down and retries a fresh pipeline (bounded), and a start requested while a
    // prior share is still releasing its capture device is queued until
    // ScreenSharePipeline::released(). Pure logic lives in ShareStartPolicy.
    ShareStartPolicy m_sharePolicy;
    QTimer m_shareConfirmTimer;
    bool m_shareConfirmArmed = false;
    QTimer m_cameraApplyTimer;   // D3 — coalesce fast camera-toggle mashing
    // #81 -- intent watchdog. Armed when the user turns the camera ON; if no
    // self-frame arrives before it fires, the enable silently no-op'd (e.g.
    // enableCamera() early-returned on a stale m_cameraEnabled with the source
    // already dead) so nothing is watching. Forces one real disable->enable to
    // clear the stale state + re-arm PublishPipeline's own frame watchdog, which
    // then drives the existing cameraError -> "Camera unavailable" notice.
    QTimer m_cameraIntentTimer;
    bool m_shareRetryTeardown = false;   // a stop() in flight is a retry, not a user stop
    void buildAndStartSharePipeline(int monitorIndex, quintptr windowHandle);
    // Resolve the screen-share encode cap for a quality level via
    // talq::screenShareCap (core/ShareCapPolicy.h) and log whichever clamps
    // bit (GPU tier / software encoder). Shared by the share build AND the
    // LIVE re-cap so the two can never diverge again.
    talq::ShareCap screenShareCapForLevel(int level, bool forceSwEncoder) const;
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
    // Per-screen-subscriber FRAME-liveness (sampled every ~2s in updateCallStats),
    // feeding the redundant-re-assert guard in the screen offer handler.
    // m_screenSubFrameMark = last decoded frameCount seen; m_screenSubStallTicks =
    // consecutive ~2s stats ticks with NO new frame (0 = decoding right now, high =
    // black/dead). A re-assert is suppressed (no flap) ONLY while the sub is
    // genuinely decoding; a dead one (lost unshareScreen / MCU publisher-slot
    // replaced — its ICE stays "connected" so no failed edge ever fires) goes stale
    // here and is then rebuilt by the next re-offer. ICE state is NOT a reliable
    // liveness signal in MCU mode.
    QHash<QString,int> m_screenSubFrameMark;
    QHash<QString,int> m_screenSubStallTicks;
    // 0.52.15 — anti-thrash startup grace. Wall-clock (ms since epoch) when each
    // screen sub was (re)built. A sub that has NEVER decoded (m_screenSubFrameMark
    // == 0) and is younger than kScreenSubStartupGraceMs is still negotiating /
    // pulling its first keyframe (~2-5s on a re-share). The publisher's reap-race
    // re-assert (+5/+11s) must NOT tear it down or it can never reach "live" → the
    // infinite ~6s rebuild loop that stranded a re-shared screen on "Starting"
    // (Kalin↔Ilko 0.52.14). Kept in lockstep with the two maps above (cleared
    // everywhere they are; re-stamped only after a successful start()).
    QHash<QString,qint64> m_screenSubBuiltMs;
    static constexpr qint64 kScreenSubStartupGraceMs = 8000;
    // 0.52.17 — last ICE state each screen sub reported. The wall-clock grace above
    // is SHORTER than the publisher's +11s reap-race re-assert, so a sub that reaches
    // ICE "checking" early (FIX 2's candidate queue makes that sooner) and STAYS there
    // — exactly Ilko's 0.52.16 failure — would age out of the grace and be torn down
    // mid-pairing by the +11s rebuild. A sub actively PROGRESSING ICE (checking/
    // connected/completed, frameMark==0) must be protected from a redundant-re-offer
    // rebuild regardless of wall-clock; this map drives that check. Lockstep with the
    // maps above (cleared everywhere they are).
    QHash<QString,QString> m_screenSubIceState;
    // Upper bound on the icePairing rebuild-protection (must exceed the publisher's
    // +11s reap-race horizon, measured from the last ICE-progress re-stamp). A sub
    // genuinely WEDGED in "checking" (e.g. a lost unshareScreen with no
    // removeScreenSubscriber) falls through to rebuild after this instead of being held
    // until ICE reports "failed". 20s > 11s + margin.
    static constexpr qint64 kScreenSubIceProgressGraceMs = 20000;
    // 0.57.22 — post-ICE keyframe budget. Wall-clock (ms since epoch) when each
    // screen sub FIRST reached ICE "completed" (stamped once in the iceStateChanged
    // handler; lockstep-cleared with the maps above). "Completed" is the END of
    // pairing, not progress toward it: from that moment the ONLY thing outstanding
    // is the first keyframe (~1-2s normally — MCU auto-PLI + our build PLIs). A sub
    // still frameMark==0 past this budget is keyframe-STARVED (reaped MCU slot /
    // lost PLI), and the publisher's +5/+11s reap-race re-assert must be allowed to
    // REBUILD it — holding it for the full ICE-progress grace ignored the very
    // re-offers that would have fixed it (field: completed at +2s, no frame ever,
    // +9s re-offer ignored, sharer gave up → viewer stranded). Within the budget we
    // still re-PLI + ignore, so a keyframe merely in flight is never torn down.
    QHash<QString,qint64> m_screenSubCompletedMs;
    static constexpr qint64 kScreenSubKeyframeBudgetMs = 3500;
    // Screen-subscriber ICE-failed auto-retry (backlog): a cross-region screen
    // share whose subscriber ICE reaches "failed" previously just sat there —
    // nothing proactively asked for a fresh offer, since the publisher's own
    // reap-race re-assert only fires during an actual re-share. The underlying
    // transport race is proven probabilistic (a fresh attempt often succeeds),
    // so request a new offer ourselves, bounded by this count per session —
    // reset once a frame actually decodes (see updateCallStats()) so a LATER
    // failure after a genuinely healthy period gets its own fresh budget.
    QHash<QString,int> m_screenSubFailRetries;
    static constexpr int kScreenSubFailMaxRetries = 4;

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
    // Screen-share equivalent of m_pendingSubCandidates. The SCREEN candidate
    // router (handleScreenOffer's sibling in candidateReceived) had NO queue,
    // unlike the camera path above: a screen subscriber's remote candidates that
    // the MCU trickles with/just before its offer were DROPPED if they landed
    // before the screen-offer handler built the SubscribePipeline, so the fresh
    // sub started candidate-starved -> ICE never left "new"/"checking" -> the
    // re-share stuck on "Starting your share" (Kalin<->Ilko 0.52.16, frame-by-
    // frame in the receiver log). Queue per remote session; the screen-offer
    // handler flushes into the sub right before setRemoteOffer (same contract as
    // the camera path). Cleared in lockstep with m_screenSubscribers.
    QHash<QString, QVector<PendingIceCandidate>> m_pendingScreenSubCandidates;

    // requestoffer retry (upstream resends ~every 8s until the offer lands)
    QSet<QString> m_pendingRequestOffers;
    QHash<QString, int> m_requestOfferAttempts;
    // 0.52.7 — per-sid count of consecutive "not_allowed" requestoffer rejections.
    // A persistently-rejected sid is escalated from blind requestoffer resends to a
    // full recoverSubscriber rebuild (the bare resend can never escape a stale/
    // un-offerable publisher slot). Cleared whenever a real offer lands / a rebuild
    // starts / the call tears down — in lockstep with m_requestOfferAttempts.
    QHash<QString, int> m_requestOfferRejections;
    QTimer m_requestOfferRetry;
};
