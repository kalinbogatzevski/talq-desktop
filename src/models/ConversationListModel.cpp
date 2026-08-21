#include "models/ConversationListModel.h"
#include <algorithm>
#include <QHash>

ConversationListModel::ConversationListModel(ApiClient *api, QObject *parent)
    : QAbstractListModel(parent)
    , m_api(api)
{
    m_autoRefreshTimer.setInterval(30000); // 30s fallback — push provides real-time
    connect(&m_autoRefreshTimer, &QTimer::timeout, this, &ConversationListModel::refresh);

    m_statusPollTimer.setInterval(60 * 1000);
    connect(&m_statusPollTimer, &QTimer::timeout, this, &ConversationListModel::fetchUserStatuses);

    // Stuck-latch watchdog (see m_loadWatchdog in the header). If a /room
    // fetch hasn't returned within 15 s, assume the reply is never coming,
    // clear the latch and retry — otherwise the whole list would freeze until
    // an app restart. 15 s sits above any healthy fetch and below the 30 s
    // fallback timer.
    m_loadWatchdog.setSingleShot(true);
    m_loadWatchdog.setInterval(15 * 1000);
    connect(&m_loadWatchdog, &QTimer::timeout, this, [this]() {
        if (!m_loading) return;
        qWarning() << "ConversationListModel: /room refresh stalled >15s — "
                      "clearing latch and retrying";
        m_loading = false;
        refresh();
    });
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
                QString state = m_userStatuses.value(c.name).state;
                if (state.isEmpty())
                    state = c.status;
                return state.isEmpty() ? "offline" : state;
            }
            return QString();
        }
        case UserStatusMessageRole: {
            if (c.type == 1 && !c.name.isEmpty())
                return m_userStatuses.value(c.name).message;
            return QString();
        }
        case UserStatusIconRole: {
            if (c.type == 1 && !c.name.isEmpty())
                return m_userStatuses.value(c.name).icon;
            return QString();
        }
        case HasTopicsRole:     return c.hasTopics;
        case NotificationLevelRole: return c.notificationLevel;
        case ParticipantTypeRole:   return c.participantType;
        case TagIdsRole:        return c.tagIds;
        case AttributesRole:    return c.attributes;
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
        {ActorIdRole,            "participantUserId"},
        {UserStatusRole,         "userStatus"},
        {UserStatusMessageRole,  "userStatusMessage"},
        {UserStatusIconRole,     "userStatusIcon"},
        {HasTopicsRole,          "hasTopics"},
        {NotificationLevelRole, "notificationLevel"},
        {ParticipantTypeRole,   "participantType"},
        {TagIdsRole,            "tagIds"},
        {AttributesRole,        "attributes"},
    };
}

void ConversationListModel::refresh()
{
    // A refresh is already in flight — don't drop this request, remember it.
    // The in-flight fetch may have started BEFORE the event that prompted this
    // call (e.g. a push announcing a brand-new room), so its response could
    // miss that room. We run exactly one more refresh when it completes.
    if (m_loading) { m_pendingRefresh = true; return; }

    // Only show loading indicator on initial load (empty list)
    bool isInitialLoad = m_conversations.isEmpty();
    m_loading = true;
    m_loadWatchdog.start();   // self-heal if the reply never arrives
    if (isInitialLoad) emit loadingChanged();

    m_api->getArray("apps/spreed/api/v4/room", [this, isInitialLoad](bool ok, const QJsonArray &data, int) {
        m_loading = false;
        m_loadWatchdog.stop();
        // If a refresh was requested while this one was in flight, honour it
        // now (after the current response is fully applied below) so a new
        // room announced mid-fetch can't be lost. Deferred via the event loop
        // to avoid deep recursion on a burst of pushes.
        if (m_pendingRefresh) {
            m_pendingRefresh = false;
            QTimer::singleShot(0, this, &ConversationListModel::refresh);
        }
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
                // This conversation has new unread messages since last check.
                // Skip emitting if the last message was sent silently — the
                // sender explicitly opted out of notifying us.
                if (!c.lastMessageSilent)
                    emit newUnreadMessage(c.displayName, c.lastMessageText, c.token);
            }

            // Detect incoming calls — use persistent m_callState so two
            // overlapping refreshes (push + auto-refresh) can't both miss the
            // false→true transition.  Only emit once per transition.
            // On the first load, seed m_callState silently (don't ring for
            // calls that were already active before we started).
            bool prevCall = m_callState.value(c.token, false);
            if (c.hasCall && !prevCall && m_callStateSeeded) {
                // 0.41.8-beta — multi-device self-ring suppression. If
                // `participantInCallFlags` is non-zero, OUR own user is
                // already in this call (presumably from a sibling
                // device — our other laptop, phone, etc.). Don't ring
                // ourselves; the user is busy elsewhere AND probably
                // the one who started the call. Mirrors the upstream
                // Talk web client's behaviour and matches the
                // userId-comparison check already in CallManager's
                // signaling-based detection path (CallManager.cpp:2125).
                if (c.participantInCallFlags > 0) {
                    qInfo() << "ConversationListModel: call started in"
                            << c.displayName
                            << "but our user is already in it (flags="
                            << c.participantInCallFlags
                            << ") — not ringing self";
                } else {
                    qDebug() << "ConversationListModel: call detected in"
                             << c.displayName << "token=" << c.token;
                    emit incomingCallDetected(c.displayName, c.token, c.callFlag);
                }
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
            QHash<QString, UserStatus> statuses;
            for (const auto &val : data) {
                QJsonObject o = val.toObject();
                UserStatus u;
                u.state   = o.value("status").toString();
                u.message = o.value("message").toString();
                u.icon    = o.value("icon").toString();
                statuses[o.value("userId").toString()] = u;
            }
            if (statuses != m_userStatuses) {
                m_userStatuses = statuses;
                if (!m_conversations.isEmpty()) {
                    emit dataChanged(index(0), index(m_conversations.size() - 1),
                                     {UserStatusRole, UserStatusMessageRole, UserStatusIconRole});
                }
            }
        });
}

void ConversationListModel::refreshUserStatuses()
{
    fetchUserStatuses();
}

int ConversationListModel::conversationTypeForToken(const QString &token) const
{
    for (const Conversation &c : m_conversations) {
        if (c.token == token) return c.type;
    }
    return 0;
}

int ConversationListModel::attributesForToken(const QString &token) const
{
    for (const Conversation &c : m_conversations) {
        if (c.token == token) return c.attributes;
    }
    return 0;
}

QStringList ConversationListModel::tagIdsForToken(const QString &token) const
{
    for (const Conversation &c : m_conversations) {
        if (c.token == token) return c.tagIds;
    }
    return {};
}

void ConversationListModel::startAutoRefresh()
{
    m_autoRefreshTimer.start();
    m_statusPollTimer.start();
}

void ConversationListModel::stopAutoRefresh()
{
    m_autoRefreshTimer.stop();
    m_statusPollTimer.stop();
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

void ConversationListModel::markReadAt(const QString &token, int lastReadMessageId)
{
    int i = indexOfToken(token);
    if (i < 0) return;
    bool changed = false;
    if (lastReadMessageId > m_conversations[i].lastReadMessage) {
        m_conversations[i].lastReadMessage = lastReadMessageId;
        changed = true;
    }
    if (m_conversations[i].unreadMessages > 0) {
        m_totalUnread = qMax(0, m_totalUnread - m_conversations[i].unreadMessages);
        m_conversations[i].unreadMessages = 0;
        m_conversations[i].unreadMention = false;
        changed = true;
        emit totalUnreadChanged();
    }
    if (changed) {
        emit dataChanged(index(i), index(i), {UnreadCountRole, UnreadMentionRole});
    }
}

int ConversationListModel::lastReadMessageForToken(const QString &token) const
{
    int i = indexOfToken(token);
    return (i >= 0) ? m_conversations[i].lastReadMessage : 0;
}

QString ConversationListModel::userStatusForToken(const QString &token) const
{
    for (const auto &c : m_conversations) {
        if (c.token == token)
            return m_userStatuses.value(c.name).state;
    }
    return {};
}

QString ConversationListModel::userStatusMessageForToken(const QString &token) const
{
    for (const auto &c : m_conversations) {
        if (c.token == token)
            return m_userStatuses.value(c.name).message;
    }
    return {};
}

QString ConversationListModel::userStatusIconForToken(const QString &token) const
{
    for (const auto &c : m_conversations) {
        if (c.token == token)
            return m_userStatuses.value(c.name).icon;
    }
    return {};
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
