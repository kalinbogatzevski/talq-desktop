#include "core/SignalingClient.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkReply>
#include <QDebug>

SignalingClient::SignalingClient(ApiClient *api, QObject *parent)
    : QObject(parent)
    , m_api(api)
{
    connect(&m_ws, &QWebSocket::connected, this, &SignalingClient::onConnected);
    connect(&m_ws, &QWebSocket::disconnected, this, &SignalingClient::onDisconnected);
    connect(&m_ws, &QWebSocket::textMessageReceived, this, &SignalingClient::onTextMessage);

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &SignalingClient::start);

    // Clear typing indicator after 15s (safety net)
    m_typingClearTimer.setSingleShot(true);
    m_typingClearTimer.setInterval(15000);
    connect(&m_typingClearTimer, &QTimer::timeout, this, [this]() {
        if (!m_typingUser.isEmpty()) {
            m_typingUser.clear();
            emit typingUserChanged();
        }
    });
}

void SignalingClient::start()
{
    fetchSettings();
}

void SignalingClient::stop()
{
    m_reconnectTimer.stop();
    m_ws.close();
    m_sessionId.clear();
    m_reconnectDelay = 2000;
    if (m_authenticated) {
        m_authenticated = false;
        emit connectedChanged();
    }
}

void SignalingClient::fetchSettings()
{
    m_api->get("apps/spreed/api/v3/signaling/settings",
        [this](bool ok, const QJsonObject &data, int) {
            if (!ok) {
                qWarning() << "Signaling: failed to get settings";
                reconnect();
                return;
            }

            m_signalingUrl = data["server"].toString().trimmed();
            if (m_signalingUrl.isEmpty()) {
                qDebug() << "Signaling: no standalone server configured";
                return;
            }

            // Get v1.0 auth params
            QJsonObject authParams = data["helloAuthParams"].toObject();
            QJsonObject v1 = authParams["1.0"].toObject();
            m_userId = v1["userid"].toString();
            m_ticket = v1["ticket"].toString();

            if (m_ticket.isEmpty()) {
                qWarning() << "Signaling: no ticket in settings";
                return;
            }

            qDebug() << "Signaling: server at" << m_signalingUrl;
            connectWebSocket();
        });
}

void SignalingClient::connectWebSocket()
{
    QString wsUrl = m_signalingUrl;
    wsUrl.replace("https://", "wss://").replace("http://", "ws://");
    if (!wsUrl.endsWith("/"))
        wsUrl += "/";
    wsUrl += "spreed";

    qDebug() << "Signaling: connecting to" << wsUrl;
    m_ws.open(QUrl(wsUrl));
}

void SignalingClient::onConnected()
{
    qDebug() << "Signaling: WebSocket connected, waiting for welcome";
    m_reconnectDelay = 2000;
}

void SignalingClient::onDisconnected()
{
    qDebug() << "Signaling: disconnected";
    bool wasAuth = m_authenticated;
    m_authenticated = false;
    m_sessionId.clear();
    if (wasAuth) emit connectedChanged();
    reconnect();
}

void SignalingClient::onTextMessage(const QString &msg)
{
    QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
    QJsonObject obj = doc.object();
    QString type = obj["type"].toString();

    if (type == "welcome") {
        qDebug() << "Signaling: received welcome, sending hello";
        sendHello();
    }
    else if (type == "hello") {
        m_sessionId = obj["hello"].toObject()["sessionid"].toString();
        m_authenticated = true;
        emit connectedChanged();
        qDebug() << "Signaling: authenticated, session:" << m_sessionId.left(20) + "...";

        // Re-join room if we had one
        if (!m_currentRoom.isEmpty()) {
            joinRoom(m_currentRoom);
        }
    }
    else if (type == "error") {
        qWarning() << "Signaling: error:" << obj["error"].toObject()["message"].toString();
    }
    else if (type == "room") {
        qDebug() << "Signaling: joined room";
    }
    else if (type == "message") {
        QJsonObject messageObj = obj["message"].toObject();
        QJsonObject msgData = messageObj["data"].toObject();
        QString msgType = msgData["type"].toString();

        // Check if this message is for our current room
        QString senderRoom = messageObj["sender"].toObject()["roomid"].toString();
        if (!senderRoom.isEmpty() && senderRoom != m_currentRoom) {
            return;  // typing from a different room — ignore
        }

        if (msgType == "startedTyping") {
            QString sender = messageObj["sender"].toObject()["displayname"].toString();
            if (sender.isEmpty())
                sender = messageObj["sender"].toObject()["userid"].toString();

            if (sender != m_userId && !sender.isEmpty()) {
                m_typingUser = sender;
                m_typingRoom = m_currentRoom;
                emit typingUserChanged();
                m_typingClearTimer.start();
            }
        }
        else if (msgType == "stoppedTyping") {
            if (!m_typingUser.isEmpty()) {
                m_typingUser.clear();
                m_typingRoom.clear();
                emit typingUserChanged();
                m_typingClearTimer.stop();
            }
        }
    }
    else if (type == "event") {
        // Room events (participants joined/left, etc.)
    }
}

void SignalingClient::sendHello()
{
    QJsonObject hello;
    hello["type"] = QString("hello");

    QJsonObject helloData;
    helloData["version"] = QString("1.0");

    QJsonObject auth;
    auth["url"] = m_api->serverUrl() + "/ocs/v2.php/apps/spreed/api/v3/signaling/backend";

    QJsonObject params;
    params["userid"] = m_userId;
    params["ticket"] = m_ticket;
    auth["params"] = params;

    helloData["auth"] = auth;
    hello["hello"] = helloData;

    m_ws.sendTextMessage(QJsonDocument(hello).toJson(QJsonDocument::Compact));
}

void SignalingClient::joinRoom(const QString &token)
{
    m_currentRoom = token;

    // Clear typing from previous room
    if (!m_typingUser.isEmpty()) {
        m_typingUser.clear();
        emit typingUserChanged();
        m_typingClearTimer.stop();
    }

    // Join as active participant — the response contains the sessionId
    // which the signaling server needs to verify room access
    QJsonObject empty;
    m_api->post("apps/spreed/api/v4/room/" + token + "/participants/active", empty,
        [this, token](bool ok, const QJsonObject &data, int) {
            qDebug() << "Signaling: participants/active response ok=" << ok
                     << "room match=" << (m_currentRoom == token)
                     << "auth=" << m_authenticated
                     << "sessionId=" << data["sessionId"].toString().left(20);
            if (!ok || m_currentRoom != token || !m_authenticated) return;

            // Extract the Nextcloud session ID from the response
            QString ncSessionId = data["sessionId"].toString();

            QJsonObject msg;
            msg["type"] = QString("room");

            QJsonObject room;
            room["roomid"] = token;
            if (!ncSessionId.isEmpty())
                room["sessionid"] = ncSessionId;
            msg["room"] = room;

            m_ws.sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
            qDebug() << "Signaling: joining room" << token << "with session" << ncSessionId.left(20) + "...";
        });
}

void SignalingClient::sendStartedTyping()
{
    if (!m_authenticated || m_currentRoom.isEmpty()) return;

    QJsonObject msg;
    msg["type"] = QString("message");

    QJsonObject message;
    QJsonObject recipient;
    recipient["type"] = QString("room");
    message["recipient"] = recipient;

    QJsonObject data;
    data["type"] = QString("startedTyping");
    message["data"] = data;

    msg["message"] = message;

    m_ws.sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

void SignalingClient::sendStoppedTyping()
{
    if (!m_authenticated || m_currentRoom.isEmpty()) return;

    QJsonObject msg;
    msg["type"] = QString("message");

    QJsonObject message;
    QJsonObject recipient;
    recipient["type"] = QString("room");
    message["recipient"] = recipient;

    QJsonObject data;
    data["type"] = QString("stoppedTyping");
    message["data"] = data;

    msg["message"] = message;

    m_ws.sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

void SignalingClient::reconnect()
{
    if (m_api->serverUrl().isEmpty()) return;
    m_reconnectTimer.start(m_reconnectDelay);
    m_reconnectDelay = qMin(m_reconnectDelay * 2, 60000);
}
