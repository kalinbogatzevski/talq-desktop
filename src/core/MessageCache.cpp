#include "core/MessageCache.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>

// ─── MessageCache (main thread API) ───

MessageCache::MessageCache(QObject *parent)
    : QObject(parent)
    , m_worker(new MessageCacheWorker)
{
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_workerThread.start();

    // Initialize DB on worker thread
    QMetaObject::invokeMethod(m_worker, "doInit", Qt::BlockingQueuedConnection);
}

MessageCache::~MessageCache()
{
    m_workerThread.quit();
    m_workerThread.wait();  // block until worker finishes — no timeout, prevents dangling lambdas
}

void MessageCache::loadMessages(const QString &token, int limit)
{
    // Fully async — result comes via messagesLoaded signal
    QMetaObject::invokeMethod(m_worker, [this, token, limit]() {
        auto result = m_worker->doLoadMessages(token, limit);
        // Emit back on the main thread (MessageCache lives on main thread)
        QMetaObject::invokeMethod(this, [this, token, result]() {
            emit messagesLoaded(token, result);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void MessageCache::saveMessages(const QString &token, const QVector<Message> &messages)
{
    QMetaObject::invokeMethod(m_worker, "doSaveMessages", Qt::QueuedConnection,
        Q_ARG(QString, token), Q_ARG(QVector<Message>, messages));
}

void MessageCache::saveThreadIndex(const QString &token, const QVector<QJsonObject> &threads)
{
    QMetaObject::invokeMethod(m_worker, "doSaveThreadIndex", Qt::QueuedConnection,
        Q_ARG(QString, token), Q_ARG(QVector<QJsonObject>, threads));
}

void MessageCache::loadThreadIndex(const QString &token)
{
    QMetaObject::invokeMethod(m_worker, [this, token]() {
        auto result = m_worker->doLoadThreadIndex(token);
        QMetaObject::invokeMethod(this, [this, token, result]() {
            emit threadIndexLoaded(token, result);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void MessageCache::clearConversation(const QString &token)
{
    QMetaObject::invokeMethod(m_worker, "doClearConversation", Qt::QueuedConnection,
        Q_ARG(QString, token));
}

void MessageCache::clearAll()
{
    QMetaObject::invokeMethod(m_worker, "doClearAll", Qt::QueuedConnection);
}

// ─── MessageCacheWorker (worker thread) ───

MessageCacheWorker::MessageCacheWorker(QObject *parent)
    : QObject(parent)
{
}

void MessageCacheWorker::doInit()
{
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    QString dbPath = dataDir + "/message_cache.db";

    m_db = QSqlDatabase::addDatabase("QSQLITE", "message_cache_worker");
    m_db.setDatabaseName(dbPath);

    if (!m_db.open()) {
        qWarning() << "Failed to open message cache DB:" << m_db.lastError().text();
        return;
    }

    QSqlQuery q(m_db);
    q.exec("PRAGMA journal_mode=WAL");
    q.exec("PRAGMA synchronous=NORMAL");

    q.exec("CREATE TABLE IF NOT EXISTS messages ("
           "  token TEXT NOT NULL,"
           "  message_id INTEGER NOT NULL,"
           "  timestamp INTEGER NOT NULL,"
           "  json TEXT NOT NULL,"
           "  PRIMARY KEY (token, message_id)"
           ")");
    q.exec("CREATE INDEX IF NOT EXISTS idx_messages_token_ts "
           "ON messages(token, timestamp ASC)");

    // Schema version check — purge stale cache from v0.7.0 which stored
    // reconstructed JSON (missing messageParameters, file info, mentions).
    // v0.8.0+ stores original server JSON for lossless round-trip.
    q.exec("CREATE TABLE IF NOT EXISTS cache_meta ("
           "  key TEXT PRIMARY KEY, value TEXT)");
    q.prepare("SELECT value FROM cache_meta WHERE key = 'schema_version'");
    q.exec();
    int schemaVersion = q.next() ? q.value(0).toInt() : 0;
    if (schemaVersion < 2) {
        qDebug() << "MessageCache: upgrading schema v" << schemaVersion << "→ 2, purging stale entries";
        QSqlQuery purge(m_db);
        purge.exec("DELETE FROM messages");
        QSqlQuery setVer(m_db);
        setVer.prepare("INSERT OR REPLACE INTO cache_meta (key, value) VALUES ('schema_version', '2')");
        setVer.exec();
    }

    q.exec("CREATE TABLE IF NOT EXISTS thread_index ("
           "token TEXT NOT NULL, "
           "thread_id INTEGER NOT NULL, "
           "title TEXT NOT NULL, "
           "icon_color INTEGER DEFAULT 0, "
           "last_activity INTEGER DEFAULT 0, "
           "last_message TEXT DEFAULT '', "
           "last_author TEXT DEFAULT '', "
           "reply_count INTEGER DEFAULT 0, "
           "last_read_message_id INTEGER DEFAULT 0, "
           "PRIMARY KEY (token, thread_id)"
           ")");

    qDebug() << "Message cache opened at" << dbPath;
}

QVector<Message> MessageCacheWorker::doLoadMessages(const QString &token, int limit)
{
    QVector<Message> result;

    QSqlQuery q(m_db);
    q.prepare("SELECT json FROM ("
              "  SELECT json, timestamp, message_id FROM messages "
              "  WHERE token = :token "
              "  ORDER BY timestamp DESC, message_id DESC LIMIT :limit"
              ") ORDER BY timestamp ASC, message_id ASC");
    q.bindValue(":token", token);
    q.bindValue(":limit", limit);

    if (!q.exec()) {
        qWarning() << "Cache load failed:" << q.lastError().text();
        return result;
    }

    while (q.next()) {
        QJsonDocument doc = QJsonDocument::fromJson(q.value(0).toString().toUtf8());
        if (!doc.isNull()) {
            result.append(Message::fromJson(doc.object()));
        }
    }

    return result;
}

void MessageCacheWorker::doSaveMessages(const QString &token, const QVector<Message> &messages)
{
    if (messages.isEmpty()) return;

    m_db.transaction();

    QSqlQuery q(m_db);
    q.prepare("INSERT OR REPLACE INTO messages (token, message_id, timestamp, json) "
              "VALUES (:token, :id, :ts, :json)");

    for (const auto &msg : messages) {
        if (msg.id <= 0) continue;  // skip optimistic/temp messages

        // Use original server JSON for lossless round-trip caching.
        // This preserves messageParameters (file attachments, mentions),
        // thread metadata, reactions, and all other fields.
        QJsonObject json = msg.rawJson;
        if (json.isEmpty()) {
            // Fallback for messages without rawJson (e.g. optimistic messages)
            json["id"] = msg.id;
            json["token"] = msg.token.isEmpty() ? token : msg.token;
            json["actorType"] = msg.actorType;
            json["actorId"] = msg.actorId;
            json["actorDisplayName"] = msg.actorDisplayName;
            json["message"] = msg.message;
            json["timestamp"] = msg.timestamp;
            json["messageType"] = msg.messageType;
            if (!msg.systemMessage.isEmpty())
                json["systemMessage"] = msg.systemMessage;
            if (!msg.replyTo.isEmpty())
                json["parent"] = msg.replyTo;
            if (!msg.reactions.isEmpty())
                json["reactions"] = msg.reactions;
        }

        q.bindValue(":token", token);
        q.bindValue(":id", msg.id);
        q.bindValue(":ts", msg.timestamp);
        q.bindValue(":json", QString::fromUtf8(
            QJsonDocument(json).toJson(QJsonDocument::Compact)));
        q.exec();
    }

    m_db.commit();
}

int MessageCacheWorker::doLastMessageId(const QString &token)
{
    QSqlQuery q(m_db);
    q.prepare("SELECT MAX(message_id) FROM messages WHERE token = :token");
    q.bindValue(":token", token);
    if (q.exec() && q.next())
        return q.value(0).toInt();
    return 0;
}

void MessageCacheWorker::doClearConversation(const QString &token)
{
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM messages WHERE token = :token");
    q.bindValue(":token", token);
    q.exec();

    QSqlQuery q2(m_db);
    q2.prepare("DELETE FROM thread_index WHERE token = ?");
    q2.addBindValue(token);
    q2.exec();
}

void MessageCacheWorker::doClearAll()
{
    QSqlQuery q(m_db);
    q.exec("DELETE FROM messages");
    q.exec("DELETE FROM thread_index");
}

void MessageCacheWorker::doSaveThreadIndex(const QString &token, const QVector<QJsonObject> &threads)
{
    m_db.transaction();

    // Delete stale threads for this token before inserting fresh data
    QSqlQuery del(m_db);
    del.prepare("DELETE FROM thread_index WHERE token = ?");
    del.addBindValue(token);
    del.exec();

    for (const auto &t : threads) {
        QSqlQuery q(m_db);
        q.prepare("INSERT OR REPLACE INTO thread_index "
                  "(token, thread_id, title, icon_color, last_activity, last_message, last_author, reply_count, last_read_message_id) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
        q.addBindValue(token);
        q.addBindValue(t["threadId"].toInt());
        q.addBindValue(t["title"].toString());
        q.addBindValue(t["iconColor"].toInt());
        q.addBindValue(t["lastActivity"].toInteger());
        q.addBindValue(t["lastMessage"].toString());
        q.addBindValue(t["lastAuthor"].toString());
        q.addBindValue(t["replyCount"].toInt());
        q.addBindValue(t["lastReadMessageId"].toInt());
        if (!q.exec())
            qWarning() << "Thread index save failed:" << q.lastError().text();
    }
    m_db.commit();
}

QVector<QJsonObject> MessageCacheWorker::doLoadThreadIndex(const QString &token)
{
    QVector<QJsonObject> result;
    QSqlQuery q(m_db);
    q.prepare("SELECT thread_id, title, icon_color, last_activity, last_message, last_author, reply_count, last_read_message_id "
              "FROM thread_index WHERE token = ? ORDER BY last_activity DESC");
    q.addBindValue(token);
    if (q.exec()) {
        while (q.next()) {
            QJsonObject t;
            t["threadId"] = q.value(0).toInt();
            t["title"] = q.value(1).toString();
            t["iconColor"] = q.value(2).toInt();
            t["lastActivity"] = q.value(3).toLongLong();
            t["lastMessage"] = q.value(4).toString();
            t["lastAuthor"] = q.value(5).toString();
            t["replyCount"] = q.value(6).toInt();
            t["lastReadMessageId"] = q.value(7).toInt();
            result.append(t);
        }
    }
    return result;
}

void MessageCacheWorker::doClearThreadIndex(const QString &token)
{
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM thread_index WHERE token = ?");
    q.addBindValue(token);
    q.exec();
}
