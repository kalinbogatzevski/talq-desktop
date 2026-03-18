#pragma once

#include <QObject>
#include <QNetworkReply>
#include <QJsonArray>
#include "core/ApiClient.h"

/**
 * Long-polls the Talk chat API for new messages.
 *
 * Uses the standard Talk polling pattern:
 *   GET /apps/spreed/api/v1/chat/{token}?lookIntoFuture=1&timeout=30&lastKnownMessageId=X
 *
 * The server holds the connection open for up to 30 seconds, returning
 * immediately if new messages arrive. On completion, the poller automatically
 * re-issues the request.
 */
class MessagePoller : public QObject
{
    Q_OBJECT

public:
    explicit MessagePoller(ApiClient *api, QObject *parent = nullptr);

    void start(const QString &conversationToken, int lastKnownMessageId = 0);
    void stop();
    bool isPolling() const { return m_polling; }
    QString currentToken() const { return m_token; }

signals:
    void messagesReceived(const QJsonArray &messages);
    void lastCommonReadChanged(int messageId);
    void pollError(const QString &error);
    void pollSuccess();  // emitted on any successful poll response (200 or 304)

private:
    void poll();
    void handlePollResponse();

    ApiClient *m_api;
    QNetworkReply *m_currentReply = nullptr;
    QString m_token;
    int m_lastKnownMessageId = 0;
    bool m_polling = false;
    static constexpr int POLL_TIMEOUT_SECS = 30;
};
