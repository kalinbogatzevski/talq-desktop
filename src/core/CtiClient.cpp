#include "CtiClient.h"

#include "TalqLog.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QWebSocket>

CtiClient::CtiClient(QObject *parent)
    : QObject(parent)
{
    m_socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_socket, &QWebSocket::connected,    this, &CtiClient::onConnected);
    connect(m_socket, &QWebSocket::disconnected, this, &CtiClient::onDisconnected);
    connect(m_socket, &QWebSocket::textMessageReceived,
            this, &CtiClient::onTextMessageReceived);

    // Qt reports transport errors separately from disconnection, but for our
    // purposes both mean "not connected" — the reconnect is driven off
    // disconnected(), which Qt emits in either case.
    connect(m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        TLOG_NET("CTI socket error:" << m_socket->errorString());
    });

    // Keepalive: ping while up so an idle proxy or NAT never culls us
    // unnoticed.
    m_keepAliveTimer.setInterval(kPingMs);
    connect(&m_keepAliveTimer, &QTimer::timeout, this, [this]() {
        if (m_socket->state() == QAbstractSocket::ConnectedState)
            m_socket->ping();
    });
    // ...and a read deadline, because a ping that is never answered is the
    // only way to notice a connection that is up in name only.
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(kIdleMs);
    connect(&m_idleTimer, &QTimer::timeout, this, [this]() {
        TWARN("CTI: no traffic for" << (kIdleMs / 1000) << "s; reconnecting");
        m_socket->abort();          // abort, not close: the peer may be gone
        onDisconnected();
    });
    connect(m_socket, &QWebSocket::pong, this, [this](quint64, const QByteArray &) {
        m_idleTimer.start();        // any traffic proves the path is alive
    });

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (m_running && !m_authRejected)
            m_socket->open(m_url);
    });
}

CtiClient::~CtiClient() = default;

void CtiClient::start(const QUrl &url, const QString &token)
{
    if (url.isEmpty() || token.isEmpty()) {
        stop();
        return;
    }

    // A changed credential deserves a fresh attempt even if the previous one
    // was rejected — that is exactly what re-pairing is for.
    m_authRejected = false;
    m_url = url;
    m_token = token;
    m_running = true;
    m_backoffMs = kMinBackoffMs;

    m_socket->close();
    m_socket->open(m_url);
}

void CtiClient::stop()
{
    m_running = false;
    m_ready = false;
    m_reconnectTimer.stop();
    m_keepAliveTimer.stop();
    m_idleTimer.stop();
    m_socket->close();
}

void CtiClient::onConnected()
{
    // Nothing is delivered until we say who we are. The token is a per-user,
    // read-only, server-revocable key — never an ERP password.
    const QJsonObject hello{ { QStringLiteral("token"), m_token } };
    m_socket->sendTextMessage(
        QString::fromUtf8(QJsonDocument(hello).toJson(QJsonDocument::Compact)));
    m_keepAliveTimer.start();
    m_idleTimer.start();
}

void CtiClient::onDisconnected()
{
    const bool wasReady = m_ready;
    m_ready = false;
    m_extension.clear();
    m_keepAliveTimer.stop();
    m_idleTimer.stop();

    if (wasReady)
        emit disconnected();

    if (m_running && !m_authRejected)
        scheduleReconnect();
}

void CtiClient::scheduleReconnect()
{
    m_reconnectTimer.start(m_backoffMs);
    m_backoffMs = qMin(m_backoffMs * 2, kMaxBackoffMs);
}

void CtiClient::onTextMessageReceived(const QString &message)
{
    m_idleTimer.start();   // any message proves the connection is alive

    const QJsonObject obj =
        QJsonDocument::fromJson(message.toUtf8()).object();
    const QString type = obj.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("ready")) {
        m_ready = true;
        m_backoffMs = kMinBackoffMs;   // a good connection resets the penalty
        m_extension = obj.value(QStringLiteral("extension")).toString();
        emit ready(m_extension, obj.value(QStringLiteral("display_name")).toString());
        return;
    }

    if (type == QLatin1String("ring")) {
        emit ringing(obj.value(QStringLiteral("call_id")).toString(),
                     obj.value(QStringLiteral("caller")).toString(),
                     obj.value(QStringLiteral("extension")).toString());
        return;
    }

    if (type == QLatin1String("end")) {
        emit ended(obj.value(QStringLiteral("call_id")).toString(),
                   obj.value(QStringLiteral("reason")).toString());
        return;
    }

    if (type == QLatin1String("error")) {
        // The daemon refused us. Reconnecting with the same token would loop
        // forever against a credential that cannot work, so stop and let the
        // UI say so once.
        const QString reason = obj.value(QStringLiteral("error")).toString();
        // Only give up on errors that retrying genuinely cannot fix. Anything
        // else -- including a code this build has never heard of -- is treated
        // as transient, because latching off permanently is the worst possible
        // response to a server that is merely having a bad day.
        const bool terminal = (reason == QLatin1String("unauthorised")
                               || reason == QLatin1String("no-extension"));
        if (!terminal) {
            TWARN("CTI: server reported" << reason << "- will retry");
            m_socket->close();
            return;
        }
        m_authRejected = true;
        m_running = false;
        m_reconnectTimer.stop();
        m_keepAliveTimer.stop();
        m_idleTimer.stop();
        m_socket->close();
        TWARN("CTI authentication refused:" << reason);
        emit authenticationFailed(reason);
        return;
    }

    // Unknown type: ignore. Forward compatibility — a newer daemon may add
    // events (call control is the obvious one) and an older client should not
    // fall over.
    TLOG_NET("CTI: ignoring unknown event type" << type);
}
