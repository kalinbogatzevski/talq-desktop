#include "models/Message.h"

Message Message::fromJson(const QJsonObject &json)
{
    Message m;
    m.id = json["id"].toInt();
    m.token = json["token"].toString();
    m.actorType = json["actorType"].toString();
    m.actorId = json["actorId"].toString();
    m.actorDisplayName = json["actorDisplayName"].toString();
    m.message = json["message"].toString();
    m.timestamp = json["timestamp"].toInteger();
    m.messageType = json["messageType"].toString();
    m.isSystem = (m.messageType == "system");

    // Reply parent
    QJsonObject parent = json["parent"].toObject();
    if (!parent.isEmpty()) {
        m.replyToId = parent["id"].toInt();
        m.replyTo = parent;
    }

    // Reactions: { "👍": 3, "❤️": 1 }
    if (json.contains("reactions")) {
        m.reactions = json["reactions"].toObject();
    }

    return m;
}
