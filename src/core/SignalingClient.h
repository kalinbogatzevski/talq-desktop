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
    void sendOffer(const QString &toSessionId, const QString &sdp,
                   const QString &sid, const QString &nick = {});
    void sendAnswer(const QString &toSessionId, const QString &sdp,
                    const QString &sid, const QString &nick = {});
    void sendCandidate(const QString &toSessionId, const QJsonObject &candidate,
                       const QString &sid);
    void sendEndOfCandidates(const QString &toSessionId, const QString &sid);
    void requestOffer(const QString &sessionId, const QString &roomType = "video");
    bool hasMcu() const { return m_hasMcu; }

signals:
    void connectedChanged();
    void typingUserChanged();

    // WebRTC signaling signals
    void offerReceived(const QString &fromSessionId, const QString &sdp, const QString &sid);
    void answerReceived(const QString &fromSessionId, const QString &sdp);
    void candidateReceived(const QString &fromSessionId, const QJsonObject &candidate);
    void endOfCandidatesReceived(const QString &fromSessionId);
    void participantJoinedCall(const QString &sessionId, int flags, const QString &displayName);
    void participantLeftCall(const QString &sessionId);
    void roomPeerJoined(const QString &sessionId);
    void roomJoined();

private:
    void fetchSettings();
    void connectWebSocket();
    void onConnected();
    void onDisconnected();
    void onTextMessage(const QString &msg);
    void sendHello();
    void reconnect();

    ApiClient *m_api;
    QWebSocket m_ws;
    QTimer m_reconnectTimer;
    QTimer m_typingClearTimer;   // clear typing indicator after 15s timeout

    QString m_signalingUrl;
    QString m_userId;
    QString m_ticket;
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

    void sendSessionMessage(const QString &toSessionId, const QString &type,
                            const QJsonObject &payload, const QString &sid);
};
