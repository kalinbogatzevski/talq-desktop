#pragma once

#include <QAbstractListModel>
#include <QVector>
#include <QJsonArray>
#include "models/Message.h"
#include "core/ApiClient.h"
#include "core/MessagePoller.h"

class MessageCache;

/**
 * QAbstractListModel for chat messages in a conversation.
 * Handles initial history load + live polling for new messages.
 */
class MessageListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
    Q_PROPERTY(QString conversationToken READ conversationToken WRITE setConversationToken NOTIFY conversationTokenChanged)
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    Q_PROPERTY(int threadId READ threadId WRITE setThreadId NOTIFY threadIdChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        ActorNameRole,
        ActorIdRole,
        MessageTextRole,
        TimestampRole,
        IsSystemRole,
        MessageTypeRole,
        IsGroupedRole,      // true if same author & close in time to previous
        ReplyToTextRole,
        ReplyToAuthorRole,
        ReactionsRole,
        TimeStringRole,
        ShowDateSeparatorRole,  // true if this message starts a new day
        DateStringRole,         // "Today", "Yesterday", "18 Mar 2026"
        IsReadRole,             // true if all participants have read this message
        SendStatusRole,         // "sent", "sending", "failed"
        ThreadIdRole,
    };

    explicit MessageListModel(ApiClient *api, MessageCache *cache, QObject *parent = nullptr);
    ~MessageListModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool isLoading() const { return m_loading; }
    bool isConnected() const { return m_connected; }
    QString conversationToken() const { return m_token; }
    void setConversationToken(const QString &token);

    int threadId() const { return m_threadId; }
    void setThreadId(int id);

    Q_INVOKABLE void sendMessage(const QString &text, int replyToId = 0);
    Q_INVOKABLE void retryMessage(int tempId);
    Q_INVOKABLE void addReaction(int messageId, const QString &emoji);
    Q_INVOKABLE void loadHistory();
    Q_INVOKABLE void deleteMessage(int messageId);
    Q_INVOKABLE void pinMessage(int messageId);
    Q_INVOKABLE QString messageLink(int messageId) const;

signals:
    void loadingChanged();
    void conversationTokenChanged();
    void messageSent();
    void errorOccurred(const QString &error);
    void newMessagesAtEnd();
    void connectedChanged();
    void threadIdChanged();

private slots:
    void onMessagesReceived(const QJsonArray &messages);

private:
    void startPoller();
    void postAndReplace(const QString &token, const QJsonObject &body, int tempId);

    ApiClient *m_api;
    MessageCache *m_cache;
    MessagePoller *m_poller;
    QVector<Message> m_messages;
    QString m_token;
    bool m_loading = false;
    int m_oldestMessageId = 0;
    int m_lastCommonRead = 0;
    int m_threadId = 0;
    bool m_connected = true;  // assume connected until proven otherwise

private slots:
    void onLastCommonReadChanged(int messageId);
};
