#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>

/**
 * Represents a single chat message from the Talk API.
 */
class Message
{
    Q_GADGET
    Q_PROPERTY(int id MEMBER id)
    Q_PROPERTY(QString actorDisplayName MEMBER actorDisplayName)
    Q_PROPERTY(QString actorId MEMBER actorId)
    Q_PROPERTY(QString message MEMBER message)
    Q_PROPERTY(qint64 timestamp MEMBER timestamp)
    Q_PROPERTY(QString messageType MEMBER messageType)
    Q_PROPERTY(bool isSystem MEMBER isSystem)
    Q_PROPERTY(int replyToId MEMBER replyToId)

public:
    int id = 0;
    QString token;
    QString actorType;     // "users", "guests", "bots"
    QString actorId;
    QString actorDisplayName;
    QString message;
    qint64 timestamp = 0;
    QString messageType;   // "comment", "system", "command"
    bool isSystem = false;
    int replyToId = 0;
    QJsonObject replyTo;   // parent message for replies
    QJsonObject reactions; // emoji → count map

    static Message fromJson(const QJsonObject &json);

    QDateTime dateTime() const {
        return QDateTime::fromSecsSinceEpoch(timestamp);
    }

    // Check if this is from the same author and close in time to another message
    bool isGroupedWith(const Message &other) const {
        return actorId == other.actorId
            && actorType == other.actorType
            && !isSystem && !other.isSystem
            && qAbs(timestamp - other.timestamp) < 300; // 5 minute window
    }
};

Q_DECLARE_METATYPE(Message)
