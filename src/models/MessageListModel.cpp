#include "models/MessageListModel.h"
#include "core/MessageCache.h"
#include <QSet>
#include <QUrlQuery>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include <QTimer>
#include <QNetworkReply>

// ============================================================================
// STORAGE CONVENTION: m_messages is stored NEWEST-FIRST.
//   m_messages[0] = newest message
//   m_messages[last] = oldest message
//
// ListView uses BottomToTop, so index 0 renders at the BOTTOM of the screen.
// This means: newest message at bottom, oldest at top — natural chat order.
//
// "Prepend" (insert at index 0) = new messages appearing at bottom.
// "Append" (insert at end) = older history appearing at top.
// ============================================================================

MessageListModel::MessageListModel(ApiClient *api, MessageCache *cache, QObject *parent)
    : QAbstractListModel(parent)
    , m_api(api)
    , m_cache(cache)
    , m_poller(new MessagePoller(api, this))
{
    connect(m_poller, &MessagePoller::messagesReceived,
            this, &MessageListModel::onMessagesReceived);
    connect(m_poller, &MessagePoller::lastCommonReadChanged,
            this, &MessageListModel::onLastCommonReadChanged);

    connect(m_poller, &MessagePoller::pollSuccess, this, [this]() {
        if (!m_connected) { m_connected = true; emit connectedChanged(); }
    });
    connect(m_poller, &MessagePoller::pollError, this, [this](const QString &) {
        if (m_connected) { m_connected = false; emit connectedChanged(); }
    });
}

MessageListModel::~MessageListModel()
{
    m_poller->stop();
}

int MessageListModel::rowCount(const QModelIndex &) const
{
    return m_messages.size();
}

QVariant MessageListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_messages.size())
        return {};

    const auto &m = m_messages[index.row()];

    switch (role) {
        case IdRole:            return m.id;
        case ActorNameRole:     return m.actorDisplayName;
        case ActorIdRole:       return m.actorId;
        case MessageTextRole:   return m.message;
        case TimestampRole:     return m.timestamp;
        case IsSystemRole:      return m.isSystem;
        case MessageTypeRole:   return m.messageType;
        case IsGroupedRole: {
            // In newest-first + BottomToTop, the message visually ABOVE us
            // is at index.row()+1 (older). We group with the older message.
            int olderIdx = index.row() + 1;
            if (olderIdx >= m_messages.size()) return false;
            return m.isGroupedWith(m_messages[olderIdx]);
        }
        case ReplyToTextRole:
            return m.replyTo.isEmpty() ? QString() : m.replyTo["message"].toString();
        case ReplyToAuthorRole:
            return m.replyTo.isEmpty() ? QString() : m.replyTo["actorDisplayName"].toString();
        case ReactionsRole: {
            QStringList parts;
            for (auto it = m.reactions.begin(); it != m.reactions.end(); ++it) {
                parts << QString("%1 %2").arg(it.key()).arg(it.value().toInt());
            }
            return parts.join("  ");
        }
        case TimeStringRole:
            return m.dateTime().toString("HH:mm");
        case ShowDateSeparatorRole: {
            // Show separator when this message is on a different day than the
            // message visually ABOVE it (= older = index+1).
            // The topmost message (oldest) always gets a separator.
            int olderIdx = index.row() + 1;
            if (olderIdx >= m_messages.size()) return true;
            return m.dateTime().date() != m_messages[olderIdx].dateTime().date();
        }
        case DateStringRole: {
            auto date = m.dateTime().date();
            auto today = QDate::currentDate();
            if (date == today)
                return QString("Today");
            if (date == today.addDays(-1))
                return QString("Yesterday");
            if (date.year() == today.year())
                return date.toString("dd MMM");
            return date.toString("dd MMM yyyy");
        }
        case IsReadRole:
            return m.id > 0 && m.id <= m_lastCommonRead;
        case SendStatusRole:
            return m.sendStatus;
        default:
            return {};
    }
}

QHash<int, QByteArray> MessageListModel::roleNames() const
{
    return {
        {IdRole,            "messageId"},
        {ActorNameRole,     "actorName"},
        {ActorIdRole,       "actorId"},
        {MessageTextRole,   "messageText"},
        {TimestampRole,     "timestamp"},
        {IsSystemRole,      "isSystem"},
        {MessageTypeRole,   "messageType"},
        {IsGroupedRole,     "isGrouped"},
        {ReplyToTextRole,   "replyToText"},
        {ReplyToAuthorRole, "replyToAuthor"},
        {ReactionsRole,     "reactions"},
        {TimeStringRole,    "timeString"},
        {ShowDateSeparatorRole, "showDateSeparator"},
        {DateStringRole,    "dateString"},
        {IsReadRole,        "isRead"},
        {SendStatusRole,    "sendStatus"},
    };
}

void MessageListModel::setConversationToken(const QString &token)
{
    if (m_token == token)
        return;

    m_poller->stop();
    m_token = token;
    m_oldestMessageId = 0;

    // Load cache inside reset — single atomic visual update
    beginResetModel();
    m_messages.clear();
    if (!token.isEmpty()) {
        // Cache returns oldest-first; we reverse to newest-first
        QVector<Message> cached = m_cache->loadMessages(token, 50);
        m_messages.reserve(cached.size());
        for (int i = cached.size() - 1; i >= 0; --i)
            m_messages.append(cached[i]);
        if (!cached.isEmpty())
            m_oldestMessageId = cached.first().id;  // oldest from cache
    }
    endResetModel();

    emit conversationTokenChanged();

    if (token.isEmpty())
        return;

    QString joinToken = token;
    m_api->post("apps/spreed/api/v4/room/" + token + "/participants/active",
        [this, joinToken](bool ok, const QJsonObject &, int) {
            if (m_token != joinToken) return;
            if (!ok) {
                emit errorOccurred("Failed to join conversation");
                return;
            }
            loadHistory();
        });
}

void MessageListModel::loadHistory()
{
    if (m_token.isEmpty()) return;

    m_loading = true;
    emit loadingChanged();

    QUrlQuery params;
    params.addQueryItem("lookIntoFuture", "0");
    params.addQueryItem("limit", "50");
    if (m_oldestMessageId > 0)
        params.addQueryItem("lastKnownMessageId", QString::number(m_oldestMessageId));

    QString currentToken = m_token;
    auto *reply = m_api->getRaw("apps/spreed/api/v1/chat/" + m_token, params);

    connect(reply, &QNetworkReply::finished, this, [this, reply, currentToken]() {
        reply->deleteLater();

        if (m_token != currentToken) return;

        m_loading = false;
        emit loadingChanged();

        QByteArray lastCommonRead = reply->rawHeader("X-Chat-Last-Common-Read");
        if (!lastCommonRead.isEmpty())
            onLastCommonReadChanged(lastCommonRead.toInt());

        if (reply->error() != QNetworkReply::NoError) {
            startPoller();
            return;
        }

        QByteArray body = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(body);
        QJsonObject ocs = doc.object()["ocs"].toObject();
        QJsonArray data = ocs["data"].toArray();

        if (data.isEmpty()) {
            startPoller();
            return;
        }

        // API returns newest-first — that's our storage order already
        QSet<int> existingIds;
        for (const auto &existing : m_messages)
            existingIds.insert(existing.id);

        // Collect older messages (not already in model), keep newest-first order
        QVector<Message> olderMsgs;
        QVector<Message> forCache;
        for (const auto &val : data) {
            Message m = Message::fromJson(val.toObject());
            if (!existingIds.contains(m.id)) {
                olderMsgs.append(m);  // already newest-first from API
            }
            forCache.append(m);
        }

        // Save all fetched messages to cache
        if (!forCache.isEmpty())
            m_cache->saveMessages(m_token, forCache);

        // Append older messages at the END of m_messages (= top of screen)
        if (!olderMsgs.isEmpty()) {
            int first = m_messages.size();
            beginInsertRows({}, first, first + olderMsgs.size() - 1);
            m_messages.append(olderMsgs);
            endInsertRows();
        }

        // Update oldest tracking
        if (!m_messages.isEmpty())
            m_oldestMessageId = m_messages.last().id;  // last = oldest

        startPoller();
    });
}

void MessageListModel::startPoller()
{
    // Newest message is at index 0
    int lastId = m_messages.isEmpty() ? 0 : m_messages.first().id;
    m_poller->start(m_token, lastId);
}

void MessageListModel::onMessagesReceived(const QJsonArray &messages)
{
    if (messages.isEmpty()) return;

    QSet<int> existingIds;
    for (const auto &existing : m_messages)
        existingIds.insert(existing.id);

    // New messages from poller — prepend at index 0 (= bottom of screen)
    QVector<Message> newMsgs;
    for (const auto &val : messages) {
        Message m = Message::fromJson(val.toObject());
        if (!existingIds.contains(m.id))
            newMsgs.append(m);
    }

    if (newMsgs.isEmpty()) return;

    // Reverse so newest is first (poller returns oldest-first)
    std::reverse(newMsgs.begin(), newMsgs.end());

    beginInsertRows({}, 0, newMsgs.size() - 1);
    // Prepend: insert at beginning
    for (int i = newMsgs.size() - 1; i >= 0; --i)
        m_messages.prepend(newMsgs[i]);
    endInsertRows();

    m_cache->saveMessages(m_token, newMsgs);
}

void MessageListModel::sendMessage(const QString &text, int replyToId)
{
    if (text.trimmed().isEmpty() || m_token.isEmpty())
        return;

    static int tempIdCounter = -1;
    int tempId = tempIdCounter--;

    Message optimistic;
    optimistic.id = tempId;
    optimistic.token = m_token;
    optimistic.actorType = "users";
    optimistic.actorId = m_api->user();
    optimistic.actorDisplayName = "";
    optimistic.message = text;
    optimistic.timestamp = QDateTime::currentSecsSinceEpoch();
    optimistic.messageType = "comment";
    optimistic.sendStatus = "sending";

    // Prepend at index 0 (newest = bottom of screen)
    beginInsertRows({}, 0, 0);
    m_messages.prepend(optimistic);
    endInsertRows();

    emit messageSent();

    QJsonObject body;
    body["message"] = text;
    if (replyToId > 0)
        body["replyTo"] = replyToId;

    QString currentToken = m_token;
    m_api->post("apps/spreed/api/v1/chat/" + currentToken, body,
        [this, tempId, currentToken](bool ok, const QJsonObject &data, int) {
            if (m_token != currentToken) return;

            int idx = -1;
            for (int i = 0; i < m_messages.size(); ++i) {
                if (m_messages[i].id == tempId) { idx = i; break; }
            }
            if (idx < 0) return;

            if (ok && !data.isEmpty()) {
                m_messages[idx] = Message::fromJson(data);
                emit dataChanged(index(idx), index(idx));
                m_cache->saveMessages(m_token, {m_messages[idx]});
            } else {
                m_messages[idx].sendStatus = "failed";
                emit dataChanged(index(idx), index(idx), {SendStatusRole});
            }
        });
}

void MessageListModel::retryMessage(int tempId)
{
    int idx = -1;
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].id == tempId) { idx = i; break; }
    }
    if (idx < 0) return;

    Message &msg = m_messages[idx];
    if (msg.sendStatus != "failed") return;

    msg.sendStatus = "sending";
    emit dataChanged(index(idx), index(idx), {SendStatusRole});

    QString text = msg.message;
    QString currentToken = m_token;
    QJsonObject body;
    body["message"] = text;

    m_api->post("apps/spreed/api/v1/chat/" + currentToken, body,
        [this, tempId, currentToken](bool ok, const QJsonObject &data, int) {
            if (m_token != currentToken) return;

            int idx = -1;
            for (int i = 0; i < m_messages.size(); ++i) {
                if (m_messages[i].id == tempId) { idx = i; break; }
            }
            if (idx < 0) return;

            if (ok && !data.isEmpty()) {
                m_messages[idx] = Message::fromJson(data);
                emit dataChanged(index(idx), index(idx));
                m_cache->saveMessages(m_token, {m_messages[idx]});
            } else {
                m_messages[idx].sendStatus = "failed";
                emit dataChanged(index(idx), index(idx), {SendStatusRole});
            }
        });
}

void MessageListModel::onLastCommonReadChanged(int messageId)
{
    if (messageId <= m_lastCommonRead)
        return;

    m_lastCommonRead = messageId;

    if (!m_messages.isEmpty()) {
        emit dataChanged(index(0), index(m_messages.size() - 1), {IsReadRole});
    }
}
