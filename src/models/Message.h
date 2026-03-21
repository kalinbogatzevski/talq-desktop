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
    int threadId = 0;           // Root thread message ID (0 = not in a thread)
    QString threadTitle;         // Thread title if this is a thread root
    int threadReplyCount = 0;    // Number of replies (for root messages)
    QJsonObject reactions;
    QString sendStatus;
    QString systemMessage;

    // File attachment (from messageParameters)
    QString fileName;
    QString fileMimetype;
    qint64 fileSize = 0;
    QString fileLink;
    QString filePreviewUrl;  // empty if no preview
    int fileId = 0;
    bool hasFile() const { return fileId > 0; }

    // Original server JSON — stored for lossless caching
    QJsonObject rawJson;

    static Message fromJson(const QJsonObject &json);

    QDateTime dateTime() const {
        return QDateTime::fromSecsSinceEpoch(timestamp);
    }

    // Reaction system messages should be filtered out (data is in reactions field)
    bool isReactionMessage() const {
        return systemMessage == "reaction" || systemMessage == "reaction_deleted"
            || systemMessage == "reaction_revoked";
    }

    // Call join/leave noise — filter from chat display
    bool isCallJoinLeave() const {
        return systemMessage == "call_joined"
            || systemMessage == "call_left";
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
