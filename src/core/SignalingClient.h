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

    QString typingUser() const { return m_typingUser; }
    QString typingRoom() const { return m_typingRoom; }

    Q_INVOKABLE void joinRoom(const QString &token);
    Q_INVOKABLE void sendStartedTyping();
    Q_INVOKABLE void sendStoppedTyping();

    // WebRTC call signaling
    QString sessionId() const { return m_sessionId; }
    QString currentRoom() const { return m_currentRoom; }
    // Our own Nextcloud user id (for same-user multi-device checks, e.g.
    // suppressing the incoming-call ring on the caller's other device).
    QString userId() const { return m_userId; }
    // Nextcloud user id behind a given HPB session id, "" if not yet known.
    QString userIdForSession(const QString &sid) const { return m_sessionToUserId.value(sid); }
    void sendOffer(const QString &toSessionId, const QString &sdp,
                   const QString &sid, const QString &nick = {},
                   const QString &roomType = "video",
                   const QString &broadcaster = {});
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
    // talq.client broadcasts. Returns empty for non-TalQ peers or unknown.
    QString peerClientInfo(const QString &userId) const { return m_peerClientInfo.value(userId); }

signals:
    void connectedChanged();
    void typingUserChanged();
    void peerClientInfoChanged(const QString &userId, const QString &info);

    // WebRTC signaling signals
    void offerReceived(const QString &fromSessionId, const QString &sdp, const QString &sid, const QString &roomType);
    void answerReceived(const QString &fromSessionId, const QString &sdp, const QString &roomType);
    void candidateReceived(const QString &fromSessionId, const QJsonObject &candidate, const QString &roomType);
    void endOfCandidatesReceived(const QString &fromSessionId);
    void participantJoinedCall(const QString &sessionId, int flags, const QString &displayName);
    void participantLeftCall(const QString &sessionId);
    void participantFlagsChanged(const QString &sessionId, int oldFlags, int newFlags);
    void roomPeerJoined(const QString &sessionId);
    void roomJoined();
    void remoteMuteChanged(const QString &sessionId, const QString &media, bool muted);
    void screenShareStopped(const QString &sessionId);

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

    QString m_signalingUrl;
    QString m_userId;
    QString m_ticket;
    QString m_helloV2Token;   // signed JWT for hello v2.0 (preferred when present)
    QString m_sessionId;
    QString m_currentRoom;
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
    QHash<QString, QString> m_sessionToUserId;  // sessionId → userId (for DC-only fallback)

    void sendTalqClientHello();

    // Persist learned peer→client identity so the TalQ badge survives restarts
    // and no longer depends on a perfectly-timed live signaling-room overlap.
    void loadPersistedPeerClients();
    void persistPeerClient(const QString &userId, const QString &info);
};
