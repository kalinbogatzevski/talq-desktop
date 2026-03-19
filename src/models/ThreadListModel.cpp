#include "ThreadListModel.h"
#include <QJsonObject>
#include <QUrlQuery>
#include <QPointer>
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
    case UnreadCountRole:   return t.unreadCount;
    case IsAllMessagesRole: return t.isAllMessages;
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
        {UnreadCountRole, "unreadCount"},
        {IsAllMessagesRole, "isAllMessages"},
    };
}

void ThreadListModel::setCache(MessageCache *cache)
{
    if (m_cache)
        disconnect(m_cache, &MessageCache::threadIndexLoaded, this, &ThreadListModel::onCachedThreadsLoaded);
    m_cache = cache;
    if (m_cache)
        connect(m_cache, &MessageCache::threadIndexLoaded, this, &ThreadListModel::onCachedThreadsLoaded);
}

void ThreadListModel::onCachedThreadsLoaded(const QString &token, const QVector<QJsonObject> &threads)
{
    if (token != m_token || threads.isEmpty() || m_conversationType == 1)
        return;

    // Only use cache if we haven't loaded from API yet
    if (!m_threads.isEmpty())
        return;

    bool hadTopics = m_threads.size() > 1;

    QVector<ThreadInfo> cached;
    for (const auto &t : threads) {
        ThreadInfo info;
        info.threadId = t["threadId"].toInt();
        info.title = t["title"].toString();
        info.iconColor = t["iconColor"].toInt();
        info.lastActivity = t["lastActivity"].toInteger();
        info.lastMessage = t["lastMessage"].toString();
        info.lastAuthor = t["lastAuthor"].toString();
        info.replyCount = t["replyCount"].toInt();
        info.lastReadMessageId = t["lastReadMessageId"].toInt();
        cached.append(info);
    }

    // Add "All Messages" at index 0
    ThreadInfo allMsg;
    allMsg.threadId = 0;
    allMsg.title = "General";
    allMsg.isAllMessages = true;
    allMsg.iconColor = 0;
    if (!cached.isEmpty()) {
        allMsg.lastActivity = cached.first().lastActivity;
    }
    cached.prepend(allMsg);

    beginResetModel();
    m_threads = std::move(cached);
    endResetModel();
    emit countChanged();

    if (hadTopics != (m_threads.size() > 1))
        emit hasTopicsChanged();
}

void ThreadListModel::setConversationToken(const QString &token)
{
    if (m_token == token)
        return;
    m_token = token;
    emit tokenChanged();

    if (!m_token.isEmpty()) {
        // Load from cache first for instant display
        if (m_cache)
            m_cache->loadThreadIndex(m_token);
        // Then fetch from API for fresh data
        fetchThreads();
    }
}

void ThreadListModel::refresh()
{
    if (!m_token.isEmpty())
        fetchThreads();
}

void ThreadListModel::markTopicRead(int threadId)
{
    for (int i = 0; i < m_threads.size(); ++i) {
        if (m_threads[i].threadId == threadId && m_threads[i].unreadCount > 0) {
            m_threads[i].unreadCount = 0;
            emit dataChanged(index(i), index(i), {UnreadCountRole});
            break;
        }
    }
}

void ThreadListModel::selectTopic(int threadId)
{
    m_selectedThreadId = threadId;
    markTopicRead(threadId);
}

int ThreadListModel::colorForThread(int threadId) const
{
    for (const auto &t : m_threads) {
        if (t.threadId == threadId)
            return t.iconColor;
    }
    return 0;
}

void ThreadListModel::fetchThreads()
{
    m_loading = true;
    emit loadingChanged();

    QUrlQuery params;
    params.addQueryItem("limit", "200");
    params.addQueryItem("lookIntoFuture", "0");

    const QString path = "apps/spreed/api/v1/chat/" + m_token;

    const QString capturedToken = m_token;
    QPointer<ThreadListModel> guard(this);
    m_api->getArray(path, params, [this, guard, capturedToken](bool success, const QJsonArray &data, int /*statusCode*/) {
        if (!guard || capturedToken != m_token) return;  // destroyed or stale
        if (!success) {
            m_loading = false;
            emit loadingChanged();
            return;
        }

        // Use the API-provided thread fields:
        //   isThread: true    — message belongs to a thread
        //   threadId: N       — the thread root message ID
        //   threadTitle: "X"  — the thread name
        // Non-thread messages have threadId == their own id and no isThread field.
        struct ThreadAccumulator {
            int threadRootId = 0;
            QString threadTitle;
            qint64 latestTimestamp = 0;
            QString latestMessage;
            QString latestAuthor;
            int count = 0;
        };
        QHash<int, ThreadAccumulator> threadMap;

        for (const QJsonValue &val : data) {
            const QJsonObject msg = val.toObject();

            // Only process messages explicitly marked as thread messages
            if (!msg["isThread"].toBool())
                continue;

            const int threadRootId = msg["threadId"].toInt();
            if (threadRootId == 0)
                continue;

            auto &acc = threadMap[threadRootId];
            acc.threadRootId = threadRootId;
            acc.count++;

            // Use the API-provided thread title
            const QString title = msg["threadTitle"].toString();
            if (acc.threadTitle.isEmpty() && !title.isEmpty())
                acc.threadTitle = title;

            // Track the most recent message in this thread
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
            info.threadId = acc.threadRootId;
            info.title = acc.threadTitle;

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

        // Insert "All Messages" at index 0
        ThreadInfo allMsg;
        allMsg.threadId = 0;
        allMsg.title = "General";
        allMsg.isAllMessages = true;
        allMsg.iconColor = 0;  // teal
        if (!threads.isEmpty()) {
            allMsg.lastActivity = threads.first().lastActivity;
            allMsg.lastMessage = threads.first().lastMessage;
            allMsg.lastAuthor = threads.first().lastAuthor;
        }
        threads.prepend(allMsg);

        bool hadTopics = m_threads.size() > 1;  // BEFORE update

        beginResetModel();
        m_threads = std::move(threads);
        endResetModel();

        m_loading = false;
        emit loadingChanged();
        emit countChanged();

        // Persist to SQLite (skip "All Messages" at index 0)
        if (m_cache && m_threads.size() > 1) {
            QVector<QJsonObject> toSave;
            for (int i = 1; i < m_threads.size(); ++i) {
                QJsonObject t;
                t["threadId"] = m_threads[i].threadId;
                t["title"] = m_threads[i].title;
                t["iconColor"] = m_threads[i].iconColor;
                t["lastActivity"] = m_threads[i].lastActivity;
                t["lastMessage"] = m_threads[i].lastMessage;
                t["lastAuthor"] = m_threads[i].lastAuthor;
                t["replyCount"] = m_threads[i].replyCount;
                t["lastReadMessageId"] = m_threads[i].lastReadMessageId;
                toSave.append(t);
            }
            m_cache->saveThreadIndex(m_token, toSave);
        }

        if (hadTopics != (m_threads.size() > 1))
            emit hasTopicsChanged();
    });
}
