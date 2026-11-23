#pragma once

// Ticking a task-list box ("- [ ] item") in one of your OWN messages, as Talk's
// web client allows: the click edits the message text, through the ordinary
// message edit (PUT /chat/{token}/{id}). Everything else stays display only.
//
// Three parts, all pure (Qt Core only):
//
//  * WHICH box. The renderer (MarkdownHtml.cpp) numbers every task item it sees,
//    in order, and writes the number on the item's tag as `-qt-user-state:N`,
//    which Qt keeps as the list item block's QTextBlock::userState() and does
//    not draw (measured on Qt 6.10.1: kept on <li> in <ul> and <ol>, inside a
//    quote's table cell too, and NOT inherited by a nested item). A box is
//    therefore known by the renderer's own number, never by looking for a
//    U+2610 in the text: a box somebody typed has no number and is not a box.
//
//  * MAY it be ticked (taskBoxesTickable). The server's own rule for an edit
//    (spreed ChatController::editMessage, v24 and v25): the author's own
//    message, a real comment, a conversation the user may post in, and created
//    within the last day -- except in Note to self, which has no age limit.
//    TalQ's edit path does not edit a file message, so neither does a tick.
//
//  * WHAT text to send (toggledTaskText). The number is the renderer's, counted
//    on the text AFTER Message.cpp parked its lenient fences and code pastes, so
//    it cannot be mapped to a position in the raw text by counting "[ ]". The
//    mark is found by trying each "[ ]" / "[x]" / "[X]" in the raw text (the
//    N-th one first) and keeping the one whose result, rendered through the
//    REAL pipeline, is the original rendering with box N flipped and nothing
//    else changed. A "[ ]" inside a fence, a code span or prose never passes,
//    so a tick can only ever change the one box that was clicked; when no
//    candidate passes, nothing is sent.
//
//  * THE MENTIONS in the text sent (mentionsForEdit). The server's text has
//    {mention-*} placeholders; each goes back as `@"<mention-id>"`, the form
//    the server itself matched when it made the placeholder (spreed
//    UserMention: "all" for @all, "group/<id>", "team/<id>", "email/<id>",
//    "federated_user/<id>@<server>", a guest's "guest/<id>") and the one the
//    web client's own tick sends. Without `mention-id` (an older server) only a
//    local user's `@"<id>"` is certain; any other mention refuses the tick
//    rather than send it changed.
//
// Test: markdown-html-test (section "task boxes").

#include <QChar>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringView>
#include <QVector>

#include <algorithm>
#include <functional>

namespace talq::tasklist {

// The box glyphs the renderer writes (MarkdownHtml.cpp). The ticked one carries
// U+FE0E so TalQ's emoji drawing leaves it as text (EmojiClusterLogic.h).
inline constexpr char16_t kBoxOpen = 0x2610;
inline constexpr char16_t kBoxTicked = 0x2611;
inline constexpr char16_t kTextPresentation = 0xFE0E;

inline QString boxGlyph(bool checked)
{
    QString s;
    if (checked) {
        s += QChar(kBoxTicked);
        s += QChar(kTextPresentation);
    } else {
        s += QChar(kBoxOpen);
    }
    return s;
}

// The opening tag of task item number `ordinal` (0-based, in document order).
// It is the ONLY style= the renderer writes, and it paints nothing.
inline QString itemOpenTag(int ordinal)
{
    return QStringLiteral("<li style='-qt-user-state:%1'>").arg(ordinal);
}

// Seconds an own message stays editable (the server refuses an edit of a
// message created more than one day ago, except in Note to self).
inline constexpr qint64 kEditWindowSecs = 24 * 60 * 60;

struct TaskBoxGate {
    bool own = false;               // the user's own message
    bool system = false;
    int messageId = 0;              // > 0: the server has it
    bool pending = false;           // still sending, or failed to send
    bool hasFile = false;           // a file share (TalQ does not edit those)
    int pollId = 0;
    bool selectionMode = false;     // a click selects the row instead
    bool roomAllowsPosting = true;  // not read-only, chat permission held
    bool noteToSelf = false;        // no age limit there
    qint64 timestampSecs = 0;       // when the message was created
    qint64 nowSecs = 0;
};

inline bool taskBoxesTickable(const TaskBoxGate &g)
{
    if (!g.own || g.system || g.messageId <= 0 || g.pending || g.hasFile || g.pollId > 0
        || g.selectionMode || !g.roomAllowsPosting)
        return false;
    if (g.noteToSelf)
        return true;
    // Strict, like the web client: at the boundary the server may already say no.
    return g.timestampSecs > 0 && g.nowSecs - g.timestampSecs < kEditWindowSecs;
}

// Where task box `ordinal` is in rendered message HTML, and its state. Returns
// the index of its glyph, or -1 when the HTML has no such box.
inline qsizetype findBox(const QString &html, int ordinal, bool *checked)
{
    const QString tag = itemOpenTag(ordinal);
    const qsizetype at = html.indexOf(tag);
    if (at < 0 || html.indexOf(tag, at + 1) >= 0)
        return -1;                                  // absent, or (never) twice
    const qsizetype g = at + tag.size();
    if (g < html.size() && html.at(g).unicode() == kBoxOpen) {
        if (checked) *checked = false;
        return g;
    }
    if (g + 1 < html.size() && html.at(g).unicode() == kBoxTicked
        && html.at(g + 1).unicode() == kTextPresentation) {
        if (checked) *checked = true;
        return g;
    }
    return -1;
}

// How many candidate marks are rendered before giving up (each try is one
// render of the whole message; the N-th mark is right unless a fence or a
// literal "[x]" comes first).
inline constexpr int kMaxToggleTries = 64;

// `raw` (the message text as the server stores it, placeholders and all) with
// task box `ordinal` flipped from `wasChecked`, or a null QString when that
// cannot be done exactly. `render` is the real pipeline
// (Message::renderBodyHtml with markdown on).
inline QString toggledTaskText(const QString &raw, int ordinal, bool wasChecked,
                               const std::function<QString(const QString &)> &render)
{
    if (raw.isEmpty() || ordinal < 0 || !render)
        return QString();

    const QString before = render(raw);
    bool shownChecked = false;
    const qsizetype glyph = findBox(before, ordinal, &shownChecked);
    if (glyph < 0 || shownChecked != wasChecked)
        return QString();                           // not the message that was clicked

    QString expected = before;
    expected.replace(glyph, wasChecked ? 2 : 1, boxGlyph(!wasChecked));

    // Every "[ ]", "[x]", "[X]" in the raw text: the index of the mark character.
    QVector<qsizetype> marks;
    for (qsizetype i = 1; i + 1 < raw.size(); ++i) {
        const char16_t c = raw.at(i).unicode();
        if (raw.at(i - 1) == QLatin1Char('[') && raw.at(i + 1) == QLatin1Char(']')
            && (c == u' ' || c == u'x' || c == u'X'))
            marks.append(i);
    }
    if (marks.isEmpty())
        return QString();

    // The N-th mark first, then outwards from it.
    QVector<qsizetype> order;
    const qsizetype n = marks.size();
    const qsizetype start = std::min<qsizetype>(ordinal, n - 1);   // ordinal >= 0
    order.append(marks.at(start));
    for (qsizetype d = 1; order.size() < n; ++d) {
        if (start + d < n) order.append(marks.at(start + d));
        if (start - d >= 0) order.append(marks.at(start - d));
    }

    int tries = 0;
    for (const qsizetype at : order) {
        const bool markChecked = raw.at(at) != QLatin1Char(' ');
        if (markChecked != wasChecked)
            continue;                               // flipping it goes the wrong way
        if (++tries > kMaxToggleTries)
            break;
        QString candidate = raw;
        candidate[at] = wasChecked ? QLatin1Char(' ') : QLatin1Char('x');
        if (render(candidate) == expected)
            return candidate;
    }
    return QString();
}

// `raw` with every {mention-*} placeholder that `params` names turned back into
// what the server re-parses as that mention (see the top of this file), in one
// pass. A placeholder `params` does not name was typed, and stays as typed.
// *ok is false (and the result must not be sent) when a mention cannot be
// restored exactly.
inline QString mentionsForEdit(const QString &raw, const QJsonObject &params, bool *ok)
{
    *ok = true;
    static const QRegularExpression rx(QStringLiteral(R"(\{(mention-[A-Za-z0-9-]+)\})"));
    QString out;
    qsizetype pos = 0;
    auto it = rx.globalMatch(raw);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString key = m.captured(1);
        const QJsonValue v = params.value(key);
        if (!v.isObject())
            continue;                               // not a parameter: typed text
        const QJsonObject p = v.toObject();
        QString id = p.value(QStringLiteral("mention-id")).toString();
        if (id.isEmpty() && key.startsWith(QLatin1String("mention-user"))
            && p.value(QStringLiteral("type")).toString() == QLatin1String("user"))
            id = p.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || id.contains(QLatin1Char('"'))) {
            *ok = false;
            return QString();
        }
        out += QStringView{raw}.mid(pos, m.capturedStart() - pos);
        out += QStringLiteral("@\"") + id + QLatin1Char('"');
        pos = m.capturedEnd();
    }
    out += QStringView{raw}.mid(pos);
    return out;
}

} // namespace talq::tasklist
