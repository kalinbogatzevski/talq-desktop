#pragma once

#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include "core/ApiClient.h"

/**
 * WebSocket client for Nextcloud Notify Push.
 *
 * Connects to wss://server/push/ws, authenticates with a pre-auth token,
 * and receives real-time push events (new messages, notifications).
 * On receiving a "notify_notification" event, emits pushReceived() so the
 * app can refresh the conversation list.
 */
class PushClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)

public:
    explicit PushClient(ApiClient *api, QObject *parent = nullptr);

    void start();
    void stop();
    bool isConnected() const { return m_connected; }

signals:
    void connectedChanged();
    void pushReceived(const QString &type);  // "notify_notification", "notify_file", etc.

private:
    void authenticate();
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString &message);
    void reconnect();

    ApiClient *m_api;
    QWebSocket m_ws;
    QTimer m_reconnectTimer;
    QString m_pushEndpoint;
    bool m_connected = false;
    bool m_authenticated = false;
    int m_reconnectDelay = 2000;
    static constexpr int MAX_RECONNECT_DELAY = 60000;
};
