#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QDateTime>

/**
 * Represents a Nextcloud Talk conversation (room).
 * Maps directly to the OCS room API response fields.
 */
class Conversation
{
    Q_GADGET
    Q_PROPERTY(QString token MEMBER token)
    Q_PROPERTY(QString displayName MEMBER displayName)
    Q_PROPERTY(int type MEMBER type)
    Q_PROPERTY(int unreadMessages MEMBER unreadMessages)
    Q_PROPERTY(bool unreadMention MEMBER unreadMention)
    Q_PROPERTY(bool favorite MEMBER favorite)
    Q_PROPERTY(QString lastMessageText MEMBER lastMessageText)
    Q_PROPERTY(QString lastMessageAuthor MEMBER lastMessageAuthor)
    Q_PROPERTY(qint64 lastActivity MEMBER lastActivity)
    Q_PROPERTY(int lastReadMessage MEMBER lastReadMessage)

public:
    // Conversation types matching Talk API
    enum Type {
        OneToOne = 1,
        Group = 2,
        Public = 3,
        Changelog = 4,
        FormerOneToOne = 5,
        NoteToSelf = 6
    };
    Q_ENUM(Type)

    QString token;
    QString displayName;
    int type = 0;
    int unreadMessages = 0;
    bool unreadMention = false;
    bool favorite = false;
    QString lastMessageText;
    QString lastMessageAuthor;
    bool lastMessageSilent = false;  // sender chose to suppress notifications
    qint64 lastActivity = 0;
    int lastReadMessage = 0;
    int participantType = 0;
    QString actorId;
    QString name;  // For 1:1 chats, this is the other user's userId
    QString status; // User status for 1:1 chats: "online", "away", "dnd", "offline"
    bool hasTopics = false;
    bool hasCall = false;   // someone is in a call in this room
    int callFlag = 0;       // call flags (1=in-call, 2=audio, 4=video)
    // 0.41.8-beta — Talk's `participantInCallFlags`: bit-flags for
    // whether OUR user is currently in this call (from any device).
    // Non-zero = we're already in the call (started elsewhere). Used
    // to suppress the multi-device self-ring.
    int participantInCallFlags = 0;
    int notificationLevel = 0;  // 0=default, 1=always, 2=mention-only, 3=never
    // Talk 24 `conversation-tags`: the ids of the user's tags applied to this
    // conversation. Per-user, not per-room — two participants see different
    // sets. Ids are numeric STRINGS (snowflakes) server-side, so they are kept
    // as strings; parsing them as ints overflows.
    QStringList tagIds;
    // Talk 24 bit-flags (lib/RoomAttributes.php). Bit 0 = voice room, i.e.
    // "join the call when joining the conversation". Absent on older servers,
    // where toInt() yields 0 = no attributes. Always read through
    // talq::isVoiceRoom() so it stays gated on the presets capability.
    int attributes = 0;

    static Conversation fromJson(const QJsonObject &json);

    // For sorting: most recent activity first
    bool operator<(const Conversation &other) const {
        if (favorite != other.favorite) return favorite > other.favorite;
        return lastActivity > other.lastActivity;
    }
};

Q_DECLARE_METATYPE(Conversation)
