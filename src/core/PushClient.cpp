#include "core/PushClient.h"
#include <QNetworkReply>
#include <QDebug>

PushClient::PushClient(ApiClient *api, QObject *parent)
    : QObject(parent)
    , m_api(api)
{
    connect(&m_ws, &QWebSocket::connected, this, &PushClient::onConnected);
    connect(&m_ws, &QWebSocket::disconnected, this, &PushClient::onDisconnected);
    connect(&m_ws, &QWebSocket::textMessageReceived, this, &PushClient::onTextMessageReceived);

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &PushClient::start);

    // Keepalive: ping every 30 s while the socket is up so an idle
    // proxy/NAT/sleeping link can't silently cull the connection. Writing the
    // ping over a dead socket forces the OS to report the broken pipe, which
    // fires disconnected() and triggers reconnect(). Started once we're
    // authenticated, stopped on disconnect/stop. (See m_keepAliveTimer.)
    m_keepAliveTimer.setInterval(30 * 1000);
    connect(&m_keepAliveTimer, &QTimer::timeout, this, [this]() {
        if (m_ws.state() == QAbstractSocket::ConnectedState)
            m_ws.ping();
    });
    connect(&m_ws, &QWebSocket::pong, this,
            [](quint64 elapsed, const QByteArray &) {
        qDebug() << "Push: keepalive pong, RTT" << elapsed << "ms";
    });
}

void PushClient::start()
{
    if (m_api->serverUrl().isEmpty()) return;
    m_stopped = false;

    QString wsUrl = m_api->serverUrl();
    wsUrl.replace("https://", "wss://").replace("http://", "ws://");
    wsUrl += "/push/ws";
    m_pushEndpoint = wsUrl;

    qDebug() << "Push: connecting to" << m_pushEndpoint;
    m_ws.open(QUrl(m_pushEndpoint));
}

void PushClient::stop()
{
    m_stopped = true;
    m_reconnectTimer.stop();
    m_keepAliveTimer.stop();
    m_ws.close();
    m_authenticated = false;
    if (m_connected) {
        m_connected = false;
        emit connectedChanged();
    }
}

void PushClient::authenticate()
{
    // Get pre_auth token and send it immediately over the open WebSocket
    auto *reply = m_api->postAbsoluteUrl("/apps/notify_push/pre_auth");

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_ws.state() != QAbstractSocket::ConnectedState) return;

        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            qWarning() << "Push: pre_auth failed:" << status << reply->errorString();
            m_ws.close();
            return;
        }

        QString token = QString::fromUtf8(reply->readAll()).trimmed();
        if (token.isEmpty()) {
            qWarning() << "Push: pre_auth returned empty token";
            m_ws.close();
            return;
        }

        // Protocol: send username first, then token (two separate messages)
        qDebug() << "Push: sending username + token";
        m_ws.sendTextMessage(m_api->user());
        m_ws.sendTextMessage(token);
    });
}

void PushClient::onConnected()
{
    qDebug() << "Push: WebSocket connected, requesting pre_auth token";
    m_reconnectDelay = 2000;
    // Get and send token NOW while the socket is fresh
    authenticate();
}

void PushClient::onDisconnected()
{
    qDebug() << "Push: WebSocket disconnected";
    m_keepAliveTimer.stop();
    m_authenticated = false;
    if (m_connected) {
        m_connected = false;
        emit connectedChanged();
    }
    if (!m_stopped)
        reconnect();
}

void PushClient::onTextMessageReceived(const QString &message)
{
    QString msg = message.trimmed();

    if (!m_authenticated) {
        if (msg == "authenticated") {
            m_authenticated = true;
            m_connected = true;
            emit connectedChanged();
            m_keepAliveTimer.start();   // keep the push socket from going zombie
            qDebug() << "Push: authenticated! Listening for events.";
        } else {
            qWarning() << "Push: auth response:" << msg;
            // Don't close — might be a delayed response
        }
        return;
    }

    qDebug() << "Push: event:" << msg;
    emit pushReceived(msg);
}

void PushClient::reconnect()
{
    if (m_api->serverUrl().isEmpty()) return;
    qDebug() << "Push: reconnecting in" << m_reconnectDelay << "ms";
    m_reconnectTimer.start(m_reconnectDelay);
    m_reconnectDelay = qMin(m_reconnectDelay * 2, MAX_RECONNECT_DELAY);
}
