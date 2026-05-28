#include "models/MessageListModel.h"
#include "core/MessageCache.h"
#include "models/ConversationListModel.h"
#include <QSet>
#include <QUrlQuery>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QTimer>
#include <QNetworkReply>
#include <QGuiApplication>
#include <QClipboard>
#include <QMimeData>
#include <QImage>
#include <QDir>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QMimeDatabase>

// ============================================================================
// STORAGE: m_messages is stored NEWEST-FIRST (reverse chronological order).
//   m_messages[0] = newest message
//   m_messages[last] = oldest message
//
// ListView uses BottomToTop — index 0 at the bottom (newest visible first).
// No positionViewAtEnd needed — the view naturally starts at the bottom.
// New messages from poller are prepended at index 0 (appear at bottom).
// History loads append at the end (appear at top on scroll-up).
// ============================================================================

MessageListModel::MessageListModel(ApiClient *api, MessageCache *cache, QObject *parent)
    : QAbstractListModel(parent)
    , m_api(api)
    , m_cache(cache)
    , m_poller(new MessagePoller(api, this))
{
    // 5s pull while a chat is open. This is the only way to learn that the
    // other party read our messages on servers whose HPB doesn't broadcast
    // read-marker events (only new-message events). The cost is one tiny
    // HTTP call every 5s carrying just the latest message + headers.
    m_readMarkerTimer.setInterval(5000);
    connect(&m_readMarkerTimer, &QTimer::timeout, this, &MessageListModel::refreshReadMarker);

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

    // Async cache results
    connect(m_cache, &MessageCache::lastCommonReadLoaded, this, [this](const QString &token, int messageId) {
        if (m_token != token) return;  // stale result
        if (messageId > m_lastCommonRead) {
            m_lastCommonRead = messageId;
            // Keep poller's request-side hint in sync — important if cache
            // load races behind startPoller(), so the very next request
            // already carries the right header.
            m_poller->setLastKnownCommonRead(messageId);
            // Refresh read indicators for all messages
            if (!m_messages.isEmpty())
                emit dataChanged(index(0), index(m_messages.size() - 1), {IsReadRole});
        }
    });

    connect(m_cache, &MessageCache::messagesLoaded, this, [this](const QString &token, const QVector<Message> &messages) {
        if (m_token != token) return;  // stale result (different conversation)

        // Display cached messages instantly (even if empty — still trigger API fetch)
        // Cache returns oldest-first; reverse to newest-first for BottomToTop display.
        if (!messages.isEmpty() && m_messages.isEmpty()) {
            QVector<Message> filtered;
            for (const auto &m : messages) {
                if (m.isReactionMessage() || m.isCallJoinLeave()
                    || m.isEditMessage())
                    continue;
                if (m_hideThreadMessages && m.threadId > 0)
                    continue;
                filtered.append(m);
            }
            if (!filtered.isEmpty()) {
                // Reverse to newest-first
                std::reverse(filtered.begin(), filtered.end());
                beginInsertRows({}, 0, filtered.size() - 1);
                m_messages = filtered;
                for (const auto &m : filtered)
                    m_messageIds.insert(m.id);
                endInsertRows();
                m_oldestMessageId = m_messages.last().id;  // oldest is now at the end
                // Don't emit newMessagesAtEnd — BottomToTop naturally positions at bottom
            }
        }

        qDebug() << "Cache loaded" << m_messages.size() << "messages for" << m_token;
        refreshLatest();
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
            // Newest-first: chronologically previous message is at index+1
            int next = index.row() + 1;
            if (next >= m_messages.size()) return false;
            return m.isGroupedWith(m_messages[next]);
        }
        case ReplyToTextRole:
            return m.replyTo.isEmpty() ? QString() : m.replyTo["message"].toString();
        case ReplyToAuthorRole:
            return m.replyTo.isEmpty() ? QString() : m.replyTo["actorDisplayName"].toString();
        case ReactionsRole: {
            QStringList parts;
            for (auto it = m.reactions.begin(); it != m.reactions.end(); ++it) {
                int count = it.value().isArray() ? it.value().toArray().size() : it.value().toInt();
                parts << QString("%1 %2").arg(it.key()).arg(count);
            }
            return parts.join("  ");
        }
        case TimeStringRole:
            return m.dateTime().toString("HH:mm");
        case ShowDateSeparatorRole: {
            // Newest-first: show separator when date differs from the next (older) message
            int next = index.row() + 1;
            if (next >= m_messages.size()) return true;  // oldest message always shows date
            auto nextDate = m_messages[next].dateTime().date();
            return m.dateTime().date() != nextDate;
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
        case ThreadIdRole:
            return m.threadId;
        case FileNameRole:
            return m.fileName;
        case FileMimeRole:
            return m.fileMimetype;
        case FileSizeRole:
            return m.fileSize;
        case FileLinkRole:
            return m.fileLink;
        case FilePreviewRole:
            return m.filePreviewUrl;
        case HasFileRole:
            return m.hasFile();
        case FileIdRole:
            return m.fileId;
        case LastEditTimestampRole:
            return m.lastEditTimestamp;
        case SilentRole:
            return m.silent;
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
        {ThreadIdRole,      "msgThreadId"},
        {FileNameRole,      "fileName"},
        {FileMimeRole,      "fileMime"},
        {FileSizeRole,      "fileSize"},
        {FileLinkRole,      "fileLink"},
        {FilePreviewRole,   "filePreview"},
        {HasFileRole,       "hasFile"},
        {FileIdRole,            "fileId"},
        {LastEditTimestampRole, "lastEditTimestamp"},
        {SilentRole,            "silent"},
    };
}

void MessageListModel::setConversationToken(const QString &token)
{
    if (m_token == token)
        return;

    int newBoundary = (m_conversations && !token.isEmpty())
        ? m_conversations->lastReadMessageForToken(token)
        : 0;
    if (newBoundary != m_unreadBoundary) {
        m_unreadBoundary = newBoundary;
        emit unreadBoundaryChanged();
    }

    m_poller->stop();
    m_readMarkerTimer.stop();

    // Cancel any in-flight requests (disconnect first to prevent re-entry)
    if (m_historyReply) {
        auto *oldReply = m_historyReply;
        m_historyReply = nullptr;
        oldReply->disconnect(this);
        oldReply->abort();
        oldReply->deleteLater();
    }
    if (m_refreshReply) {
        auto *oldReply = m_refreshReply;
        m_refreshReply = nullptr;
        oldReply->disconnect(this);
        oldReply->abort();
        oldReply->deleteLater();
    }
    if (m_readMarkerReply) {
        auto *oldReply = m_readMarkerReply;
        m_readMarkerReply = nullptr;
        oldReply->disconnect(this);
        oldReply->abort();
        oldReply->deleteLater();
    }

    m_token = token;
    m_generation++;  // invalidate all in-flight async callbacks
    m_oldestMessageId = 0;
    m_threadId = 0;

    // #26 — figure out whether to enable bot auto-mention for this
    // room. Conditions: room type == 1 (one_to_one), AND exactly one
    // bot enabled (a state=1 entry in /apps/spreed/api/v1/bot/<token>).
    // Default empty (no auto-prepend) until the API answers.
    m_autoMentionBot.clear();
    if (m_conversations && !token.isEmpty()
        && m_conversations->conversationTypeForToken(token) == 1) {
        const int gen = m_generation;
        m_api->getArray("apps/spreed/api/v1/bot/" + token,
            [this, token, gen](bool ok, const QJsonArray &data, int) {
            // Generation guard so a stale reply for a previous token
            // doesn't pollute the current m_autoMentionBot.
            if (!ok || gen != m_generation || token != m_token) return;
            QString chosen;
            int enabledCount = 0;
            for (const QJsonValue &v : data) {
                const QJsonObject b = v.toObject();
                // The bot list response shape (NC Talk Bot API v1):
                // { "id": <int>, "state": 0|1|2, "name": "Aelita",
                //   "description": "...", "features": ["webhook",...] }
                // state == 1 means enabled in this conversation.
                if (b.value(QStringLiteral("state")).toInt() != 1) continue;
                ++enabledCount;
                // Slug: prefer the lowercased display name; fall back
                // to the numeric id ("@bot-7") if name is empty.
                const QString name = b.value(QStringLiteral("name"))
                                          .toString().trimmed().toLower();
                chosen = !name.isEmpty() ? name
                       : QStringLiteral("bot-%1").arg(
                             b.value(QStringLiteral("id")).toInt());
            }
            if (enabledCount == 1) {
                m_autoMentionBot = chosen;
                qDebug() << "MessageListModel: auto-mention bot set to @"
                         << m_autoMentionBot << "for 1:1 room";
            }
        });
    }

    m_lastCommonRead = 0;  // async load below; will update via lastCommonReadLoaded signal
    if (m_cache) m_cache->loadLastCommonRead(token);
    m_loading = false;
    m_hasMoreHistory = true;
    emit hasMoreHistoryChanged();

    // Clear messages
    if (!m_messages.isEmpty()) {
        beginRemoveRows({}, 0, m_messages.size() - 1);
        m_messages.clear();
        m_messageIds.clear();
        endRemoveRows();
    }

    emit conversationTokenChanged();

    if (token.isEmpty())
        return;

    // Load last 20 from local cache for instant display,
    // then fetch fresh from API in background (triggered after cache loads)
    m_cache->loadMessages(token, 50);

    // Mark as read
    QJsonObject body;
    m_api->post("apps/spreed/api/v1/chat/" + token + "/read", body,
        [](bool, const QJsonObject &, int) {});

    // Begin the periodic read-marker pull while this chat is open.
    m_readMarkerTimer.start();
}

void MessageListModel::setThreadId(int id)
{
    if (m_threadId == id)
        return;

    m_poller->stop();

    beginResetModel();
    m_messages.clear();
    m_messageIds.clear();
    endResetModel();

    m_threadId = id;
    m_oldestMessageId = 0;
    emit threadIdChanged();

    m_poller->setThreadId(id);

    if (!m_token.isEmpty())
        loadHistory();
}

void MessageListModel::setHideThreadMessages(bool hide)
{
    if (m_hideThreadMessages == hide)
        return;
    m_hideThreadMessages = hide;
    emit hideThreadMessagesChanged();

    // Reload to apply the filter
    if (!m_token.isEmpty()) {
        m_poller->stop();
        beginResetModel();
        m_messages.clear();
        m_messageIds.clear();
        endResetModel();
        m_oldestMessageId = 0;
        loadHistory();
    }
}

void MessageListModel::loadHistory()
{
    if (m_token.isEmpty() || m_loading) return;
    if (!m_hasMoreHistory && m_oldestMessageId > 0) return;  // no more pages

    m_loading = true;
    emit loadingChanged();

    QUrlQuery params;
    params.addQueryItem("lookIntoFuture", "0");
    params.addQueryItem("limit", "50");
    if (m_oldestMessageId > 0)
        params.addQueryItem("lastKnownMessageId", QString::number(m_oldestMessageId));

    if (m_threadId > 0)
        params.addQueryItem("threadId", QString::number(m_threadId));

    QString currentToken = m_token;
    auto *reply = m_api->getRaw("apps/spreed/api/v1/chat/" + m_token, params);
    m_historyReply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply, currentToken]() {
        if (m_historyReply == reply) m_historyReply = nullptr;
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
            m_hasMoreHistory = false;
            emit hasMoreHistoryChanged();
            startPoller();
            return;
        }

        // Less than requested = no more pages
        if (data.size() < 50) {
            m_hasMoreHistory = false;
            emit hasMoreHistoryChanged();
        }

        // API returns newest-first; keep as-is (our storage is newest-first)
        QVector<Message> olderMsgs;
        for (const auto &val : data) {
            Message m = Message::fromJson(val.toObject());
            if (m_messageIds.contains(m.id) || m.isReactionMessage()
                || m.isCallJoinLeave() || m.isEditMessage())
                continue;
            if (m_hideThreadMessages && m.threadId > 0)
                continue;
            olderMsgs.append(m);
        }

        if (!olderMsgs.isEmpty()) {
            m_cache->saveMessages(m_token, olderMsgs);

            for (const auto &m : olderMsgs)
                m_messageIds.insert(m.id);

            // Append older messages at the END (newest-first: old = end)
            int first = m_messages.size();
            beginInsertRows({}, first, first + olderMsgs.size() - 1);
            m_messages.append(olderMsgs);
            endInsertRows();
        }

        if (!m_messages.isEmpty())
            m_oldestMessageId = m_messages.last().id;  // oldest is at the end

        // Don't emit newMessagesAtEnd() — these are OLDER messages appended
        // at the end (top of BottomToTop view). No scroll needed.
        // would jump the user away from what they were reading.

        if (m_historyUntilTargetId > 0) {
            bool found = false;
            for (const auto &m : m_messages) {
                if (m.id == m_historyUntilTargetId) { found = true; break; }
            }
            if (found || --m_historyUntilRemainingPages <= 0) {
                int id = m_historyUntilTargetId;
                m_historyUntilTargetId = 0;
                m_historyUntilRemainingPages = 0;
                emit historyUntilSettled(id, found);
                if (!found)
                    emit errorOccurred(QStringLiteral("Message not found in recent history"));
            } else {
                loadHistory();
                return;
            }
        }

        startPoller();
    });
}

void MessageListModel::loadHistoryUntil(int messageId)
{
    for (const auto &m : m_messages) {
        if (m.id == messageId) {
            emit historyUntilSettled(messageId, true);
            return;
        }
    }
    static constexpr int kMaxPages = 5;
    m_historyUntilTargetId = messageId;
    m_historyUntilRemainingPages = kMaxPages;
    loadHistory();
}

void MessageListModel::startPoller()
{
    int lastId = m_messages.isEmpty() ? 0 : m_messages.first().id;  // newest is at index 0
    if (lastId <= 0) {
        qDebug() << "Poller: NOT starting — no messages loaded yet for" << m_token;
        return;  // never poll with lastKnown=0, it downloads entire history
    }
    m_poller->setThreadId(m_threadId);
    m_poller->setLastKnownCommonRead(m_lastCommonRead);
    m_poller->start(m_token, lastId);
}

void MessageListModel::trimOldMessages()
{
    static constexpr int MAX_MESSAGES = 200;
    if (m_messages.size() <= MAX_MESSAGES) return;

    int trimCount = m_messages.size() - MAX_MESSAGES;
    int first = m_messages.size() - trimCount;
    for (int i = first; i < m_messages.size(); ++i)
        m_messageIds.remove(m_messages[i].id);
    beginRemoveRows({}, first, m_messages.size() - 1);
    m_messages.remove(first, trimCount);
    endRemoveRows();

    if (!m_messages.isEmpty())
        m_oldestMessageId = m_messages.last().id;
    m_hasMoreHistory = true;  // can re-fetch trimmed messages on scroll-up
    emit hasMoreHistoryChanged();
}

void MessageListModel::refresh()
{
    refreshLatest();
}

void MessageListModel::refreshReadMarker()
{
    if (m_token.isEmpty()) return;
    if (m_readMarkerReply) return;  // a probe is already in flight

    // Cheapest possible chat request: 1 message back, no read-marker side
    // effects. We only care about the X-Chat-Last-Common-Read header.
    QUrlQuery params;
    params.addQueryItem("lookIntoFuture", "0");
    params.addQueryItem("limit", "1");
    params.addQueryItem("setReadMarker", "0");

    QString currentToken = m_token;
    int capturedGen = m_generation;
    auto *reply = m_api->getRaw("apps/spreed/api/v1/chat/" + m_token, params);
    m_readMarkerReply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply, currentToken, capturedGen]() {
        if (m_readMarkerReply == reply) m_readMarkerReply = nullptr;
        reply->deleteLater();
        if (m_generation != capturedGen) return;  // conversation switched
        if (m_token != currentToken) return;
        if (reply->error() != QNetworkReply::NoError) return;

        QByteArray lastCommonRead = reply->rawHeader("X-Chat-Last-Common-Read");
        if (!lastCommonRead.isEmpty())
            onLastCommonReadChanged(lastCommonRead.toInt());
    });
}

void MessageListModel::refreshLatest()
{
    if (m_token.isEmpty()) return;

    // Fetch the latest 50 messages from the server (lookIntoFuture=0, no lastKnownMessageId)
    // This gets the absolute newest messages, regardless of what the cache had.
    QUrlQuery params;
    params.addQueryItem("lookIntoFuture", "0");
    params.addQueryItem("limit", "50");

    if (m_threadId > 0)
        params.addQueryItem("threadId", QString::number(m_threadId));

    QString currentToken = m_token;
    int capturedGen = m_generation;
    auto *reply = m_api->getRaw("apps/spreed/api/v1/chat/" + m_token, params);
    m_refreshReply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply, currentToken, capturedGen]() {
        if (m_refreshReply == reply) m_refreshReply = nullptr;
        reply->deleteLater();
        if (m_generation != capturedGen) return;  // conversation switched — discard
        if (m_token != currentToken) return;

        // Read receipt header — must be read before error check
        QByteArray lastCommonRead = reply->rawHeader("X-Chat-Last-Common-Read");
        if (!lastCommonRead.isEmpty())
            onLastCommonReadChanged(lastCommonRead.toInt());

        if (reply->error() != QNetworkReply::NoError) {
            startPoller();
            return;
        }

        QByteArray body = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(body);
        QJsonArray data = doc.object()["ocs"].toObject()["data"].toArray();
        if (data.isEmpty()) {
            startPoller();
            return;
        }

        // 0.41.2-beta — capture the pre-refresh newest cached ID so we
        // can detect the "multi-device gap": cache has IDs 1-100, the
        // server's latest 50 are 251-300 because another device sent
        // 200 messages while this one was offline. Without this we'd
        // silently leave a gap from 101-250 unreachable (scroll-up's
        // lastKnownMessageId points to the cache's oldest = 1, so
        // pagination returns empty and the gap is invisible).
        int newestPreRefreshId = 0;
        for (int id : m_messageIds)
            newestPreRefreshId = qMax(newestPreRefreshId, id);

        // Build index for existing messages (for edit detection)
        QHash<int, int> idToIndex;
        for (int i = 0; i < m_messages.size(); i++)
            idToIndex[m_messages[i].id] = i;

        // API returns newest-first; process to find missing and edited messages
        QVector<Message> missing;
        for (const auto &val : data) {
            Message m = Message::fromJson(val.toObject());
            if (m.isReactionMessage() || m.isCallJoinLeave()
                || m.isEditMessage())
                continue;
            if (m_hideThreadMessages && m.threadId > 0) continue;

            if (!m_messageIds.contains(m.id)) {
                missing.append(m);
            } else {
                // Existing message — check if edited (message text changed)
                int idx = idToIndex.value(m.id, -1);
                if (idx >= 0 && m_messages[idx].message != m.message) {
                    m_messages[idx] = m;
                    QModelIndex mi = index(idx);
                    emit dataChanged(mi, mi);
                }
            }
        }

        if (!missing.isEmpty()) {
            // Simple approach: merge missing messages into existing list,
            // sort newest-first, and reset the model. This handles all cases:
            // gaps, interleaved messages, edits, etc.
            for (const auto &m : missing) {
                m_messageIds.insert(m.id);
                m_messages.append(m);
            }

            // Remove duplicates (by ID) and sort newest-first
            QHash<int, int> seen;
            QVector<Message> deduped;
            deduped.reserve(m_messages.size());
            for (const auto &m : m_messages) {
                if (!seen.contains(m.id)) {
                    seen[m.id] = 1;
                    deduped.append(m);
                }
            }
            std::sort(deduped.begin(), deduped.end(), [](const Message &a, const Message &b) {
                return a.id > b.id;
            });

            beginResetModel();
            m_messages = std::move(deduped);
            endResetModel();

            if (!m_messages.isEmpty())
                m_oldestMessageId = m_messages.last().id;

            // Save updated cache
            m_cache->saveMessages(m_token, m_messages);

            qDebug() << "MessageListModel: refreshLatest merged" << missing.size()
                     << "missing, total=" << m_messages.size();

            // 0.41.2-beta — diagnostic. Count how many fetched messages
            // had a threadId vs were thread-less. Helps tell apart the
            // two field-bug shapes: (a) server has the messages but
            // tagged with a different/no threadId, or (b) server has
            // nothing at all under this token.
            int withThread = 0, withoutThread = 0;
            for (const auto &m : missing)
                (m.threadId > 0 ? withThread : withoutThread) += 1;
            qInfo().nospace() << "MessageListModel: refreshLatest thread-stats "
                              << "token=" << m_token
                              << " filter=" << m_threadId
                              << " missing-with-thread=" << withThread
                              << " missing-without-thread=" << withoutThread;

            // 0.41.2-beta — multi-device gap detection. If the oldest
            // missing message ID is more than one above the newest
            // pre-refresh cached ID, there is a HOLE between them that
            // no scroll-up will ever reach. Kick off the gap-fill loop:
            // page lookIntoFuture=0 from oldestFetched backward until
            // we cross newestPreRefreshId (or hit the page budget).
            int oldestFetchedId = std::numeric_limits<int>::max();
            for (const auto &m : missing)
                oldestFetchedId = qMin(oldestFetchedId, m.id);
            if (newestPreRefreshId > 0
                && oldestFetchedId != std::numeric_limits<int>::max()
                && oldestFetchedId > newestPreRefreshId + 1) {
                static constexpr int kMaxGapFillPages = 20;   // 20 × 100 = up to 2000 missed messages
                m_gapFillCursor         = oldestFetchedId;    // pages older than this
                m_gapFillTargetId       = newestPreRefreshId;
                m_gapFillPagesRemaining = kMaxGapFillPages;
                qInfo().nospace() << "MessageListModel: gap-fill triggered for "
                                  << m_token
                                  << " thread=" << m_threadId
                                  << " — bridging " << newestPreRefreshId
                                  << "+1 → " << oldestFetchedId << "-1";
                runGapFillStep();
            }
        }

        // Restart poller from the true latest message
        m_poller->stop();
        startPoller();
    });
}

void MessageListModel::runGapFillStep()
{
    if (m_token.isEmpty()
        || m_gapFillCursor <= 0
        || m_gapFillPagesRemaining <= 0) {
        m_gapFillCursor         = 0;
        m_gapFillTargetId       = 0;
        m_gapFillPagesRemaining = 0;
        return;
    }
    QUrlQuery params;
    params.addQueryItem("lookIntoFuture",     "0");
    params.addQueryItem("limit",              "100");
    params.addQueryItem("lastKnownMessageId", QString::number(m_gapFillCursor));
    if (m_threadId > 0)
        params.addQueryItem("threadId", QString::number(m_threadId));

    const QString  token    = m_token;
    const int      gen      = m_generation;
    const int      targetId = m_gapFillTargetId;
    QNetworkReply *reply    = m_api->getRaw("apps/spreed/api/v1/chat/" + m_token, params);
    m_gapFillReply = reply;

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, token, gen, targetId]() {
        if (m_gapFillReply == reply) m_gapFillReply = nullptr;
        reply->deleteLater();
        if (m_generation != gen || m_token != token) {
            m_gapFillCursor = 0; m_gapFillTargetId = 0; m_gapFillPagesRemaining = 0;
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "MessageListModel: gap-fill page errored — abandoning";
            m_gapFillCursor = 0; m_gapFillTargetId = 0; m_gapFillPagesRemaining = 0;
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonArray    data = doc.object()["ocs"].toObject()["data"].toArray();
        if (data.isEmpty()) {
            m_gapFillCursor = 0; m_gapFillTargetId = 0; m_gapFillPagesRemaining = 0;
            return;
        }
        QVector<Message> filled;
        int               oldestInPage = std::numeric_limits<int>::max();
        for (const auto &val : data) {
            Message m = Message::fromJson(val.toObject());
            if (m.isReactionMessage() || m.isCallJoinLeave() || m.isEditMessage())
                continue;
            if (m_hideThreadMessages && m.threadId > 0)
                continue;
            oldestInPage = qMin(oldestInPage, m.id);
            if (m_messageIds.contains(m.id))
                continue;
            filled.append(m);
        }
        if (!filled.isEmpty()) {
            for (const auto &m : filled) {
                m_messageIds.insert(m.id);
                m_messages.append(m);
            }
            QHash<int, int> seen;
            QVector<Message> deduped;
            deduped.reserve(m_messages.size());
            for (const auto &m : m_messages) {
                if (!seen.contains(m.id)) {
                    seen[m.id] = 1;
                    deduped.append(m);
                }
            }
            std::sort(deduped.begin(), deduped.end(),
                      [](const Message &a, const Message &b) { return a.id > b.id; });
            beginResetModel();
            m_messages = std::move(deduped);
            endResetModel();
            if (!m_messages.isEmpty())
                m_oldestMessageId = m_messages.last().id;
            m_cache->saveMessages(m_token, m_messages);
            qDebug() << "MessageListModel: gap-fill page added"
                     << filled.size() << "msgs, total=" << m_messages.size();
        }
        // Continue paging if the oldest message in this page is still
        // newer than the target (gap not yet closed).
        if (oldestInPage != std::numeric_limits<int>::max()
            && oldestInPage > targetId + 1) {
            m_gapFillCursor          = oldestInPage;
            m_gapFillPagesRemaining -= 1;
            runGapFillStep();
        } else {
            qInfo() << "MessageListModel: gap-fill complete for" << m_token
                    << "thread=" << m_threadId;
            m_gapFillCursor = 0; m_gapFillTargetId = 0; m_gapFillPagesRemaining = 0;
        }
    });
}

void MessageListModel::onMessagesReceived(const QJsonArray &messages)
{
    if (messages.isEmpty()) return;

    QVector<Message> newMsgs;
    for (const auto &val : messages) {
        Message m = Message::fromJson(val.toObject());
        if (m_messageIds.contains(m.id) || m.isReactionMessage()
            || m.isCallJoinLeave() || m.isEditMessage())
            continue;
        if (m_hideThreadMessages && m.threadId > 0)
            continue;
        newMsgs.append(m);
    }

    if (newMsgs.isEmpty()) return;

    // Save only the newly received messages to cache (not the full list)
    QVector<Message> toCache = newMsgs;

    // Prepend new messages at index 0 (newest-first: new = front)
    // In BottomToTop view, index 0 is at the bottom — new messages appear at bottom
    for (const auto &m : newMsgs)
        m_messageIds.insert(m.id);
    std::reverse(newMsgs.begin(), newMsgs.end());
    beginInsertRows({}, 0, newMsgs.size() - 1);
    newMsgs.append(std::move(m_messages));
    m_messages = std::move(newMsgs);
    endInsertRows();

    m_cache->saveMessages(m_token, toCache);

    // Trim old messages to prevent unbounded memory growth
    trimOldMessages();

    emit newMessagesAtEnd();

    // Auto-mark as read when new messages arrive
    markAsRead();
}

void MessageListModel::postAndReplace(const QString &token, const QJsonObject &body, int tempId)
{
    m_api->post("apps/spreed/api/v1/chat/" + token, body,
        [this, tempId, token](bool ok, const QJsonObject &data, int) {
            if (m_token != token) return;

            int idx = -1;
            for (int i = 0; i < m_messages.size(); ++i) {
                if (m_messages[i].id == tempId) { idx = i; break; }
            }
            if (idx < 0) return;

            if (ok && !data.isEmpty()) {
                Message real = Message::fromJson(data);

                // Check if the poller already added this message (race condition)
                bool alreadyExists = false;
                for (int i = 0; i < m_messages.size(); ++i) {
                    if (i != idx && m_messages[i].id == real.id) {
                        alreadyExists = true;
                        break;
                    }
                }

                if (alreadyExists) {
                    // Poller beat us — remove the optimistic placeholder
                    beginRemoveRows({}, idx, idx);
                    m_messageIds.remove(tempId);
                    m_messages.removeAt(idx);
                    endRemoveRows();
                } else {
                    // Replace optimistic with real — update ID tracking
                    m_messageIds.remove(tempId);
                    m_messageIds.insert(real.id);
                    m_messages[idx] = real;
                    emit dataChanged(index(idx), index(idx));
                    m_cache->saveMessages(m_token, {real});
                }
            } else {
                m_messages[idx].sendStatus = "failed";
                emit dataChanged(index(idx), index(idx), {SendStatusRole});
            }
        });
}

void MessageListModel::setAutoMentionBot(const QString &mentionSlug)
{
    m_autoMentionBot = mentionSlug.trimmed();
}

void MessageListModel::sendMessage(const QString &text, int replyToId, bool silent)
{
    if (text.trimmed().isEmpty() || m_token.isEmpty())
        return;

    // #26 — in a one_to_one room with exactly one bot enabled, prepend
    // the bot's @-mention so the message reaches it without the user
    // having to type "@aelita" every time.
    //
    // We do NOT auto-prepend if:
    //   * the bot slug is empty (no eligible bot in this room),
    //   * we're sending as a reply (replyToId > 0; reply context already
    //     implies the addressee),
    //   * the text already mentions THIS bot anywhere (case-insensitive).
    //
    // Earlier revision used "text.contains('@')" as the gate but that
    // suppressed auto-mention on perfectly innocent inputs like email
    // addresses, code decorators (@param, @Override, @router.get),
    // Twitter handles — exactly the kind of text users paste into a bot
    // chat ABOUT code. The current gate triggers only when the message
    // truly already addresses the bot.
    QString actualText = text;
    if (!m_autoMentionBot.isEmpty() && replyToId <= 0
        && !text.contains(QStringLiteral("@") + m_autoMentionBot,
                          Qt::CaseInsensitive)) {
        actualText = QStringLiteral("@%1 %2").arg(m_autoMentionBot, text);
    }

    static int tempIdCounter = -1;
    int tempId = tempIdCounter--;

    Message optimistic;
    optimistic.id = tempId;
    optimistic.token = m_token;
    optimistic.actorType = "users";
    optimistic.actorId = m_api->user();
    optimistic.actorDisplayName = "";
    optimistic.message = actualText;  // #26 — auto-prepended @<bot> visible in own bubble
    optimistic.timestamp = QDateTime::currentSecsSinceEpoch();
    optimistic.messageType = "comment";
    optimistic.sendStatus = "sending";

    // Prepend at index 0 (newest-first: new = front)
    m_messageIds.insert(tempId);
    beginInsertRows({}, 0, 0);
    m_messages.prepend(optimistic);
    endInsertRows();

    emit messageSent();
    emit newMessagesAtEnd();

    QJsonObject body;
    body["message"] = actualText;
    if (replyToId > 0)
        body["replyTo"] = replyToId;
    if (silent) body["silent"] = true;
    // 0.40.9 — Talk's send-message API takes a top-level `threadId`
    // parameter for posting INTO an existing thread. Before this, the
    // composer wired the active thread id into `replyTo`, which the
    // server then rendered as a reply-quote of the seed message, so
    // every message in a topic looked like "↳ replying to 📌 Refunds".
    // threadId is the proper hook: the message joins the thread without
    // a spurious reply-quote.
    if (m_threadId > 0) body["threadId"] = m_threadId;

    // 0.41.2-beta — diagnostic. We're chasing a "zero messages in
    // Refunds thread" field bug where one client's messages don't
    // appear under another client's topic tab. Log the threadId we're
    // actually attaching to the send so the server-side state can be
    // compared against the receiver's topic filter.
    qInfo().nospace() << "MessageListModel: sendMessage token=" << m_token
                      << " threadId=" << m_threadId
                      << " replyTo=" << replyToId
                      << " len=" << actualText.length();

    postAndReplace(m_token, body, tempId);
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

    postAndReplace(currentToken, body, tempId);
}

void MessageListModel::addReaction(int messageId, const QString &emoji)
{
    if (m_token.isEmpty() || messageId <= 0) return;

    qDebug() << "Adding reaction" << emoji << "to message" << messageId << "in" << m_token;

    QJsonObject body;
    body["reaction"] = emoji;

    QString currentToken = m_token;
    QString reactionPath = "apps/spreed/api/v1/reaction/" + currentToken + "/" + QString::number(messageId);
    m_api->post(reactionPath,
        body, [this, messageId, currentToken, emoji, reactionPath](bool ok, const QJsonObject &data, int statusCode) {
            // 409 = already reacted → remove it (toggle)
            if (statusCode == 409) {
                QUrlQuery params;
                params.addQueryItem("reaction", emoji);
                m_api->del(reactionPath, params, [this, messageId, currentToken](bool ok2, const QJsonObject &data2, int) {
                    if (m_token != currentToken || !ok2) return;
                    updateReactions(messageId, data2);
                });
                return;
            }
            if (m_token != currentToken || !ok) return;

            updateReactions(messageId, data);
        });
}

void MessageListModel::updateReactions(int messageId, const QJsonObject &data)
{
    int idx = -1;
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].id == messageId) { idx = i; break; }
    }
    if (idx < 0) return;

    QJsonObject reactionsMap;
    for (auto it = data.begin(); it != data.end(); ++it) {
        if (it.value().isArray())
            reactionsMap[it.key()] = it.value().toArray().size();
        else
            reactionsMap[it.key()] = it.value().toInt();
    }
    m_messages[idx].reactions = reactionsMap;
    // Also update rawJson so the cache saves the new reactions
    if (!m_messages[idx].rawJson.isEmpty())
        m_messages[idx].rawJson["reactions"] = data;
    emit dataChanged(index(idx), index(idx), {ReactionsRole});
    m_cache->saveMessages(m_token, {m_messages[idx]});
}

void MessageListModel::createTopic(const QString &title)
{
    if (title.trimmed().isEmpty() || m_token.isEmpty())
        return;

    // Send root message (becomes the thread root)
    QJsonObject rootBody;
    rootBody["message"] = title.trimmed();
    QString token = m_token;

    m_api->post("apps/spreed/api/v1/chat/" + token, rootBody,
        [this, token, title](bool ok, const QJsonObject &data, int) {
            if (!ok || m_token != token) return;

            int rootId = data["id"].toInt();
            if (rootId <= 0) return;

            // Immediately reply to bootstrap the thread
            QJsonObject replyBody;
            replyBody["message"] = QString::fromUtf8("\u2709\uFE0F Topic created");  // ✉️
            replyBody["replyTo"] = rootId;

            m_api->post("apps/spreed/api/v1/chat/" + token, replyBody,
                [this, token](bool ok2, const QJsonObject &, int) {
                    if (!ok2) return;
                    // Refresh thread list to pick up the new topic
                    if (m_token == token)
                        emit messageSent();
                });
        });
}

void MessageListModel::deleteMessage(int messageId)
{
    if (m_token.isEmpty() || messageId <= 0) return;

    // Remove locally immediately
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].id == messageId) {
            beginRemoveRows({}, i, i);
            m_messageIds.remove(messageId);
            m_messages.removeAt(i);
            endRemoveRows();
            break;
        }
    }

    // Tell server
    QString path = "apps/spreed/api/v1/chat/" + m_token + "/" + QString::number(messageId);
    m_api->del(path, [this](bool ok, const QJsonObject &, int) {
        if (!ok)
            emit errorOccurred("Failed to delete message");
    });
}

void MessageListModel::editMessage(int messageId, const QString &newText)
{
    if (m_token.isEmpty() || messageId <= 0) return;

    int idx = -1;
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].id == messageId) { idx = i; break; }
    }
    if (idx < 0) {
        emit errorOccurred(QStringLiteral("Cannot edit: message not found locally"));
        return;
    }

    QJsonObject body;
    body["message"] = newText;
    QString path = "apps/spreed/api/v1/chat/" + m_token + "/" + QString::number(messageId);
    QString token = m_token;  // stale-callback guard

    m_api->put(path, body,
        [this, messageId, token](bool ok, const QJsonObject &data, int status) {
            if (m_token != token) return;
            if (!ok) {
                emit errorOccurred(
                    QStringLiteral("Failed to edit message (HTTP %1)").arg(status));
                return;
            }
            // 0.40.1 fix (#33) — Talk's PUT /chat/{token}/{id} returns the
            // SYSTEM "message_edited" notification, NOT the edited message
            // itself. The updated message body lives in the response's
            // `parent` field. Earlier code replaced the bubble with the
            // system notification, which the painter/filter rightly hides,
            // so the bubble disappeared until a chat reopen reloaded the
            // original (with new text) from cache/server.
            QJsonObject updatedObj = data;
            if (data.contains(QStringLiteral("parent"))) {
                const QJsonValue parent = data.value(QStringLiteral("parent"));
                if (parent.isObject())
                    updatedObj = parent.toObject();
            }
            const Message updated = Message::fromJson(updatedObj);
            if (updated.id <= 0) return;
            for (int i = 0; i < m_messages.size(); ++i) {
                if (m_messages[i].id == messageId) {
                    m_messages[i] = updated;
                    QModelIndex mi = index(i);
                    emit dataChanged(mi, mi);
                    m_cache->saveMessages(m_token, {updated});
                    break;
                }
            }
        });
}

void MessageListModel::pinMessage(int messageId)
{
    if (m_token.isEmpty() || messageId <= 0) return;

    // POST to pin endpoint (no body needed)
    QJsonObject empty;
    QString path = "apps/spreed/api/v1/chat/" + m_token + "/" + QString::number(messageId) + "/pin";
    m_api->post(path, empty, [this](bool ok, const QJsonObject &, int) {
        if (!ok) {
            emit errorOccurred("Failed to pin message");
        }
    });
}

QString MessageListModel::messageLink(int messageId) const
{
    if (m_token.isEmpty() || messageId <= 0) return {};

    // Build the Talk web URL for this message
    QString serverUrl = m_api->serverUrl();
    return serverUrl + "/call/" + m_token + "#message_" + QString::number(messageId);
}

void MessageListModel::sendFile(const QString &filePath)
{
    sendFileWithCaption(filePath, QString());
}

void MessageListModel::sendFileWithCaption(const QString &filePath, const QString &caption)
{
    if (m_token.isEmpty() || filePath.isEmpty()) return;

    // Accept both raw paths and file:// URLs
    QString localPath = filePath;
    if (localPath.startsWith("file:///"))
        localPath = QUrl(localPath).toLocalFile();

    // Resolve junctions in path (Qt 6 blocks traversal of untrusted mount points)
    {
        QFileInfo fi(localPath);
        QString absPath = fi.absoluteFilePath();
        QStringList parts = absPath.split('/', Qt::SkipEmptyParts);
        if (!parts.isEmpty()) {
            QString resolved = parts.first() + "/";
            for (int i = 1; i < parts.size(); ++i) {
                resolved += parts[i];
                QFileInfo info(resolved);
                if (info.isJunction())
                    resolved = info.junctionTarget();
                if (i < parts.size() - 1)
                    resolved += "/";
            }
            localPath = resolved;
        }
    }

    // If the path traverses a junction/symlink, copy to temp first
    // (Windows blocks junction traversal from protected dirs like Program Files)
    QString readPath = localPath;
    QString tempCopy;
    QFile file(readPath);
    if (!file.open(QIODevice::ReadOnly)) {
        // Try copying to temp as fallback
        tempCopy = QDir::tempPath() + "/talq_upload_" + QFileInfo(localPath).fileName();
        if (QFile::copy(localPath, tempCopy)) {
            readPath = tempCopy;
            file.setFileName(readPath);
            if (!file.open(QIODevice::ReadOnly)) {
                emit errorOccurred("Cannot open file: " + readPath + "\n" + file.errorString());
                QFile::remove(tempCopy);
                return;
            }
        } else {
            emit errorOccurred("Cannot open file: " + localPath + "\n" + file.errorString());
            return;
        }
    }

    QString fileName = QFileInfo(localPath).fileName();
    fileName.remove(QRegularExpression("[/\\\\?#%]"));  // sanitize for WebDAV path safety (S5)
    fileName.replace("..", "_");
    if (fileName.isEmpty()) fileName = "upload";

    // D3: reject files over 100 MB to avoid freezing the main thread on readAll
    // TODO: replace readAll with chunked/streaming upload for large files
    if (file.size() > 100 * 1024 * 1024) {
        emit errorOccurred("File too large (max 100 MB)");
        return;
    }
    QByteArray fileData = file.readAll();
    file.close();
    if (!tempCopy.isEmpty())
        QFile::remove(tempCopy);

    qDebug() << "Uploading file:" << fileName << "(" << fileData.size() << "bytes)";

    // Show upload progress
    m_uploadProgress = 0;
    m_uploadFileName = fileName;
    emit uploadProgressChanged();

    // Step 1: Upload via WebDAV PUT
    QString uploadPath = "/remote.php/dav/files/" + m_api->user() + "/Talk/" + fileName;
    auto *uploadReply = m_api->putAbsoluteUrl(uploadPath, fileData);

    // Track upload progress
    connect(uploadReply, &QNetworkReply::uploadProgress, this, [this](qint64 sent, qint64 total) {
        if (total > 0) {
            m_uploadProgress = static_cast<double>(sent) / total;
            emit uploadProgressChanged();
        }
    });

    QString token = m_token;
    connect(uploadReply, &QNetworkReply::finished, this, [this, uploadReply, fileName, token, caption]() {
        uploadReply->deleteLater();

        int status = uploadReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 201 && status != 204) {
            qWarning() << "File upload failed:" << status << uploadReply->errorString();
            m_uploadProgress = -1;
            m_uploadFileName.clear();
            emit uploadProgressChanged();
            emit errorOccurred("Failed to upload file");
            return;
        }

        m_uploadProgress = 1.0;
        emit uploadProgressChanged();
        qDebug() << "File uploaded, sharing to conversation" << token;

        // Step 2: Share to conversation
        QJsonObject body;
        body["shareType"] = 10;  // share to Talk conversation
        body["shareWith"] = token;
        body["path"] = QString("Talk/" + fileName);
        body["permissions"] = 1;  // read permission for recipients

        // Attach caption via talkMetaData (server capability: media-caption)
        if (!caption.isEmpty()) {
            QJsonObject metaData;
            metaData["caption"] = caption;
            body["talkMetaData"] = QString::fromUtf8(QJsonDocument(metaData).toJson(QJsonDocument::Compact));
        }

        m_api->post("apps/files_sharing/api/v1/shares", body,
            [this, fileName, token](bool ok, const QJsonObject &, int) {
                m_uploadProgress = -1;
                m_uploadFileName.clear();
                emit uploadProgressChanged();

                if (ok) {
                    qDebug() << "File shared:" << fileName;
                    // Refresh to pick up the server-generated file share message
                    // (file shares don't create an optimistic placeholder)
                    if (m_token == token) {
                        refreshLatest();
                    }
                } else {
                    emit errorOccurred("Failed to share file to conversation");
                }
            });
    });
}

void MessageListModel::onLastCommonReadChanged(int messageId)
{
    if (messageId <= m_lastCommonRead)
        return;

    int oldRead = m_lastCommonRead;
    m_lastCommonRead = messageId;

    // Keep the poller's request-side hint in sync. Without this the poller
    // would keep telling the server "I know value 0" and the server would
    // wake the long-poll on every read advance forever (cheap but noisy).
    m_poller->setLastKnownCommonRead(messageId);

    // Persist to cache
    if (m_cache && !m_token.isEmpty())
        m_cache->saveLastCommonRead(m_token, messageId);

    // Only emit dataChanged for messages that actually changed status
    int first = -1, last = -1;
    for (int i = 0; i < m_messages.size(); ++i) {
        int id = m_messages[i].id;
        if (id > oldRead && id <= messageId) {
            if (first < 0) first = i;
            last = i;
        }
    }
    if (first >= 0)
        emit dataChanged(index(first), index(last), {IsReadRole});
}

void MessageListModel::downloadFile(int fileId, const QString &fileName)
{
    if (fileId <= 0 || fileName.isEmpty()) return;

    // Download via Nextcloud file ID endpoint (works for any participant with access)
    QString downloadPath = "/index.php/f/" + QString::number(fileId) + "/download";
    auto *reply = m_api->getAbsoluteUrl(downloadPath);

    connect(reply, &QNetworkReply::finished, this, [this, reply, fileName, fileId]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "File download failed:" << reply->errorString();
            // Fallback: open in browser
            QDesktopServices::openUrl(QUrl(m_api->serverUrl() + "/f/" + QString::number(fileId)));
            return;
        }

        QByteArray data = reply->readAll();
        if (data.isEmpty()) {
            emit errorOccurred("Downloaded file is empty");
            return;
        }

        // Save to Downloads folder — sanitize filename to prevent path traversal
        QString safeFileName = QFileInfo(fileName).fileName();  // strip directory components
        safeFileName.remove(QRegularExpression("[/\\\\]"));     // extra safety
        if (safeFileName.isEmpty()) safeFileName = "download";
        QString downloadsDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        QString savePath = downloadsDir + "/" + safeFileName;

        // Avoid overwriting — add (1), (2) etc.
        if (QFile::exists(savePath)) {
            QString base = QFileInfo(safeFileName).completeBaseName();
            QString ext = QFileInfo(safeFileName).suffix();
            int n = 1;
            while (QFile::exists(savePath)) {
                savePath = downloadsDir + "/" + base + " (" + QString::number(n++) + ")" +
                    (ext.isEmpty() ? "" : "." + ext);
            }
        }

        QFile file(savePath);
        if (!file.open(QIODevice::WriteOnly)) {
            emit errorOccurred("Cannot save file: " + savePath);
            return;
        }
        file.write(data);
        file.close();

        qDebug() << "File downloaded to:" << savePath;

        // Open the file
        QDesktopServices::openUrl(QUrl::fromLocalFile(savePath));
    });
}

bool MessageListModel::pasteClipboardImage()
{
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *mimeData = clipboard->mimeData();
    if (!mimeData)
        return false;

    // Case 1: Image data in clipboard (screenshot, copied image)
    if (mimeData->hasImage()) {
        QImage image = clipboard->image();
        if (!image.isNull()) {
            QString tempPath = QDir::tempPath() + "/talq_paste_"
                + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".png";
            if (!image.save(tempPath, "PNG")) {
                qWarning() << "Failed to save clipboard image to" << tempPath;
                return false;
            }
            qDebug() << "Clipboard image saved:" << tempPath << image.size();
            emit pasteReady("file:///" + tempPath, image.width(), image.height());
            return true;
        }
    }

    // Case 2: File(s) copied from Explorer or file manager
    if (mimeData->hasUrls()) {
        QList<QUrl> urls = mimeData->urls();
        for (const auto &url : urls) {
            if (!url.isLocalFile()) continue;
            QString path = url.toLocalFile();
            if (!QFile::exists(path)) continue;

            QString mime = QMimeDatabase().mimeTypeForFile(path).name();
            if (mime.startsWith("image/")) {
                // Image file — show paste confirmation with preview
                QImage img(path);
                qDebug() << "Clipboard file paste (image):" << path;
                emit pasteReady(url.toString(), img.width(), img.height());
                return true;
            } else {
                // Non-image file — show confirmation with caption
                qDebug() << "Clipboard file paste:" << path;
                emit pasteReady(url.toString(), 0, 0);
                return true;
            }
        }
    }

    return false;
}

void MessageListModel::promptFileSend(const QString &filePath)
{
    QUrl url(filePath);
    QString path = url.isLocalFile() ? url.toLocalFile() : filePath;
    QString mime = QMimeDatabase().mimeTypeForFile(path).name();

    int w = 0, h = 0;
    if (mime.startsWith("image/")) {
        QImage img(path);
        w = img.width();
        h = img.height();
    }
    emit pasteReady(filePath, w, h);
}

void MessageListModel::cleanupTempFile(const QString &filePath)
{
    QUrl url(filePath);
    QString path = url.isLocalFile() ? url.toLocalFile() : filePath;
    if (path.contains("talq_paste_") && QFile::exists(path)) {
        QFile::remove(path);
        qDebug() << "Cleaned up temp file:" << path;
    }
}

void MessageListModel::sendMessageToToken(const QString &targetToken, const QString &text)
{
    if (targetToken.isEmpty() || text.trimmed().isEmpty()) return;

    QJsonObject body;
    body["message"] = text;
    body["replyTo"] = 0;

    QString path = QStringLiteral("apps/spreed/api/v1/chat/%1").arg(targetToken);
    m_api->post(path, body, [](bool, const QJsonObject &, int) {
        // Fire and forget
    });
}

void MessageListModel::markAsRead()
{
    if (m_token.isEmpty()) return;

    // Find the newest message ID (index 0 in newest-first order)
    int lastId = 0;
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].id > 0) {
            lastId = m_messages[i].id;
            break;
        }
    }

    if (lastId <= 0) return;

    QString token = m_token;

    // POST /apps/spreed/api/v1/chat/{token}/read
    QJsonObject body;
    body["lastReadMessage"] = lastId;
    m_api->post("apps/spreed/api/v1/chat/" + token + "/read", body,
        [this, token, lastId](bool ok, const QJsonObject &, int) {
            if (!ok || !m_conversations) return;
            // Mirror the server-side state into our cache. Without this the
            // ConversationListModel would still think this room had old
            // unread messages until the next 30 s auto-refresh — and a
            // chat switch in that window would re-show the divider.
            m_conversations->markReadAt(token, lastId);
        });
}

void MessageListModel::scheduleMessage(const QString &text, qint64 sendAt,
                                       int replyToId, bool silent)
{
    if (m_token.isEmpty() || text.trimmed().isEmpty()) return;
    if (sendAt <= QDateTime::currentSecsSinceEpoch()) {
        // Refuse to schedule in the past — the server would reject anyway,
        // but giving an inline error here is friendlier than waiting for the
        // OCS round-trip.
        emit errorOccurred(tr("Send time must be in the future"));
        return;
    }

    QJsonObject body;
    body["message"]  = text;
    body["sendAt"]   = sendAt;
    if (replyToId > 0) body["replyTo"] = replyToId;
    if (silent)        body["silent"]  = true;
    if (m_threadId > 0) body["threadId"] = m_threadId;

    QString token = m_token;
    m_api->post("apps/spreed/api/v1/chat/" + token + "/schedule", body,
        [this, sendAt, token](bool ok, const QJsonObject &data, int status) {
            if (!ok) {
                QString msg = data.value(QStringLiteral("ocs")).toObject()
                                  .value(QStringLiteral("meta")).toObject()
                                  .value(QStringLiteral("message")).toString();
                emit errorOccurred(msg.isEmpty()
                    ? tr("Could not schedule message (HTTP %1)").arg(status)
                    : tr("Could not schedule message: %1").arg(msg));
                return;
            }
            if (m_token == token)
                emit messageScheduled(sendAt);
        });
}

void MessageListModel::markAsUnread(int messageId)
{
    if (m_token.isEmpty() || messageId <= 0) return;

    // Setting lastReadMessage to (messageId - 1) effectively makes messageId
    // and everything newer "unread" from the server's perspective. The IDs
    // aren't guaranteed sequential client-side (reactions/joins are filtered
    // out of m_messages) but they are sequential server-side, and the server
    // accepts any integer here — it just compares numerically.
    const int newBoundary = messageId - 1;

    // Update the local unread divider immediately so the "New messages"
    // pill pops in above the targeted message — otherwise the only visible
    // signal would be the sidebar's unread badge, which is easy to miss
    // while you're still looking at the chat you just acted on.
    if (newBoundary != m_unreadBoundary) {
        m_unreadBoundary = newBoundary;
        emit unreadBoundaryChanged();
    }

    QString token = m_token;
    QJsonObject body;
    body["lastReadMessage"] = newBoundary;
    m_api->post("apps/spreed/api/v1/chat/" + token + "/read", body,
        [this, token](bool ok, const QJsonObject &, int) {
            if (!ok) return;
            // Refresh the conversation list so the sidebar's unread badge
            // and last-read pointer pick up the change immediately, without
            // waiting for the next 30 s auto-refresh.
            if (m_conversations) m_conversations->refresh();
        });
}
