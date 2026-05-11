#include "core/MessagePoller.h"
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
    m_token = conversationToken;
    m_lastKnownMessageId = lastKnownMessageId;
    m_polling = true;
    qDebug() << "Poller: starting for" << conversationToken << "lastKnown:" << lastKnownMessageId;
    poll();
}

void MessagePoller::stop()
{
    m_polling = false;
    if (m_currentReply) {
        auto *reply = m_currentReply;
        m_currentReply = nullptr;
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
    params.addQueryItem("setReadMarker", "true");
    params.addQueryItem("includeLastKnown", "0");

    if (m_lastKnownMessageId > 0)
        params.addQueryItem("lastKnownMessageId", QString::number(m_lastKnownMessageId));

    if (m_threadId > 0)
        params.addQueryItem("threadId", QString::number(m_threadId));

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
        // Intentional stop
        reply->deleteLater();
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
