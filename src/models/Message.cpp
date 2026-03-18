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

    // Resolve messageParameters — replace {placeholder} with styled mentions
    QJsonObject params = json["messageParameters"].toObject();
    bool hasMentions = false;
    for (auto it = params.begin(); it != params.end(); ++it) {
        QJsonObject param = it.value().toObject();
        QString type = param["type"].toString();
        QString name = param["name"].toString();
        if (name.isEmpty())
            name = param["id"].toString();
        if (!name.isEmpty()) {
            QString placeholder = "{" + it.key() + "}";
            if (type == "user" || type == "call" || type == "guest") {
                // Wrap in styled span for rich text rendering
                m.message.replace(placeholder,
                    "<b style='color:#2ec4b6'>@" + name.toHtmlEscaped() + "</b>");
                hasMentions = true;
            } else if (type == "file") {
                m.message.replace(placeholder,
                    "<b style='color:#2ec4b6'>\xF0\x9F\x93\x84 " + name.toHtmlEscaped() + "</b>");
                hasMentions = true;
            } else {
                m.message.replace(placeholder, name);
            }
        }
    }
    // If we injected HTML, the message needs rich text rendering
    // We signal this by keeping the HTML tags in the message string
    // QML will detect <b> tags and use Text.RichText

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
