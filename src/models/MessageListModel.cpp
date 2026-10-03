#include "core/ForwardLogic.h"
#include "models/MessageListModel.h"
#include <QPointer>
#include "core/MessageCache.h"
#include "core/ChatSyncLogic.h"
#include "core/SendRetryPolicy.h"
#include "core/TalqLog.h"
#include "models/ConversationListModel.h"
#include <QCryptographicHash>
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
#include <QUuid>
#include <functional>
#include <memory>
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

// Chat history page size. NC Talk's /chat endpoint caps `limit` at 200
// (default 100); requesting the max means the initial open and scroll-back
// need ~4x fewer round-trips than the old 50. The server cost is negligible
// (the chat table is tiny and fully cached); the win is one fat request over
// a warm HTTP/2 connection instead of many serial ones over WAN latency.
static constexpr int kChatPageLimit = 200;

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
        // "Nothing loaded yet" means no SERVER message: a pending send redrawn by
        // reinsertOutboxTemps() sits in front and must not keep the cached
        // history from showing.
        if (!messages.isEmpty() && !hasRealMessages()) {
            QVector<Message> filtered;
            for (const auto &m : messages) {
                if (m.isReactionMessage() || m.isCallJoinLeave()
                    || m.isEditMessage() || m.isDeletedMessage())
                    continue;
                if (!passesThreadFilter(m))
                    continue;
                filtered.append(m);
            }
            if (!filtered.isEmpty()) {
                // bug 12 — enforce the model invariant explicitly: newest-first
                // by id (index 0 = newest), matching every live path
                // (onMessagesReceived prepend, refreshLatest a.id>b.id). Don't
                // merely reverse the cache's row order: if the cache ever orders
                // by anything but id, a reply (newest id, lower/tied timestamp)
                // would land back near its parent on reopen. Sorting by id here
                // makes the reload order identical to the live order.
                std::sort(filtered.begin(), filtered.end(),
                          [](const Message &a, const Message &b) { return talq::messageSortsBefore(a.id, b.id); });
                // Behind any pending temps already at the front (usually none).
                const int firstNew = m_messages.size();
                beginInsertRows({}, firstNew, firstNew + filtered.size() - 1);
                m_messages.append(filtered);
                for (const auto &m : filtered)
                    m_messageIds.insert(m.id);
                endInsertRows();
                m_oldestMessageId = oldestRealId();  // oldest is now at the end
                // Don't emit newMessagesAtEnd — BottomToTop naturally positions at bottom
            }
        }

        qDebug() << "Cache loaded" << m_messages.size() << "messages for" << m_token;
        refreshLatest();
    });

    // Read-on-focus catch-up: when TalQ becomes the FOREGROUND/active app, mark
    // the open room read (messages received while unfocused were deliberately
    // left unread). Driven by the app foreground state (robust) rather than a
    // cached per-window flag, which on Windows could stick "inactive" after a
    // tray-restore / notification focus-steal / restart and stop marking read.
    // markAsRead live-checks the state and self-dedupes, so it is a no-op if no
    // room is open or the marker is already current.
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState s) {
        if (s == Qt::ApplicationActive) {
            markAsRead();
            // Coming back to the window (or waking the laptop) is the commonest
            // moment the link has just been fixed: let an unsent message try soon.
            kickOutbox();
        }
    });

    // Send outbox: one cheap pass every 2 s while any unsent message is still
    // active; stopped when there is none (syncOutboxTimer).
    m_outboxTimer.setInterval(2000);
    connect(&m_outboxTimer, &QTimer::timeout, this, &MessageListModel::outboxTick);
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
            // In a thread view, Talk sets every in-thread message's replyTo to
            // the thread ROOT — so the painter would prefix EVERY message with a
            // quote of the topic. Suppress the quote for messages replying
            // DIRECTLY to the root; genuine replies to OTHER in-thread messages
            // (parent != root) still show their quote.
            if (m_threadId > 0 && !m.replyTo.isEmpty()
                && m.replyTo.value("id").toInt() == m_threadId)
                return QString();
            return m.replyTo.isEmpty() ? QString() : m.replyTo["message"].toString();
        case ReplyToAuthorRole:
            return m.replyTo.isEmpty() ? QString() : m.replyTo["actorDisplayName"].toString();
        case ReplyToIdRole:
            // Mirrors ReplyToTextRole's suppression exactly: inside a thread the
            // root is not quoted on every reply, so it must not be a jump target
            // either -- the id and the quote the user can actually see have to
            // agree, or the clickable region says one thing and the pixels say
            // another.
            if (m.replyTo.isEmpty()) return 0;
            if (m_threadId > 0 && m.replyTo.value("id").toInt() == m_threadId)
                return 0;
            return m.replyTo.value("id").toInt();
        case ReactionsSelfRole: return m.reactionsSelf;
        case PollIdRole:        return m.pollId;
        case PollQuestionRole:  return m.pollQuestion;
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
        case FilePathRole:
            return m.filePath;
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
        case FileHideDownloadRole:
            return m.fileHideDownload;
        case LastEditTimestampRole:
            return m.lastEditTimestamp;
        case SilentRole:
            return m.silent;
        case ReferenceIdRole:
            return m.referenceId;
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
        {ReplyToIdRole,     "replyToId"},
        {ReactionsRole,     "reactions"},
        {ReactionsSelfRole, "reactionsSelf"},
        {PollIdRole,        "pollId"},
        {PollQuestionRole,  "pollQuestion"},
        {TimeStringRole,    "timeString"},
        {ShowDateSeparatorRole, "showDateSeparator"},
        {DateStringRole,    "dateString"},
        {IsReadRole,        "isRead"},
        {SendStatusRole,    "sendStatus"},
        {ThreadIdRole,      "msgThreadId"},
        {FileNameRole,      "fileName"},
        {FilePathRole,      "filePath"},
        {FileMimeRole,      "fileMime"},
        {FileSizeRole,      "fileSize"},
        {FileLinkRole,      "fileLink"},
        {FilePreviewRole,   "filePreview"},
        {HasFileRole,       "hasFile"},
        {FileIdRole,            "fileId"},
        {FileHideDownloadRole,  "fileHideDownload"},
        {LastEditTimestampRole, "lastEditTimestamp"},
        {SilentRole,            "silent"},
    };
}

void MessageListModel::setConversationToken(const QString &token)
{
    if (m_token == token) {
        // bug 1 — re-selecting the already-open conversation must not be a
        // dead no-op. If the live poll loop died (an unexpected socket abort,
        // or a room the server isn't pushing chat-refresh events for) this is
        // the user's instinctive recovery action. Re-sync against the server
        // and restart the poller (refreshLatest ends in startPoller), WITHOUT
        // a full model reset that would wipe scroll position.
        if (!token.isEmpty())
            refreshLatest();
        return;
    }

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

    // A search jump that is still paging when the user switches rooms must
    // not follow them. Aborting m_historyReply means the settle never fires,
    // which (a) stranded the heap QMetaObject::Connection MainWindow created
    // for it and (b) left these two members set, so the NEXT room's first
    // loadHistory chased the OLD room's message id for up to five more pages
    // and then showed a spurious "Message not found in recent history".
    if (m_historyUntilTargetId > 0) {
        const auto abandoned = m_historyUntilTargetId;
        m_historyUntilTargetId = 0;
        m_historyUntilRemainingPages = 0;
        emit historyUntilSettled(abandoned, false);   // lets MainWindow clean up
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

    // Anything this room still owes the server comes back on screen. Without it
    // a message that failed while the user was elsewhere simply vanished.
    reinsertOutboxTemps();

    emit conversationTokenChanged();

    if (token.isEmpty())
        return;

    // Opening a room is a good moment to try an unsent message in it.
    kickOutbox();

    // Show the most recent page from the local cache instantly, then fetch
    // fresh from the API in the background (triggered after the cache loads).
    m_cache->loadMessages(token, kChatPageLimit);

    // Mark as read on open — but ONLY if TalQ is the foreground app. Opening a
    // room while TalQ is in the background (a notification raising it, or a
    // saved room restored on launch into the tray) must not mark it read; focus
    // returning later does, via the applicationStateChanged hook in the ctor.
    if (QGuiApplication::applicationState() == Qt::ApplicationActive) {
        QJsonObject body;
        m_api->post("apps/spreed/api/v1/chat/" + token + "/read", body,
            [](bool, const QJsonObject &, int) {});
    }

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
    reinsertOutboxTemps();   // the reset above dropped unsent messages of this view
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
        reinsertOutboxTemps();
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
    // Do not let a BACKGROUND read dismiss the user's Talk
    // notifications. The chat GET defaults to markNotificationsAsRead=1,
    // so merely paging history, refreshing the read marker or filling a
    // gap silently cleared notifications the user had not looked at --
    // and it defeated the deliberate "opening a room while TalQ is in the
    // background must not mark it read" guard in setConversationToken,
    // because the follow-up GET dismissed them anyway.
    // Dismissal still happens, via the explicit POST .../read, which
    // calls markMentionNotificationsRead server-side
    // (ChatController.php:1902). Capability: chat-keep-notifications.
    params.addQueryItem("markNotificationsAsRead", "0");
    params.addQueryItem("limit", QString::number(kChatPageLimit));
    // Read marker must NOT advance as a side-effect of fetching history — the
    // NC server marks read by DEFAULT when setReadMarker is absent. Reads are
    // driven only by the focus-gated markAsRead(). (Upstream Talk forces 0 too.)
    params.addQueryItem("setReadMarker", "0");
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
        if (data.size() < kChatPageLimit) {
            m_hasMoreHistory = false;
            emit hasMoreHistoryChanged();
        }

        // API returns newest-first; keep as-is (our storage is newest-first)
        QVector<Message> olderMsgs;
        for (const auto &val : data) {
            const QJsonObject obj = val.toObject();
            Message m = Message::fromJson(obj);
            // Control rows are absorbed even on a history page: a reaction or a
            // deletion that only ever appeared here used to be thrown away.
            if (absorbControlMessage(obj, m))
                continue;
            // An unsent message that DID land is redrawn by reinsertOutboxTemps()
            // right before this page loads (topic switch, hide-topics toggle); join
            // it to its real copy here, as refreshLatest and the poller do, or both
            // would be drawn until the next refresh.
            replaceTempByReferenceId(m);
            if (m_messageIds.contains(m.id))
                continue;
            if (!passesThreadFilter(m))
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

            // bug 1 — defensive: the append assumes every fetched id is older
            // than the current tail. lookIntoFuture=0 guarantees that today, but
            // keep the same single ordering authority here so a future API/race
            // change can't silently bury a row at the append seam.
            enforceNewestFirstInvariant();
        }

        if (!m_messages.isEmpty())
            m_oldestMessageId = oldestRealId();  // oldest is at the end

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
                    emit errorOccurred(tr("Message not found in recent history"));
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
    // Preferred path: ask the server for the messages AROUND the target in one
    // request.
    //
    // The fallback below walks history backwards a page at a time and gives up
    // after kMaxPages, so a search hit — or a clicked notification — older than
    // roughly 5 x kChatPageLimit messages simply never scrolled into view, and
    // the user got "Message not found in recent history" for a message that is
    // plainly still there. In a busy room that is days of history, not months.
    if (m_contextCapable && m_api && !m_token.isEmpty()) {
        const QString currentToken = m_token;
        QPointer<MessageListModel> guard(this);
        // Centred on the target, so half the window lands either side of it.
        m_api->fetchMessageContext(m_token, messageId, kChatPageLimit,
            [this, guard, currentToken, messageId](bool ok, const QJsonArray &data, int) {
                if (!guard || m_token != currentToken) return;   // destroyed or stale
                if (!ok) {
                    // Endpoint refused: fall back to the page walk rather than
                    // failing the jump outright.
                    qWarning() << "chat context fetch failed for" << messageId
                               << "- falling back to paging history backwards";
                    beginPagedHistoryUntil(messageId);
                    return;
                }

                // Same admission rules as the history merge, so a context
                // fetch cannot smuggle in rows the normal path filters out.
                QVector<Message> fetched;
                for (const auto &val : data) {
                    const QJsonObject obj = val.toObject();
                    Message m = Message::fromJson(obj);
                    if (absorbControlMessage(obj, m))
                        continue;
                    if (m_messageIds.contains(m.id))
                        continue;
                    if (!passesThreadFilter(m))
                        continue;
                    fetched.append(m);
                }

                if (!fetched.isEmpty()) {
                    m_cache->saveMessages(m_token, fetched);
                    for (const auto &m : fetched)
                        m_messageIds.insert(m.id);
                    const int first = m_messages.size();
                    beginInsertRows({}, first, first + fetched.size() - 1);
                    m_messages.append(fetched);
                    endInsertRows();
                    // ⚠ Load-bearing here in a way it is not for loadHistory().
                    // A context window is centred on the target, so it can
                    // contain messages NEWER than our current newest — the
                    // append-is-always-older assumption does not hold. This is
                    // the single ordering authority and it rebuilds the dedup
                    // mirror and the oldest cursor from the corrected list.
                    enforceNewestFirstInvariant();
                }
                if (!m_messages.isEmpty())
                    m_oldestMessageId = oldestRealId();

                bool found = false;
                for (const auto &m : m_messages)
                    if (m.id == messageId) { found = true; break; }
                emit historyUntilSettled(messageId, found);
                if (!found)
                    emit errorOccurred(tr("Message not found in recent history"));
                startPoller();
            });
        return;
    }

    beginPagedHistoryUntil(messageId);
}

// The pre-0.65.3 behaviour: page backwards until the target shows up or the
// budget runs out. Still the whole implementation on a server without
// `chat-get-context`, and the fallback when the context endpoint refuses.
void MessageListModel::beginPagedHistoryUntil(int messageId)
{
    static constexpr int kMaxPages = 5;
    m_historyUntilTargetId = messageId;
    m_historyUntilRemainingPages = kMaxPages;
    loadHistory();
}

void MessageListModel::startPoller()
{
    // Newest SERVER id. Index 0 is a pending temp (negative id) while a send is
    // unconfirmed -- seeding from it refused to start the poller at all, and
    // refreshLatest() had just stopped it (RCA 2026-10-03: "Poller: NOT
    // starting" twice, then silence until the resend's POST happened to work).
    int lastId = newestRealId();
    if (lastId <= 0) {
        qDebug() << "Poller: NOT starting — no messages loaded yet for" << m_token;
        return;  // never poll with lastKnown=0, it downloads entire history
    }
    // bug 1 — never seed the poll cursor BACKWARD. The open sequence fires
    // several competing startPoller() calls (cache→refreshLatest, topic-mode
    // reload, push/activation refresh); one of them can run while m_messages
    // index-0 is momentarily the older cached id, which reverted the live
    // cursor (e.g. 18055→18054) and triggered a redundant re-fetch + model
    // reset — the churn behind the intermittent "message not in the room".
    // Clamp to the value the poller already reached for this same room.
    if (m_poller->currentToken() == m_token)
        lastId = qMax(lastId, m_poller->lastKnownMessageId());
    m_poller->setThreadId(m_threadId);
    m_poller->setLastKnownCommonRead(m_lastCommonRead);
    m_poller->start(m_token, lastId);
}

void MessageListModel::trimOldMessages()
{
    // Memory cap for the live model. Holds several full pages so a live
    // message arriving (which triggers this trim) doesn't discard history the
    // user just scrolled back to load. Trimmed rows are re-fetchable on scroll.
    static constexpr int MAX_MESSAGES = 600;
    if (m_messages.size() <= MAX_MESSAGES) return;

    int trimCount = m_messages.size() - MAX_MESSAGES;
    int first = m_messages.size() - trimCount;
    for (int i = first; i < m_messages.size(); ++i)
        m_messageIds.remove(m_messages[i].id);
    beginRemoveRows({}, first, m_messages.size() - 1);
    m_messages.remove(first, trimCount);
    endRemoveRows();

    if (!m_messages.isEmpty())
        m_oldestMessageId = oldestRealId();
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
    params.addQueryItem("markNotificationsAsRead", "0");
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

    // Fetch the latest page from the server (lookIntoFuture=0, no lastKnownMessageId).
    // This gets the absolute newest messages, regardless of what the cache had.
    QUrlQuery params;
    params.addQueryItem("lookIntoFuture", "0");
    params.addQueryItem("markNotificationsAsRead", "0");
    params.addQueryItem("limit", QString::number(kChatPageLimit));
    // Do NOT mark read here — the server marks read by default when
    // setReadMarker is absent, which would defeat the focus gate on open
    // (refreshLatest runs on every open, focused or not). The focus-gated
    // markAsRead() below is the only authority that advances the marker.
    params.addQueryItem("setReadMarker", "0");

    if (m_threadId > 0)
        params.addQueryItem("threadId", QString::number(m_threadId));

    QString currentToken = m_token;
    int capturedGen = m_generation;
    auto *reply = m_api->getRaw("apps/spreed/api/v1/chat/" + m_token, params);
    m_refreshReply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply, currentToken, capturedGen]() {
        if (m_refreshReply == reply) m_refreshReply = nullptr;
        reply->deleteLater();
        // 0.41.6-beta — diagnostics for the "new messages visible in
        // conversation list but not in chat history" field report.
        // Suspect: a fast tab-switch on chat-open bumped m_generation
        // before refreshLatest's reply landed and the result was
        // silently discarded. The two logs below pinpoint which path
        // ate the reply.
        if (m_generation != capturedGen) {
            qInfo().nospace() << "MessageListModel: refreshLatest DROPPED "
                              << "(generation race: capturedGen=" << capturedGen
                              << " current=" << m_generation
                              << " token=" << currentToken << ")";
            return;
        }
        if (m_token != currentToken) {
            qInfo().nospace() << "MessageListModel: refreshLatest DROPPED "
                              << "(token switched: was=" << currentToken
                              << " now=" << m_token << ")";
            return;
        }

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

        // Clear-history reconciliation: if the fetch carries a "history_cleared"
        // event, purge our (possibly stale, cached) view BEFORE merging. Covers
        // a device that wasn't viewing the room when another device cleared it —
        // it opens, loads the stale cache, then this fetch wipes it.
        for (const QJsonValue &val : data) {
            if (val.toObject().value(QStringLiteral("systemMessage")).toString()
                    == QLatin1String("history_cleared")) {
                clearLocalHistory();
                break;
            }
        }

        // bug 1 (FIX) — rebuild the dedup index from the AUTHORITATIVE message
        // list before missing-detection. A concurrent open-time reset can
        // leave m_messageIds holding ids no longer in m_messages; the
        // `!m_messageIds.contains(m.id)` test below would then skip a
        // genuinely-new server message as "already known", so it is never
        // merged and the room is missing it until a full re-open. This is the
        // SECOND ingest path with the same desync hazard as onMessagesReceived.
        m_messageIds.clear();
        for (const auto &existing : m_messages)
            m_messageIds.insert(existing.id);

        // [BUG1] trace (verbose only): data is newest-first, so data[0].id is
        // the server's newest. If fetchedNewest carries the missing message
        // but the final model newest (logged at DONE) does not, it was lost in
        // the merge; if fetchedNewest itself lacks it, the server fetch didn't
        // return it.
        if (TalqLog::g_verbose)
            qDebug().nospace() << "[BUG1] refreshLatest token=" << currentToken
                          << " threadId=" << m_threadId
                          << " fetched=" << data.size()
                          << " fetchedNewest=" << (data.isEmpty() ? 0 : data.first().toObject().value(QStringLiteral("id")).toInt())
                          << " modelBefore=" << m_messages.size()
                          << " newestBefore=" << (m_messages.isEmpty() ? 0 : m_messages.first().id);

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
            const QJsonObject obj = val.toObject();
            Message m = Message::fromJson(obj);
            if (m.systemMessage == QLatin1String("history_cleared"))
                continue;   // purge already handled by the pre-scan above
            if (absorbControlMessage(obj, m))
                continue;
            // 0.41.3-beta — own-message echo dedup (see onMessagesReceived).
            replaceTempByReferenceId(m);
            if (!passesThreadFilter(m)) continue;

            if (!m_messageIds.contains(m.id)) {
                missing.append(m);
            } else {
                // Existing message — check if edited (message text changed).
                // H2 (ChatSyncLogic.h): replaceTempByReferenceId() above can
                // remove a row earlier in THIS loop, shifting/invalidating the
                // idToIndex map built once before the loop. Indexing
                // m_messages[idx] with a stale idx was an out-of-bounds
                // read+write that crashed the app on a just-sent/concurrent
                // message (bug 7). Validate the cached index still maps to this
                // id; if not, fall back to a fresh lookup so edit detection
                // still works and we never index out of bounds.
                int idx = idToIndex.value(m.id, -1);
                const int idAtIdx = (idx >= 0 && idx < m_messages.size())
                                        ? m_messages[idx].id : -1;
                if (!talq::reconcileIndexValid(idx, m_messages.size(), idAtIdx, m.id)) {
                    idx = -1;
                    for (int j = 0; j < m_messages.size(); ++j)
                        if (m_messages[j].id == m.id) { idx = j; break; }
                }
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
                return talq::messageSortsBefore(a.id, b.id);
            });

            beginResetModel();
            m_messages = std::move(deduped);
            endResetModel();

            // Oldest cursor must be a REAL id, never a pending temp's negative
            // id (which would poison scroll-up pagination). Temps sort to the
            // front, so the last positive id is the oldest real message.
            for (int i = m_messages.size() - 1; i >= 0; --i)
                if (m_messages[i].id > 0) { m_oldestMessageId = m_messages[i].id; break; }

            // Save updated cache
            m_cache->saveMessages(m_token, m_messages);

            qDebug() << "MessageListModel: refreshLatest merged" << missing.size()
                     << "missing, total=" << m_messages.size();

            // Advance the read marker for messages that arrived via THIS path.
            // The poller's onMessagesReceived marks read on new messages, but
            // when the "chat refresh" signaling hint makes refreshLatest merge a
            // new message FIRST, the poller then dedups it out and returns at its
            // `newMsgs.isEmpty()` guard BEFORE reaching its own markAsRead() — so
            // a message you're looking at in the open room is shown but never
            // marked read on the server (the intermittent "I see it but it's not
            // tagged as read" field bug, 2026-06-03). markAsRead is forward-only
            // and self-dedupes, so it's a safe no-op on a pure gap-fill/re-merge.
            markAsRead();

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

        // bug 1 (REAL FIX) — enforce the newest-first invariant. The in-place
        // (no-missing) merge path above does not re-sort, so a model that had
        // drifted out of order would keep the newest messages buried below the
        // fold. Shared single authority (see enforceNewestFirstInvariant()).
        enforceNewestFirstInvariant();

        // Read-receipt guarantee: mark read on ANY latest-id advance while the
        // open room is visible+active — not only on the missing-merge path that
        // already calls markAsRead() above. This covers the in-place merge (no
        // `missing`) and the cache-load→refreshLatest reopen where the server
        // fetch returns nothing new, both of which previously left the newest
        // message unread on the server. markAsRead() is forward-only,
        // focus-gated (ApplicationActive) and self-dedupes via
        // shouldPostReadMarker(lastId > knownServerRead), so it's a cheap no-op
        // when nothing advanced — never spams /read, never clears unread for a
        // backgrounded room.
        markAsRead();

        // [BUG1] trace (verbose only): final model state after the merge.
        if (TalqLog::g_verbose)
            qDebug().nospace() << "[BUG1] refreshLatest DONE token=" << currentToken
                               << " model=" << m_messages.size()
                               << " newest=" << (m_messages.isEmpty() ? 0 : m_messages.first().id);

        // Restart poller from the true latest message
        m_poller->stop();
        startPoller();
    });
}

bool MessageListModel::enforceNewestFirstInvariant()
{
    // Cheap O(n) ordered-check first: pay the reset+sort cost only on real drift.
    // messagePairOrdered shares the exact ordering the sort below uses, so a
    // pending optimistic temp (negative id) at the FRONT counts as correctly
    // newest-first and does NOT trigger a spurious re-sort.
    bool ordered = true;
    for (int i = 1; i < m_messages.size(); ++i)
        if (!talq::messagePairOrdered(m_messages[i - 1].id, m_messages[i].id)) {
            ordered = false; break;
        }
    if (ordered)
        return false;

    // The view renders newest-at-bottom from index 0, so an older id at the
    // front buries the genuinely-newest messages and they stay invisible until
    // a room switch forces a full reload — the "message missing from the open
    // room until I switch away and back" field bug. Re-sort to newest-first and
    // rebuild the dedup mirror + oldest cursor from the corrected list.
    if (TalqLog::g_verbose)
        qDebug().nospace() << "[BUG1] enforceNewestFirstInvariant RE-SORT (front was "
                           << (m_messages.isEmpty() ? 0 : m_messages.first().id) << ")";
    beginResetModel();
    std::sort(m_messages.begin(), m_messages.end(),
              [](const Message &a, const Message &b) {
                  return talq::messageSortsBefore(a.id, b.id);
              });
    endResetModel();
    m_messageIds.clear();
    for (const auto &mm : m_messages)
        m_messageIds.insert(mm.id);
    // Oldest cursor must be a REAL (server) id — never a pending temp's
    // negative id, which would poison scroll-up pagination. After the sort
    // temps are at the front and reals descend, so the last positive id is the
    // oldest real message.
    for (int i = m_messages.size() - 1; i >= 0; --i)
        if (m_messages[i].id > 0) { m_oldestMessageId = m_messages[i].id; break; }
    return true;
}

bool MessageListModel::passesThreadFilter(const Message &m) const
{
    // System messages and own optimistic-still-pending always pass —
    // upstream Talk does the same so the user's just-sent text doesn't
    // disappear the moment they switch tabs.
    if (m.id < 0) return true;
    if (m_threadId > 0) {
        // 0.41.6-beta — reply-chain fallback. If a peer's client (older
        // TalQ or an out-of-band web client) posted INTO this thread but
        // forgot to attach `threadId` to its POST body, the message
        // still has `replyTo` pointing at the seed (because Talk's
        // composer wires replyTo from the active thread context). Admit
        // those by checking the replyTo.id chain. Without this, the
        // strict m.threadId equality blackholed untagged-style
        // thread replies — the "topic shows zero messages" field bug.
        if (m.threadId == m_threadId || m.id == m_threadId)
            return true;
        // replyTo is a JSON object — id is the parent message's numeric id.
        if (m.replyTo.contains(QStringLiteral("id"))
            && m.replyTo.value(QStringLiteral("id")).toInt() == m_threadId)
            return true;
        return false;
    }
    return !(m_hideThreadMessages && m.threadId > 0);
}

bool MessageListModel::replaceTempByReferenceId(const Message &real)
{
    if (real.referenceId.isEmpty()) return false;
    for (int i = 0; i < m_messages.size(); ++i) {
        const auto &t = m_messages[i];
        if (t.id < 0 && t.referenceId == real.referenceId) {
            // REPLACE IN PLACE (do not remove + rely on the caller to re-add).
            // The old remove-only form lost the message whenever a later
            // `continue` in the ingest loop (thread filter) skipped the re-add,
            // and it left a negative-id temp in the list across a sort — which
            // reordered/duplicated the user's just-sent message and corrupted
            // m_oldestMessageId. Swapping the row in place keeps the message
            // present unconditionally, updates the dedup mirror, and — because
            // no row is removed — leaves all other cached indices valid (this
            // also removes the H2 mid-loop index-invalidation hazard).
            // Bug: "send two messages, the first one disappears", 2026-06-04.
            const int tempId = t.id;
            m_messageIds.remove(tempId);
            m_messageIds.insert(real.id);
            m_messages[i] = real;
            const QModelIndex mi = index(i);
            emit dataChanged(mi, mi);
            // The server has it: the outbox is no longer responsible for it, and
            // its reply callback (if still outstanding) must not touch it.
            if (dropOutboxEntry(tempId))
                qInfo().nospace() << "SendOutbox: delivered via echo token=" << m_token
                                  << " temp=" << tempId << " id=" << real.id;
            return true;
        }
    }
    return false;
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
    params.addQueryItem("markNotificationsAsRead", "0");
    params.addQueryItem("limit",              "100");
    params.addQueryItem("setReadMarker",      "0");  // never mark read on backfill (server default is 1)
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
            const QJsonObject obj = val.toObject();
            Message m = Message::fromJson(obj);
            if (absorbControlMessage(obj, m))
                continue;
            // 0.41.3-beta — same client-side filter as the receive paths.
            if (!passesThreadFilter(m)) continue;
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
                      [](const Message &a, const Message &b) { return talq::messageSortsBefore(a.id, b.id); });
            beginResetModel();
            m_messages = std::move(deduped);
            endResetModel();
            if (!m_messages.isEmpty())
                m_oldestMessageId = oldestRealId();
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

    // bug 1 (TRUE root cause) — the open-time load race can leave m_messageIds
    // (the dedup index) out of sync with m_messages: an id is present in the
    // SET but absent from the LIST. The dedup below then silently DROPS a
    // genuinely-new poll message as a "duplicate" it doesn't actually have —
    // the reproduced "message shows in the conversation list but is missing
    // from the open room" (field log: poller reported "29 new" yet msgs:62
    // never grew). Rebuild the index from the authoritative list so dedup can
    // never reject a message that isn't truly present. O(n) once per batch.
    m_messageIds.clear();
    for (const auto &existing : m_messages)
        m_messageIds.insert(existing.id);

    // [BUG1] trace (verbose only).
    if (TalqLog::g_verbose)
        qDebug().nospace() << "[BUG1] onMessagesReceived n=" << messages.size()
                      << " token=" << m_token
                      << " threadId=" << m_threadId
                      << " modelBefore=" << m_messages.size()
                      << " newestBefore=" << (m_messages.isEmpty() ? 0 : m_messages.first().id);

    QVector<Message> newMsgs;
    for (const auto &val : messages) {
        const QJsonObject obj = val.toObject();
        Message m = Message::fromJson(obj);
        if (m.systemMessage == QLatin1String("history_cleared")) {
            // Whole-conversation clear (DELETE /chat/{token}) — purge locally on
            // every device that receives the event (actor + peer). Upstream says
            // to drop all cached messages on this system message.
            clearLocalHistory();
            continue;
        }
        if (absorbControlMessage(obj, m))
            continue;
        // 0.41.3-beta — own-message echo dedup. If the long-poll
        // returns a message whose referenceId matches a still-pending
        // optimistic temp, replace the temp in-place so the user
        // doesn't see a duplicate (or, worse, lose the temp because
        // it was racing the POST callback).
        replaceTempByReferenceId(m);
        if (m_messageIds.contains(m.id)) {
            if (TalqLog::g_verbose)
                qDebug().nospace() << "[BUG1] skip DEDUP id=" << m.id;
            continue;
        }
        // 0.41.3-beta — client-side thread filter. We no longer ask
        // the server for a threadId-filtered poll (upstream doesn't),
        // so the poll returns ALL room messages. Filter here.
        if (!passesThreadFilter(m)) {
            if (TalqLog::g_verbose)
                qDebug().nospace() << "[BUG1] skip THREADFILTER id=" << m.id
                              << " m.threadId=" << m.threadId
                              << " filterThreadId=" << m_threadId;
            continue;
        }
        newMsgs.append(m);
    }

    // [BUG1] trace (verbose only): how many survived dedup+filter. If the
    // poller reported new messages but this is 0, they were dropped here.
    if (TalqLog::g_verbose)
        qDebug().nospace() << "[BUG1] onMessagesReceived passedDedupFilter=" << newMsgs.size()
                      << " of " << messages.size() << " incoming";
    if (newMsgs.isEmpty()) return;

    // Save only the newly received messages to cache (not the full list)
    QVector<Message> toCache = newMsgs;

    // Prepend new messages (newest-first: new = front)
    // In BottomToTop view, index 0 is at the bottom — new messages appear at bottom
    //
    // "Front" means just BEHIND any unsent messages pinned there: a pending temp
    // (negative id) sorts newer than every server id. Inserting at row 0 left the
    // list out of order, so enforceNewestFirstInvariant() below paid a model reset
    // and a cold relayout of every row for EACH incoming batch, for as long as one
    // unsent message was waiting -- now the steady state during a reconnect.
    for (const auto &m : newMsgs)
        m_messageIds.insert(m.id);
    std::reverse(newMsgs.begin(), newMsgs.end());
    int lead = 0;
    while (lead < m_messages.size() && m_messages[lead].id < 0) ++lead;
    beginInsertRows({}, lead, lead + newMsgs.size() - 1);
    QVector<Message> merged;
    merged.reserve(m_messages.size() + newMsgs.size());
    for (int i = 0; i < lead; ++i)
        merged.append(std::move(m_messages[i]));
    merged.append(newMsgs);
    for (int i = lead; i < m_messages.size(); ++i)
        merged.append(std::move(m_messages[i]));
    m_messages = std::move(merged);
    endInsertRows();

    // bug 1 (REAL FIX) — the prepend above assumes the batch strictly dominates
    // the current head. Under concurrency (thread-filtered subsets, a late POST
    // echo, the poll cursor running ahead of the model head) that can leave an
    // older id at index 0, burying the newest messages with NO refreshLatest to
    // correct it while the user sits in the room. Enforce the invariant on every
    // live batch — same single authority as refreshLatest, so it can't drift.
    enforceNewestFirstInvariant();

    // [BUG1] trace (verbose only): final state after prepend.
    if (TalqLog::g_verbose)
        qDebug().nospace() << "[BUG1] onMessagesReceived added=" << toCache.size()
                      << " modelAfter=" << m_messages.size()
                      << " newestAfter=" << (m_messages.isEmpty() ? 0 : m_messages.first().id);

    m_cache->saveMessages(m_token, toCache);

    // Trim old messages to prevent unbounded memory growth
    trimOldMessages();

    // Tell listeners where the newest SERVER message is. It is row 0 only when no
    // unsent message is pinned in front; the toast and the sidebar preview used to
    // read row 0 and so showed (or dropped) the user's OWN unsent text instead of
    // the peer's message that had just arrived.
    {
        int newestReal = 0;
        while (newestReal < m_messages.size() && m_messages[newestReal].id < 0) ++newestReal;
        m_latestArrivedRow = newestReal;
    }
    emit newMessagesAtEnd();

    // Auto-mark as read when new messages arrive
    markAsRead();
}

// ===========================================================================
// SEND OUTBOX  (design and rules: see the OutboxEntry comment in the header and
// core/SendRetryPolicy.h; evidence: RCA 2026-10-03, memory
// rca_message-send-retry-lost-on-reconnect)
//
// Before this, a composer message that failed was marked "failed" in RAM and
// never touched again: nothing re-sent it when the link came back, retryMessage
// had no caller since the QML UI went away, and a room switch wiped it.
// ===========================================================================

static qint64 outboxNowMs() { return QDateTime::currentMSecsSinceEpoch(); }

int MessageListModel::newestRealId() const
{
    for (const auto &m : m_messages)
        if (m.id > 0) return m.id;       // temps (negative) sort to the front
    return 0;
}

int MessageListModel::oldestRealId() const
{
    for (int i = m_messages.size() - 1; i >= 0; --i)
        if (m_messages[i].id > 0) return m_messages[i].id;
    return 0;
}

MessageListModel::OutboxEntry *MessageListModel::findEntry(int tempId)
{
    for (auto &e : m_outbox)
        if (e.tempId == tempId) return &e;
    return nullptr;
}

QString MessageListModel::outboxOwner() const
{
    // user + server: an entry written under one account must never be probed,
    // POSTed or redrawn under another (shared machine, account switch).
    return m_api->user() + QLatin1Char('@') + m_api->serverUrl();
}

bool MessageListModel::hasPendingSends() const
{
    for (const auto &e : m_outbox)
        if (!e.permanent) return true;
    return false;
}

bool MessageListModel::canRetry(int tempId) const
{
    for (const auto &e : m_outbox)
        if (e.tempId == tempId) return !e.inFlight;
    return false;
}

int MessageListModel::latestArrivedRow() const
{
    return qBound(0, m_latestArrivedRow, qMax(0, int(m_messages.size()) - 1));
}

void MessageListModel::clearOutbox()
{
    if (m_outbox.isEmpty() && m_sendQueue.isEmpty()) return;
    qInfo().nospace() << "SendOutbox: cleared (" << m_outbox.size()
                      << " unsent message(s) dropped -- session ended)";
    // Their rows go too, or a dead row would stay on screen with nothing behind it.
    QVector<int> temps;
    for (const auto &e : m_outbox) temps.append(e.tempId);
    for (int id : temps) removeTempRow(id);
    m_outbox.clear();
    m_sendQueue.clear();
    // m_probing is left alone on purpose: a probe in flight clears its own entry
    // when its reply lands, and the reply finds no entries to act on.
    syncOutboxTimer();
}

QString MessageListModel::statusFor(const OutboxEntry &e) const
{
    using talq::sendretry::Display;
    return talq::sendretry::displayState(e.inFlight, e.slow, e.everPosted, e.permanent)
            == Display::Failed
        ? QStringLiteral("failed") : QStringLiteral("sending");
}

Message MessageListModel::tempMessageFor(const OutboxEntry &e) const
{
    Message m;
    m.id = e.tempId;
    m.token = e.token;
    m.actorType = "users";
    m.actorId = m_api->user();
    m.actorDisplayName = "";
    m.message = e.text;   // #26 — auto-prepended @<bot> visible in own bubble
    m.timestamp = e.sentAtSecs;
    m.messageType = "comment";
    m.sendStatus = statusFor(e);
    m.referenceId = e.referenceId;
    m.silent = e.silent;
    // bug 4 — the quote renders immediately, from the parent already in the
    // model when the message was sent; the echo later refines it.
    if (!e.replyTo.isEmpty()) {
        m.replyToId = e.replyToId;
        m.replyTo = e.replyTo;
    }
    // 0.41.3-beta — tag the temp with its thread so the client-side thread
    // filter keeps it visible until the server's echo lands.
    if (e.threadId > 0) m.threadId = e.threadId;
    return m;
}

QJsonObject MessageListModel::postBodyFor(const OutboxEntry &e) const
{
    // The WHOLE original request. The old retryMessage rebuilt it from {message}
    // alone and silently dropped the reply target, the topic and the silent flag.
    QJsonObject body;
    body["message"] = e.text;
    body["referenceId"] = e.referenceId;
    if (e.replyToId > 0)
        body["replyTo"] = e.replyToId;
    if (e.silent) body["silent"] = true;
    // 0.40.9 — Talk's send-message API takes a top-level `threadId` parameter for
    // posting INTO an existing thread; wiring it into `replyTo` rendered every
    // topic message as a reply-quote of the seed.
    if (e.threadId > 0) body["threadId"] = e.threadId;
    return body;
}

bool MessageListModel::entryVisibleInView(const OutboxEntry &e) const
{
    // Same rule as passesThreadFilter for a real message. (passesThreadFilter
    // itself waves every temp through, which is right while the user is typing
    // in a tab but would draw a message from topic A inside topic B on redraw.)
    if (m_threadId > 0) return e.threadId == m_threadId;
    return !(m_hideThreadMessages && e.threadId > 0);
}

void MessageListModel::refreshRowStatus(int tempId)
{
    const OutboxEntry *e = findEntry(tempId);
    if (!e || e->token != m_token) return;
    const QString status = statusFor(*e);
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].id != tempId) continue;
        if (m_messages[i].sendStatus != status) {
            m_messages[i].sendStatus = status;
            emit dataChanged(index(i), index(i), {SendStatusRole});
        }
        return;
    }
}

void MessageListModel::syncOutboxTimer()
{
    bool active = false;
    for (const auto &e : m_outbox)
        if (!e.permanent) { active = true; break; }
    if (active && !m_outboxTimer.isActive())
        m_outboxTimer.start();
    else if (!active && m_outboxTimer.isActive())
        m_outboxTimer.stop();
}

bool MessageListModel::dropOutboxEntry(int tempId)
{
    for (int i = 0; i < m_outbox.size(); ++i) {
        if (m_outbox[i].tempId != tempId) continue;
        const QString token = m_outbox[i].token;
        m_outbox.removeAt(i);
        auto it = m_sendQueue.find(token);
        if (it != m_sendQueue.end()) {
            it->removeAll(tempId);
            if (it->isEmpty()) m_sendQueue.erase(it);
        }
        syncOutboxTimer();
        return true;
    }
    return false;
}

void MessageListModel::reinsertOutboxTemps()
{
    if (m_token.isEmpty() || m_outbox.isEmpty()) return;
    QVector<Message> temps;
    const QString owner = outboxOwner();
    for (const auto &e : m_outbox) {
        if (e.token != m_token || e.owner != owner || m_messageIds.contains(e.tempId)
            || !entryVisibleInView(e))
            continue;
        temps.append(tempMessageFor(e));
    }
    if (temps.isEmpty()) return;
    std::sort(temps.begin(), temps.end(), [](const Message &a, const Message &b) {
        return talq::messageSortsBefore(a.id, b.id);
    });
    for (const auto &t : temps)
        m_messageIds.insert(t.id);
    qInfo().nospace() << "SendOutbox: redrawing " << temps.size()
                      << " unsent message(s) token=" << m_token;
    beginInsertRows({}, 0, temps.size() - 1);
    temps.append(std::move(m_messages));
    m_messages = std::move(temps);
    endInsertRows();
}

void MessageListModel::startAttempt(int tempId)
{
    OutboxEntry *e = findEntry(tempId);
    if (!e || e->inFlight) return;

    e->postAttempts++;
    // From here on this message may be on the server. A queued one that has not
    // reached this line cannot be, so the history window proves nothing about it
    // and cannot give it up (see afterProbe). A forced re-send (manual) is spent
    // by the POST it forces -- it must not outlive the user's click.
    e->everPosted = true;
    e->manual = false;
    e->attemptSeq = ++m_attemptCounter;
    e->inFlight = true;
    e->slow = false;
    e->inFlightSinceMs = e->lastActivityMs = outboxNowMs();
    const quint64 seq = e->attemptSeq;
    const QString token = e->token;
    qInfo().nospace() << "SendOutbox: POST token=" << token << " temp=" << tempId
                      << " attempt=" << e->postAttempts << " len=" << e->text.length();
    refreshRowStatus(tempId);

    m_api->post("apps/spreed/api/v1/chat/" + token, postBodyFor(*e),
        [this, tempId, seq, token](bool ok, const QJsonObject &data, int status) {
            onAttemptFinished(tempId, seq, token, ok, data, status);
        });

    // Display only. After 20 s without an answer the bubble says "failed" so the
    // user is never left watching "Sending..." forever (bug 7) -- but this does
    // NOT end the attempt: ApiClient::post has no handle to abort and its own
    // transfer timeout is 30 s, so the request can still land. The attempt stays
    // in flight, and nothing re-sends it, until the reply callback fires.
    QTimer::singleShot(20000, this, [this, tempId, seq]() { onAttemptSlow(tempId, seq); });
}

void MessageListModel::onAttemptSlow(int tempId, quint64 seq)
{
    OutboxEntry *e = findEntry(tempId);
    if (!e || e->attemptSeq != seq || !e->inFlight) return;   // answered, or a newer attempt
    e->slow = true;
    qInfo().nospace() << "SendOutbox: no answer after 20 s token=" << e->token
                      << " temp=" << tempId << " attempt=" << e->postAttempts
                      << " -- shown as failed, request still live";
    refreshRowStatus(tempId);
}

void MessageListModel::onAttemptFinished(int tempId, quint64 seq, const QString &token,
                                         bool ok, const QJsonObject &data, int status)
{
    OutboxEntry *e = findEntry(tempId);
    if (!e) {
        // Already reconciled by the poller's echo (which dropped the entry), or
        // discarded by the user. Whatever was queued behind it has been waiting
        // for exactly this reply: let it go.
        qInfo().nospace() << "SendOutbox: reply for an untracked send temp=" << tempId
                          << " ok=" << ok << " status=" << status;
        sendNextQueued(token);
        return;
    }

    if (ok && !data.isEmpty()) {
        qInfo().nospace() << "SendOutbox: delivered token=" << token << " temp=" << tempId
                          << " attempts=" << e->postAttempts << " id="
                          << data.value(QStringLiteral("id")).toInt();
        const Message real = Message::fromJson(data);
        deliverReal(token, tempId, real);          // also drops the entry
        sendNextQueued(token);                      // the next one waiting behind it
        return;
    }
    if (seq != e->attemptSeq) {
        qInfo().nospace() << "SendOutbox: ignoring a stale failure token=" << token
                          << " temp=" << tempId << " status=" << status;
        return;
    }

    // A failure. NOT proof of non-delivery: Talk saves before it notifies the HPB,
    // so a timeout, a 5xx or a 400 can follow a message that is already stored.
    // The next step is therefore a history probe, never a blind re-POST.
    e->inFlight = false;
    e->slow = false;
    e->lastStatus = status;
    e->lastActivityMs = outboxNowMs();
    e->needFreshConnection = (status == 0);
    // How long it hung before failing: a slow failure means the server may still be
    // working on it, so the first check waits longer (SendRetryPolicy::kSlowFailureMs).
    const qint64 hungMs = qMax<qint64>(0, e->lastActivityMs - e->inFlightSinceMs);
    const auto outcome = talq::sendretry::onPostFailed(status, e->postAttempts, hungMs);
    if (outcome.giveUp) {
        e->permanent = true;
        qInfo().nospace() << "SendOutbox: giving up token=" << token << " temp=" << tempId
                          << " status=" << status << " attempts=" << e->postAttempts
                          << " (needs a manual retry)";
    } else {
        e->nextTryMs = e->lastActivityMs + outcome.nextTryDelayMs;
        qInfo().nospace() << "SendOutbox: attempt failed token=" << token << " temp=" << tempId
                          << " status=" << status << " attempts=" << e->postAttempts
                          << " next check in " << outcome.nextTryDelayMs << " ms";
    }
    m_sendQueue.remove(token);    // whatever was queued behind this waits behind it
    refreshRowStatus(tempId);
    syncOutboxTimer();
}

void MessageListModel::deliverReal(const QString &token, int tempId, const Message &real)
{
    dropOutboxEntry(tempId);
    // A room that is not open has nothing to redraw; opening it later reads the
    // message from the server like any other.
    if (m_token != token) return;

    int idx = -1;
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].id == tempId) { idx = i; break; }
    }
    // 0.41.3-beta — the optimistic temp may already be gone: the long-poll caught
    // the same message first and replaceTempByReferenceId() swapped it. If we
    // already hold the real id, we're done — no action needed.
    if (idx < 0) {
        if (m_messageIds.contains(real.id))
            return;   // poller delivered + dedup handled it
        // Edge case: temp gone, real not in model (rare, e.g. cache cleared or the
        // room reset mid-flight). Insert fresh so the user doesn't lose their
        // just-sent message -- behind any newer pending temps, which sort first.
        if (!passesThreadFilter(real)) return;
        int at = 0;
        while (at < m_messages.size() && m_messages[at].id < 0) ++at;
        m_messageIds.insert(real.id);
        beginInsertRows({}, at, at);
        m_messages.insert(at, real);
        endInsertRows();
        m_cache->saveMessages(m_token, {real});
        return;
    }

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
        return;
    }

    // Replace optimistic with real — update ID tracking
    m_messageIds.remove(tempId);
    m_messageIds.insert(real.id);
    m_messages[idx] = real;
    emit dataChanged(index(idx), index(idx));
    m_cache->saveMessages(m_token, {real});
    // Two unsent messages can be delivered newest-first; a real row must never
    // sit in front of a pending temp.
    enforceNewestFirstInvariant();
    // bug 7 — a brand-new group opened empty never started the poller (startPoller
    // bails when there is no message id to anchor the long-poll on). Now that the
    // first send has a real server id, start the live poll loop so peer replies
    // appear without the user having to re-select the room.
    if (m_poller && !m_poller->isPolling())
        startPoller();
}

void MessageListModel::sendNextQueued(const QString &token)
{
    auto it = m_sendQueue.find(token);
    if (it == m_sendQueue.end()) return;
    while (!it->isEmpty()) {
        const int id = it->takeFirst();
        const OutboxEntry *e = findEntry(id);
        if (!e || e->permanent || e->inFlight) continue;
        startAttempt(id);
        return;
    }
    m_sendQueue.erase(it);
}

void MessageListModel::outboxTick()
{
    const qint64 now = outboxNowMs();

    // Unsent text belongs to the SESSION. clearOutbox() is the prompt path (logout);
    // this is the net under it: an entry written by another account is never
    // probed, POSTed or kept.
    {
        const QString owner = outboxOwner();
        QVector<int> foreign;
        for (const auto &e : m_outbox)
            if (e.owner != owner) foreign.append(e.tempId);
        for (int id : foreign) {
            qInfo().nospace() << "SendOutbox: dropping an unsent message written by another account temp=" << id;
            dropOutboxEntry(id);
            removeTempRow(id);
        }
    }

    // The deadlines are wall-clock. A clock stepped back after a resume would leave
    // one hours away, and a stepped-back in-flight stamp a watchdog that never fires.
    for (auto &e : m_outbox) {
        e.nextTryMs = talq::sendretry::sanitizeNextTryMs(e.nextTryMs, now);
        if (e.inFlightSinceMs > now) e.inFlightSinceMs = now;
    }

    // Watchdog: an attempt whose callback never fires (impossible under the 30 s
    // transfer timeout, but the cost of being wrong is a message stuck forever).
    // Its attemptSeq moves on, so a callback that arrives late is stale: a
    // success is still honoured (the message exists), a failure is ignored.
    QVector<int> released;
    for (auto &e : m_outbox) {
        if (!e.inFlight || now - e.inFlightSinceMs <= talq::sendretry::kInFlightWatchdogMs)
            continue;
        qWarning().nospace() << "SendOutbox: attempt never answered, releasing token=" << e.token
                             << " temp=" << e.tempId << " attempt=" << e.postAttempts;
        e.inFlight = false;
        e.slow = false;
        e.attemptSeq = ++m_attemptCounter;
        e.lastStatus = 0;
        e.lastActivityMs = now;
        e.needFreshConnection = true;
        const auto o = talq::sendretry::onPostFailed(0, e.postAttempts,
                                                     now - e.inFlightSinceMs);
        if (o.giveUp) e.permanent = true;
        else          e.nextTryMs = now + o.nextTryDelayMs;
        m_sendQueue.remove(e.token);
        released.append(e.tempId);
    }
    for (int id : released) refreshRowStatus(id);

    // One probe per room, started only by that room's OLDEST active entry: the
    // ones behind it share its fate, so a retry never overtakes an older message.
    QSet<QString> seen;
    QStringList toProbe;
    for (const auto &e : m_outbox) {
        if (e.permanent || seen.contains(e.token)) continue;
        seen.insert(e.token);
        if (talq::sendretry::headReady(e.inFlight, e.permanent, e.nextTryMs, now))
            toProbe.append(e.token);
    }
    for (const QString &token : toProbe)
        probeToken(token);

    syncOutboxTimer();
}

void MessageListModel::probeToken(const QString &token)
{
    if (m_probing.contains(token)) return;

    // After a transport failure the pooled HTTP/2 connection is the likeliest
    // thing that died; the probe and the POST after it would queue onto it.
    bool freshConnection = false;
    for (auto &e : m_outbox) {
        if (e.token != token || e.permanent || e.inFlight) continue;
        if (e.needFreshConnection) { freshConnection = true; e.needFreshConnection = false; }
    }
    if (freshConnection)
        m_api->dropPooledConnections();

    m_probing.insert(token);

    // The newest page of the room's history. This one request is BOTH the "is the
    // connection back" test and the "did it already land" check. Same flags as
    // refreshLatest: never advance the read marker or clear notifications.
    QUrlQuery params;
    params.addQueryItem("lookIntoFuture", "0");
    params.addQueryItem("markNotificationsAsRead", "0");
    params.addQueryItem("limit", QString::number(kChatPageLimit));
    params.addQueryItem("setReadMarker", "0");
    qInfo().nospace() << "SendOutbox: checking history token=" << token
                      << (freshConnection ? " (fresh connection)" : "");
    const qint64 probeStart = outboxNowMs();
    QNetworkReply *reply = m_api->getRaw("apps/spreed/api/v1/chat/" + token, params);

    connect(reply, &QNetworkReply::finished, this, [this, reply, token, probeStart]() {
        m_probing.remove(token);
        reply->deleteLater();
        const qint64 now = outboxNowMs();
        const int httpStatus =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        // An empty history is HTTP 304 with no body (ChatController.php:1109) --
        // a real, conclusive answer. Anything else that is not a clean 200 with a
        // parsable envelope tells us nothing (a captive portal answers 200 with
        // HTML), so it must never be read as "the room is empty".
        QJsonArray page;
        bool answered = false;
        if (reply->error() == QNetworkReply::NoError) {
            if (httpStatus == 304) {
                answered = true;
            } else {
                const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
                const QJsonObject ocs = doc.object().value(QStringLiteral("ocs")).toObject();
                if (!doc.isNull() && ocs.value(QStringLiteral("data")).isArray()) {
                    page = ocs.value(QStringLiteral("data")).toArray();
                    answered = true;
                }
            }
        }

        if (!answered) {
            const bool giveUp = talq::sendretry::classifyProbeFailure(httpStatus)
                                == talq::sendretry::ProbeFailure::GiveUp;
            QVector<int> touched;
            for (auto &e : m_outbox) {
                if (e.token != token || e.permanent || e.inFlight) continue;
                e.lastActivityMs = now;
                if (giveUp) {
                    e.permanent = true;
                } else {
                    ++e.probeFailures;
                    e.nextTryMs = now + talq::sendretry::probeDelayMs(e.probeFailures);
                    if (httpStatus == 0) e.needFreshConnection = true;
                }
                touched.append(e.tempId);
            }
            qInfo().nospace() << "SendOutbox: history check failed token=" << token
                              << " http=" << httpStatus << " error=" << reply->errorString()
                              << (giveUp ? " -- giving up on this room's unsent messages"
                                         : " -- will try again");
            for (int id : touched) refreshRowStatus(id);
            syncOutboxTimer();
            return;
        }

        // The page, indexed by referenceId.
        QHash<QString, QJsonObject> byRef;
        for (const QJsonValue &v : page) {
            const QJsonObject o = v.toObject();
            const QString ref = o.value(QStringLiteral("referenceId")).toString();
            if (!ref.isEmpty()) byRef.insert(ref, o);
        }
        // How far back the window REALLY reaches. Not the number of messages on the
        // page: Talk applies `limit` to the raw comments and only then drops the ones
        // the caller may not see, so a full window can come back short. X-Chat-Last-Given
        // is the id of the oldest RAW comment in the window (ChatController.php:1212-1214).
        // No header and no messages means the room has no comments at all (the 200-with-[]
        // form of "empty"; the usual form is the 304 above).
        const qint64 lastGiven = reply->rawHeader("X-Chat-Last-Given").toLongLong();
        const bool emptyHistory = httpStatus == 304 || (page.isEmpty() && lastGiven == 0);

        // Decide for every entry of this room first, apply afterwards: delivering
        // removes entries, which must not happen while we walk the list.
        QVector<QPair<int, Message>> delivered;
        QVector<int> toSend, giveUp;
        for (auto &e : m_outbox) {
            if (e.token != token || e.permanent || e.inFlight) continue;
            // Something happened to it while this request was out (its attempt
            // finished, or it was only just queued): the page predates that, and
            // judging it against the page would be judging it against stale data.
            // The next probe takes it.
            if (e.lastActivityMs > probeStart) continue;
            e.probeFailures = 0;
            e.lastActivityMs = now;
            const bool found = byRef.contains(e.referenceId);
            const auto verdict = talq::sendretry::probeVerdict(
                found, emptyHistory, lastGiven, e.knownNewestId);
            switch (talq::sendretry::afterProbe(verdict, e.manual, e.everPosted)) {
            case talq::sendretry::AfterProbe::MarkDelivered:
                delivered.append({e.tempId, Message::fromJson(byRef.value(e.referenceId))});
                break;
            case talq::sendretry::AfterProbe::Resend:
                toSend.append(e.tempId);
                break;
            case talq::sendretry::AfterProbe::GiveUp:
                e.permanent = true;
                giveUp.append(e.tempId);
                break;
            }
        }

        qInfo().nospace() << "SendOutbox: history checked token=" << token
                          << " page=" << page.size() << " lastGiven=" << lastGiven
                          << (emptyHistory ? " (empty room)" : "")
                          << " delivered=" << delivered.size() << " resend=" << toSend.size()
                          << " unclear=" << giveUp.size();
        for (int id : giveUp) {
            qInfo().nospace() << "SendOutbox: history window does not provably cover temp=" << id
                              << " -- not re-sending automatically (could duplicate); Retry forces it";
            refreshRowStatus(id);
        }
        for (const auto &d : delivered) {
            qInfo().nospace() << "SendOutbox: already on the server token=" << token
                              << " temp=" << d.first << " id=" << d.second.id << " -- not re-sending";
            deliverReal(token, d.first, d.second);
        }
        if (!toSend.isEmpty()) {
            m_sendQueue[token] = QList<int>(toSend.begin(), toSend.end());
            sendNextQueued(token);
        }
        syncOutboxTimer();
    });
}

void MessageListModel::kickOutbox()
{
    const qint64 now = outboxNowMs();
    bool any = false;
    for (auto &e : m_outbox) {
        if (e.permanent || e.inFlight) continue;
        e.nextTryMs = talq::sendretry::kickedNextTryMs(e.nextTryMs, now, e.lastActivityMs);
        any = true;
    }
    if (!any) return;
    qInfo() << "SendOutbox: recovery hint -- unsent messages will be checked shortly";
    syncOutboxTimer();     // the 2 s tick picks up the pulled-forward deadline
}

void MessageListModel::retryMessage(int tempId)
{
    OutboxEntry *e = findEntry(tempId);
    if (!e) return;
    if (e->inFlight) {
        qInfo().nospace() << "SendOutbox: Retry ignored, attempt still in flight temp=" << tempId;
        return;
    }
    // Retry means "check, then send if it is not there" -- the user cannot know
    // whether the failed attempt landed, and neither can we. It starts a fresh
    // attempt budget, forgives a permanent classification once, and lets an
    // inconclusive history window through (manual), since otherwise a message in
    // a very busy room could never be re-sent at all.
    e->permanent = false;
    e->manual = true;
    e->postAttempts = 0;
    e->probeFailures = 0;
    e->nextTryMs = 0;
    e->lastActivityMs = outboxNowMs();
    // "Now" is for the room's whole queue: a message behind an older one that is
    // waiting out a backoff would otherwise sit until that deadline, because only
    // the oldest active entry of a room may start a check.
    for (auto &o : m_outbox)
        if (o.token == e->token && !o.permanent && !o.inFlight) o.nextTryMs = 0;
    qInfo().nospace() << "SendOutbox: manual retry token=" << e->token << " temp=" << tempId;
    refreshRowStatus(tempId);
    syncOutboxTimer();
    QTimer::singleShot(0, this, &MessageListModel::outboxTick);
}

void MessageListModel::discardFailed(int tempId)
{
    if (!findEntry(tempId)) return;
    qInfo().nospace() << "SendOutbox: discarded by the user temp=" << tempId;
    dropOutboxEntry(tempId);
    removeTempRow(tempId);
}

void MessageListModel::removeTempRow(int tempId)
{
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].id != tempId) continue;
        beginRemoveRows({}, i, i);
        m_messageIds.remove(tempId);
        m_messages.removeAt(i);
        endRemoveRows();
        return;
    }
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

    // 0.41.3-beta — referenceId for upstream-compatible dedup. SHA-256
    // hex of a unique tag (mirrors `prepareTemporaryMessage.ts:96-120`).
    // The server echoes the same referenceId back on (a) the POST
    // response with the real numeric id, AND (b) the long-poll event
    // that arrives concurrently for the same own message. Without
    // this key, back-to-back sends + late POST callbacks raced and
    // the FIRST optimistic could be erased — the field bug.
    //
    // It identifies our message; it does NOT make a second POST harmless -- Talk
    // 24 stores a duplicate (no server-side dedup). A re-send therefore goes
    // through the outbox's history check first (see the header).
    const QByteArray refSeed = QByteArray("talq-")
        + QByteArray::number(QDateTime::currentMSecsSinceEpoch())
        + QByteArray("-")
        + QByteArray::number(tempId);
    const QString referenceId = QString::fromLatin1(
        QCryptographicHash::hash(refSeed, QCryptographicHash::Sha256).toHex());

    // Everything needed to send this message again, exactly as it is sent now.
    OutboxEntry entry;
    entry.token = m_token;
    entry.owner = outboxOwner();
    // The newest server id this room has right now. A message the server saves
    // from this send on gets a larger one, which is what lets a later history
    // window prove it covers the send (SendRetryPolicy::probeVerdict).
    entry.knownNewestId = newestRealId();
    entry.tempId = tempId;
    entry.text = actualText;
    entry.referenceId = referenceId;
    entry.replyToId = replyToId > 0 ? replyToId : 0;
    entry.silent = silent;
    entry.threadId = m_threadId > 0 ? m_threadId : 0;
    entry.sentAtSecs = QDateTime::currentSecsSinceEpoch();
    entry.sentAtMs = entry.lastActivityMs = outboxNowMs();

    // bug 4 — populate the optimistic message's reply parent from the target
    // already in the model, so the quote renders IMMEDIATELY rather than only
    // after the server echo replaces the temp. The echo later refines replyTo
    // with the server's full parent object.
    if (replyToId > 0) {
        for (const auto &p : m_messages) {
            if (p.id != replyToId) continue;
            QJsonObject r;
            r[QStringLiteral("id")]               = p.id;
            r[QStringLiteral("actorId")]          = p.actorId;
            r[QStringLiteral("actorType")]        = p.actorType;
            r[QStringLiteral("actorDisplayName")] = p.actorDisplayName;
            r[QStringLiteral("message")]          = p.message;
            entry.replyTo = r;
            break;
        }
    }

    // An older unsent message in this room is still waiting to be retried: sending
    // this one straight away would land it AHEAD of that one. Queue behind it
    // instead; the outbox sends both, oldest first. An older message that is
    // merely in flight (the user typing quickly) does not hold this one back.
    bool behindOlder = false;
    for (const auto &o : m_outbox) {
        if (o.token == m_token
            && talq::sendretry::blocksNewSend(o.inFlight, o.permanent)) {
            behindOlder = true;
            break;
        }
    }

    m_outbox.append(entry);
    const Message optimistic = tempMessageFor(m_outbox.last());

    // Prepend at index 0 (newest-first: new = front)
    m_messageIds.insert(tempId);
    beginInsertRows({}, 0, 0);
    m_messages.prepend(optimistic);
    endInsertRows();

    emit messageSent();
    m_latestArrivedRow = 0;      // our own temp, which is the front row
    emit newMessagesAtEnd();

    // 0.41.2-beta — diagnostic. We're chasing a "zero messages in
    // Refunds thread" field bug where one client's messages don't
    // appear under another client's topic tab. Log the threadId we're
    // actually attaching to the send so the server-side state can be
    // compared against the receiver's topic filter.
    qInfo().nospace() << "MessageListModel: sendMessage token=" << m_token
                      << " threadId=" << m_threadId
                      << " replyTo=" << replyToId
                      << " len=" << actualText.length()
                      << (behindOlder ? " (queued behind an older unsent message)" : "");

    syncOutboxTimer();
    if (behindOlder)
        kickOutbox();
    else
        startAttempt(tempId);
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

void MessageListModel::applyReactionSystemMessage(const QJsonObject &systemMessageJson)
{
    // bug 8 — Nextcloud Talk delivers a reaction by another user as a system
    // message (systemMessage == "reaction" / "reaction_revoked") whose `parent`
    // is the target comment, carrying the authoritative updated reactions map.
    // Previously these events were dropped at ingestion and the target's
    // reactions never updated (and stayed missing across reopen/restart because
    // the cached snapshot was never refreshed). Apply the delta via the same
    // path the local addReaction uses — which updates m_messages, rawJson, the
    // ReactionsRole, and the cache. No-op if the parent/map is absent or the
    // target isn't currently loaded.
    const QJsonObject parent = systemMessageJson.value(QStringLiteral("parent")).toObject();
    const int targetId = parent.value(QStringLiteral("id")).toInt();
    if (targetId <= 0 || !parent.contains(QStringLiteral("reactions")))
        return;
    updateReactions(targetId, parent.value(QStringLiteral("reactions")).toObject());
}

// Control rows are EVENTS, not content. Reactions, edits and deletions all
// arrive as ordinary-looking entries in the same arrays as real messages, and
// each one carries an authoritative update for a message we may already be
// holding -- in memory, in the cache, or both.
//
// Every receive path used to filter them out with its own copy of the same
// condition, and the copies had drifted: the live poll applied a reaction but a
// history page silently dropped it, an edit was discarded everywhere, and a
// deletion never reached the cache from any path at all. One funnel instead, so
// a rule fixed here is fixed on every path that ingests server data.
//
// Returns true when the row was absorbed and must not be admitted as content.
bool MessageListModel::absorbControlMessage(const QJsonObject &obj, const Message &m)
{
    if (m.isReactionMessage()) {
        applyReactionSystemMessage(obj);
        return true;
    }
    if (m.isEditMessage()) {
        applyEditSystemMessage(obj);
        return true;
    }
    if (m.isDeletedMessage()) {
        // "message_deleted" names its victim in `parent`; a "comment_deleted"
        // row IS the victim, already replaced by its tombstone.
        int target = m.id;
        if (m.systemMessage == QLatin1String("message_deleted"))
            target = obj.value(QStringLiteral("parent")).toObject()
                        .value(QStringLiteral("id")).toInt();
        removeMessageById(target);
        return true;
    }
    if (m.isCallJoinLeave())
        return true;
    return false;
}

void MessageListModel::applyEditSystemMessage(const QJsonObject &systemMessageJson)
{
    // Mirrors applyReactionSystemMessage: the authoritative new state rides on
    // `parent`, so re-parse that and replace the loaded copy in place, then
    // write it back to the cache so a restart does not resurrect the old text.
    const QJsonObject parent = systemMessageJson.value(QStringLiteral("parent")).toObject();
    const int targetId = parent.value(QStringLiteral("id")).toInt();
    if (targetId <= 0)
        return;

    const Message edited = Message::fromJson(parent);

    int idx = -1;
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].id == targetId) { idx = i; break; }
    }
    if (idx < 0) {
        // Not loaded right now. The cache still holds the stale body, and
        // nothing later would correct it, so overwrite that copy directly --
        // saveMessages keys on (token, message_id), so this replaces it.
        if (m_cache && !m_token.isEmpty())
            m_cache->saveMessages(m_token, {edited});
        return;
    }

    m_messages[idx] = edited;
    emit dataChanged(index(idx), index(idx),
                     {MessageTextRole, LastEditTimestampRole});
    if (m_cache && !m_token.isEmpty())
        m_cache->saveMessages(m_token, {m_messages[idx]});
}

void MessageListModel::removeMessageById(int id)
{
    if (id <= 0)
        return;

    // Purge the CACHE first, and unconditionally.
    //
    // Two bugs lived in the old guard `if (!m_messageIds.contains(id)) return;`.
    // It dropped the row from m_messages and nothing else, so SQLite kept the
    // deleted message forever and handed it straight back on the next load --
    // a message deleted on the server reappeared in the chat after a restart.
    // And because the guard returned early when the target was not currently
    // loaded, a deletion for anything scrolled out of the window (or trimmed by
    // trimOldMessages) was discarded entirely, so the cache was never told at
    // all -- that copy could never be reclaimed by any later event.
    //
    // The reaction path already learned this exact lesson; see
    // applyReactionSystemMessage, whose comment records reactions "stayed
    // missing across reopen/restart because the cached snapshot was never
    // refreshed". Deletion is the same shape of bug with a worse symptom:
    // content the sender explicitly retracted staying on someone's screen.
    if (m_cache && !m_token.isEmpty())
        m_cache->deleteMessage(m_token, id);

    if (!m_messageIds.contains(id))
        return;                       // not on screen; the cache purge was the point
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].id == id) {
            beginRemoveRows(QModelIndex(), i, i);
            m_messages.removeAt(i);
            endRemoveRows();
            m_messageIds.remove(id);
            return;
        }
    }
}

void MessageListModel::clearLocalHistory()
{
    qInfo() << "MessageListModel: clearing local history for" << m_token;
    if (!m_messages.isEmpty()) {
        beginResetModel();
        m_messages.clear();
        m_messageIds.clear();
        endResetModel();
    } else {
        m_messageIds.clear();
    }
    m_oldestMessageId = 0;
    m_unreadBoundary = 0;
    m_hasMoreHistory = false;
    reinsertOutboxTemps();   // a cleared room does not cancel what we still owe it
    if (m_cache && !m_token.isEmpty())
        m_cache->clearConversation(m_token);
    emit unreadBoundaryChanged();
    emit hasMoreHistoryChanged();
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

    const qint64 fileSize = file.size();

    // Files at/above the threshold stream via chunked upload (Nextcloud v2) so
    // we never readAll() the whole thing into memory or block the main thread.
    // Default boundary is 100 MB (the old hard reject limit); TALQ_CHUNK_-
    // THRESHOLD_BYTES lowers it so the harness can exercise chunking on a small
    // file deterministically.
    qint64 chunkThreshold = 100LL * 1024 * 1024;
    if (qEnvironmentVariableIsSet("TALQ_CHUNK_THRESHOLD_BYTES"))
        chunkThreshold = qMax<qint64>(1, qEnvironmentVariable("TALQ_CHUNK_THRESHOLD_BYTES").toLongLong());

    // Show upload progress
    m_uploadProgress = 0;
    m_uploadFileName = fileName;
    emit uploadProgressChanged();

    if (fileSize >= chunkThreshold) {
        file.close();   // uploadFileChunked re-opens and streams it in slices
        uploadFileChunked(readPath, fileName, m_token, caption, tempCopy);
        return;
    }

    // Small file: a single WebDAV PUT of the whole body is fine under the
    // threshold.
    QByteArray fileData = file.readAll();
    file.close();
    if (!tempCopy.isEmpty())
        QFile::remove(tempCopy);

    qDebug() << "Uploading file:" << fileName << "(" << fileData.size() << "bytes)";

    QString uploadPath = "/remote.php/dav/files/" + m_api->user()
                         + "/" + attachmentFolder() + "/" + fileName;
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
        shareUploadedFile(fileName, token, caption);
    });
}

// Where uploads go, and where their share path is rooted. The user can set
// this server-side (capabilities config.attachments.folder); TalQ hard-coded
// "Talk" until 0.65.3 and so ignored the choice. All three call sites -- the
// WebDAV upload path, the share body, and the chunked-upload destination --
// must agree, which is why they all come through here.
QString MessageListModel::attachmentFolder() const
{
    QString f = m_attachmentFolder.isEmpty() ? QStringLiteral("Talk") : m_attachmentFolder;
    while (f.startsWith(QLatin1Char('/'))) f.remove(0, 1);
    while (f.endsWith(QLatin1Char('/')))   f.chop(1);
    return f.isEmpty() ? QStringLiteral("Talk") : f;
}

// Build the exact body to POST when forwarding `messageId`.
//
// Reads `rawJson`, deliberately NOT Message::message: fromJson() rewrites that
// field IN PLACE into rendered html (escape -> markdown -> placeholder
// substitution -> linkify), so by the time anything else sees it the original
// markup is gone. Forwarding used to take that html and flatten it further with
// QTextDocument::toPlainText(), which is why a forwarded message arrived as
// plain text with every marker eaten. rawJson is the server's own bytes and the
// only lossless copy we keep.
QString MessageListModel::rawBodyFor(int messageId) const
{
    for (const Message &m : m_messages) {
        if (m.id != messageId) continue;

        const QString raw = m.rawJson.value("message").toString();
        if (raw.isEmpty()) return QString();   // optimistic send: no server copy yet

        std::vector<talq::MentionParam> mentions;
        std::vector<std::string> otherKeys;
        const QJsonObject params = m.rawJson.value("messageParameters").toObject();
        for (auto it = params.begin(); it != params.end(); ++it) {
            const QJsonObject p = it.value().toObject();
            if (it.key().startsWith(QLatin1String("mention"))) {
                mentions.push_back({ it.key().toStdString(),
                                     p.value("id").toString().toStdString(),
                                     p.value("name").toString().toStdString() });
            } else {
                otherKeys.push_back(it.key().toStdString());
            }
        }

        // A poll / deck card / shared file is not prose. Upstream re-shares
        // those through their own endpoint; sending the raw "{object}" would
        // deliver literal braces, so hand the caller nothing and let it stay on
        // whatever path already handles them.
        if (talq::carriesRichObject(raw.toStdString(), otherKeys))
            return QString();

        // No attribution here -- this is the SOURCE text. forwardBodyFor() adds
        // the "Forwarded from" line; editing must not.
        return QString::fromStdString(
            talq::forwardBody(raw.toStdString(), mentions, std::string()));
    }
    return QString();   // not in the model
}

// The body to POST when forwarding: the source text with an attribution line.
QString MessageListModel::forwardBodyFor(int messageId) const
{
    const QString body = rawBodyFor(messageId);
    if (body.isEmpty()) return QString();

    QString author;
    for (const Message &m : m_messages)
        if (m.id == messageId) { author = m.actorDisplayName; break; }

    return QString::fromStdString(
        talq::forwardBody(body.toStdString(), {}, author.toStdString()));
}

// Forward an existing attachment by re-sharing its path into another
// conversation. Talk 24 has no forward endpoint, so this is what forwarding a
// file means; `path` comes from the file rich-object on the source message.
void MessageListModel::shareExistingFile(const QString &path, const QString &token)
{
    if (path.isEmpty() || token.isEmpty()) return;
    QJsonObject body;
    body["shareType"]   = 10;          // share to a Talk conversation
    body["shareWith"]   = token;
    body["path"]        = path;
    body["permissions"] = 1;           // read-only for recipients
    m_api->post("apps/files_sharing/api/v1/shares", body,
        [this, token](bool ok, const QJsonObject &, int) {
            if (!ok) {
                emit errorOccurred(tr("Could not forward the attachment."));
                return;
            }
            if (m_token == token) refreshLatest();
        });
}

void MessageListModel::shareUploadedFile(const QString &fileName, const QString &token,
                                         const QString &caption)
{
    qDebug() << "File uploaded, sharing to conversation" << token;

    QJsonObject body;
    body["shareType"] = 10;  // share to Talk conversation
    body["shareWith"] = token;
    body["path"] = attachmentFolder() + "/" + fileName;
    body["permissions"] = 1;  // read permission for recipients

    // talkMetaData carries everything about the share that is not the file
    // itself. Until 0.65.3 only the caption was sent, so attaching a file while
    // a topic was open dropped it into the ROOM ROOT instead of the topic —
    // silently, in the feature 0.65.x had just shipped. The user saw their
    // upload vanish from the topic they were looking at.
    QJsonObject metaData;
    if (!caption.isEmpty())
        metaData["caption"] = caption;            // capability: media-caption
    // ⚠ Only when the open conversation IS the one being shared into. Uploads
    // complete asynchronously, so the user can have moved on by the time this
    // runs — and m_threadId belongs to whatever is open NOW. Without the
    // token check, a share finishing after a room switch would be filed under
    // a topic id from a different conversation.
    // Read server-side by Chat/SystemMessage/Listener.php:435.
    if (m_threadId > 0 && m_token == token)
        metaData["threadId"] = m_threadId;        // capability: threads

    // A voice message is an ordinary audio attachment PLUS this flag; without
    // it every client renders a file pill instead of a player. The server only
    // honours it for audio/mpeg or audio/wav (ChatController.php:2556), which
    // is why VoiceRecorder writes mp3 and not the opus used elsewhere.
    //
    // Keyed by FILENAME rather than a member flag: uploads complete out of
    // order, so a flag set at send time can be read by whichever upload
    // happens to finish next.
    if (m_voiceMessageFiles.remove(fileName))
        metaData["messageType"] = QStringLiteral("voice-message");
    if (!metaData.isEmpty())
        body["talkMetaData"] = QString::fromUtf8(QJsonDocument(metaData).toJson(QJsonDocument::Compact));

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
}

// Chunked-upload state. Held alive only by the in-flight QNetworkReply callback;
// when the last reply completes the shared_ptr drops and the file handle closes
// — no self-referential std::function, so nothing leaks.
struct ChunkUploadState {
    QFile   file;
    qint64  size = 0;
    qint64  offset = 0;
    int     index = 0;
    qint64  chunkSize = 10LL * 1024 * 1024;   // 10 MB (Nextcloud allows 5 MB–5 GB)
    QString uploadFolder;                     // /remote.php/dav/uploads/<user>/<uuid>
    QString destUrl;                          // full URL of the final dav/files target
    QString fileName, token, caption, tempCopy;
};

void MessageListModel::uploadFileChunked(const QString &readPath, const QString &fileName,
                                         const QString &token, const QString &caption,
                                         const QString &tempCopy)
{
    // Per-upload state shared across the async MKCOL → chunk PUTs → MOVE chain.
    auto st = std::make_shared<ChunkUploadState>();
    st->file.setFileName(readPath);
    if (!st->file.open(QIODevice::ReadOnly)) {
        emit errorOccurred("Cannot open file for upload: " + st->file.errorString());
        if (!tempCopy.isEmpty()) QFile::remove(tempCopy);
        m_uploadProgress = -1; m_uploadFileName.clear(); emit uploadProgressChanged();
        return;
    }
    st->size = st->file.size();
    st->fileName = fileName; st->token = token; st->caption = caption; st->tempCopy = tempCopy;
    if (qEnvironmentVariableIsSet("TALQ_CHUNK_SIZE_BYTES"))
        st->chunkSize = qMax<qint64>(1, qEnvironmentVariable("TALQ_CHUNK_SIZE_BYTES").toLongLong());

    const QString uuid = QUuid::createUuid().toString(QUuid::Id128);  // 32 hex chars, no braces
    st->uploadFolder = "/remote.php/dav/uploads/" + m_api->user() + "/talq-" + uuid;
    st->destUrl = m_api->serverUrl()
                + "/remote.php/dav/files/" + m_api->user()
                + "/" + attachmentFolder() + "/" + fileName;

    qDebug() << "Chunked upload:" << fileName << "(" << st->size << "bytes, "
             << st->chunkSize << "B chunks) ->" << st->uploadFolder;

    // Ensure the destination parent (…/Talk) exists, then MKCOL the upload
    // session folder, then start sending chunks. Each MKCOL accepts 201
    // (created) or 405 (already exists). Creating Talk/ defensively avoids the
    // 409-on-MOVE when a fresh account has never had the folder auto-created.
    const QString destParent = "/remote.php/dav/files/" + m_api->user() + "/Talk";
    auto *mkParent = m_api->davRequest("MKCOL", destParent);
    connect(mkParent, &QNetworkReply::finished, this, [this, mkParent, st]() {
        mkParent->deleteLater();
        const int ps = mkParent->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (ps != 201 && ps != 405) {
            qWarning() << "Chunked upload: destination-parent MKCOL failed:" << ps;
            failChunkedUpload(st, "Failed to prepare upload destination");
            return;
        }
        auto *mk = m_api->davRequest("MKCOL", st->uploadFolder);
        connect(mk, &QNetworkReply::finished, this, [this, mk, st]() {
            mk->deleteLater();
            const int status = mk->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status != 201 && status != 405) {
                qWarning() << "Chunked upload MKCOL failed:" << status << mk->errorString();
                failChunkedUpload(st, "Failed to start chunked upload");
                return;
            }
            uploadNextChunk(st);   // keeps st alive only via the in-flight reply
        });
    });
}

void MessageListModel::failChunkedUpload(const std::shared_ptr<ChunkUploadState> &st,
                                         const QString &msg)
{
    st->file.close();
    if (!st->tempCopy.isEmpty()) QFile::remove(st->tempCopy);
    m_uploadProgress = -1; m_uploadFileName.clear(); emit uploadProgressChanged();
    emit errorOccurred(msg);
}

void MessageListModel::uploadNextChunk(std::shared_ptr<ChunkUploadState> st)
{
    if (st->offset >= st->size) {
        // All chunks uploaded — MOVE .file to assemble at the destination. The
        // Destination/OC-Total-Length headers go ONLY on the MOVE: empirically
        // this Nextcloud rejects a Destination header on the chunk PUTs with
        // 404 (the v2 doc says to send it, but the deployed server does not).
        QMap<QByteArray, QByteArray> moveHeaders;
        moveHeaders["Destination"]     = st->destUrl.toUtf8();
        moveHeaders["OC-Total-Length"] = QByteArray::number(st->size);
        auto *mv = m_api->davRequest("MOVE", st->uploadFolder + "/.file",
                                     QByteArray(), moveHeaders);
        connect(mv, &QNetworkReply::finished, this, [this, st, mv]() {
            mv->deleteLater();
            const int status = mv->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status != 201 && status != 204) {
                qWarning() << "Chunked upload MOVE failed:" << status << mv->errorString();
                failChunkedUpload(st, "Failed to assemble uploaded file");
                return;
            }
            st->file.close();
            if (!st->tempCopy.isEmpty()) QFile::remove(st->tempCopy);
            m_uploadProgress = 1.0; emit uploadProgressChanged();
            shareUploadedFile(st->fileName, st->token, st->caption);
        });
        return;
    }

    const qint64 thisLen = qMin(st->chunkSize, st->size - st->offset);
    st->file.seek(st->offset);
    const QByteArray chunk = st->file.read(thisLen);
    if (chunk.size() != thisLen) {
        failChunkedUpload(st, "Failed to read file for upload");
        return;
    }
    ++st->index;
    const QString chunkName = QStringLiteral("%1").arg(st->index, 5, 10, QLatin1Char('0'));
    const qint64 base = st->offset;
    auto *put = m_api->davRequest("PUT", st->uploadFolder + "/" + chunkName, chunk);
    connect(put, &QNetworkReply::uploadProgress, this, [this, st, base](qint64 sent, qint64) {
        if (st->size > 0) {
            m_uploadProgress = static_cast<double>(base + sent) / st->size;
            emit uploadProgressChanged();
        }
    });
    connect(put, &QNetworkReply::finished, this, [this, st, put, thisLen]() {
        put->deleteLater();
        const int status = put->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 201 && status != 204 && status != 200) {
            qWarning() << "Chunk PUT failed:" << status << put->errorString();
            failChunkedUpload(st, "Failed to upload file chunk");
            return;
        }
        st->offset += thisLen;
        uploadNextChunk(st);
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

// ── Downloading an attachment ────────────────────────────────────────────
//
// Until 0.69.7 this issued GET /index.php/f/<id>/download, which is not a
// route Nextcloud has ever declared (apps/files/appinfo/routes.php declares
// only /f/{fileid} -> View#showFile). It returned 404 on every request, for
// every file, for every user, and the error branch then quietly opened
// /f/<id> in a browser -- so "Download" reliably produced the Nextcloud web
// UI and never a file. The silent hand-off is why it went unnoticed: it was
// byte-identical to the "Open in Nextcloud" menu entry sitting next to it, so
// the failure was indistinguishable from a different action succeeding.
//
// The route that works is the user's own WebDAV home. Talk resolves the file
// rich-object's `path` against the REQUESTING user's folder, so a file another
// participant uploaded is reachable at the recipient's own DAV path -- no
// share juggling, no owner lookup.
//
// `fileId` stays the key the callers pass, because it is the only durable one:
// `path` is a snapshot taken when the message was parsed and goes stale the
// moment anyone renames or moves the file, while the id is valid forever. So
// the path is looked up from the model here rather than threaded through the
// UI, and when it is missing or stale the id is resolved back to a live path
// over DAV SEARCH before giving up.
//
// Deliberately NOT used: the OCS direct-download API (POST
// apps/dav/api/v1/direct). It works, but it mints an unauthenticated, reusable
// URL valid for eight hours, and it is gated on link sharing being enabled
// server-wide -- a security posture change and an admin-toggleable dependency,
// in exchange for nothing this route does not already do.
// Send a recording as a voice message. The upload is the ordinary attachment
// path; the only difference is the messageType flag added at share time.
void MessageListModel::sendVoiceMessage(const QString &filePath)
{
    if (filePath.isEmpty() || !QFileInfo::exists(filePath))
        return;
    m_voiceMessageFiles.insert(QFileInfo(filePath).fileName());
    sendFileWithCaption(filePath, QString());
}

void MessageListModel::downloadFile(int fileId, const QString &fileName,
                                    bool openWhenDone)
{
    startFileFetch(fileId, fileName, openWhenDone, QString());
}

void MessageListModel::saveFileAs(int fileId, const QString &fileName,
                                  const QString &destPath)
{
    if (destPath.isEmpty()) return;
    startFileFetch(fileId, fileName, /*openWhenDone*/ false, destPath);
}

void MessageListModel::startFileFetch(int fileId, const QString &fileName,
                                      bool openWhenDone, const QString &destPath)
{
    if (fileId <= 0 || fileName.isEmpty()) return;

    withDavPath(fileId,
        [this, fileId, fileName, openWhenDone, destPath](const QString &path) {
        if (path.isEmpty()) {
            emit errorOccurred(tr("Could not locate \"%1\" on the server.").arg(fileName));
            return;
        }
        fetchDavFile(path, fileId, fileName, openWhenDone, destPath);
    });
}

void MessageListModel::withDavPath(int fileId, std::function<void(const QString &)> done)
{
    // The path the message was parsed with, if this message is still loaded.
    QString davPath;
    for (const Message &m : m_messages) {
        if (m.fileId == fileId) {
            davPath = m.filePath;
            break;
        }
    }

    // A `path` that carries no directory component is the server's degraded
    // answer for a share it could not resolve into the viewer's home (see the
    // guest / allowInaccurate branches of SystemMessage.php). It is a display
    // name, not a location, so it must not be turned into a DAV URL.
    if (!davPath.contains(QLatin1Char('/')))
        davPath.clear();

    if (davPath.isEmpty()) {
        resolveDavPathById(fileId, std::move(done));
        return;
    }
    done(davPath);
}

void MessageListModel::fetchFileBytes(int fileId, qint64 maxBytes, QObject *context,
                                      std::function<void(const QByteArray &, const QString &)> done)
{
    if (fileId <= 0) {
        done({}, QStringLiteral("no file id"));
        return;
    }
    const QPointer<QObject> ctx(context);
    withDavPath(fileId, [this, ctx, maxBytes, done](const QString &path) {
        if (!ctx) return;
        if (path.isEmpty()) {
            done({}, QStringLiteral("not found on the server"));
            return;
        }
        const QString url = QStringLiteral("/remote.php/dav/files/") + m_api->davUser()
                            + QLatin1Char('/') + ApiClient::encodeDavPath(path);
        // Inactivity, not a total cap: a large original crawling over a slow
        // link keeps going, a black-holed one fails into the caller's fallback
        // instead of hanging there for good.
        QNetworkReply *reply = m_api->getAbsoluteUrl(url, 30'000);
        ApiClient::bindReplyLifetime(reply, ctx.data());

        // Counted as it streams, so an oversized file is dropped at the cap
        // rather than buffered whole first.
        auto body = std::make_shared<QByteArray>();
        auto overCap = std::make_shared<bool>(false);
        connect(reply, &QNetworkReply::readyRead, ctx.data(), [reply, body, overCap, maxBytes]() {
            if (*overCap) return;
            body->append(reply->readAll());
            if (body->size() > maxBytes) {
                *overCap = true;
                body->clear();
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::finished, ctx.data(), [reply, body, overCap, maxBytes, done]() {
            if (*overCap) {
                done({}, QStringLiteral("larger than %1 bytes").arg(maxBytes));
                return;
            }
            if (reply->error() != QNetworkReply::NoError) {
                done({}, reply->errorString());
                return;
            }
            body->append(reply->readAll());   // whatever readyRead did not take
            if (body->size() > maxBytes) {
                done({}, QStringLiteral("larger than %1 bytes").arg(maxBytes));
                return;
            }
            done(*body, QString());
        });
    });
}

// Resolve a fileId back to a path in the current user's DAV home. Nextcloud
// exposes no /f/<id> alias over DAV and its REPORT filter only understands
// system tags, but the SEARCH backend declares oc:fileid as a searchable
// property -- so one authenticated round trip recovers the live href.
void MessageListModel::resolveDavPathById(int fileId,
                                          std::function<void(const QString &)> done)
{
    const QString body = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<d:searchrequest xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\">"
        "<d:basicsearch>"
        "<d:select><d:prop><oc:fileid/></d:prop></d:select>"
        "<d:from><d:scope><d:href>/files/%1</d:href><d:depth>infinity</d:depth></d:scope></d:from>"
        "<d:where><d:eq><d:prop><oc:fileid/></d:prop><d:literal>%2</d:literal></d:eq></d:where>"
        "<d:limit><d:nresults>1</d:nresults></d:limit>"
        "</d:basicsearch></d:searchrequest>")
        .arg(m_api->davUser(), QString::number(fileId));

    auto *reply = m_api->davRequest("SEARCH", QStringLiteral("/remote.php/dav/"),
                                    body.toUtf8(),
                                    {{"Content-Type", "text/xml; charset=utf-8"}});

    connect(reply, &QNetworkReply::finished, this, [this, reply, done]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "DAV SEARCH by fileid failed:" << reply->errorString();
            done(QString());
            return;
        }

        // The response href is already percent-encoded and prefixed with the
        // DAV root; hand back the portion below the user's home so the caller
        // can treat it exactly like a `path` from a message.
        const QString xml = QString::fromUtf8(reply->readAll());
        const QString prefix = QStringLiteral("/remote.php/dav/files/")
                               + m_api->davUser() + QLatin1Char('/');
        const int at = xml.indexOf(prefix);
        if (at < 0) {
            done(QString());
            return;
        }
        const int end = xml.indexOf(QLatin1Char('<'), at);
        if (end < 0) {
            done(QString());
            return;
        }
        done(QUrl::fromPercentEncoding(
            xml.mid(at + prefix.size(), end - at - prefix.size()).toUtf8()));
    });
}

void MessageListModel::fetchDavFile(const QString &davPath, int fileId,
                                    const QString &fileName, bool openWhenDone,
                                    const QString &destPath)
{
    const QString url = QStringLiteral("/remote.php/dav/files/") + m_api->davUser()
                        + QLatin1Char('/') + ApiClient::encodeDavPath(davPath);
    auto *reply = m_api->getAbsoluteUrl(url);

    // Choose the destination BEFORE the bytes arrive and stream into it. The
    // previous implementation buffered the whole reply with readAll(), which
    // holds an entire video in memory to write it straight back out.
    //
    // An explicit destPath is a place the user just picked in a save dialog,
    // so it is written as given -- they have already answered the overwrite
    // question. Only the implicit Downloads destination gets de-duplicated.
    const QString savePath = destPath.isEmpty() ? uniqueDownloadPath(fileName) : destPath;
    if (savePath.isEmpty()) {
        reply->abort();
        reply->deleteLater();
        emit errorOccurred(tr("Cannot write to the Downloads folder."));
        return;
    }

    auto *out = new QFile(savePath);
    if (!out->open(QIODevice::WriteOnly)) {
        delete out;
        reply->abort();
        reply->deleteLater();
        emit errorOccurred(tr("Cannot save \"%1\".").arg(QFileInfo(savePath).fileName()));
        return;
    }

    connect(reply, &QNetworkReply::readyRead, out, [reply, out]() {
        out->write(reply->readAll());
    });

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, out, savePath, fileId, fileName, openWhenDone, destPath]() {
        reply->deleteLater();
        const bool ok = reply->error() == QNetworkReply::NoError;
        if (ok)
            out->write(reply->readAll());   // whatever readyRead did not take
        out->close();
        const qint64 written = out->size();
        delete out;

        if (!ok || written <= 0) {
            // A half-written file is worse than none: it looks like a download
            // that worked until you open it.
            QFile::remove(savePath);
            qWarning() << "File download failed:" << reply->errorString()
                       << "id" << fileId;
            // NO silent browser hand-off. That branch is what hid the broken
            // route for the whole life of this feature, and it is exactly the
            // "Open in Nextcloud" action the user can already choose
            // deliberately. Say what went wrong instead.
            emit errorOccurred(tr("Could not download \"%1\".").arg(fileName));
            return;
        }

        qDebug() << "File downloaded to:" << savePath << written << "bytes";
        if (!destPath.isEmpty()) {
            // The user chose this location. Announcing it back to them would
            // be telling them something they just typed.
            emit fileSavedAs(savePath);
            return;
        }
        emit fileDownloaded(savePath, openWhenDone);
        if (openWhenDone)
            QDesktopServices::openUrl(QUrl::fromLocalFile(savePath));
    });
}

// Downloads/<name>, with " (1)", " (2)" … appended rather than overwriting.
QString MessageListModel::uniqueDownloadPath(const QString &fileName)
{
    QString safe = QFileInfo(fileName).fileName();       // strip any directory
    safe.remove(QRegularExpression("[/\\\\]"));          // extra safety
    if (safe.isEmpty()) safe = QStringLiteral("download");

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty() || !QDir().mkpath(dir))
        return QString();

    QString path = dir + QLatin1Char('/') + safe;
    if (!QFile::exists(path))
        return path;

    const QString base = QFileInfo(safe).completeBaseName();
    const QString ext  = QFileInfo(safe).suffix();
    for (int n = 1; QFile::exists(path); ++n) {
        path = dir + QLatin1Char('/') + base + " (" + QString::number(n) + ")"
               + (ext.isEmpty() ? QString() : QLatin1Char('.') + ext);
    }
    return path;
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

    // Read marker only advances while TalQ is the FOREGROUND/active app —
    // opening a room (or having one open in the background) must NOT mark its
    // messages read; the user hasn't actually seen them. Query the LIVE app
    // state, never a cached flag: on Windows a cached "is active" bool could
    // stick false after a tray-restore / notification focus-steal / restart and
    // then never mark anything read again (the 0.50.2 regression). markAsRead
    // is re-invoked on focus-return (applicationStateChanged) and on every new
    // message, so a live query self-corrects.
    if (QGuiApplication::applicationState() != Qt::ApplicationActive) return;

    // Find the newest message ID (index 0 in newest-first order)
    int lastId = 0;
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].id > 0) {
            lastId = m_messages[i].id;
            break;
        }
    }

    if (lastId <= 0) return;

    // Forward-only read marker (H1, ChatSyncLogic.h). The /read endpoint stores
    // exactly the value we POST — it does NOT forward-clamp (that is precisely
    // how markAsUnread() below moves the marker backward). So if this device is
    // behind the per-user read marker — stale cache, a dropped refreshLatest
    // (generation race), another device that read further, or a topic/thread
    // filter making `lastId` the newest THREAD message rather than the room's
    // true newest — POSTing `lastId` would drag the server marker BACKWARD and
    // the next /room refresh would re-inflate the server-authoritative unread
    // badge and re-seed the "New messages" divider above already-read messages
    // (bugs 2/5/6). Only POST when it strictly advances the known marker.
    const int knownServerRead = m_conversations
        ? m_conversations->lastReadMessageForToken(m_token) : 0;
    if (!talq::shouldPostReadMarker(lastId, knownServerRead))
        return;

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
            // Advance the local "New messages" divider anchor on this confirmed
            // read so the separator moves below the messages we just read
            // WITHIN the session — not only after navigating away and back
            // (bug 6). Forward-only, mirroring markReadAt's own guard.
            if (lastId > m_unreadBoundary) {
                m_unreadBoundary = lastId;
                emit unreadBoundaryChanged();
            }
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
