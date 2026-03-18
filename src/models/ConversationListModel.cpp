#include "models/ConversationListModel.h"
#include <algorithm>

ConversationListModel::ConversationListModel(ApiClient *api, QObject *parent)
    : QAbstractListModel(parent)
    , m_api(api)
{
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
    };
}

void ConversationListModel::refresh()
{
    m_loading = true;
    emit loadingChanged();

    m_api->getArray("apps/spreed/api/v4/room", [this](bool ok, const QJsonArray &data, int) {
        m_loading = false;
        emit loadingChanged();

        if (!ok) {
            emit errorOccurred("Failed to load conversations");
            return;
        }

        beginResetModel();
        m_conversations.clear();
        m_conversations.reserve(data.size());
        for (const auto &val : data) {
            m_conversations.append(Conversation::fromJson(val.toObject()));
        }
        std::sort(m_conversations.begin(), m_conversations.end());
        endResetModel();
        emit countChanged();
    });
}

QString ConversationListModel::tokenAt(int index) const
{
    if (index >= 0 && index < m_conversations.size())
        return m_conversations[index].token;
    return {};
}

int ConversationListModel::lastReadMessageForToken(const QString &token) const
{
    for (const auto &c : m_conversations) {
        if (c.token == token)
            return c.lastReadMessage;
    }
    return 0;
}
