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
    qint64 lastEditTimestamp = 0;  // 0 = never edited
    QString lastEditActorId;
    QString messageType;   // "comment", "system", "command"
    bool isSystem = false;
    int replyToId = 0;
    QJsonObject replyTo;   // parent message for replies
    int threadId = 0;           // Root thread message ID (0 = not in a thread)
    QString threadTitle;         // Thread title if this is a thread root
    int threadReplyCount = 0;    // Number of replies (for root messages)
    // 0.41.3-beta — upstream Talk dedup key. Sent on POST; the server
    // echoes it on the real-message response AND on the long-poll
    // event for the same message. We match incoming referenceId to a
    // local optimistic temp message to avoid back-to-back-send races
    // (the "first message disappears" field bug). Empty for system
    // messages and historical fetches; only meaningful on temp + the
    // server's echo of an own send. SHA-256-hex by upstream convention
    // but we treat it as an opaque string.
    QString referenceId;
    QJsonObject reactions;
    QString sendStatus;
    QString systemMessage;
    bool silent = false;   // sender used "Send silently" — receivers must
                           // not raise desktop notifications for this message

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

    // Edit-event noise: when a message is edited, spreed emits a separate
    // system message ("{actor} edited a message") in addition to updating
    // the original message's body in place. If we render that system
    // event, it appears alongside (and on some scroll patterns visually
    // replaces) the actual edited message body. Upstream Talk filters
    // these out for exactly that reason — the in-place body update is
    // the source of truth. Includes the moderator variant for completeness.
    bool isEditMessage() const {
        return systemMessage == "message_edited"
            || systemMessage == "message_edited_everyone";
    }

    // Deleted-message noise. Talk replaces a deleted comment with messageType
    // "comment_deleted" and emits a "message_deleted" system event — which the
    // upstream API docs explicitly say clients should NOT show (use it to remove
    // the original from storage). We hide both for Telegram-style clean deletes.
    bool isDeletedMessage() const {
        return messageType == "comment_deleted"
            || systemMessage == "message_deleted";
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
