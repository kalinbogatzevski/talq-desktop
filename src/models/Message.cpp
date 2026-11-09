#include "models/Message.h"
#include "models/CodePasteHeuristic.h"
#include "core/BareUrlLogic.h"
#include "core/LinkPreviewLogic.h"
#include "core/MarkdownHtml.h"
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
// Only when the body STARTS ON ITS OWN LINE, i.e. the fence (or its language
// tag) was followed by a newline (```python\n    code). When the content starts
// on the fence's own line ("``` ls -la\ncd /tmp```") the space after the fence
// is not indentation, it is the gap between the fence and the text, and keeping
// it shifted the first line one column right of the second; that and a fence
// with no newline at all (```foo bar```, an inline span that merely used three
// backticks) keep the old trimmed() behaviour.
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

// A fenced block: ```lang\nbody```. Greedy-min body, multiline OK.
//
// The language tag is dropped ONLY when a line break follows it: that is the
// one sign that the word is metadata and not something the author typed. The
// tag used to be matched unconditionally, so ```hello world``` lost "hello"
// and ```ls``` rendered as an empty box -- the author's own text, silently
// gone. Now it stays in the body: a word that did not need to be there is a
// harmless extra, a word that is missing is lost for good. (A tag may be
// followed by trailing blanks before its line break; '#' and '.' let c#, f#
// and vb.net through, which used to leave a stray "#" at the top of the box.)
//
// Group 1 is the line break after the tag, LF or CRLF (empty when the content
// starts on the fence's own line), group 2 the body: fenceBody needs to know
// which. One regex for the markdown pass AND for the reply quote
// (Message::quoteText), so the two can never disagree about what a fence is.
const QRegularExpression &codeBlockRegex()
{
    static const QRegularExpression rx(
        QStringLiteral(R"(```(?:[A-Za-z0-9_+#.-]+[ \t]*(?=\r?\n))?((?:\r?\n)?)([\s\S]*?)```)"));
    return rx;
}

// QString::toHtmlEscaped() undone: "&amp;" is decoded LAST, so a typed "&lt;"
// (stored as "&amp;lt;") comes back as "&lt;" and not as "<".
QString unescapeHtml(QString s)
{
    s.replace(QLatin1String("&quot;"), QLatin1String("\""));
    s.replace(QLatin1String("&gt;"), QLatin1String(">"));
    s.replace(QLatin1String("&lt;"), QLatin1String("<"));
    s.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return s;
}

// Render a message body as Talk does: CommonMark + GitHub-flavoured markdown
// (headings, emphasis, strikethrough, inline and fenced code, nested block
// quotes, nested bullet / numbered / task lists, tables, rules, [text](url)
// links, autolinks, backslash escapes, a single newline as a line break), parsed
// by md4c in core/MarkdownHtml.cpp.
//
// Input is the RAW text the sender typed -- NOT HTML-escaped. Escaping happens
// once, on the way out, inside the renderer, so a character is never escaped
// twice and `\*`, `>` and `&copy;` mean what they mean in markdown. (Until
// 0.80 this took already-escaped text, which is why a `>` quote could not be
// seen and TalQ had no backslash escapes.) Output is HTML.
//
// Two TalQ rules run BEFORE the parser, because they are lenient where
// CommonMark is strict and a NOC team depends on that: a fenced block
// (codeBlockRegex(): a fence typed on one line, a fence with no line break) and
// an unfenced code paste (CodePasteHeuristic.h). Both are parked under sentinel
// tokens, so the parser never sees their content -- a literal `**` inside a code
// block stays literal -- and swapped back for their <pre> at the end.
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
    const QString original = text;

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

    // ── Fenced code blocks: ```lang\nbody``` ──  (codeBlockRegex(), above)
    bool hadFence = false;
    {
        QString out;
        int pos = 0;
        auto it = codeBlockRegex().globalMatch(text);
        while (it.hasNext()) {
            auto m = it.next();
            out += QStringView{text}.mid(pos, m.capturedStart() - pos);
            out += park(QStringLiteral("<pre>")
                        + fenceBody(m.captured(2), !m.captured(1).isEmpty()).toHtmlEscaped()
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
        // The heuristic was tuned, and is tested, on ESCAPED text (it matches
        // "&gt;" and "&quot;"), so it still sees escaped text and its decisions
        // are exactly what they were. What it leaves behind is turned back into
        // raw text for the parser; a run it boxes is already parked, escaped.
        const int parkedBefore = stash.size();
        const QString boxed = talq::codepaste::wrapRuns(
            text.toHtmlEscaped(), [&](const QString &run) {
                return park(QStringLiteral("<pre>") + run + QStringLiteral("</pre>"));
            });
        if (stash.size() != parkedBefore)
            text = unescapeHtml(boxed);
    }

    // ── Everything else: the markdown parser ──
    QString html = talq::markdown::toHtml(text);

    // Swap each parked block back in, in one pass. (The tokens are \x01M<n>\x02;
    // the parser passed them through as ordinary text.)
    if (!stash.isEmpty()) {
        QString out;
        out.reserve(html.size() + 64);
        // Each parked block goes back AT MOST ONCE. The markers cannot be typed
        // (stripped above) or written as an entity (MarkdownHtml.cpp), so this
        // should never matter -- but if one ever got through, repeating it must
        // not repeat a block: that is a 30000-character block times a 3000-marker
        // message, written into a chat bubble.
        QVector<bool> restored(stash.size(), false);
        qsizetype i = 0;
        const qsizetype n = html.size();
        while (i < n) {
            const QChar c = html.at(i);
            if (c.unicode() == talq::markdown::kParkOpen) {
                const qsizetype close = html.indexOf(QChar(talq::markdown::kParkClose), i + 1);
                bool ok = false;
                const int idx = close > i + 2
                    ? QStringView{html}.mid(i + 2, close - i - 2).toInt(&ok) : 0;
                if (ok && idx >= 0 && idx < stash.size()) {
                    if (!restored.at(idx)) {
                        out += stash.at(idx);
                        restored[idx] = true;
                    }
                    i = close + 1;
                    continue;
                }
            }
            out += c;
            ++i;
        }
        html = out;
    }
    // A body that is only blanks parses to nothing. It still arrived, and a
    // bubble must never lose what the sender sent: show it as typed.
    if (html.isEmpty())
        return original.toHtmlEscaped();
    return html;
}

// STEP ONE of turning a message body into HTML: markdown (or, for a message the
// server says is not markdown, plain escaping). Runs BEFORE any
// {mention-*}/{file*} placeholder is substituted, so the markers around a
// mention are formatted but the mention's own markup is never re-read as
// emphasis. Either way every character of the raw text leaves here escaped.
QString escapeAndMarkup(const QString &rawText, bool markdown)
{
    if (markdown)
        return applyMarkdown(rawText);
    return rawText.toHtmlEscaped();
}

// STEP TWO, for a message that is NOT markdown: turn bare http(s) links into
// clickable <a> tags. A markdown message does not come through here -- the
// parser has already linked its addresses (and applied the same
// trailing-punctuation rule). Runs AFTER the placeholders are substituted, and
// its guard looks at the WHOLE resulting string -- a message that already
// carries an href is left alone, which is also why a URL inside it is never
// double-wrapped.
void linkifyBareUrls(QString &html)
{
    // Don't linkify URLs already inside <a> or <b> tags
    if (html.contains(QLatin1String("href=")))
        return;

    // Where an address ENDS is talq::bareurl::linkLength's call (BareUrlLogic.h):
    // "(see https://x.y)" and "go to https://x.y." used to link the ")" and the
    // "." and open a dead page. It also keeps a trailing backslash out of the
    // link, which Qt's HTML parser would read as an escaped quote and swallow
    // the rest of the message.
    html = talq::bareurl::linkify(html);
}

} // namespace

QString Message::renderBodyHtml(const QString &rawText, bool markdown)
{
    // Same two steps fromJson() runs, minus the placeholder substitution that
    // sits between them -- a body typed in the composer has no
    // messageParameters. Keep it that way: the moment this and fromJson()
    // diverge, an unsent bubble reshapes itself when the server's echo lands.
    QString html = escapeAndMarkup(rawText, markdown);
    if (!markdown)
        linkifyBareUrls(html);
    return html;
}

QString Message::quoteText(const QJsonObject &parent)
{
    QString text = parent.value(QStringLiteral("message")).toString();
    if (text.isEmpty())
        return text;

    // A quote of a code message used to read "``` ls -la ```" in the one line a
    // quote has: three of its few characters were the fence. Show the code, the
    // same way the bubble does (same regex, same body rule).
    {
        QString out;
        qsizetype pos = 0;
        auto it = codeBlockRegex().globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            out += QStringView{text}.mid(pos, m.capturedStart() - pos);
            out += fenceBody(m.captured(2), !m.captured(1).isEmpty());
            pos = m.capturedEnd();
        }
        if (pos > 0) {
            out += QStringView{text}.mid(pos);
            text = out;
        }
    }

    // Placeholders, in ONE pass over the text. A name that itself looks like a
    // placeholder (a file called "{mention-user1}.txt") must not be substituted
    // again, which a replace-per-parameter loop would do. Same rule per type as
    // fromJson, so a quote reads like the bubble it quotes: user / call / guest
    // are "@name", anything else (a file, a poll, a deck card) is its name.
    const QJsonObject params = parent.value(QStringLiteral("messageParameters")).toObject();
    if (!params.isEmpty()) {
        static const QRegularExpression placeholderRx(
            QStringLiteral(R"(\{([^{}\s]+)\})"));
        QString out;
        qsizetype pos = 0;
        auto it = placeholderRx.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            const QJsonValue v = params.value(m.captured(1));
            if (!v.isObject())
                continue;                      // not ours: leave it as typed
            const QJsonObject p = v.toObject();
            QString name = p.value(QStringLiteral("name")).toString();
            if (name.isEmpty())
                name = p.value(QStringLiteral("id")).toString();
            if (name.isEmpty())
                continue;                      // fromJson leaves it too
            const QString type = p.value(QStringLiteral("type")).toString();
            const bool mention = type == QLatin1String("user")
                || type == QLatin1String("call") || type == QLatin1String("guest");
            out += QStringView{text}.mid(pos, m.capturedStart() - pos);
            out += mention ? QStringLiteral("@") + name : name;
            pos = m.capturedEnd();
        }
        if (pos > 0) {
            out += QStringView{text}.mid(pos);
            text = out;
        }
    }
    return text;
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
    // Linkify URLs — turn bare http(s) links into clickable <a> tags. Only for a
    // message that is not markdown: the markdown parser has linked its own.
    if (!m.markdown)
        linkifyBareUrls(m.message);

    // Which addresses may get a preview card. From the RAW text, not the HTML
    // built above: that HTML has escaped entities and href markup around the
    // URL. The card applies the same tail rule as the link (trailing punctuation
    // and an unbalanced closing bracket stay out of the address). A file, a poll,
    // a system line and a deleted comment have no card.
    if (!m.isSystem && m.fileId == 0 && m.pollId == 0
        && m.messageType != QLatin1String("comment_deleted"))
        m.previewUrls = talq::linkpreview::extractPreviewUrls(
            json["message"].toString(), talq::linkpreview::kMaxCardsPerMessage, m.markdown);

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
