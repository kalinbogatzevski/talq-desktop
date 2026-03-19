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
}

void PushClient::start()
{
    if (m_api->serverUrl().isEmpty()) return;

    m_pushEndpoint = m_api->serverUrl();
    m_pushEndpoint.replace("https://", "wss://").replace("http://", "ws://");
    m_pushEndpoint += "/push/ws";

    authenticate();
}

void PushClient::stop()
{
    m_reconnectTimer.stop();
    m_ws.close();
    m_authenticated = false;
    if (m_connected) {
        m_connected = false;
        emit connectedChanged();
    }
}

void PushClient::authenticate()
{
    // POST to pre_auth (absolute path, not OCS)
    auto *reply = m_api->postAbsoluteUrl("/apps/notify_push/pre_auth");

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (reply->error() != QNetworkReply::NoError || status != 200) {
            qWarning() << "Push pre_auth failed:" << status << reply->errorString();
            reconnect();
            return;
        }

        m_pendingToken = QString::fromUtf8(reply->readAll()).trimmed();
        if (m_pendingToken.isEmpty()) {
            qWarning() << "Push pre_auth returned empty token";
            reconnect();
            return;
        }

        qDebug() << "Push: pre_auth token obtained, connecting to" << m_pushEndpoint;
        m_ws.open(QUrl(m_pushEndpoint));
    });
}

void PushClient::onConnected()
{
    qDebug() << "Push: WebSocket connected, authenticating";
    m_reconnectDelay = 2000;

    if (!m_pendingToken.isEmpty()) {
        m_ws.sendTextMessage(m_pendingToken);
        m_pendingToken.clear();
    }
}

void PushClient::onDisconnected()
{
    qDebug() << "Push: WebSocket disconnected";
    m_authenticated = false;
    if (m_connected) {
        m_connected = false;
        emit connectedChanged();
    }
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
            qDebug() << "Push: authenticated, listening for events";
        } else {
            qWarning() << "Push: auth failed:" << msg;
            m_ws.close();
        }
        return;
    }

    qDebug() << "Push: event received:" << msg;
    emit pushReceived(msg);
}

void PushClient::reconnect()
{
    if (m_api->serverUrl().isEmpty()) return;
    qDebug() << "Push: reconnecting in" << m_reconnectDelay << "ms";
    m_reconnectTimer.start(m_reconnectDelay);
    m_reconnectDelay = qMin(m_reconnectDelay * 2, MAX_RECONNECT_DELAY);
}
