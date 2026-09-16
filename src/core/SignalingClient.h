#pragma once

#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <functional>
#include <vector>
#include "core/ApiClient.h"
#include "core/HpbRehomePolicy.h"
#include "core/SignalingWatchdogPolicy.h"

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
    void forceCallParticipantResync() { m_participantCallFlags.clear(); m_participantsUpdateSeen = false; }

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
    // set optimistically before the join handshake completes. A successful
    // session RESUME restores the ack the dropped session had for the same room
    // (the server kept the membership; a resume gets no "room" reply).
    bool roomJoinAcked() const { return m_roomJoinAcked; }
    // The signaling/HPB server URL this client is connected to (for telemetry).
    QString signalingUrl() const { return m_signalingUrl; }
    // Measured RTT (ms) to the selected HPB from the nearest-server probe, or -1
    // if unknown (single candidate / manual pin / probe found nothing). Telemetry.
    int signalingRttMs() const { return m_signalingRttMs; }
    // Per-instance signaling-server override (highest precedence). Used by
    // talq-call-test to pin each bot to a specific HPB for a cross-server call.
    void setServerOverride(const QString &url) { m_serverOverride = url; }
    // Idle HPB re-homing (HpbRehomePolicy.h) must never move signaling under a
    // call. CallManager reports every state change here (CallManager::setState):
    // true while ringing, connecting, live or reconnecting.
    void setCallBusy(bool busy);
    // Re-homing is on by default. A client that drives calls WITHOUT
    // CallManager (talq-call-test) cannot report call state, so it turns
    // re-homing off rather than risk a switch mid-call.
    void setIdleRehomeEnabled(bool enabled) { m_idleRehomeEnabled = enabled; }
    // Our own Nextcloud user id (for same-user multi-device checks, e.g.
    // suppressing the incoming-call ring on the caller's other device).
    QString userId() const { return m_userId; }
    // Nextcloud user id behind a given HPB session id, "" if not yet known.
    QString userIdForSession(const QString &sid) const { return m_sessionToUserId.value(sid); }
    // #bug5 — translate a NEXTCLOUD session id (the id space REST
    // call/{token} reports) into the HPB signaling sid that requestoffer /
    // MCU subscribes actually route on. Populated from the room "join"
    // events (each entry carries sessionid + userid + roomsessionid,
    // including the initial in-the-room list the server sends when WE join)
    // and from participants updates that carry nextcloudSessionId — so the
    // mapping exists even when no participants update ever fires for an
    // ESTABLISHED in-call peer (the caller-rings-out-against-an-active-call
    // field bug, 2026-07-08). "" if not (yet) known.
    QString hpbSessionForNcSession(const QString &ncSessionId) const
    { return m_ncSessionToHpbSid.value(ncSessionId); }
    // #bug5 — every known HPB session of a Nextcloud user (multi-device
    // peers hold several at once). Fallback mapping for the REST poll when
    // roomsessionid was absent. Order is arbitrary.
    QStringList sessionsForUser(const QString &userId) const
    {
        QStringList out;
        for (auto it = m_sessionToUserId.constBegin(); it != m_sessionToUserId.constEnd(); ++it)
            if (it.value() == userId) out.append(it.key());
        return out;
    }
    // #bug4 — last-known call flags of a session (CALL_FLAG_WITH_AUDIO|VIDEO...).
    // Entries persist while a session is inCall>0 and are pruned on its leave
    // edge, so this is a safe "does this sibling claim media" probe.
    int callFlagsForSession(const QString &sid) const { return m_participantCallFlags.value(sid, 0); }
    // Sessions the HPB lists in the current room's call (in-call flag set), our
    // own session excluded. Order is arbitrary.
    QStringList inCallSessions() const
    {
        QStringList out;
        for (auto it = m_participantCallFlags.constBegin(); it != m_participantCallFlags.constEnd(); ++it)
            if (it.value() & 1)   // Participant::FLAG_IN_CALL
                out.append(it.key());
        return out;
    }
    // Display name for a session, resolved the way participantJoinedCall
    // resolves it (name cache by user id, else the user id); "" if unknown.
    QString displayNameForSession(const QString &sid) const
    {
        const QString uid = m_sessionToUserId.value(sid);
        return uid.isEmpty() ? QString() : m_participantNames.value(uid, uid);
    }
    // 2026-09-16 review CR-3 — the HPB's view of who ELSE is in the current
    // room's call, for "has everybody left?" decisions that must not trust REST
    // alone (GET call/{token} drops sessions whose POP stopped pinging the
    // backend). Only meaningful once participantsUpdateSeen(): joining a room
    // delivers no in-call list, and each update lists only the sessions that
    // changed, so the map is what the HPB has told us since the join.
    int otherSessionsInCall() const
    {
        int n = 0;
        for (int flags : m_participantCallFlags)
            if (flags & 1)   // Participant::FLAG_IN_CALL
                ++n;
        return n;
    }
    bool participantsUpdateSeen() const { return m_participantsUpdateSeen; }
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
    // `sid` is the MCU handle the candidate belongs to ("" when the sender set
    // none, e.g. P2P). Trailing so 3-argument connects keep compiling.
    void candidateReceived(const QString &fromSessionId, const QJsonObject &candidate, const QString &roomType,
                           const QString &sid);
    void endOfCandidatesReceived(const QString &fromSessionId);
    // 0.52.7 — the MCU rejected a requestoffer ("not_allowed: Not allowed to
    // request offer."). Carries NO sid (the HPB error has none); CallManager
    // correlates it to the peers it currently has a requestoffer outstanding for.
    void requestOfferRejected();
    // Our OWN session's inCall flags as listed in an HPB participants update for
    // `roomToken` (every update, not only changes). CallManager treats inCall>0
    // after its join POST as proof the join committed, even while that POST's
    // response is still hanging (2026-09-16: ~60 s before the REST 200).
    void selfCallFlagsUpdated(const QString &roomToken, int inCall);
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
    // The server moved us to another conversation (breakout rooms). The window
    // follows it; nothing else can, because only the server decides this.
    void switchedToRoom(const QString &token);
    // A moderator force-muted us. Talk sends this as msgType "control"; TalQ
    // had no branch for it at all, so the moderator's UI believed the mute had
    // landed while this client carried on transmitting.
    void forceMuted();
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
    // The settings fetch's result: failure bookkeeping, or apply the server
    // URL, pool and hello auth, then connect.
    void onSettingsFetched(bool ok, const QJsonObject &data);
    void connectWebSocket();
    // Probe the candidate HPB pool (Nextcloud server + branded-build pool) and
    // connect to the nearest reachable one; fail-safe to the Nextcloud default.
    void selectNearestHpbAndConnect();
    // The candidate HPB urls, deduplicated by host, Nextcloud's server first.
    QStringList hpbCandidateUrls() const;
    // A server pinned by hand (per-instance or QSettings override).
    bool manualHpbPin() const;
    // Measure TCP:443 RTT to every candidate WITHOUT touching the live socket,
    // then report per-candidate samples (same order as `cands`) to `done`.
    using HpbProbeDone = std::function<void(const QStringList &urls,
                                            const std::vector<talq::HpbProbeSample> &samples)>;
    void probeHpbPool(const QStringList &cands, HpbProbeDone done);
    // Idle re-homing: run on every keepalive tick; probes when the policy says
    // so and switches gracefully when a clearly nearer HPB answers and its
    // signaling passes verifyHpbSignaling().
    void maybeStartRehomeProbe();
    talq::HpbRehomeGates rehomeGates() const;
    // Throw-away WebSocket handshake to `baseUrl`: ok only if the signaling
    // server sends `welcome` within talq::kHpbVerifyTimeoutMs.
    using HpbVerifyDone = std::function<void(bool ok, const QString &why)>;
    void verifyHpbSignaling(const QString &baseUrl, HpbVerifyDone done);
    // `settings`: signaling/settings fetched just before the switch; the
    // reconnect it triggers uses them instead of fetching again.
    void switchHpbTo(const QString &url, int rttMs, const QJsonObject &settings);
    // A connect attempt ended before hello (socket failure, connect bound, or
    // the settings fetch failing): decide what happens to a held resume id
    // (talq::resumeAfterFailedAttempt).
    void onAttemptFailedBeforeHello();
    // Apply a pong-watchdog verdict. Returns true if the socket was aborted.
    bool applyWatchdogVerdict(talq::PongWatchdogPolicy::Verdict verdict);
    talq::SigClock sigNow() const;
    void onConnected();
    void onDisconnected();
    void onTextMessage(const QString &msg);
    void sendHello();
    void sendBye();
    void sendRoomMessage(const QString &msgType);
public:
    // Whether this user shares their typing status (server-side setting,
    // capabilities config.chat.typing-privacy: 0 = public, 1 = private).
    // Defaults true = share, which is Talk's default and what TalQ did before
    // it could read the setting at all.
    void setShareTypingStatus(bool share) { m_shareTypingStatus = share; }
private:
    bool m_shareTypingStatus = true;
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
    // Pong watchdog (SignalingWatchdogPolicy.h). The keepalive used to only
    // LOG pongs, so a POP whose egress died went unnoticed until Windows gave
    // up on the TCP connection ~20 s after the first unanswered ping, and never
    // behind a CONNECT proxy. Armed on each ping, stopped by the pong or any
    // inbound frame.
    QTimer m_pongDeadline;
    talq::PongWatchdogPolicy m_pongWatchdog;
    // Bounds every m_ws.open() (SignalingWatchdogPolicy.h). Without it a fast
    // resume to a dead POP waited out the OS/Qt connect timeout: 42.1 s in the
    // field, longer than the server's 30 s resume grace.
    QTimer m_connectTimer;
    int    m_connectTimeouts = 0;   // consecutive, reset on a successful connect
    qint64 m_connectBoundMs = 0;    // the bound m_connectTimer was armed with
    QString m_connectHost;          // host of the connect in flight
    qint64 m_connectStartedMs = -1; // m_monoClock at that connect's open(), -1 = none
    // How long the last successful connect to each HPB host took (ms). Keeps a
    // host that needs longer than the first bound (a dual-stack host with a dead
    // IPv6 path) from being aborted on every reconnect.
    QHash<QString, qint64> m_lastConnectMsByHost;
    QElapsedTimer m_monoClock;      // monotonic half of sigNow()
    // Idle re-homing (HpbRehomePolicy.h): selection used to run only on a
    // reconnect, so a client stayed on a far POP for hours after its nearer POP
    // recovered.
    talq::HpbRehomePolicy m_rehome;
    bool    m_callBusy = false;           // last value CallManager reported
    bool    m_idleRehomeEnabled = true;
    // Set by switchHpbTo(): the verified HPB the background probe chose. The
    // reconnect it triggers connects straight to it instead of probing again.
    // Consumed by the next settings fetch, success or failure.
    QString m_rehomeUrl;
    // The server switchHpbTo() left. If the connect to the target fails before
    // hello, onDisconnected() goes straight back here (via m_rehomeUrl) without
    // probing. Cleared on hello, on use, and by a normal selection.
    QString m_rehomeReturnUrl;
    bool    m_rehomeConnectPending = false;   // a direct re-home connect has not reached hello yet
    // signaling/settings fetched right before switchHpbTo() gave up the working
    // session (a Nextcloud that cannot answer that GET must not cost us the
    // session). Consumed by the re-home's own settings step; empty otherwise.
    QJsonObject m_rehomeSettings;

    QString m_signalingUrl;
    QString m_serverOverride;   // per-instance HPB pin (talq-call-test cross-server)
    int     m_signalingRttMs = -1;   // measured RTT to the selected HPB (nearest-server probe)
    QString m_lastConnectedHpbHost;  // HPB host of the last SUCCESSFUL WS connect —
                                     // the "incumbent" the nearest-HPB probe stays
                                     // sticky to unless a challenger wins by margin.
    // Server-provided HPB discovery: the OPTIONAL "servers" field a patched
    // Nextcloud (apps/spreed) may include in the signaling-settings response
    // alongside the single "server" it already picked. Unpatched/stock
    // Nextcloud omits it entirely, so this is just empty -- selectNearestHpbAndConnect()
    // falls back to exactly today's behaviour. Populated for BOTH generic and
    // branded builds (the source is the user's own Nextcloud, not brand infra),
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
    // The fast resume failed at the socket (dead POP, connect bound). The next
    // attempt fetches settings and probes, but still offers m_resumeId: any
    // cluster member resumes or proxies it. One attempt only; if it fails too
    // the id is dropped (talq::resumeAfterFailedAttempt).
    bool    m_resumeViaSettings = false;
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
    // A successful RESUME restores it (see m_roomAckedBeforeDrop).
    bool    m_roomJoinAcked = false;
    // The room ack as it stood when an authenticated session dropped. A resume
    // keeps the session's room membership server-side and never gets a "room"
    // reply, so the resume restores the ack from this snapshot, if the room is
    // still the same. Without it roomJoinAcked() stayed false for as long as the
    // conversation stayed open: idle re-homing never ran, and the next same-room
    // joinRoom() re-joined (minting a new Nextcloud session). Taken only on the
    // drop of an AUTHENTICATED session: failed reconnect attempts must not
    // overwrite it. Voided by joinRoom(), stop() and a fresh hello.
    bool    m_roomAckedBeforeDrop = false;
    QString m_roomAckedTokenBeforeDrop;
    QString m_typingUser;
    QString m_typingRoom;  // room token where typing was detected
    bool m_authenticated = false;
    bool m_hasMcu = false;
    int m_reconnectDelay = 2000;

    // Track participant inCall flags for change detection
    QHash<QString, int> m_participantCallFlags;
    bool m_participantsUpdateSeen = false;   // a participants update for m_currentRoom since joinRoom/resync
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
    // #bug5 — Nextcloud (room) session id → HPB signaling sid, from room join
    // events + participants updates. Room-scoped: cleared on a room SWITCH
    // (same policy as m_sessionToUserId) and pruned on room leave events.
    QHash<QString, QString> m_ncSessionToHpbSid;

    void sendTalqClientHello();

    // Persist learned peer→client identity so the TalQ badge survives restarts
    // and no longer depends on a perfectly-timed live signaling-room overlap.
    void loadPersistedPeerClients();
    void persistPeerClient(const QString &userId, const QString &info);
};
