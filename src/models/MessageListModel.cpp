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

    // Reverse mapping: index 0 = newest (bottom), last index = oldest (top)
    // This works with ListView.BottomToTop for natural chat layout
    int ri = m_messages.size() - 1 - index.row();
    const auto &m = m_messages[ri];

    switch (role) {
        case IdRole:            return m.id;
        case ActorNameRole:     return m.actorDisplayName;
        case ActorIdRole:       return m.actorId;
        case MessageTextRole:   return m.message;
        case TimestampRole:     return m.timestamp;
        case IsSystemRole:      return m.isSystem;
        case MessageTypeRole:   return m.messageType;
        case IsGroupedRole: {
            // In reversed view, the "previous" message is at ri-1 (older, visually above)
            if (ri == 0) return false;
            return m.isGroupedWith(m_messages[ri - 1]);
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
            // Show date separator when this message starts a new day vs the one above it
            if (ri == 0) return true;
            auto prevDate = m_messages[ri - 1].dateTime().date();
            return m.dateTime().date() != prevDate;
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

    // Stop polling old conversation
    m_poller->stop();

    m_token = token;
    m_oldestMessageId = 0;

    // Load cache inside reset — single atomic visual update, no flash
    beginResetModel();
    m_messages.clear();
    if (!token.isEmpty()) {
        m_messages = m_cache->loadMessages(token, 50);
        if (!m_messages.isEmpty())
            m_oldestMessageId = m_messages.first().id;
    }
    endResetModel();

    emit conversationTokenChanged();
    emit newMessagesAtEnd();  // scroll to bottom for the initial load

    if (token.isEmpty())
        return;

    // Join conversation and fetch fresh data in background
    QString joinToken = token;
    m_api->post("apps/spreed/api/v4/room/" + token + "/participants/active",
        [this, joinToken](bool ok, const QJsonObject &, int) {
            if (m_token != joinToken)
                return;
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

        if (m_token != currentToken)
            return;

        m_loading = false;
        emit loadingChanged();

        // Capture read receipt header
        QByteArray lastCommonRead = reply->rawHeader("X-Chat-Last-Common-Read");
        if (!lastCommonRead.isEmpty()) {
            onLastCommonReadChanged(lastCommonRead.toInt());
        }

        if (reply->error() != QNetworkReply::NoError) {
            int lastId = m_messages.isEmpty() ? 0 : m_messages.last().id;
            m_poller->start(m_token, lastId);
            return;
        }

        QByteArray body = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(body);
        QJsonObject root = doc.object();
        QJsonObject ocs = root["ocs"].toObject();
        QJsonArray data = ocs["data"].toArray();

        if (data.isEmpty()) {
            int lastId = m_messages.isEmpty() ? 0 : m_messages.last().id;
            m_poller->start(m_token, lastId);
            return;
        }

        // History messages come newest-first, we need oldest-first
        QVector<Message> newMsgs;
        for (int i = data.size() - 1; i >= 0; --i) {
            newMsgs.append(Message::fromJson(data[i].toObject()));
        }

        // Merge with existing (cached) messages — avoid duplicates
        QSet<int> existingIds;
        for (const auto &existing : m_messages)
            existingIds.insert(existing.id);

        QVector<Message> toInsert;
        for (const auto &msg : newMsgs) {
            if (!existingIds.contains(msg.id))
                toInsert.append(msg);
        }

        if (!toInsert.isEmpty()) {
            // Prepending to m_messages = inserting at the visual end (top of screen)
            int visualEnd = m_messages.size();
            beginInsertRows({}, visualEnd, visualEnd + toInsert.size() - 1);
            for (int i = toInsert.size() - 1; i >= 0; --i)
                m_messages.prepend(toInsert[i]);
            endInsertRows();

            // Save new messages to cache
            m_cache->saveMessages(m_token, toInsert);
        }

        if (!m_messages.isEmpty())
            m_oldestMessageId = m_messages.first().id;

        // Start long-polling for new messages
        int lastId = m_messages.isEmpty() ? 0 : m_messages.last().id;
        m_poller->start(m_token, lastId);
    });
}

void MessageListModel::onMessagesReceived(const QJsonArray &messages)
{
    appendMessages(messages);
}

void MessageListModel::appendMessages(const QJsonArray &arr)
{
    if (arr.isEmpty()) return;

    QSet<int> existingIds;
    for (const auto &existing : m_messages)
        existingIds.insert(existing.id);

    QVector<Message> newMsgs;
    for (const auto &val : arr) {
        Message m = Message::fromJson(val.toObject());
        if (!existingIds.contains(m.id))
            newMsgs.append(m);
    }

    if (newMsgs.isEmpty()) return;

    // Visual index 0 = newest (bottom). Appending to m_messages = inserting at visual index 0.
    beginInsertRows({}, 0, newMsgs.size() - 1);
    m_messages.append(newMsgs);
    endInsertRows();

    // Save new messages to cache
    m_cache->saveMessages(m_token, newMsgs);
}

void MessageListModel::sendMessage(const QString &text, int replyToId)
{
    if (text.trimmed().isEmpty() || m_token.isEmpty())
        return;

    // Create optimistic message — show instantly
    static int tempIdCounter = -1;
    int tempId = tempIdCounter--;

    Message optimistic;
    optimistic.id = tempId;
    optimistic.token = m_token;
    optimistic.actorType = "users";
    optimistic.actorId = m_api->user();
    optimistic.actorDisplayName = "";  // own messages don't show name
    optimistic.message = text;
    optimistic.timestamp = QDateTime::currentSecsSinceEpoch();
    optimistic.messageType = "comment";
    optimistic.sendStatus = "sending";

    // Insert at end (visual bottom)
    beginInsertRows({}, 0, 0);
    m_messages.append(optimistic);
    endInsertRows();

    emit messageSent();

    // Send to server
    QJsonObject body;
    body["message"] = text;
    if (replyToId > 0)
        body["replyTo"] = replyToId;

    QString currentToken = m_token;
    m_api->post("apps/spreed/api/v1/chat/" + currentToken, body,
        [this, tempId, currentToken](bool ok, const QJsonObject &data, int) {
            if (m_token != currentToken) return;

            // Find the optimistic message by tempId
            int idx = -1;
            for (int i = 0; i < m_messages.size(); ++i) {
                if (m_messages[i].id == tempId) { idx = i; break; }
            }
            if (idx < 0) return;

            if (ok && !data.isEmpty()) {
                // Replace optimistic with real server message
                m_messages[idx] = Message::fromJson(data);
                int visualIdx = m_messages.size() - 1 - idx;
                emit dataChanged(index(visualIdx), index(visualIdx));

                // Save to cache
                QVector<Message> toCache;
                toCache.append(m_messages[idx]);
                m_cache->saveMessages(m_token, toCache);
            } else {
                // Mark as failed
                m_messages[idx].sendStatus = "failed";
                int visualIdx = m_messages.size() - 1 - idx;
                emit dataChanged(index(visualIdx), index(visualIdx), {SendStatusRole});
            }
        });
}

void MessageListModel::retryMessage(int tempId)
{
    // Find the failed message
    int idx = -1;
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].id == tempId) { idx = i; break; }
    }
    if (idx < 0) return;

    Message &msg = m_messages[idx];
    if (msg.sendStatus != "failed") return;

    msg.sendStatus = "sending";
    int visualIdx = m_messages.size() - 1 - idx;
    emit dataChanged(index(visualIdx), index(visualIdx), {SendStatusRole});

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
                int visualIdx = m_messages.size() - 1 - idx;
                emit dataChanged(index(visualIdx), index(visualIdx));

                QVector<Message> toCache;
                toCache.append(m_messages[idx]);
                m_cache->saveMessages(m_token, toCache);
            } else {
                m_messages[idx].sendStatus = "failed";
                int visualIdx = m_messages.size() - 1 - idx;
                emit dataChanged(index(visualIdx), index(visualIdx), {SendStatusRole});
            }
        });
}

void MessageListModel::onLastCommonReadChanged(int messageId)
{
    if (messageId <= m_lastCommonRead)
        return;

    m_lastCommonRead = messageId;

    // Notify that isRead may have changed for all visible messages
    if (!m_messages.isEmpty()) {
        emit dataChanged(index(0), index(m_messages.size() - 1), {IsReadRole});
    }
}
