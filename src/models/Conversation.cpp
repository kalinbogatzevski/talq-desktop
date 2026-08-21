#include "models/Conversation.h"

#include <QJsonArray>

Conversation Conversation::fromJson(const QJsonObject &json)
{
    Conversation c;
    c.token = json["token"].toString();
    c.displayName = json["displayName"].toString();
    c.type = json["type"].toInt();
    c.unreadMessages = json["unreadMessages"].toInt();
    c.unreadMention = json["unreadMention"].toBool();
    c.favorite = json["isFavorite"].toBool();
    c.lastActivity = json["lastActivity"].toInteger();
    c.lastReadMessage = json["lastReadMessage"].toInt();
    c.participantType = json["participantType"].toInt();
    c.actorId = json["actorId"].toString();
    c.name = json["name"].toString();
    c.status = json["status"].toString(); // 1:1 chats include user status
    c.hasCall = json["hasCall"].toBool();
    c.callFlag = json["callFlag"].toInt();
    c.participantInCallFlags = json["participantInCallFlags"].toInt();
    c.notificationLevel = json["notificationLevel"].toInt(0);
    // Talk 24. Both keys are simply absent on older servers: toArray() yields
    // an empty array and toInt() yields 0, so an old server parses as
    // "untagged, no attributes" with no special-casing needed here.
    c.attributes = json["attributes"].toInt(0);
    const QJsonArray tagIds = json["tagIds"].toArray();
    c.tagIds.reserve(tagIds.size());
    for (const auto &t : tagIds) {
        // Strings only, deliberately. These are snowflake ids that EXCEED
        // 2^53, and QJsonValue stores every number as a double — so a value
        // that arrived as a JSON number has already lost precision by the time
        // we see it and cannot be recovered. Silently keeping a corrupted id
        // would be worse than dropping it: it would be sent back on the next
        // assign and tag the wrong conversation. The server sends
        // `numeric-string`, so this is the only path that should ever fire.
        if (!t.isString()) {
            qWarning() << "conversation" << c.token
                       << "sent a non-string tagId — ignoring (precision unsafe)";
            continue;
        }
        const QString id = t.toString();
        if (!id.isEmpty())
            c.tagIds.append(id);
    }

    // Extract last message preview
    QJsonObject lastMsg = json["lastMessage"].toObject();
    if (!lastMsg.isEmpty()) {
        c.lastMessageText = lastMsg["message"].toString();
        c.lastMessageAuthor = lastMsg["actorDisplayName"].toString();
        c.lastMessageSilent = lastMsg["silent"].toBool();
    }

    return c;
}
