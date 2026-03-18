#pragma once

#include <QAbstractListModel>
#include <QVector>
#include <QJsonArray>
#include "models/Message.h"
#include "core/ApiClient.h"
#include "core/MessagePoller.h"

/**
 * QAbstractListModel for chat messages in a conversation.
 * Handles initial history load + live polling for new messages.
 */
class MessageListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
    Q_PROPERTY(QString conversationToken READ conversationToken WRITE setConversationToken NOTIFY conversationTokenChanged)

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
    };

    explicit MessageListModel(ApiClient *api, QObject *parent = nullptr);
    ~MessageListModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool isLoading() const { return m_loading; }
    QString conversationToken() const { return m_token; }
    void setConversationToken(const QString &token);

    Q_INVOKABLE void sendMessage(const QString &text, int replyToId = 0);
    Q_INVOKABLE void loadHistory();

signals:
    void loadingChanged();
    void conversationTokenChanged();
    void messageSent();
    void errorOccurred(const QString &error);

private slots:
    void onMessagesReceived(const QJsonArray &messages);

private:
    void appendMessages(const QJsonArray &arr);

    ApiClient *m_api;
    MessagePoller *m_poller;
    QVector<Message> m_messages;
    QString m_token;
    bool m_loading = false;
    int m_oldestMessageId = 0;
    int m_lastCommonRead = 0;

private slots:
    void onLastCommonReadChanged(int messageId);
};
