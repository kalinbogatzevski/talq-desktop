#include "models/Message.h"
#include "models/CodePasteHeuristic.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QVector>

namespace {

// The body of a fenced block, from the text between the fence's language tag
// and its closing ```.
//
// Indentation of the FIRST real line is content. This used to be a plain
// .trimmed(), which stripped it: a snippet copied from the middle of a file
// ("    if (x) {\n        y();\n    }") came out with its first line flush left
// and the rest still indented, i.e. misaligned. So: drop whole leading blank
// lines, keep the first real line exactly as typed, and trim only the tail.
//
// Only when the body STARTS ON ITS OWN LINE, i.e. the fence's language tag was
// followed by a newline (```python\n    code). When the content starts on the
// fence's own line ("``` ls -la\ncd /tmp```", "```bash ls -la") the space after
// the tag is not indentation, it is the gap between the fence and the text, and
// keeping it shifted the first line one column right of the second; that and a
// fence with no newline at all (```foo bar```, an inline span that merely used
// three backticks) keep the old trimmed() behaviour.
QString fenceBody(const QString &captured, bool startsOnOwnLine)
{
    if (!startsOnOwnLine || !captured.contains(QLatin1Char('\n')))
        return captured.trimmed();

    qsizetype start = 0;
    for (;;) {
        const qsizetype nl = captured.indexOf(QLatin1Char('\n'), start);
        if (nl < 0) break;                       // the rest is one line: keep it
        bool blank = true;
        for (qsizetype i = start; i < nl; ++i) {
            if (!captured.at(i).isSpace()) { blank = false; break; }
        }
        if (!blank) break;
        start = nl + 1;
    }

    qsizetype end = captured.size();
    while (end > start && captured.at(end - 1).isSpace())
        --end;
    return captured.mid(start, end - start);
}

// Render the inline-markdown subset we support: code blocks, inline code,
// bold, italic, strikethrough. Input must already be HTML-escaped; output
// is HTML. Code blocks/inline code are stashed under sentinel tokens before
// emphasis runs, so their content isn't re-parsed (a literal `**` inside a
// code block must stay literal).
//
// Limited to a chat-message subset on purpose: no headings, no lists, no
// horizontal rules, no images. People don't use those in chat, and adding
// them invites false positives on punctuation-heavy plain text.
//
// COLOURLESS BY CONTRACT -- do not add a style='color:...' / background here,
// and the same goes for the mention and link markup further down. This
// function has no theme and cannot get one: it runs at parse time, once, and
// its output is what MessageCache replays and what every theme then renders.
// An inline colour also outranks both the document's default style sheet and
// the paint-time palette, so anything baked in here wins everywhere. That is
// precisely how a dark-tuned #1f2937 code fill reached the light "Paper"
// theme and painted near-black-on-slate at 1.22:1. Emit semantic markup only
// (<pre>, <code>, <b class='mention'>, <a href>); the colour comes from
// PainterTheme::richTextStyleSheet(), applied per theme in
// LayoutEngine::createBodyDoc and rebuilt on every theme change.
QString applyMarkdown(QString text)
{
    if (text.isEmpty()) return text;

    // The sentinels below are \x01 and \x02. A message that already contains
    // those control characters (a paste from a terminal) could otherwise name a
    // stash slot of its own, and the restore loop would splice a code block in
    // twice. They are not printable and have no business in a chat message.
    text.remove(QChar(0x01));
    text.remove(QChar(0x02));

    QVector<QString> stash;
    auto park = [&](const QString &html) {
        const int n = stash.size();
        stash.append(html);
        return QStringLiteral("\x01M%1\x02").arg(n);
    };

    // ── Fenced code blocks: ```lang\nbody``` ──
    // Greedy-min body, optional language tag is dropped. Multiline OK.
    // Group 1 is the line break after the tag, LF or CRLF (empty when the content
    // starts on the fence's own line), group 2 the body: fenceBody needs to know
    // which.
    static const QRegularExpression codeBlockRx(
        QStringLiteral(R"(```[A-Za-z0-9_+-]*((?:\r?\n)?)([\s\S]*?)```)"));
    bool hadFence = false;
    {
        QString out;
        int pos = 0;
        auto it = codeBlockRx.globalMatch(text);
        while (it.hasNext()) {
            auto m = it.next();
            out += QStringView{text}.mid(pos, m.capturedStart() - pos);
            out += park(QStringLiteral("<pre>")
                        + fenceBody(m.captured(2), !m.captured(1).isEmpty())
                        + QStringLiteral("</pre>"));
            pos = m.capturedEnd();
        }
        if (pos > 0) {
            out += QStringView{text}.mid(pos);
            text = out;
            hadFence = true;
        }
    }

    // ── Unfenced code paste ──
    // Indented code, pretty-printed JSON, a stack trace, typed or pasted with
    // no fence: HTML would collapse its indentation and it would get none of a
    // code block's treatment. Box the run as a <pre>, parked like a fence so
    // the `**` / `_` / backticks inside it are never re-read as emphasis.
    //
    // Only when the author used NO fence at all -- not even a dangling one:
    // somebody who typed ``` knows what they want, and a half-fenced message
    // is the commonest place for a second guess to be wrong. What counts as a
    // run, and why every rule leans towards "not code", is in
    // CodePasteHeuristic.h.
    if (!hadFence && !text.contains(QLatin1String("```"))) {
        text = talq::codepaste::wrapRuns(text, [&](const QString &run) {
            return park(QStringLiteral("<pre>") + run + QStringLiteral("</pre>"));
        });
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
            out += park(QStringLiteral("<code>")
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

// STEP ONE of turning a message body into HTML: escape, then (optionally) the
// markdown subset. Runs BEFORE any {mention-*}/{file*} placeholder is
// substituted, so the markers around a mention are formatted but the mention's
// own markup is never re-read as emphasis.
//
// The markdown subset runs on the escaped text. Code-block content is stashed
// first so its inner '**' / '_' aren't re-interpreted.
QString escapeAndMarkup(const QString &rawText, bool markdown)
{
    QString html = rawText.toHtmlEscaped();
    if (markdown)
        html = applyMarkdown(html);
    return html;
}

// STEP TWO: turn bare http(s) links into clickable <a> tags. Runs AFTER the
// placeholders are substituted, and its guard looks at the WHOLE resulting
// string -- a message that already carries an href (a mention's link, a file)
// is left alone, which is also why a URL inside it is never double-wrapped.
void linkifyBareUrls(QString &html)
{
    // The LAST character of the match may not be a backslash. The href is
    // written inside single quotes, and Qt's HTML parser reads \' as an escaped
    // quote -- so "http://e.com/a\" followed by text produced an href that never
    // closed and swallowed the rest of the message (the bubble showed "see " and
    // nothing else; verified). A backslash in the MIDDLE of a URL is untouched.
    static QRegularExpression urlRx(
        R"((https?://[^\s<>"']*[^\s<>"'\\]))",
        QRegularExpression::CaseInsensitiveOption);
    // Don't linkify URLs already inside <a> or <b> tags
    if (!html.contains("href=")) {
        html.replace(urlRx,
            R"(<a href='\1'>\1</a>)");
    }
}

} // namespace

QString Message::renderBodyHtml(const QString &rawText, bool markdown)
{
    // Same two steps fromJson() runs, minus the placeholder substitution that
    // sits between them -- a body typed in the composer has no
    // messageParameters. Keep it that way: the moment this and fromJson()
    // diverge, an unsent bubble reshapes itself when the server's echo lands.
    QString html = escapeAndMarkup(rawText, markdown);
    linkifyBareUrls(html);
    return html;
}

Message Message::fromJson(const QJsonObject &json)
{
    Message m;
    m.rawJson = json;  // preserve original for lossless caching
    m.id = json["id"].toInt();
    m.token = json["token"].toString();
    m.actorType = json["actorType"].toString();
    m.actorId = json["actorId"].toString();
    m.actorDisplayName = json["actorDisplayName"].toString();
    // Talk tells us per message whether it is markdown at all. Until 0.65.3
    // TalQ ignored that and formatted everything, so a plain-text message
    // containing `C:\path\**\*.log` or `__init__` came out with chunks bolded
    // and the markers eaten — silently, and worst in exactly the pasted paths
    // and code that a NOC team sends each other all day. Missing key -> true,
    // so an older server still gets the historic unconditional behaviour.
    m.markdown = json["markdown"].toBool(true);
    m.message = escapeAndMarkup(json["message"].toString(), m.markdown);
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
                    "<b class='mention'>@" + name.toHtmlEscaped() + "</b>");
            } else if (type == "file") {
                m.fileName = param["name"].toString();
                m.fileMimetype = param["mimetype"].toString();
                m.fileSize = param["size"].toInteger();
                m.fileLink = param["link"].toString();
                m.filePath = param["path"].toString();
                m.fileId = param["id"].toString().toInt();
                m.fileHideDownload =
                    param["hide-download"].toString() == QLatin1String("yes");
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
                        "<b class='mention'>\xF0\x9F\x93\x84 " + name.toHtmlEscaped() + "</b>");
                }
            } else if (type == "talk-poll") {
                // A poll is a real object, not a sentence. Before 0.65.3 it
                // fell through the generic branch below and was flattened to
                // its question text, so a poll was indistinguishable from
                // somebody typing that question -- no icon, no affordance, and
                // no way to vote without opening the web UI.
                m.pollId = param["id"].toString().toInt();
                m.pollQuestion = name;
                m.message.replace(placeholder, QString());
            } else {
                // ⚠ ESCAPED. The other branches escape and this one did not,
                // so any rich object type TalQ does not special-case injected
                // its `name` into the message HTML raw.
                m.message.replace(placeholder, name.toHtmlEscaped());
            }
        }
    }
    // Linkify URLs — turn bare http(s) links into clickable <a> tags
    linkifyBareUrls(m.message);

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

    // 0.41.3-beta — optimistic-send dedup key (upstream convention).
    m.referenceId = json["referenceId"].toString();

    // Reactions: { "👍": 3, "❤️": 1 }
    if (json.contains("reactions")) {
        m.reactions = json["reactions"].toObject();
    }
    // ...and which of them are ours: [ "👍" ]. Absent on older servers and
    // deliberately absent from the `lastMessage` embedded in a conversation,
    // so an empty list means "not told", never "the user reacted to nothing".
    if (json.contains("reactionsSelf")) {
        const QJsonArray self = json["reactionsSelf"].toArray();
        m.reactionsSelf.reserve(self.size());
        for (const auto &r : self) {
            const QString emoji = r.toString();
            if (!emoji.isEmpty())
                m.reactionsSelf.append(emoji);
        }
    }

    return m;
}
