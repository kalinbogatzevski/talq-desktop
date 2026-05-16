#include "models/Message.h"
#include <QRegularExpression>
#include <QVector>

namespace {

// Render the inline-markdown subset we support: code blocks, inline code,
// bold, italic, strikethrough. Input must already be HTML-escaped; output
// is HTML. Code blocks/inline code are stashed under sentinel tokens before
// emphasis runs, so their content isn't re-parsed (a literal `**` inside a
// code block must stay literal).
//
// Limited to a chat-message subset on purpose: no headings, no lists, no
// horizontal rules, no images. People don't use those in chat, and adding
// them invites false positives on punctuation-heavy plain text.
QString applyMarkdown(QString text)
{
    if (text.isEmpty()) return text;

    QVector<QString> stash;
    auto park = [&](const QString &html) {
        const int n = stash.size();
        stash.append(html);
        return QStringLiteral("\x01M%1\x02").arg(n);
    };

    // ── Fenced code blocks: ```lang\nbody``` ──
    // Greedy-min body, optional language tag is dropped. Multiline OK.
    static const QRegularExpression codeBlockRx(
        QStringLiteral(R"(```[A-Za-z0-9_+-]*\n?([\s\S]*?)```)"));
    {
        QString out;
        int pos = 0;
        auto it = codeBlockRx.globalMatch(text);
        while (it.hasNext()) {
            auto m = it.next();
            out += QStringView{text}.mid(pos, m.capturedStart() - pos);
            out += park(QStringLiteral("<pre style='background:#1f2937;border-radius:6px;"
                                      "padding:8px;font-family:Consolas,monospace'>")
                        + m.captured(1).trimmed()
                        + QStringLiteral("</pre>"));
            pos = m.capturedEnd();
        }
        if (pos > 0) {
            out += QStringView{text}.mid(pos);
            text = out;
        }
    }

    // ── Inline code: `body` ──
    static const QRegularExpression inlineCodeRx(
        QStringLiteral(R"(`([^`\n]+)`)"));
    {
        QString out;
        int pos = 0;
        auto it = inlineCodeRx.globalMatch(text);
        while (it.hasNext()) {
            auto m = it.next();
            out += QStringView{text}.mid(pos, m.capturedStart() - pos);
            out += park(QStringLiteral("<code style='background:#1f2937;"
                                      "border-radius:3px;padding:1px 4px;"
                                      "font-family:Consolas,monospace'>")
                        + m.captured(1)
                        + QStringLiteral("</code>"));
            pos = m.capturedEnd();
        }
        if (pos > 0) {
            out += QStringView{text}.mid(pos);
            text = out;
        }
    }

    // ── Bold: **body** ──  no internal '*', no newline
    static const QRegularExpression boldRx(
        QStringLiteral(R"(\*\*([^*\n]+?)\*\*)"));
    text.replace(boldRx, QStringLiteral("<b>\\1</b>"));

    // ── Strikethrough: ~~body~~ ──
    static const QRegularExpression strikeRx(
        QStringLiteral(R"(~~([^~\n]+?)~~)"));
    text.replace(strikeRx, QStringLiteral("<s>\\1</s>"));

    // ── Italic: *body* ──  CommonMark "flanking delimiter" rule: opening
    // '*' must be followed by non-whitespace and not be a leftover '**'.
    // (?<!\*) and (?!\*) prevent matching across a bold pair.
    static const QRegularExpression italicStarRx(
        QStringLiteral(R"((?<!\*)\*(\S[^*\n]*?\S|\S)\*(?!\*))"));
    text.replace(italicStarRx, QStringLiteral("<i>\\1</i>"));

    // ── Italic: _body_ ──  word-boundary so we don't eat snake_case_names.
    static const QRegularExpression italicUnderRx(
        QStringLiteral(R"(\b_(\S[^_\n]*?\S|\S)_\b)"));
    text.replace(italicUnderRx, QStringLiteral("<i>\\1</i>"));

    // Restore stashed code segments verbatim.
    for (int i = 0; i < stash.size(); ++i)
        text.replace(QStringLiteral("\x01M%1\x02").arg(i), stash[i]);

    return text;
}

} // namespace

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
    // Markdown subset (bold/italic/strike/inline-code/code-block) runs on the
    // escaped text, before mentions/files are substituted. Code-block content
    // is stashed first so its inner '**' / '_' aren't re-interpreted.
    m.message = applyMarkdown(m.message);
    m.timestamp = json["timestamp"].toInteger();
    m.lastEditTimestamp = json["lastEditTimestamp"].toInteger();
    m.lastEditActorId   = json["lastEditActorId"].toString();
    m.messageType = json["messageType"].toString();
    m.isSystem = (m.messageType == "system");
    m.systemMessage = json["systemMessage"].toString();
    // NC Talk marks "silent" messages at the top level of the JSON. Missing
    // field == false, which is the right default for old servers.
    m.silent = json["silent"].toBool();

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
                    "<b style='color:#14b8a6'>@" + name.toHtmlEscaped() + "</b>");
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
                        "<b style='color:#14b8a6'>\xF0\x9F\x93\x84 " + name.toHtmlEscaped() + "</b>");
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
