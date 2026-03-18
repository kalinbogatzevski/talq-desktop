#pragma once

#include <QObject>
#include <QString>
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
    qint64 lastActivity = 0;
    int lastReadMessage = 0;
    int participantType = 0;
    QString actorId;
    QString name;  // For 1:1 chats, this is the other user's userId

    static Conversation fromJson(const QJsonObject &json);

    // For sorting: most recent activity first
    bool operator<(const Conversation &other) const {
        if (favorite != other.favorite) return favorite > other.favorite;
        return lastActivity > other.lastActivity;
    }
};

Q_DECLARE_METATYPE(Conversation)
