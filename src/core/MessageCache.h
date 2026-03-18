#pragma once

#include <QObject>
#include <QVector>
#include <QJsonObject>
#include <QSqlDatabase>
#include "models/Message.h"

class MessageCache : public QObject
{
    Q_OBJECT

public:
    explicit MessageCache(QObject *parent = nullptr);

    QVector<Message> loadMessages(const QString &token, int limit = 200);
    void saveMessages(const QString &token, const QVector<Message> &messages);
    int lastMessageId(const QString &token);
    int oldestMessageId(const QString &token);
    void clearConversation(const QString &token);
    void clearAll();

private:
    void initDatabase();
    QSqlDatabase m_db;
};
