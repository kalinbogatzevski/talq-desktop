#include "core/MessageCache.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>

MessageCache::MessageCache(QObject *parent)
    : QObject(parent)
{
    initDatabase();
}

void MessageCache::initDatabase()
{
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    QString dbPath = dataDir + "/message_cache.db";

    m_db = QSqlDatabase::addDatabase("QSQLITE", "message_cache");
    m_db.setDatabaseName(dbPath);

    if (!m_db.open()) {
        qWarning() << "Failed to open message cache DB:" << m_db.lastError().text();
        return;
    }

    QSqlQuery q(m_db);
    q.exec("CREATE TABLE IF NOT EXISTS messages ("
           "  token TEXT NOT NULL,"
           "  message_id INTEGER NOT NULL,"
           "  timestamp INTEGER NOT NULL,"
           "  json TEXT NOT NULL,"
           "  PRIMARY KEY (token, message_id)"
           ")");
    q.exec("CREATE INDEX IF NOT EXISTS idx_messages_token_ts "
           "ON messages(token, timestamp ASC)");

    qDebug() << "Message cache opened at" << dbPath;
}

QVector<Message> MessageCache::loadMessages(const QString &token, int limit)
{
    QVector<Message> result;

    QSqlQuery q(m_db);
    // Get the NEWEST N messages, returned in oldest-first order
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

void MessageCache::saveMessages(const QString &token, const QVector<Message> &messages)
{
    if (messages.isEmpty()) return;

    m_db.transaction();

    QSqlQuery q(m_db);
    q.prepare("INSERT OR REPLACE INTO messages (token, message_id, timestamp, json) "
              "VALUES (:token, :id, :ts, :json)");

    for (const auto &msg : messages) {
        QJsonObject json;
        json["id"] = msg.id;
        json["token"] = msg.token.isEmpty() ? token : msg.token;
        json["actorType"] = msg.actorType;
        json["actorId"] = msg.actorId;
        json["actorDisplayName"] = msg.actorDisplayName;
        json["message"] = msg.message;
        json["timestamp"] = msg.timestamp;
        json["messageType"] = msg.messageType;
        if (!msg.replyTo.isEmpty()) {
            json["parent"] = msg.replyTo;
        }
        if (!msg.reactions.isEmpty()) {
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

int MessageCache::lastMessageId(const QString &token)
{
    QSqlQuery q(m_db);
    q.prepare("SELECT MAX(message_id) FROM messages WHERE token = :token");
    q.bindValue(":token", token);
    if (q.exec() && q.next())
        return q.value(0).toInt();
    return 0;
}

int MessageCache::oldestMessageId(const QString &token)
{
    QSqlQuery q(m_db);
    q.prepare("SELECT MIN(message_id) FROM messages WHERE token = :token");
    q.bindValue(":token", token);
    if (q.exec() && q.next())
        return q.value(0).toInt();
    return 0;
}

void MessageCache::clearConversation(const QString &token)
{
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM messages WHERE token = :token");
    q.bindValue(":token", token);
    q.exec();
}

void MessageCache::clearAll()
{
    QSqlQuery q(m_db);
    q.exec("DELETE FROM messages");
}
