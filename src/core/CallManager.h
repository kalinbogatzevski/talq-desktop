#pragma once

#include <QObject>
#include <QTimer>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QPair>
#include <QByteArray>
#include <functional>
#include "core/ApiClient.h"
#include "core/SignalingClient.h"
#include "core/PublishPipeline.h"
#include "core/SubscribePipeline.h"
#include "core/SubscribeWebrtcSrc.h"
#include "core/PeerPipeline.h"
#include "core/MediaDeviceManager.h"
#include "core/VideoFrameProvider.h"
#include "core/ScreenSharePipeline.h"
#include "core/CallParticipant.h"

class CallManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(CallState state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool isMuted READ isMuted NOTIFY muteChanged)
    Q_PROPERTY(bool isCameraOn READ isCameraOn NOTIFY cameraChanged)
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
    enum CallState { Idle, Outgoing, Incoming, Connecting, Active, Ending };
    Q_ENUM(CallState)

    explicit CallManager(ApiClient *api, SignalingClient *signaling, MediaDeviceManager *deviceMgr, QObject *parent = nullptr);

    CallState state() const { return m_state; }
    bool isMuted() const { return m_muted; }
    bool isCameraOn() const { return m_cameraOn; }
    int callDuration() const { return m_callDuration; }
    QString remotePeerName() const { return m_remotePeerName; }
    QString remotePeerId() const { return m_remotePeerId; }
    double audioLevel() const { return m_audioLevel; }
    QString callStats() const { return m_callStats; }
    VideoFrameProvider *remoteVideoProvider() const { return m_remoteVideoProvider; }
    VideoFrameProvider *localVideoProvider() const { return m_localVideoProvider; }
    QString statusDetail() const { return m_statusDetail; }
    bool callsAvailable() const { return m_callsAvailable; }
    QString callsUnavailableReason() const { return m_callsUnavailableReason; }
    bool remoteVideoMuted() const { return m_remoteVideoMuted; }
    bool remoteAudioMuted() const { return m_remoteAudioMuted; }
    QString remotePeerClient() const { return m_remotePeerClient; }
    QString gpuAccelStatus() const { return m_gpuAccelStatus; }
    QString activeVideoCodec() const;
    QString activeVideoDecoder() const;
    // Publish (or screen-share) encoder, e.g. "H264 · nvh264enc · hw".
    QString activeVideoEncoder() const;
    // Whether the active video encoder is hardware-accelerated (pill tint).
    bool activeVideoEncoderIsHw() const;
    // Decoded resolution of the incoming stream (active simulcast layer),
    // e.g. "1280×720"; empty until the first frame decodes.
    QString activeRxResolution() const;
    // Send resolution + live bitrate, e.g. "1280×720 · 2.5 Mbps".
    QString videoTxLabel() const;
    // Current outbound stream bandwidth (GCC-applied video + Opus audio),
    // e.g. "↑ 2.45 Mbps". Empty when nothing is being sent.
    QString streamBandwidthLabel() const;

    Q_INVOKABLE void startCall(const QString &token, bool withVideo);
    Q_INVOKABLE void setRemotePeerInfo(const QString &name, const QString &peerId);
    Q_INVOKABLE void acceptCall(bool withVideo);
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

signals:
    void stateChanged();
    void muteChanged();
    void cameraChanged();
    void durationChanged();
    void callInfoChanged();
    void incomingCall(const QString &callerName, const QString &token, bool withVideo);
    void callEnded(const QString &reason);
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
                           std::function<void()> onDone = {}, int attempt = 0);
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
    // A subscriber feed died mid-call (SFU end-session / its webrtcbin ICE
    // went "failed" — both happen on a normal publisher renegotiation).
    // Tear down just that subscriber and re-subscribe; never kill the call.
    void recoverSubscriber(const QString &sessionId, const QString &reason);

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
    QHash<QString, SubscribeWebrtcSrc*> m_subscribePipelines;
    QHash<QString, QString> m_subscriberSids;  // sessionId -> current MCU sid
    // #132 simulcast: per-peer desired receive substream (0/1/2). Default
    // 2 (highest) so a fresh subscription / 1:1 call gets 720p; CallStage
    // refines it down per tile size. Sent via selectStream on connect +
    // on change.
    QHash<QString, int> m_desiredSubstream;
    QHash<QString, int> m_subscriberRecoveries;  // sessionId -> mid-call re-subscribe count (bounded; reset on connect)
    QByteArray m_ringtoneData;  // backing buffer for the selected ring (SND_ASYNC reads from it)
    // P2P single pipeline
    PeerPipeline *m_peerPipeline = nullptr;
    bool m_useP2P = false;
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
    void checkGStreamerPlugins();
    void updateCallStats();
    void setStatusDetail(const QString &detail) {
        if (m_statusDetail != detail) { m_statusDetail = detail; emit statusDetailChanged(); }
    }

    QTimer m_durationTimer;
    QTimer m_ringTimeout;
    QTimer m_statsTimer;
    // Publisher ICE "failed" is often transient (Wi-Fi/NAT blip, HPB
    // renegotiation) and self-heals to "connected" within seconds. The
    // old code hung up the whole call on the first "failed" — an
    // 11-minute call died on a momentary blip. Grace-debounce it: only
    // tear down if ICE is still failed when this single-shot timer
    // fires; any "connected"/"completed" in the window cancels it.
    QTimer m_pubIceGrace;
    int    m_pubIceRecoveries = 0;

    bool m_joinedCall = false;
    bool m_userActionReady = false;

    QString m_lastDeclinedToken;
    QDateTime m_lastDeclinedTime;
    QDateTime m_incomingTime;  // when incoming call was detected
    bool m_remoteVideoMuted = true;
    bool m_remoteAudioMuted = true;

    VideoFrameProvider *m_remoteVideoProvider = nullptr;
    VideoFrameProvider *m_localVideoProvider = nullptr;

    // Screen sharing
    ScreenSharePipeline *m_screenSharePipeline = nullptr;
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
    VideoFrameProvider *m_remoteScreenProvider = nullptr;
    QHash<QString, SubscribePipeline*> m_screenSubscribers;

    // Offers received before ICE servers are available (P3 race guard)
    struct PendingOffer { QString fromSessionId; QString sdp; QString sid; };
    QList<PendingOffer> m_pendingOffers;
    void processPendingOffers();

    // requestoffer retry (upstream resends ~every 8s until the offer lands)
    QSet<QString> m_pendingRequestOffers;
    QHash<QString, int> m_requestOfferAttempts;
    QTimer m_requestOfferRetry;
};
