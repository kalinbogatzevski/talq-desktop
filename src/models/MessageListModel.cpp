#include "models/MessageListModel.h"
#include "core/MessageCache.h"
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
// STORAGE: m_messages is stored OLDEST-FIRST (natural chronological order).
//   m_messages[0] = oldest message
//   m_messages[last] = newest message
//
// ListView uses TopToBottom. QML scrolls to end after load.
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

    // Async cache results
    connect(m_cache, &MessageCache::messagesLoaded, this, [this](const QString &token, const QVector<Message> &messages) {
        if (m_token != token) return;  // stale result

        // Display cached messages instantly (even if empty — still trigger API fetch)
        if (!messages.isEmpty() && m_messages.isEmpty()) {
            QVector<Message> filtered;
            for (const auto &m : messages) {
                if (m.isReactionMessage() || m.isCallJoinLeave())
                    continue;
                if (m_hideThreadMessages && m.threadId > 0)
                    continue;
                filtered.append(m);
            }
            if (!filtered.isEmpty()) {
                beginInsertRows({}, 0, filtered.size() - 1);
                m_messages = filtered;
                endInsertRows();
                m_oldestMessageId = m_messages.first().id;
                emit newMessagesAtEnd();
            }
        }

        // Refresh latest messages from server (fills gaps between cache and live)
        refreshLatest();
        // Also fetch older history in background
        loadHistory();
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
            if (index.row() == 0) return false;
            return m.isGroupedWith(m_messages[index.row() - 1]);
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
            if (index.row() == 0) return true;
            auto prevDate = m_messages[index.row() - 1].dateTime().date();
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
        {FileIdRole,        "fileId"},
    };
}

void MessageListModel::setConversationToken(const QString &token)
{
    if (m_token == token)
        return;

    m_poller->stop();

    // Cancel any in-flight history request (disconnect first to prevent re-entry)
    if (m_historyReply) {
        auto *oldReply = m_historyReply;
        m_historyReply = nullptr;
        oldReply->disconnect(this);
        oldReply->abort();
        oldReply->deleteLater();
    }

    m_token = token;
    m_oldestMessageId = 0;
    m_threadId = 0;
    m_lastCommonRead = 0;
    m_loading = false;
    m_hasMoreHistory = true;
    emit hasMoreHistoryChanged();

    // Clear messages
    if (!m_messages.isEmpty()) {
        beginRemoveRows({}, 0, m_messages.size() - 1);
        m_messages.clear();
        endRemoveRows();
    }

    emit conversationTokenChanged();

    if (token.isEmpty())
        return;

    // Load last 20 from local cache for instant display,
    // then fetch fresh from API in background (triggered after cache loads)
    m_cache->loadMessages(token, 20);

    // Mark as read
    QJsonObject body;
    m_api->post("apps/spreed/api/v1/chat/" + token + "/read", body,
        [](bool, const QJsonObject &, int) {});
}

void MessageListModel::setThreadId(int id)
{
    if (m_threadId == id)
        return;

    m_poller->stop();

    beginResetModel();
    m_messages.clear();
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

        // API returns newest-first; reverse to oldest-first
        QSet<int> existingIds;
        for (const auto &existing : m_messages)
            existingIds.insert(existing.id);

        QVector<Message> olderMsgs;
        for (int i = data.size() - 1; i >= 0; --i) {
            Message m = Message::fromJson(data[i].toObject());
            if (existingIds.contains(m.id) || m.isReactionMessage() || m.isCallJoinLeave())
                continue;
            if (m_hideThreadMessages && m.threadId > 0)
                continue;
            olderMsgs.append(m);
        }

        if (!olderMsgs.isEmpty()) {
            // Prepend older messages at the beginning (single insert, not O(n^2) prepend loop)
            beginInsertRows({}, 0, olderMsgs.size() - 1);
            olderMsgs.append(std::move(m_messages));
            m_messages = std::move(olderMsgs);
            endInsertRows();

            m_cache->saveMessages(m_token, olderMsgs);
        }

        if (!m_messages.isEmpty())
            m_oldestMessageId = m_messages.first().id;

        emit newMessagesAtEnd();
        startPoller();
    });
}

void MessageListModel::startPoller()
{
    m_poller->setThreadId(m_threadId);
    int lastId = m_messages.isEmpty() ? 0 : m_messages.last().id;
    m_poller->start(m_token, lastId);
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
    auto *reply = m_api->getRaw("apps/spreed/api/v1/chat/" + m_token, params);

    connect(reply, &QNetworkReply::finished, this, [this, reply, currentToken]() {
        reply->deleteLater();
        if (m_token != currentToken) return;

        if (reply->error() != QNetworkReply::NoError) return;

        QByteArray body = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(body);
        QJsonArray data = doc.object()["ocs"].toObject()["data"].toArray();
        if (data.isEmpty()) return;

        // Build set of existing message IDs for quick lookup
        QSet<int> existingIds;
        QHash<int, int> idToIndex;
        for (int i = 0; i < m_messages.size(); i++) {
            existingIds.insert(m_messages[i].id);
            idToIndex[m_messages[i].id] = i;
        }

        // API returns newest-first; process to find missing and edited messages
        QVector<Message> missing;
        for (const auto &val : data) {
            Message m = Message::fromJson(val.toObject());
            if (m.isReactionMessage() || m.isCallJoinLeave()) continue;
            if (m_hideThreadMessages && m.threadId > 0) continue;

            if (!existingIds.contains(m.id)) {
                // Missing message — needs to be inserted
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
            // Sort missing messages by ID (oldest first) for correct insertion
            std::sort(missing.begin(), missing.end(), [](const Message &a, const Message &b) {
                return a.id < b.id;
            });

            // Append at the end (they're newer than what we had)
            int first = m_messages.size();
            beginInsertRows({}, first, first + missing.size() - 1);
            m_messages.append(missing);
            endInsertRows();

            m_cache->saveMessages(m_token, missing);
            emit newMessagesAtEnd();

            qDebug() << "MessageListModel: refreshLatest found" << missing.size() << "missing messages";
        }

        // Restart poller from the true latest message
        m_poller->stop();
        startPoller();
    });
}

void MessageListModel::onMessagesReceived(const QJsonArray &messages)
{
    if (messages.isEmpty()) return;

    QSet<int> existingIds;
    for (const auto &existing : m_messages)
        existingIds.insert(existing.id);

    QVector<Message> newMsgs;
    for (const auto &val : messages) {
        Message m = Message::fromJson(val.toObject());
        if (existingIds.contains(m.id) || m.isReactionMessage() || m.isCallJoinLeave())
            continue;
        if (m_hideThreadMessages && m.threadId > 0)
            continue;
        newMsgs.append(m);
    }

    if (newMsgs.isEmpty()) return;

    // Append new messages at the end (newest)
    int first = m_messages.size();
    beginInsertRows({}, first, first + newMsgs.size() - 1);
    m_messages.append(newMsgs);
    endInsertRows();

    m_cache->saveMessages(m_token, newMsgs);

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
                    m_messages.removeAt(idx);
                    endRemoveRows();
                } else {
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

    // Append at end (newest)
    int pos = m_messages.size();
    beginInsertRows({}, pos, pos);
    m_messages.append(optimistic);
    endInsertRows();

    emit messageSent();
    emit newMessagesAtEnd();

    QJsonObject body;
    body["message"] = text;
    if (replyToId > 0)
        body["replyTo"] = replyToId;

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
    m_api->post("apps/spreed/api/v1/reaction/" + currentToken + "/" + QString::number(messageId),
        body, [this, messageId, currentToken](bool ok, const QJsonObject &data, int statusCode) {
            qDebug() << "Reaction response: ok=" << ok << "status=" << statusCode << "data keys:" << data.keys();
            if (m_token != currentToken || !ok) return;

            // Update reactions on the message from server response
            int idx = -1;
            for (int i = 0; i < m_messages.size(); ++i) {
                if (m_messages[i].id == messageId) { idx = i; break; }
            }
            if (idx < 0) return;

            // Server returns reactions as { "👍": [{...}], "❤️": [{...}] }
            // We need to convert to { "👍": count, "❤️": count }
            QJsonObject reactionsMap;
            for (auto it = data.begin(); it != data.end(); ++it) {
                if (it.value().isArray()) {
                    reactionsMap[it.key()] = it.value().toArray().size();
                } else {
                    reactionsMap[it.key()] = it.value().toInt();
                }
            }
            m_messages[idx].reactions = reactionsMap;
            emit dataChanged(index(idx), index(idx), {ReactionsRole});

            // Save updated message to cache
            m_cache->saveMessages(m_token, {m_messages[idx]});
        });
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

    QUrl fileUrl(filePath);
    QString localPath = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : filePath;

    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit errorOccurred("Cannot open file: " + localPath);
        return;
    }

    QString fileName = QFileInfo(localPath).fileName();
    fileName.remove(QRegularExpression("[/\\\\]"));  // sanitize for WebDAV path safety
    if (fileName.isEmpty()) fileName = "upload";
    QByteArray fileData = file.readAll();

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

        m_api->post("apps/files_sharing/api/v1/shares", body,
            [this, fileName, token, caption](bool ok, const QJsonObject &, int) {
                m_uploadProgress = -1;
                m_uploadFileName.clear();
                emit uploadProgressChanged();

                if (ok) {
                    qDebug() << "File shared:" << fileName;
                    // Send caption as a follow-up message if provided
                    if (!caption.isEmpty() && m_token == token) {
                        sendMessage(caption, 0);
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

    m_lastCommonRead = messageId;

    if (!m_messages.isEmpty()) {
        emit dataChanged(index(0), index(m_messages.size() - 1), {IsReadRole});
    }
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

void MessageListModel::markAsRead()
{
    if (m_token.isEmpty()) return;

    // Find the last message ID
    int lastId = 0;
    for (int i = m_messages.size() - 1; i >= 0; --i) {
        if (m_messages[i].id > 0) {
            lastId = m_messages[i].id;
            break;
        }
    }

    if (lastId <= 0) return;

    // POST /apps/spreed/api/v1/chat/{token}/read
    QJsonObject body;
    body["lastReadMessage"] = lastId;
    m_api->post("apps/spreed/api/v1/chat/" + m_token + "/read", body,
        [](bool, const QJsonObject &, int) {
            // Fire and forget
        });
}
