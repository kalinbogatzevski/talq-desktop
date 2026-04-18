#include "models/Message.h"
#include <QRegularExpression>

Message Message::fromJson(const QJsonObject &json)
{
    Message m;
    m.rawJson = json;  // preserve original for lossless caching
    m.id = json["id"].toInt();
    m.token = json["token"].toString();
    m.actorType = json["actorType"].toString();
    m.actorId = json["actorId"].toString();
    m.actorDisplayName = json["actorDisplayName"].toString();
    m.message = json["message"].toString();
    m.message = m.message.toHtmlEscaped();
    m.timestamp = json["timestamp"].toInteger();
    m.lastEditTimestamp = json["lastEditTimestamp"].toInteger();
    m.lastEditActorId   = json["lastEditActorId"].toString();
    m.messageType = json["messageType"].toString();
    m.isSystem = (m.messageType == "system");
    m.systemMessage = json["systemMessage"].toString();

    // Resolve messageParameters — replace {placeholder} with styled mentions
    QJsonObject params = json["messageParameters"].toObject();
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
            } else if (type == "file") {
                m.fileName = param["name"].toString();
                m.fileMimetype = param["mimetype"].toString();
                m.fileSize = param["size"].toInteger();
                m.fileLink = param["link"].toString();
                m.fileId = param["id"].toString().toInt();
                if (param["preview-available"].toString() == "yes") {
                    // Build preview URL from file ID
                    m.filePreviewUrl = param["link"].toString().section("/f/", 0, 0)
                        + "/index.php/core/preview?fileId="
                        + QString::number(m.fileId) + "&x=400&y=400";
                }
                // Replace placeholder with filename
                if (m.fileMimetype.startsWith("image/")) {
                    m.message.replace(placeholder, "");  // image will show as preview
                } else {
                    m.message.replace(placeholder,
                        "<b style='color:#2ec4b6'>\xF0\x9F\x93\x84 " + name.toHtmlEscaped() + "</b>");
                }
            } else {
                m.message.replace(placeholder, name);
            }
        }
    }
    // Linkify URLs — turn bare http(s) links into clickable <a> tags
    // Only process text outside existing HTML tags
    static QRegularExpression urlRx(
        R"((https?://[^\s<>"']+))",
        QRegularExpression::CaseInsensitiveOption);
    // Don't linkify URLs already inside <a> or <b> tags
    if (!m.message.contains("href=")) {
        m.message.replace(urlRx,
            R"(<a href='\1' style='color:#5a9ecf'>\1</a>)");
    }

    // Reply parent
    QJsonObject parent = json["parent"].toObject();
    if (!parent.isEmpty()) {
        m.replyToId = parent["id"].toInt();
        m.replyTo = parent;
    }

    // Thread metadata (from API: isThread, threadId, threadTitle)
    if (json["isThread"].toBool()) {
        m.threadId = json["threadId"].toInt();
        m.threadTitle = json["threadTitle"].toString();
    }

    // Reactions: { "👍": 3, "❤️": 1 }
    if (json.contains("reactions")) {
        m.reactions = json["reactions"].toObject();
    }

    return m;
}
