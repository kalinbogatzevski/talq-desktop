#include "core/MessagePoller.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include <QTimer>
#include <QDebug>

MessagePoller::MessagePoller(ApiClient *api, QObject *parent)
    : QObject(parent)
    , m_api(api)
{
}

void MessagePoller::start(const QString &conversationToken, int lastKnownMessageId)
{
    stop();
    m_stopping = false;   // fresh run — clear the teardown flag stop() just set
    m_token = conversationToken;
    m_lastKnownMessageId = lastKnownMessageId;
    m_loggedTransport = false;
    m_polling = true;
    qDebug() << "Poller: starting for" << conversationToken << "lastKnown:" << lastKnownMessageId;
    poll();
}

void MessagePoller::stop()
{
    m_stopping = true;
    m_polling = false;
    if (m_currentReply) {
        auto *reply = m_currentReply;
        m_currentReply = nullptr;
        // A late finished() must not re-enter a half-torn-down poller.
        disconnect(reply, nullptr, this, nullptr);
        // During application teardown (~QCoreApplication: the event loop and
        // network dispatcher are already gone) abort() dereferences dead Qt
        // state and crashes. The reply is a child of the NAM and is freed with
        // it, so skipping abort() at shutdown leaks nothing. In normal
        // operation (e.g. switching conversations) the loop is alive, so the
        // in-flight long-poll is aborted exactly as before.
        if (!QCoreApplication::closingDown())
            reply->abort();
        reply->deleteLater();
    }
}

void MessagePoller::poll()
{
    if (!m_polling || m_token.isEmpty()) {
        qDebug() << "Poller: poll() skipped — polling:" << m_polling << "token:" << m_token;
        return;
    }

    QUrlQuery params;
    params.addQueryItem("lookIntoFuture", "1");
    params.addQueryItem("timeout", QString::number(POLL_TIMEOUT_SECS));
    params.addQueryItem("limit", "100");
    // Do NOT mark messages read as a side-effect of polling — the read marker
    // must only advance while TalQ is the focused window (driven by
    // MessageListModel::markAsRead under m_windowActive). Polling here merely
    // fetches new messages; an open room in the background stays unread.
    params.addQueryItem("setReadMarker", "false");
    params.addQueryItem("includeLastKnown", "0");

    if (m_lastKnownMessageId > 0)
        params.addQueryItem("lastKnownMessageId", QString::number(m_lastKnownMessageId));

    // 0.41.3-beta — DELIBERATELY DO NOT pass threadId on the long-poll.
    // Upstream Talk's pollNewMessages omits it; the client (MessageList-
    // Model) filters by threadId locally via passesThreadFilter. Server-
    // side filtering caused the "topic tab shows zero messages" bug
    // when a peer's client posted into the room without attaching a
    // threadId — those messages were excluded from the receiver's
    // filtered long-poll and silently lost from the topic view.
    (void)m_threadId;

    QString path = "apps/spreed/api/v1/chat/" + m_token;
    QMap<QByteArray, QByteArray> headers;
    if (m_lastKnownCommonRead > 0) {
        // Hint to the server so it can break the long-poll early when the
        // room's common-read marker advances (otherwise reads-only events
        // are invisible until a real chat message arrives). Not all NC Talk
        // servers honor this — we keep a periodic pull in MessageListModel
        // as a fallback.
        headers["X-Chat-Last-Common-Read"] = QByteArray::number(m_lastKnownCommonRead);
    }
    m_currentReply = m_api->getLongPoll(path, params, POLL_TIMEOUT_SECS, headers);

    connect(m_currentReply, &QNetworkReply::finished, this, &MessagePoller::handlePollResponse);
}

void MessagePoller::handlePollResponse()
{
    if (!m_currentReply)
        return;

    auto *reply = m_currentReply;
    m_currentReply = nullptr;

    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() == QNetworkReply::OperationCanceledError) {
        reply->deleteLater();
        // Our own stop()/teardown sets m_stopping AND nulls m_currentReply
        // (so it is caught by the !m_currentReply guard above and never gets
        // here). An OperationCanceledError reaching this point while we are
        // NOT stopping is therefore an UNEXPECTED abort — sleep/wake, VPN or
        // proxy drop, an idle keep-alive socket teardown, or a global
        // cancelAll. The old code treated it as an intentional stop and
        // returned without re-polling and without emitting pollError, so the
        // open room's ONLY live-update path died silently (m_connected stayed
        // true, no reconnect UI) while the conversation list kept refreshing —
        // "new message in the list but missing in the open room" (bug 1).
        // Surface it and resume.
        if (m_stopping || !m_polling)
            return;
        emit pollError(QStringLiteral("poll connection aborted; reconnecting"));
        QTimer::singleShot(2000, this, &MessagePoller::poll);
        return;
    }

    if (reply->error() != QNetworkReply::NoError && status == 0) {
        QString errorMsg = reply->errorString();
        reply->deleteLater();
        emit pollError(errorMsg);
        // Retry after a short delay
        if (m_polling) {
            QTimer::singleShot(2000, this, &MessagePoller::poll);
        }
        return;
    }

    QByteArray body = reply->readAll();

    // Update lastKnownMessageId from response header
    QByteArray lastGiven = reply->rawHeader("X-Chat-Last-Given");
    if (!lastGiven.isEmpty()) {
        m_lastKnownMessageId = lastGiven.toInt();
    }

    // Read receipt: last message all participants have read.
    // Only arrives on 200 responses — 304 cannot carry custom headers (RFC
    // restriction). The periodic refreshReadMarker() in MessageListModel
    // is the reliable path for pure-read updates on servers whose HPB does
    // not broadcast read-marker events.
    QByteArray lastCommonRead = reply->rawHeader("X-Chat-Last-Common-Read");
    if (!lastCommonRead.isEmpty()) {
        int v = lastCommonRead.toInt();
        if (v > m_lastKnownCommonRead) m_lastKnownCommonRead = v;
        emit lastCommonReadChanged(v);
    }

    // One-shot transport proof for the field log: confirm the long-poll is on a
    // reused HTTP/2 connection (the basis of the chat-speed work), not a fresh
    // connect each cycle. Logged once per poller run to avoid per-cycle spam.
    if (!m_loggedTransport) {
        m_loggedTransport = true;
        qInfo().nospace() << "Poller: transport for " << m_token
                          << " HTTP/2=" << reply->attribute(QNetworkRequest::Http2WasUsedAttribute).toBool();
    }

    reply->deleteLater();

    if (status == 304) {
        // No new messages (timeout expired) — poll again immediately
        emit pollSuccess();
        if (m_polling) {
            poll();
        }
        return;
    }

    if (status == 200) {
        emit pollSuccess();
        QJsonDocument doc = QJsonDocument::fromJson(body);
        QJsonObject root = doc.object();
        QJsonObject ocs = root["ocs"].toObject();
        QJsonValue dataVal = ocs["data"];

        if (dataVal.isArray()) {
            QJsonArray messages = dataVal.toArray();
            if (!messages.isEmpty()) {
                qDebug() << "Poller:" << messages.size() << "new message(s) in" << m_token
                         << "lastKnown:" << m_lastKnownMessageId;
                emit messagesReceived(messages);
            }
        }
    } else if (status != 304) {
        qDebug() << "Poller: unexpected status" << status << "for" << m_token;
    }

    // Continue polling
    if (m_polling) {
        poll();
    }
}
