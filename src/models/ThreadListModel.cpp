#include "ThreadListModel.h"
#include <QJsonObject>
#include <QUrlQuery>
#include <algorithm>

ThreadListModel::ThreadListModel(ApiClient *api, QObject *parent)
    : QAbstractListModel(parent)
    , m_api(api)
{
}

int ThreadListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_threads.size();
}

QVariant ThreadListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_threads.size())
        return {};

    const auto &t = m_threads.at(index.row());
    switch (role) {
    case ThreadIdRole:    return t.threadId;
    case TitleRole:       return t.title;
    case LastMessageRole: return t.lastMessage;
    case LastAuthorRole:  return t.lastAuthor;
    case LastActivityRole:return t.lastActivity;
    case ReplyCountRole:  return t.replyCount;
    case IconColorRole:   return t.iconColor;
    }
    return {};
}

QHash<int, QByteArray> ThreadListModel::roleNames() const
{
    return {
        {ThreadIdRole, "threadId"},
        {TitleRole, "title"},
        {LastMessageRole, "lastMessage"},
        {LastAuthorRole, "lastAuthor"},
        {LastActivityRole, "lastActivity"},
        {ReplyCountRole, "replyCount"},
        {IconColorRole, "iconColor"},
    };
}

void ThreadListModel::setConversationToken(const QString &token)
{
    if (m_token == token)
        return;
    m_token = token;
    emit tokenChanged();

    if (!m_token.isEmpty())
        fetchThreads();
}

void ThreadListModel::refresh()
{
    if (!m_token.isEmpty())
        fetchThreads();
}

void ThreadListModel::fetchThreads()
{
    m_loading = true;
    emit loadingChanged();

    QUrlQuery params;
    params.addQueryItem("limit", "200");
    params.addQueryItem("lookIntoFuture", "0");

    const QString path = "apps/spreed/api/v1/chat/" + m_token;

    m_api->getArray(path, params, [this](bool success, const QJsonArray &data, int /*statusCode*/) {
        if (!success) {
            m_loading = false;
            emit loadingChanged();
            return;
        }

        // Group messages by parent.id to find threads
        // Key: parent message id -> collected info
        struct ThreadAccumulator {
            int parentId = 0;
            QString parentMessage;
            qint64 latestTimestamp = 0;
            QString latestMessage;
            QString latestAuthor;
            int count = 0;
        };
        QHash<int, ThreadAccumulator> threadMap;

        for (const QJsonValue &val : data) {
            const QJsonObject msg = val.toObject();
            const QJsonObject parent = msg["parent"].toObject();

            if (parent.isEmpty())
                continue;

            const int parentId = parent["id"].toInt();
            if (parentId == 0)
                continue;

            auto &acc = threadMap[parentId];
            acc.parentId = parentId;
            acc.count++;

            // Store the parent's message text as thread title
            const QString parentText = parent["message"].toString();
            if (acc.parentMessage.isEmpty() && !parentText.isEmpty())
                acc.parentMessage = parentText;

            // Track the most recent reply
            const qint64 ts = msg["timestamp"].toVariant().toLongLong();
            if (ts > acc.latestTimestamp) {
                acc.latestTimestamp = ts;
                acc.latestMessage = msg["message"].toString();
                acc.latestAuthor = msg["actorDisplayName"].toString();
            }
        }

        // Build ThreadInfo list
        QVector<ThreadInfo> threads;
        threads.reserve(threadMap.size());

        for (auto it = threadMap.cbegin(); it != threadMap.cend(); ++it) {
            const auto &acc = it.value();
            ThreadInfo info;
            info.threadId = acc.parentId;

            // Title: first 50 chars of the parent message
            QString title = acc.parentMessage;
            if (title.length() > 50)
                title = title.left(50) + QStringLiteral("...");
            info.title = title;

            info.lastMessage = acc.latestMessage;
            info.lastAuthor = acc.latestAuthor;
            info.lastActivity = acc.latestTimestamp;
            info.replyCount = acc.count;

            // Deterministic color from title hash
            uint hash = qHash(info.title);
            info.iconColor = static_cast<int>(hash % 6);

            threads.append(info);
        }

        // Sort by most recent activity first
        std::sort(threads.begin(), threads.end(), [](const ThreadInfo &a, const ThreadInfo &b) {
            return a.lastActivity > b.lastActivity;
        });

        beginResetModel();
        m_threads = std::move(threads);
        endResetModel();

        m_loading = false;
        emit loadingChanged();
        emit countChanged();
    });
}
