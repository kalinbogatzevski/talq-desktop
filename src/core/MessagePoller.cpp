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
    if (!m_polling || m_token.isEmpty())
        return;

    QUrlQuery params;
    params.addQueryItem("lookIntoFuture", "1");
    params.addQueryItem("timeout", QString::number(POLL_TIMEOUT_SECS));
    params.addQueryItem("limit", "100");
    params.addQueryItem("setReadMarker", "true");
    params.addQueryItem("includeLastKnown", "0");

    if (m_lastKnownMessageId > 0)
        params.addQueryItem("lastKnownMessageId", QString::number(m_lastKnownMessageId));

    QString path = "apps/spreed/api/v1/chat/" + m_token;
    m_currentReply = m_api->getLongPoll(path, params, POLL_TIMEOUT_SECS);

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

    // Read receipt: last message all participants have read
    QByteArray lastCommonRead = reply->rawHeader("X-Chat-Last-Common-Read");
    if (!lastCommonRead.isEmpty()) {
        emit lastCommonReadChanged(lastCommonRead.toInt());
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
                emit messagesReceived(messages);
            }
        }
    }

    // Continue polling
    if (m_polling) {
        poll();
    }
}
