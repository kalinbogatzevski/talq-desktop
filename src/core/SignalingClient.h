#pragma once

#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include "core/ApiClient.h"

struct TurnServer {
    QStringList urls;
    QString username;
    QString credential;
};

/**
 * Standalone Signaling (HPB) WebSocket client for typing indicators and WebRTC call signaling.
 *
 * Protocol: connect → wait for welcome → hello with ticket → join room → send/receive messages
 * Call signaling: offer/answer SDP exchange, ICE candidate trickle, participant state events
 */
class SignalingClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    Q_PROPERTY(QString typingUser READ typingUser NOTIFY typingUserChanged)
    Q_PROPERTY(QString typingRoom READ typingRoom NOTIFY typingUserChanged)

public:
    explicit SignalingClient(ApiClient *api, QObject *parent = nullptr);

    void start();
    void stop();
    bool isConnected() const { return m_authenticated; }
    // Forget the cached per-session inCall flags so the NEXT participants
    // update re-emits participantJoinedCall for everyone currently in the
    // call. Called by CallManager when WE start/join a call: peers already
    // in the call produced no flag transition (we recorded them while idle),
    // so without this the call rings to "no answer" against an already-active
    // room (the "remote keeps the call open, I join" case).
    void forceCallParticipantResync() { m_participantCallFlags.clear(); }

    QString typingUser() const { return m_typingUser; }
    QString typingRoom() const { return m_typingRoom; }

    // force=true bypasses the same-room re-join guard — ONLY for the
    // reconnect path (a fresh hello genuinely must re-enter the room).
    // Everyone else must leave it false: a redundant same-room re-join mints
    // a fresh Nextcloud session and retires the one carrying the in-call
    // flag, collapsing a live call (see joinRoom in the .cpp).
    Q_INVOKABLE void joinRoom(const QString &token, bool force = false);
    Q_INVOKABLE void sendStartedTyping();
    Q_INVOKABLE void sendStoppedTyping();

    // WebRTC call signaling
    QString sessionId() const { return m_sessionId; }
    QString currentRoom() const { return m_currentRoom; }
    // True only when the HPB has confirmed membership of currentRoom() (the WS
    // "room" ack arrived). Prefer this over `currentRoom() == token` when
    // deciding whether it is safe to proceed as if joined — currentRoom() is
    // set optimistically before the join handshake completes.
    bool roomJoinAcked() const { return m_roomJoinAcked; }
    // The signaling/HPB server URL this client is connected to (for telemetry).
    QString signalingUrl() const { return m_signalingUrl; }
    // Measured RTT (ms) to the selected HPB from the nearest-server probe, or -1
    // if unknown (single candidate / manual pin / probe found nothing). Telemetry.
    int signalingRttMs() const { return m_signalingRttMs; }
    // Per-instance signaling-server override (highest precedence). Used by
    // talq-call-test to pin each bot to a specific HPB for a cross-server call.
    void setServerOverride(const QString &url) { m_serverOverride = url; }
    // Our own Nextcloud user id (for same-user multi-device checks, e.g.
    // suppressing the incoming-call ring on the caller's other device).
    QString userId() const { return m_userId; }
    // Nextcloud user id behind a given HPB session id, "" if not yet known.
    QString userIdForSession(const QString &sid) const { return m_sessionToUserId.value(sid); }
    // 0.41.9-beta — mcuCodecHints: when true (default) the offer carries
    // audiocodec/videocodec fields that the HPB uses to provision a Janus
    // MCU publisher (the production path). For TRUE peer-to-peer relay we
    // must OMIT them, otherwise the HPB intercepts the offer as an MCU
    // publish (Janus answers "from self" + trickled candidates hit no
    // Janus handle → client_not_found). P2P callers pass false.
    void sendOffer(const QString &toSessionId, const QString &sdp,
                   const QString &sid, const QString &nick = {},
                   const QString &roomType = "video",
                   const QString &broadcaster = {},
                   bool mcuCodecHints = true);
    void sendAnswer(const QString &toSessionId, const QString &sdp,
                    const QString &sid, const QString &nick = {},
                    const QString &roomType = "video");
    void sendCandidate(const QString &toSessionId, const QJsonObject &candidate,
                       const QString &sid, const QString &roomType = "video");
    void sendEndOfCandidates(const QString &toSessionId, const QString &sid,
                             const QString &roomType = "video");
    // #132 simulcast: ask the SFU which substream (0=180p/1=360p/2=720p)
    // + temporal layer to forward for our subscription to `toSessionId`.
    // Janus defaults a simulcast subscriber to substream 0; this is the
    // only thing that steps it up. Wire format verified against spreed +
    // the HPB Go relay (selectStream → Janus videoroom configure).
    void sendSelectStream(const QString &toSessionId, const QString &sid,
                          int substream, int temporal = 2,
                          const QString &roomType = "video");
    void requestOffer(const QString &sessionId, const QString &roomType = "video");
    void sendSessionMessage(const QString &toSessionId, const QString &type,
                            const QJsonObject &payload, const QString &sid,
                            const QJsonObject &extraData = {},
                            const QString &roomType = "video");
    void sendBroadcastMessage(const QJsonObject &data);
    void sendMinimalMessage(const QString &toSessionId, const QJsonObject &data);
    bool hasMcu() const { return m_hasMcu; }

    // 0.41.x-beta — TalQ-private P2P signaling overlay (Zoom-style: direct
    // P2P for 1:1, MCU for 3+). The HPB hijacks the reserved offer/answer/
    // candidate types as MCU operations, but transparently RELAYS custom
    // session-targeted message types (same path as the talq.client
    // broadcast). So we ride SDP + ICE on a custom "talq.p2p.<subtype>"
    // type, which Janus never sees — the media goes direct. subtype is
    // "offer" | "answer" | "candidate" | "end". payload carries sdp /
    // candidate. Routed session-to-session; emits p2pSignalReceived.
    void sendP2pSignal(const QString &toSessionId, const QString &subtype,
                       const QJsonObject &payload);

    // TalQ peer client info: userId -> "TalQ/X.Y.Z". Populated from HPB
    // talq.client broadcasts and (bug 3) refreshed from the in-call data
    // channel. Returns empty for non-TalQ peers, unknown peers, OR a value not
    // freshly observed within the freshness window — so the UI shows no version
    // chip rather than a confidently-wrong long-stale number (the home-shows-
    // 0.28.3-while-office-shows-0.40.x divergence).
    QString peerClientInfo(const QString &userId) const;

    // Record/refresh a peer's TalQ client string with a fresh timestamp and
    // persist it. Called by the HPB broadcast handlers and by CallManager when
    // a live call observes the peer's version over the data channel (which
    // heals the cache even on servers without standalone signaling). Ignores
    // the self user and empty ids.
    void updatePeerClient(const QString &userId, const QString &info);

signals:
    void connectedChanged();
    void typingUserChanged();
    void peerClientInfoChanged(const QString &userId, const QString &info);
    // Emitted whenever the keepalive pong refreshes signalingRttMs() (~every
    // 25s while connected), so listeners (the home-screen SIGNALING tile) can
    // repaint without a periodic timer of their own.
    void signalingRttChanged(int rttMs);

    // A2 (0.53.x robustness) — our signaling session was RESET: the server
    // rejected a resume (or we otherwise got a brand-new session id) WHILE we
    // were already in a room/call. The old session's Janus MCU publisher is
    // dead and every peer is now re-subscribing to a session that has no
    // publisher behind it. CallManager must, for the new sid: rebuild+re-offer
    // our publisher, re-join the CALL on the server (joinRoom alone only
    // re-POSTs the room, not the call record), and rebuild all subscribers.
    // Distinct from connectedChanged (a plain reconnect that RESUMED cleanly
    // emits no sessionReset — the call survives untouched there).
    void sessionReset(const QString &newSessionId);

    // WebRTC signaling signals
    void offerReceived(const QString &fromSessionId, const QString &sdp, const QString &sid, const QString &roomType);
    void answerReceived(const QString &fromSessionId, const QString &sdp, const QString &roomType);
    void candidateReceived(const QString &fromSessionId, const QJsonObject &candidate, const QString &roomType);
    void endOfCandidatesReceived(const QString &fromSessionId);
    // 0.52.7 — the MCU rejected a requestoffer ("not_allowed: Not allowed to
    // request offer."). Carries NO sid (the HPB error has none); CallManager
    // correlates it to the peers it currently has a requestoffer outstanding for.
    void requestOfferRejected();
    // 0.41.x-beta — TalQ-private P2P overlay (bypasses the MCU for 1:1).
    void p2pSignalReceived(const QString &fromSessionId, const QString &subtype,
                           const QJsonObject &payload);
    void participantJoinedCall(const QString &sessionId, int flags, const QString &displayName);
    void participantLeftCall(const QString &sessionId);
    void participantFlagsChanged(const QString &sessionId, int oldFlags, int newFlags);
    void roomPeerJoined(const QString &sessionId);
    // #bug2 -- a peer's signaling session LEFT the room (HPB room/leave).
    // Distinct from participantLeftCall, which needs an inCall>0 -> 0 transition
    // that a vanished session never emits. Lets CallManager drop a now-zombie
    // subscriber whose publisher reconnected under a new session/SSRCs.
    void roomPeerLeft(const QString &sessionId);
    void roomJoined();
    void remoteMuteChanged(const QString &sessionId, const QString &media, bool muted);
    void screenShareStopped(const QString &sessionId);
    // Peer signalled a DELIBERATE hangup (TalQ-private) — end a 1:1 call now
    // rather than waiting out the peer-grace hold. See CallManager::onPeerHungUp.
    void peerHungUp(const QString &sessionId);

    // HPB-broadcast chat refresh hint: emitted for any room-scoped chat
    // event from the standalone signaling server (new message, read marker
    // advance, edit, reaction, deletion). Listeners should treat it as
    // "the chat in this room changed — pull fresh state." This is the
    // mechanism the official web client uses; without it we'd only see
    // read-receipt advances when a new message also arrives.
    void chatRefreshNeeded(const QString &roomToken);

private:
    void fetchSettings();
    void connectWebSocket();
    // Probe the candidate HPB pool (Nextcloud server + branded 123NET pool) and
    // connect to the nearest reachable one; fail-safe to the Nextcloud default.
    void selectNearestHpbAndConnect();
    void onConnected();
    void onDisconnected();
    void onTextMessage(const QString &msg);
    void sendHello();
    void sendBye();
    void sendRoomMessage(const QString &msgType);
    void reconnect();

    ApiClient *m_api;
    QWebSocket m_ws;
    QTimer m_reconnectTimer;
    QTimer m_typingClearTimer;   // clear typing indicator after 15s timeout
    // Periodic talq.client re-announce. Defense-in-depth: the one-shot
    // hello on room-join is enough when both peers join at the same
    // time, but a peer who joined the room BEFORE us upgraded silently
    // can keep a stale-version cache for the lifetime of the session.
    // Ticking every 5 min refreshes their cache without spam.
    QTimer m_talqClientReannounce;
    // HPB signaling keepalive. The standalone signaling server (strukturag)
    // closes a session whose WebSocket goes quiet for 60 s (pongWait): it
    // PINGs every 54 s and drops the socket if no inbound frame arrives in
    // time, then removes the session after a 30 s grace -- which ends a 1:1
    // call for the OTHER party too ("waiting for others to join"). Qt's
    // QWebSocket auto-PONGs the server's pings, but an idle proxy/NAT can
    // still cull the otherwise-silent TCP flow around the ~3-min mark. So we
    // also send our OWN ping every 25 s: traffic then flows both ways (the
    // server auto-PONGs ours) with comfortable margin under the 60 s
    // deadline. Purely transport-level -- the protocol has no JSON ping.
    QTimer m_keepAliveTimer;

    QString m_signalingUrl;
    QString m_serverOverride;   // per-instance HPB pin (talq-call-test cross-server)
    int     m_signalingRttMs = -1;   // measured RTT to the selected HPB (nearest-server probe)
    // Server-provided HPB discovery: the OPTIONAL "servers" field a patched
    // Nextcloud (apps/spreed) may include in the signaling-settings response
    // alongside the single "server" it already picked. Unpatched/stock
    // Nextcloud omits it entirely, so this is just empty -- selectNearestHpbAndConnect()
    // falls back to exactly today's behaviour. Populated for BOTH generic and
    // branded builds (the source is the user's own Nextcloud, not 123NET infra),
    // unlike TalQHpb::kPool which is branded-only.
    QStringList m_discoveredHpbPool;
    QString m_userId;
    QString m_ticket;
    QString m_helloV2Token;   // signed JWT for hello v2.0 (preferred when present)
    QString m_sessionId;
    // HPB session resume (api-v1 "Resuming sessions"): the resume id from the
    // hello response. On a WS blip we reconnect and send a short hello with
    // this id to RESUME the same session -- staying in the room/call -- instead
    // of a fresh hello that starts a new session and drops us from the call
    // (the strukturag server holds the session ~30s for exactly this). Without
    // it, every transient disconnect on a long path killed the call.
    QString m_resumeId;
    bool    m_resuming = false;  // a resume hello is in flight (vs fresh auth)
    // A1 (0.53.x robustness) — fast resume: when we hold a resume id and a
    // cached signaling URL, reconnect goes STRAIGHT to the WebSocket (skipping
    // the REST settings fetch) on a short backoff, so the resume lands inside
    // the server's short ~30s session-resume grace instead of blowing it (the
    // 2 s backoff + a full settings round-trip is what turned a 4 s blip into a
    // session-killing full re-hello in the field). True while such an attempt
    // is outstanding (no auth yet); cleared on auth, or on failure we fall back
    // to a full settings refresh.
    bool    m_fastResumePending = false;
    // A2 — set once we have ever authenticated a session this app-run. Lets a
    // LATER fresh (non-resumed) hello be recognised as a session RESET (vs the
    // very first cold hello, which must not trigger publisher rebuilds).
    bool    m_sessionEstablished = false;
    QString m_currentRoom;
    // True only once the HPB has ACKed the current room (the WS "room" response
    // arrived), false while a join is in flight or after a disconnect.
    // m_currentRoom alone is set optimistically BEFORE the async
    // participants/active POST + WS ack, so it must NOT be used to decide "we
    // are safely in this room" — a transient REST failure or a mid-flight join
    // would otherwise read as joined (dead live-push, a call that proceeds
    // before HPB room membership exists). This is the authoritative flag.
    bool    m_roomJoinAcked = false;
    QString m_typingUser;
    QString m_typingRoom;  // room token where typing was detected
    bool m_authenticated = false;
    bool m_hasMcu = false;
    int m_reconnectDelay = 2000;

    // Track participant inCall flags for change detection
    QHash<QString, int> m_participantCallFlags;
    QHash<QString, QString> m_participantNames;  // userId → displayName

    // TalQ peer client info — userId → "TalQ/X.Y.Z". Populated from HPB
    // broadcasts. Survives room switches because TalQ identity travels with
    // the user, not the session.
    QHash<QString, QString> m_peerClientInfo;
    // bug 3 — per-userId last-observed timestamp (ms since epoch), parallel to
    // m_peerClientInfo. A value older than the freshness window is treated as
    // unknown so a long-stale version is never displayed as if current.
    QHash<QString, qint64>  m_peerClientSeen;
    QHash<QString, QString> m_sessionToUserId;  // sessionId → userId (for DC-only fallback)

    void sendTalqClientHello();

    // Persist learned peer→client identity so the TalQ badge survives restarts
    // and no longer depends on a perfectly-timed live signaling-room overlap.
    void loadPersistedPeerClients();
    void persistPeerClient(const QString &userId, const QString &info);
};
