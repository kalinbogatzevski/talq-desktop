#include "models/MessageListModel.h"
#include <QUrlQuery>
#include <QJsonObject>
#include <QDateTime>
#include <QTimer>

MessageListModel::MessageListModel(ApiClient *api, QObject *parent)
    : QAbstractListModel(parent)
    , m_api(api)
    , m_poller(new MessagePoller(api, this))
{
    connect(m_poller, &MessagePoller::messagesReceived,
            this, &MessageListModel::onMessagesReceived);
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
            if (index.row() == 0) return false;
            return m.isGroupedWith(m_messages[index.row() - 1]);
        }
        case ReplyToTextRole:
            return m.replyTo.isEmpty() ? QString() : m.replyTo["message"].toString();
        case ReplyToAuthorRole:
            return m.replyTo.isEmpty() ? QString() : m.replyTo["actorDisplayName"].toString();
        case ReactionsRole: {
            // Convert { "👍": 3 } to displayable string
            QStringList parts;
            for (auto it = m.reactions.begin(); it != m.reactions.end(); ++it) {
                parts << QString("%1 %2").arg(it.key()).arg(it.value().toInt());
            }
            return parts.join("  ");
        }
        case TimeStringRole:
            return m.dateTime().toString("HH:mm");
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
    };
}

void MessageListModel::setConversationToken(const QString &token)
{
    if (m_token == token)
        return;

    // Stop polling old conversation
    m_poller->stop();

    // Clear messages
    beginResetModel();
    m_messages.clear();
    endResetModel();

    m_token = token;
    m_oldestMessageId = 0;
    emit conversationTokenChanged();

    if (token.isEmpty())
        return;

    // Join conversation — capture token to guard against stale callbacks
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
    m_api->getArray("apps/spreed/api/v1/chat/" + m_token, params,
        [this, currentToken](bool ok, const QJsonArray &data, int) {
            if (m_token != currentToken)
                return; // conversation changed while loading
            m_loading = false;
            emit loadingChanged();

            if (!ok || data.isEmpty()) {
                // Start polling from wherever we are
                int lastId = m_messages.isEmpty() ? 0 : m_messages.last().id;
                m_poller->start(m_token, lastId);
                return;
            }

            // History messages come newest-first, we need oldest-first
            QJsonArray reversed;
            for (int i = data.size() - 1; i >= 0; --i)
                reversed.append(data[i]);

            int insertPos = 0; // prepend history at top
            QVector<Message> newMsgs;
            for (const auto &val : reversed) {
                newMsgs.append(Message::fromJson(val.toObject()));
            }

            if (!newMsgs.isEmpty()) {
                beginInsertRows({}, 0, newMsgs.size() - 1);
                for (int i = newMsgs.size() - 1; i >= 0; --i)
                    m_messages.prepend(newMsgs[i]);
                endInsertRows();

                m_oldestMessageId = m_messages.first().id;
            }

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

    QVector<Message> newMsgs;
    for (const auto &val : arr) {
        Message m = Message::fromJson(val.toObject());
        // Avoid duplicates
        bool exists = false;
        for (const auto &existing : m_messages) {
            if (existing.id == m.id) { exists = true; break; }
        }
        if (!exists)
            newMsgs.append(m);
    }

    if (newMsgs.isEmpty()) return;

    int first = m_messages.size();
    beginInsertRows({}, first, first + newMsgs.size() - 1);
    m_messages.append(newMsgs);
    endInsertRows();
}

void MessageListModel::sendMessage(const QString &text, int replyToId)
{
    if (text.trimmed().isEmpty() || m_token.isEmpty())
        return;

    QJsonObject body;
    body["message"] = text;
    if (replyToId > 0)
        body["replyTo"] = replyToId;

    m_api->post("apps/spreed/api/v1/chat/" + m_token, body,
        [this](bool ok, const QJsonObject &data, int) {
            if (ok) {
                emit messageSent();
            } else {
                emit errorOccurred("Failed to send message");
            }
        });
}
