#include "models/ConversationListModel.h"
#include <algorithm>
#include <QHash>

ConversationListModel::ConversationListModel(ApiClient *api, QObject *parent)
    : QAbstractListModel(parent)
    , m_api(api)
{
    m_autoRefreshTimer.setInterval(30000); // 30s fallback — push provides real-time
    connect(&m_autoRefreshTimer, &QTimer::timeout, this, &ConversationListModel::refresh);
}

int ConversationListModel::rowCount(const QModelIndex &) const
{
    return m_conversations.size();
}

QVariant ConversationListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_conversations.size())
        return {};

    const auto &c = m_conversations[index.row()];
    switch (role) {
        case TokenRole:         return c.token;
        case DisplayNameRole:   return c.displayName;
        case TypeRole:          return c.type;
        case UnreadCountRole:   return c.unreadMessages;
        case UnreadMentionRole: return c.unreadMention;
        case FavoriteRole:      return c.favorite;
        case LastMessageRole:   return c.lastMessageText;
        case LastAuthorRole:    return c.lastMessageAuthor;
        case LastActivityRole:  return c.lastActivity;
        case ActorIdRole:       return c.name;
        case UserStatusRole: {
            // For 1:1 chats — prefer user_status API, fallback to room API
            if (c.type == 1 && !c.name.isEmpty()) {
                QString status = m_userStatuses.value(c.name);
                if (status.isEmpty())
                    status = c.status;
                return status.isEmpty() ? "offline" : status;
            }
            return QString();
        }
        case HasTopicsRole:     return c.hasTopics;
        case NotificationLevelRole: return c.notificationLevel;
        default:                return {};
    }
}

QHash<int, QByteArray> ConversationListModel::roleNames() const
{
    return {
        {TokenRole,         "token"},
        {DisplayNameRole,   "displayName"},
        {TypeRole,          "conversationType"},
        {UnreadCountRole,   "unreadCount"},
        {UnreadMentionRole, "unreadMention"},
        {FavoriteRole,      "isFavorite"},
        {LastMessageRole,   "lastMessage"},
        {LastAuthorRole,    "lastAuthor"},
        {LastActivityRole,  "lastActivity"},
        {ActorIdRole,       "participantUserId"},
        {UserStatusRole,    "userStatus"},
        {HasTopicsRole,     "hasTopics"},
        {NotificationLevelRole, "notificationLevel"},
    };
}

void ConversationListModel::refresh()
{
    if (m_loading) return;

    // Only show loading indicator on initial load (empty list)
    bool isInitialLoad = m_conversations.isEmpty();
    m_loading = true;
    if (isInitialLoad) emit loadingChanged();

    m_api->getArray("apps/spreed/api/v4/room", [this, isInitialLoad](bool ok, const QJsonArray &data, int) {
        m_loading = false;
        if (isInitialLoad) emit loadingChanged();

        if (!ok) {
            emit errorOccurred("Failed to load conversations");
            return;
        }

        // Snapshot old state for preservation across refresh
        QHash<QString, int> oldUnread;
        QHash<QString, bool> oldHasTopics;
        for (const auto &c : m_conversations) {
            oldUnread[c.token] = c.unreadMessages;
            oldHasTopics[c.token] = c.hasTopics;
        }

        // Parse new data
        QVector<Conversation> newConversations;
        newConversations.reserve(data.size());
        for (const auto &val : data) {
            newConversations.append(Conversation::fromJson(val.toObject()));
        }
        std::sort(newConversations.begin(), newConversations.end());

        // Preserve hasTopics flag across refresh
        for (auto &c : newConversations) {
            c.hasTopics = oldHasTopics.value(c.token, false);
        }

        // Detect new unread messages and emit notifications
        int newTotalUnread = 0;
        for (const auto &c : newConversations) {
            newTotalUnread += c.unreadMessages;

            int prev = oldUnread.value(c.token, 0);
            if (c.unreadMessages > prev && prev >= 0 && !oldUnread.isEmpty()) {
                // This conversation has new unread messages since last check
                emit newUnreadMessage(c.displayName, c.lastMessageText, c.token);
            }

            // Detect incoming calls — use persistent m_callState so two
            // overlapping refreshes (push + auto-refresh) can't both miss the
            // false→true transition.  Only emit once per transition.
            // On the first load, seed m_callState silently (don't ring for
            // calls that were already active before we started).
            bool prevCall = m_callState.value(c.token, false);
            if (c.hasCall && !prevCall && m_callStateSeeded) {
                qDebug() << "ConversationListModel: call detected in" << c.displayName << "token=" << c.token;
                emit incomingCallDetected(c.displayName, c.token, c.callFlag);
            }
            m_callState[c.token] = c.hasCall;
        }
        if (!m_callStateSeeded) m_callStateSeeded = true;

        // Prune stale call state entries for conversations no longer in the list
        QSet<QString> activeTokens;
        for (const auto &c : newConversations) activeTokens.insert(c.token);
        for (auto it = m_callState.begin(); it != m_callState.end(); ) {
            if (!activeTokens.contains(it.key())) it = m_callState.erase(it);
            else ++it;
        }

        // Update model — use beginResetModel only when structure changes
        int oldSize = m_conversations.size();
        int newSize = newConversations.size();

        if (oldSize == newSize) {
            // Same count — just update data in-place (fast, no delegate destruction)
            m_conversations = newConversations;
            if (newSize > 0)
                emit dataChanged(index(0), index(newSize - 1));
        } else {
            // Count changed — must reset (rare: conversation added/removed)
            beginResetModel();
            m_conversations = newConversations;
            endResetModel();
        }
        emit countChanged();
        fetchUserStatuses();

        if (m_totalUnread != newTotalUnread) {
            m_totalUnread = newTotalUnread;
            emit totalUnreadChanged();
        }
    });
}

void ConversationListModel::fetchUserStatuses()
{
    m_api->getArray("apps/user_status/api/v1/statuses",
        [this](bool ok, const QJsonArray &data, int) {
            if (!ok) return;

            QHash<QString, QString> statuses;
            for (const auto &val : data) {
                QJsonObject u = val.toObject();
                statuses[u["userId"].toString()] = u["status"].toString();
            }

            if (statuses != m_userStatuses) {
                m_userStatuses = statuses;
                // Notify all 1:1 conversations that status may have changed
                if (!m_conversations.isEmpty())
                    emit dataChanged(index(0), index(m_conversations.size() - 1), {UserStatusRole});
            }
        });
}

void ConversationListModel::startAutoRefresh()
{
    m_autoRefreshTimer.start();
}

void ConversationListModel::stopAutoRefresh()
{
    m_autoRefreshTimer.stop();
}

QString ConversationListModel::tokenAt(int index) const
{
    if (index >= 0 && index < m_conversations.size())
        return m_conversations[index].token;
    return {};
}

int ConversationListModel::indexOfToken(const QString &token) const
{
    for (int i = 0; i < m_conversations.size(); ++i) {
        if (m_conversations[i].token == token)
            return i;
    }
    return -1;
}

void ConversationListModel::clearUnreadForToken(const QString &token)
{
    int i = indexOfToken(token);
    if (i < 0 || m_conversations[i].unreadMessages == 0) return;

    m_totalUnread = qMax(0, m_totalUnread - m_conversations[i].unreadMessages);
    m_conversations[i].unreadMessages = 0;
    m_conversations[i].unreadMention = false;
    emit dataChanged(index(i), index(i), {UnreadCountRole, UnreadMentionRole});
    emit totalUnreadChanged();
}

int ConversationListModel::lastReadMessageForToken(const QString &token) const
{
    int i = indexOfToken(token);
    return (i >= 0) ? m_conversations[i].lastReadMessage : 0;
}

void ConversationListModel::setHasTopics(const QString &token, bool has)
{
    int i = indexOfToken(token);
    if (i < 0 || m_conversations[i].hasTopics == has) return;

    m_conversations[i].hasTopics = has;
    emit dataChanged(index(i), index(i), {HasTopicsRole});
}

void ConversationListModel::setNotificationLevel(int idx, int level)
{
    if (idx < 0 || idx >= m_conversations.size()) return;
    QString token = m_conversations[idx].token;
    m_api->setNotificationLevel(token, level,
        [this, token, level](bool success, const QJsonObject &, int) {
            if (!success) return;
            int i = indexOfToken(token);
            if (i < 0) return;
            m_conversations[i].notificationLevel = level;
            emit dataChanged(index(i), index(i), {NotificationLevelRole});
        });
}

void ConversationListModel::updateLastMessage(const QString &token, const QString &author, const QString &text)
{
    int i = indexOfToken(token);
    if (i < 0) return;
    m_conversations[i].lastMessageText = text;
    m_conversations[i].lastMessageAuthor = author;
    emit dataChanged(index(i), index(i), {LastMessageRole, LastAuthorRole});
}
