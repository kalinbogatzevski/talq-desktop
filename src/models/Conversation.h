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
    // What the popup gate (core/ToastGatePolicy.h) needs to know about the last
    // message without showing it: its mention markup (for "mentions only") and
    // the topic it was written in (0 = none), because a muted topic is muted
    // even in a chat that is not.
    QJsonObject lastMessageParams;
    int lastMessageThreadId = 0;
    int lastMessageId = 0;           // 0 = none / unknown
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

    // 0.65.3. Whether THIS user may start a call here. Talk decides it from the
    // room's `start-call-flag` setting plus the user's participant type, and
    // sends the answer per room — a client cannot derive it. Defaults TRUE so a
    // server that does not send it behaves exactly as TalQ did before, i.e.
    // offer the call action and let the server reject it.
    bool canStartCall = true;
    // Per-room "ring me when a call starts" switch (`notification-calls`),
    // independent of notificationLevel: a room can be on mentions-only and
    // still ring for calls, which is the combination people actually want in a
    // busy room. Defaults true = Talk's own default.
    bool notificationCalls = true;
    // Someone mentioned YOU specifically, as opposed to @all
    // (`direct-mention-flag`). unreadMention is true for both, which is why an
    // @all in a busy room currently lights the same badge as a real mention.
    bool unreadMentionDirect = false;
    // Read-only rooms (`readOnly` == 1) refuse chat posts. Without this the
    // composer looks live and the user only finds out when the POST fails.
    bool readOnly = false;
    QString description;
    // Talk's recording state for this room (Room.php:53-58):
    //   0 none · 1 video · 2 audio · 3 video-starting · 4 audio-starting · 5 failed
    // Anything non-zero means a recording is running or coming up, and every
    // participant must be able to see that.
    int callRecording = 0;
    // Archived: still joined, still receiving, but out of the default list.
    // The answer to "rooms I am neither in nor willing to leave".
    bool archived = false;
    // Important: keeps notifying even while archived, and Talk exempts it from
    // the "silence archived conversations" behaviour.
    bool important = false;
    // SIP dial-in. `sipEnabled` is the room's state (0 disabled, 1 enabled,
    // 2 enabled without a per-user PIN); `attendeePin` is THIS user's dial-in
    // code and is only sent when SIP is on. Both absent on a server with no
    // SIP bridge, which is the normal case.
    int sipEnabled = 0;
    QString attendeePin;

    // --- Talk 25 (channels, announcements, classified, preserved) ----------
    // None of these was parsed before. Each defaults to the answer that keeps
    // the pre-25 behaviour when the key is absent, so an older server reads
    // exactly as it did (see core/Talk25Rooms.h for the rules built on them).
    //
    // `permissions`: the EFFECTIVE permission mask of THIS user in this room (a
    // moderator already has the maximum). `hasPermissions` is false when the
    // server did not send it, which must read as "allowed", never as "none".
    bool hasPermissions = false;
    int permissions = 0;
    // The server's own verdict on Leave / Delete. TRUE when absent. False for a
    // member of an announcement (cannot leave) and for a preserved room (cannot
    // be deleted); the dialog hides or disables the action instead of letting
    // the user click into a refusal.
    bool canLeave = true;
    bool canDelete = true;
    // The user's own "hide message content" switch (`sensitive-conversations`).
    // The server already leaves `lastMessage` out of the list for such a room;
    // this lets the client keep the text out of the places IT fills from the
    // live chat (the list preview, the toast).
    bool isSensitive = false;
    // `objectType` is "classified" while a classified room is queued for
    // automatic deletion, "classified_persist" once a moderator kept it.
    // Empty for an ordinary room. objectId is not used: the sweep runs on
    // lastActivity, not on it (core/Talk25Rooms.h::classifiedDeleteAt).
    QString objectType;

    static Conversation fromJson(const QJsonObject &json);

    // For sorting: most recent activity first
    bool operator<(const Conversation &other) const {
        if (favorite != other.favorite) return favorite > other.favorite;
        return lastActivity > other.lastActivity;
    }
};

Q_DECLARE_METATYPE(Conversation)
