#pragma once

#include <QObject>
#include <QThread>
#include <QVector>
#include <QJsonObject>
#include <QMutex>
#include <QSqlDatabase>
#include "models/Message.h"

Q_DECLARE_METATYPE(QVector<QJsonObject>)

class MessageCacheWorker;

/**
 * Thread-safe message cache backed by SQLite.
 * All database operations run on a worker thread — never blocks the UI.
 *
 * All calls are fully asynchronous — results arrive via signals.
 * saveMessages(), saveLastCommonRead() are fire-and-forget (queued to worker thread).
 */
class MessageCache : public QObject
{
    Q_OBJECT

public:
    explicit MessageCache(QObject *parent = nullptr);
    ~MessageCache() override;

    void loadMessages(const QString &token, int limit = 200);
    void saveMessages(const QString &token, const QVector<Message> &messages);
    void saveThreadIndex(const QString &token, const QVector<QJsonObject> &threads);
    void loadThreadIndex(const QString &token);
    void saveLastCommonRead(const QString &token, int messageId);
    void loadLastCommonRead(const QString &token);  // result via lastCommonReadLoaded signal

signals:
    void messagesLoaded(const QString &token, const QVector<Message> &messages);
    void threadIndexLoaded(const QString &token, const QVector<QJsonObject> &threads);
    void lastCommonReadLoaded(const QString &token, int messageId);

public slots:
    void clearConversation(const QString &token);
    void clearAll();

private:
    QThread m_workerThread;
    MessageCacheWorker *m_worker;
};

/**
 * Worker that runs on a dedicated thread — owns the SQLite connection.
 */
class MessageCacheWorker : public QObject
{
    Q_OBJECT

public:
    explicit MessageCacheWorker(QObject *parent = nullptr);

    // These run on the worker thread
    Q_INVOKABLE QVector<Message> doLoadMessages(const QString &token, int limit);
    Q_INVOKABLE void doSaveMessages(const QString &token, const QVector<Message> &messages);
    Q_INVOKABLE int doLastMessageId(const QString &token);
    Q_INVOKABLE void doClearConversation(const QString &token);
    Q_INVOKABLE void doClearAll();
    Q_INVOKABLE void doInit();
    Q_INVOKABLE void doSaveThreadIndex(const QString &token, const QVector<QJsonObject> &threads);
    Q_INVOKABLE QVector<QJsonObject> doLoadThreadIndex(const QString &token);
    Q_INVOKABLE void doClearThreadIndex(const QString &token);
    Q_INVOKABLE void doSaveLastCommonRead(const QString &token, int messageId);
    Q_INVOKABLE int doLoadLastCommonRead(const QString &token);

private:
    QSqlDatabase m_db;
};
