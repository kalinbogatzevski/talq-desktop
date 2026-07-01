#include "ThreadListModel.h"
#include "models/ConversationListModel.h"
#include <QJsonObject>
#include <QUrlQuery>
#include <QPointer>
#include <QSettings>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <memory>

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
    // 0.40.7 — previously bailed when m_conversationType == 1 (one-to-one),
    // back when topics were group-only. 0.40.5 enabled topic creation in
    // 1:1 chats, so the cache path must also serve them — otherwise a user
    // who reopens a P2P with an existing topic sees nothing in the bar
    // until the API round-trip lands.
    if (token != m_token || threads.isEmpty())
        return;

    // Only use cache if we haven't loaded from API yet
    if (!m_threads.isEmpty())
        return;

    bool hadTopics = m_threads.size() > 1;

    QVector<ThreadInfo> cached;
    for (const auto &t : threads) {
        const int tid = t["threadId"].toInt();
        if (m_hiddenTopics.contains(tid))
            continue;   // user hid this topic (client-side, persisted)
        ThreadInfo info;
        info.threadId = tid;
        info.title = t["title"].toString();
        info.iconColor = t["iconColor"].toInt();
        info.lastActivity = t["lastActivity"].toInteger();
        info.lastMessage = t["lastMessage"].toString();
        info.lastAuthor = t["lastAuthor"].toString();
        info.replyCount = t["replyCount"].toInt();
        info.lastReadMessageId = t["lastReadMessageId"].toInt();
        cached.append(info);
    }

    // Match the live ordering: most recent first (the cache carries no unread
    // counts, so unread-first is moot here — fetchThreads re-sorts with unread
    // once it lands).
    std::sort(cached.begin(), cached.end(), [](const ThreadInfo &a, const ThreadInfo &b) {
        return a.lastActivity > b.lastActivity;
    });

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
    loadHiddenTopics();   // per-room hidden set

    // Blank the list immediately — without this, the previous room's topics
    // stay visible (and also gate out the cache path, which bails when
    // m_threads isn't empty).
    const bool hadTopics = m_threads.size() > 1;
    beginResetModel();
    m_threads.clear();
    endResetModel();
    emit countChanged();
    if (hadTopics) emit hasTopicsChanged();

    if (!m_token.isEmpty()) {
        // Cache load is async and paints instantly when it returns;
        // fetchThreads then overwrites with fresh server data.
        if (m_cache)
            m_cache->loadThreadIndex(m_token);
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

void ThreadListModel::deleteTopic(int threadId)
{
    if (threadId <= 0 || m_token.isEmpty())
        return;

    const QString token = m_token;
    QUrlQuery params;
    params.addQueryItem("limit", "200");
    params.addQueryItem("lookIntoFuture", "0");
    params.addQueryItem("setReadMarker", "0");  // collecting a topic's msgs to delete must not mark the room read (server default is 1)

    QPointer<ThreadListModel> guard(this);
    // No server-side thread delete exists (upstream #17146), so collect the
    // topic's messages — the root (id == threadId) plus any reply whose
    // threadId matches — and delete each individually, best-effort.
    m_api->getArray("apps/spreed/api/v1/chat/" + token, params,
        [this, guard, token, threadId](bool ok, const QJsonArray &data, int) {
        if (!guard || token != m_token || !ok)
            return;

        QVector<int> ids;
        for (const QJsonValue &v : data) {
            const QJsonObject m = v.toObject();
            const int id  = m["id"].toInt();
            const int tId = m["threadId"].toInt();
            if (id > 0 && (id == threadId || tId == threadId))
                ids.append(id);
        }

        if (ids.isEmpty()) {
            emit topicDeleteFinished(threadId, 0, 0);
            refresh();
            return;
        }

        // Shared tally across the async per-message deletes; the last callback
        // to land reports the result and refreshes the bar.
        auto remaining = std::make_shared<int>(ids.size());
        auto failed    = std::make_shared<int>(0);
        const int total = ids.size();

        for (int id : ids) {
            const QString path = "apps/spreed/api/v1/chat/" + token
                                 + "/" + QString::number(id);
            m_api->del(path, [this, guard, threadId, total, remaining, failed]
                              (bool delOk, const QJsonObject &, int) {
                if (!delOk)
                    ++(*failed);
                if (--(*remaining) == 0 && guard) {
                    // Talk only tombstones messages, so a FULLY-deleted topic
                    // would otherwise linger (full of "deleted" markers) — hide
                    // it client-side so it disappears as the user expects. But if
                    // anything could NOT be deleted (others' messages, or past
                    // the edit window), the topic still holds real content, so
                    // do NOT hide it — leave it visible and just report what
                    // remained.
                    if (*failed == 0)
                        hideTopic(threadId);
                    emit topicDeleteFinished(threadId, total - *failed, *failed);
                }
            });
        }
    });
}

void ThreadListModel::hideTopic(int threadId)
{
    if (threadId <= 0) return;
    if (!m_hiddenTopics.contains(threadId)) {
        m_hiddenTopics.insert(threadId);
        saveHiddenTopics();
    }
    if (m_selectedThreadId == threadId)
        m_selectedThreadId = -1;
    // Remove the chip immediately (no server round-trip); the persisted hidden
    // set filters it out of every future rebuild.
    for (int i = 0; i < m_threads.size(); ++i) {
        if (m_threads[i].threadId == threadId) {
            const bool hadTopics = m_threads.size() > 1;
            beginRemoveRows(QModelIndex(), i, i);
            m_threads.removeAt(i);
            endRemoveRows();
            emit countChanged();
            if (hadTopics != (m_threads.size() > 1))
                emit hasTopicsChanged();
            break;
        }
    }
}

void ThreadListModel::unhideAllTopics()
{
    if (m_hiddenTopics.isEmpty()) return;
    m_hiddenTopics.clear();
    saveHiddenTopics();
    refresh();   // re-fetch to bring the previously-hidden topics back
}

void ThreadListModel::loadHiddenTopics()
{
    m_hiddenTopics.clear();
    if (m_token.isEmpty()) return;
    QSettings s(QStringLiteral("TalQ"), QStringLiteral("TalQ"));
    const QStringList ids =
        s.value(QStringLiteral("Topics/hidden/") + m_token).toStringList();
    for (const QString &id : ids) {
        bool ok = false;
        const int v = id.toInt(&ok);
        if (ok && v > 0)
            m_hiddenTopics.insert(v);
    }
}

void ThreadListModel::saveHiddenTopics()
{
    if (m_token.isEmpty()) return;
    QStringList ids;
    for (int id : m_hiddenTopics)
        ids << QString::number(id);
    QSettings s(QStringLiteral("TalQ"), QStringLiteral("TalQ"));
    s.setValue(QStringLiteral("Topics/hidden/") + m_token, ids);
}

void ThreadListModel::fetchThreads()
{
    m_loading = true;
    emit loadingChanged();

    QUrlQuery params;
    params.addQueryItem("limit", "200");
    params.addQueryItem("lookIntoFuture", "0");
    params.addQueryItem("setReadMarker", "0");  // fetching the thread list must not mark the room read (server default is 1)

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

        // Pull the room's current read marker so per-topic unread counts reflect
        // the latest read position at scan time (ConversationListModel keeps it
        // fresh via its poll). Falls back to any value set via setRoomLastReadId.
        if (m_conversations)
            m_roomLastReadId = m_conversations->lastReadMessageForToken(m_token);

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
            int unread = 0;   // messages in this topic with id > room read marker
        };
        QHash<int, ThreadAccumulator> threadMap;

        for (const QJsonValue &val : data) {
            const QJsonObject msg = val.toObject();

            // A message belongs to a thread if EITHER:
            //   (a) it carries isThread:true (the API tags replies inside
            //       a thread this way), OR
            //   (b) it carries a non-empty threadTitle (which is how the
            //       API marks a titled thread ROOT — the seed message
            //       itself does NOT get isThread:true, only its replies do).
            // 0.40.7 — previously we only checked (a), so a freshly-created
            // topic with zero replies never showed up in the bar even after
            // refresh(): the seed message was filtered out here.
            const bool isThreadFlag      = msg["isThread"].toBool();
            const QString threadTitleStr = msg["threadTitle"].toString();
            if (!isThreadFlag && threadTitleStr.isEmpty())
                continue;

            const int threadRootId = msg["threadId"].toInt();
            if (threadRootId == 0)
                continue;

            auto &acc = threadMap[threadRootId];
            acc.threadRootId = threadRootId;

            // Only real comments count toward the reply count and unread badge —
            // exclude deleted tombstones (messageType "comment_deleted") and
            // system events ("system"), so a topic whose messages were all
            // deleted shows 0, not the count of "You deleted a message" markers.
            const bool isRealComment = (msg["messageType"].toString() == QLatin1String("comment"));

            // The root contributes its existence but not a "reply" — count only
            // the replies (isThread:true), and only real comments.
            if (isThreadFlag && isRealComment)
                acc.count++;

            // Per-topic unread: real comments newer than the room read marker.
            // The marker advances as the user reads (or sends), so own/just-read
            // messages stop counting on the next refresh. Drives the "· N" badge.
            if (isRealComment && m_roomLastReadId > 0
                && msg["id"].toInt() > m_roomLastReadId)
                acc.unread++;

            // Use the API-provided thread title
            if (acc.threadTitle.isEmpty() && !threadTitleStr.isEmpty())
                acc.threadTitle = threadTitleStr;

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
            if (m_hiddenTopics.contains(acc.threadRootId))
                continue;   // user hid this topic (client-side, persisted)
            ThreadInfo info;
            info.threadId = acc.threadRootId;
            info.title = acc.threadTitle;

            info.lastMessage = acc.latestMessage;
            info.lastAuthor = acc.latestAuthor;
            info.lastActivity = acc.latestTimestamp;
            info.replyCount = acc.count;
            // 0.52.7 — the topic the user is CURRENTLY viewing is read by
            // definition; never recompute it as unread. Without this, opening a
            // topic clears its count optimistically (markTopicRead) but the next
            // fetchThreads recomputes from the room read marker, which lags a
            // round-trip behind the just-performed read — so the count would flash
            // back on the open topic the instant any message arrived. (0.52.7 made
            // fetchThreads fire on every inbound batch, which would have made that
            // flicker constant.)
            info.unreadCount = (acc.threadRootId == m_selectedThreadId
                                && m_selectedThreadId > 0) ? 0 : acc.unread;

            // Deterministic color from title hash
            uint hash = qHash(info.title);
            info.iconColor = static_cast<int>(hash % 6);

            threads.append(info);
        }

        // Sort: topics with unread first, then by most recent activity. Keeps
        // the topics that need attention at the front of the bar; everything
        // else is ordered by the last message received.
        std::sort(threads.begin(), threads.end(), [](const ThreadInfo &a, const ThreadInfo &b) {
            const bool au = a.unreadCount > 0;
            const bool bu = b.unreadCount > 0;
            if (au != bu) return au;
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
