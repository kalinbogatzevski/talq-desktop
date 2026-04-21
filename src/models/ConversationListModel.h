#pragma once

#include <QAbstractListModel>
#include <QVector>
#include <QTimer>
#include "models/Conversation.h"
#include "core/ApiClient.h"

/**
 * QAbstractListModel exposing conversations to QML.
 * Fetches from Talk API and keeps the list sorted (favorites first, then by activity).
 * Auto-refreshes every 30 seconds to catch new messages in all conversations.
 */
class ConversationListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int totalUnread READ totalUnread NOTIFY totalUnreadChanged)

public:
    enum Roles {
        TokenRole = Qt::UserRole + 1,
        DisplayNameRole,
        TypeRole,
        UnreadCountRole,
        UnreadMentionRole,
        FavoriteRole,
        LastMessageRole,
        LastAuthorRole,
        LastActivityRole,
        ActorIdRole,
        UserStatusRole,     // "online", "away", "dnd", "offline"
        UserStatusMessageRole,
        UserStatusIconRole,
        HasTopicsRole,
        NotificationLevelRole,
        ParticipantTypeRole,
    };

    explicit ConversationListModel(ApiClient *api, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void startAutoRefresh();
    Q_INVOKABLE void stopAutoRefresh();
    Q_INVOKABLE QString tokenAt(int index) const;
    Q_INVOKABLE int lastReadMessageForToken(const QString &token) const;
    Q_INVOKABLE QString userStatusForToken(const QString &token) const;
    Q_INVOKABLE QString userStatusMessageForToken(const QString &token) const;
    Q_INVOKABLE QString userStatusIconForToken(const QString &token) const;
    Q_INVOKABLE void clearUnreadForToken(const QString &token);
    Q_INVOKABLE void setHasTopics(const QString &token, bool has);
    Q_INVOKABLE void setNotificationLevel(int index, int level);
    Q_INVOKABLE void updateLastMessage(const QString &token, const QString &author, const QString &text);

    bool isLoading() const { return m_loading; }
    int totalUnread() const { return m_totalUnread; }

signals:
    void loadingChanged();
    void countChanged();
    void totalUnreadChanged();
    void errorOccurred(const QString &error);
    // Emitted when a conversation gets new unread messages
    void newUnreadMessage(const QString &conversationName, const QString &lastMessage, const QString &token);
    // Emitted when someone starts a call in a conversation
    void incomingCallDetected(const QString &conversationName, const QString &token, int callFlag);

private:
    int indexOfToken(const QString &token) const;
    void fetchUserStatuses();

    struct UserStatus {
        QString state;
        QString message;
        QString icon;
        bool operator==(const UserStatus &other) const {
            return state == other.state && message == other.message && icon == other.icon;
        }
        bool operator!=(const UserStatus &other) const { return !(*this == other); }
    };

    ApiClient *m_api;
    QVector<Conversation> m_conversations;
    QHash<QString, UserStatus> m_userStatuses;  // userId → UserStatus
    QHash<QString, bool> m_callState;        // persistent call state — survives across refreshes
    bool m_callStateSeeded = false;          // true after first load — prevents ringing for stale calls
    QTimer m_autoRefreshTimer;
    QTimer m_statusPollTimer;
    bool m_loading = false;
    int m_totalUnread = 0;
};
