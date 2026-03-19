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

        // Snapshot old unread counts for notification detection
        QHash<QString, int> oldUnread;
        for (const auto &c : m_conversations) {
            oldUnread[c.token] = c.unreadMessages;
        }

        // Parse new data
        QVector<Conversation> newConversations;
        newConversations.reserve(data.size());
        for (const auto &val : data) {
            newConversations.append(Conversation::fromJson(val.toObject()));
        }
        std::sort(newConversations.begin(), newConversations.end());

        // Detect new unread messages and emit notifications
        int newTotalUnread = 0;
        for (const auto &c : newConversations) {
            newTotalUnread += c.unreadMessages;

            int prev = oldUnread.value(c.token, 0);
            if (c.unreadMessages > prev && prev >= 0 && !oldUnread.isEmpty()) {
                // This conversation has new unread messages since last check
                emit newUnreadMessage(c.displayName, c.lastMessageText, c.token);
            }
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

        if (m_totalUnread != newTotalUnread) {
            m_totalUnread = newTotalUnread;
            emit totalUnreadChanged();
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

void ConversationListModel::clearUnreadForToken(const QString &token)
{
    for (int i = 0; i < m_conversations.size(); ++i) {
        if (m_conversations[i].token == token && m_conversations[i].unreadMessages > 0) {
            m_totalUnread -= m_conversations[i].unreadMessages;
            m_conversations[i].unreadMessages = 0;
            m_conversations[i].unreadMention = false;
            emit dataChanged(index(i), index(i), {UnreadCountRole, UnreadMentionRole});
            emit totalUnreadChanged();
            break;
        }
    }
}

int ConversationListModel::lastReadMessageForToken(const QString &token) const
{
    for (const auto &c : m_conversations) {
        if (c.token == token)
            return c.lastReadMessage;
    }
    return 0;
}
