#include "models/Conversation.h"

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

    // Extract last message preview
    QJsonObject lastMsg = json["lastMessage"].toObject();
    if (!lastMsg.isEmpty()) {
        c.lastMessageText = lastMsg["message"].toString();
        c.lastMessageAuthor = lastMsg["actorDisplayName"].toString();
        c.lastMessageSilent = lastMsg["silent"].toBool();
    }

    return c;
}
