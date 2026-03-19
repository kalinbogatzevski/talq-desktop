#pragma once

#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QJsonObject>
#include "core/ApiClient.h"

/**
 * Standalone Signaling (HPB) WebSocket client for typing indicators.
 *
 * Protocol: connect → wait for welcome → hello with ticket → join room → send/receive messages
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

signals:
    void connectedChanged();
    void typingUserChanged();

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
    int m_reconnectDelay = 2000;
};
