#pragma once

#include <QDateTime>
#include <QLatin1Char>
#include <QLatin1String>
#include <QRegularExpression>
#include <QString>
#include <QTextDocument>

namespace talq {

// The recipes that turn a message's stored HTML (Message::message, i.e. the
// output of Message::fromJson / Message::renderBodyHtml) into what the chat
// shows and into plain text.
//
// They exist as functions here because the chat body used to be built by
// LayoutEngine::createBodyDoc alone while a handful of other places re-did a
// half of it by hand (setHtml without the newline step, regex tag stripping that
// never unescaped). Each copy was wrong in a different way: the clipboard got
// "a &lt; b" instead of "a < b", or lost every line break. A test that fed
// setHtml directly built a document shaped nothing like the app's. Anything that
// wants "what the chat shows" or "what was typed" goes through here.
//
// Stored HTML is HTML-ESCAPED (Message.cpp toHtmlEscaped) with markdown applied
// on top, so a regex that strips tags leaves every "<", ">", "&" and '"' as an
// entity -- exactly the characters code is made of.

// What a QTextDocument is fed to LAY OUT the message.
//
//  0. "\r\n" and a lone "\r" become "\n". A CRLF body would otherwise turn
//     into "line \n" plus, around a fence, a "\r" between the <br> and the
//     <pre> that the rule below cannot see (measured).
//
//  1. "\n" -> "<br>". The stored text carries real newlines; HTML collapses
//     them to spaces, so without this a multi-line message becomes one line.
//     (QML's Text did this implicitly, which is where the step comes from.)
//     Inside a <pre> the <br> becomes a QChar::LineSeparator within ONE text
//     block -- measured: a fenced block is exactly one QTextBlock.
//
//  2. Drop a <br> that touches a <pre>. A fence's own newline is not content.
//     Qt absorbs it into the NEIGHBOURING prose block as an empty line, which
//     stacks on top of the code block's 12px margins and left a visible gap
//     above and below every code block (measured: 141px -> 107px for a sample
//     message). A deliberate blank line is two newlines, so one <br> survives.
inline QString layoutHtml(const QString &html)
{
    QString out = html;
    out.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    out.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    out.replace(QLatin1String("\n"), QLatin1String("<br>"));
    out.replace(QLatin1String("<br><pre>"), QLatin1String("<pre>"));
    out.replace(QLatin1String("</pre><br>"), QLatin1String("</pre>"));
    return out;
}

// What the sender TYPED, as plain text: entities decoded ("&lt;" -> "<"), line
// breaks as '\n', and -- unlike the chat display, which collapses them like any
// HTML -- indentation and runs of spaces kept. This is the clipboard's and the
// previews' recipe, and the difference from layoutHtml is deliberate: a copied
// message should be what was written, not what a browser would have made of it.
// `white-space:pre-wrap` on a wrapper does it (the same mechanism that keeps a
// code block's spacing); the line breaks themselves are layoutHtml's, so the
// copy has the very line structure the chat shows -- the <br> that touches a
// <pre> is dropped, a deliberate blank line keeps one.
//
// The line breaks MUST be <br> elements and not raw newlines. This used to strip
// the newline that touched a <pre> from the string and feed the rest as text,
// and Qt then dropped a blank line that was followed by INLINE markup:
// "<pre>code</pre>\n\n<b>bold</b>" pasted as "code\nbold" (a link and a mention
// too), while the same blank line before plain text survived. A <br> is an
// element, not whitespace, so it is never collapsed.
//
// QTextDocument::toPlainText maps U+2028 and U+2029 to '\n' (QTextCursor::
// selectedText() does not -- it leaves both in).
//
// Builds a QTextDocument per call: fine on a click or once per arriving
// message, NOT for a paintEvent -- use plainTextFromInlineHtml there.
inline QString plainTextFromBodyHtml(const QString &html)
{
    QTextDocument doc;
    doc.setHtml(QLatin1String("<div style='white-space:pre-wrap'>") + layoutHtml(html)
                + QLatin1String("</div>"));
    return doc.toPlainText();
}

// `s` without its leading BLANK LINES and its trailing whitespace -- but with the
// first real line's indentation intact, which QString::trimmed() strips. A
// message that opens with an indented snippet (a fence whose first code line is
// indented) otherwise pasted with that line flush left and the rest still
// indented: misaligned. The copy chip and the context-menu Copy keep it; the
// selection bar's Copy and the forward/edit fallbacks used to trim it away, so
// three copies of one message disagreed.
inline QString trimBlankEdges(const QString &s)
{
    qsizetype start = 0;
    for (;;) {
        const qsizetype nl = s.indexOf(QLatin1Char('\n'), start);
        if (nl < 0) break;
        bool blank = true;
        for (qsizetype i = start; i < nl; ++i)
            if (!s.at(i).isSpace()) { blank = false; break; }
        if (!blank) break;
        start = nl + 1;
    }
    qsizetype end = s.size();
    while (end > start && s.at(end - 1).isSpace()) --end;
    return s.mid(start, end - start);
}

// The header line the selection bar's Copy puts above each copied message:
// "[Alice, 2026-10-14 09:05]".
//
// It used to carry only MessageListModel::TimeStringRole, "HH:mm". That is right
// on a bubble, which sits under a date separator, and wrong on the clipboard: a
// paste into a ticket or an e-mail has no separator, so the lines had no day at
// all, and a selection that crossed midnight (or a week) read as one afternoon.
// ISO order, local time, no locale words -- it sorts, it means the same to every
// locale on the team, and it never depends on "Today"/"Yesterday".
//
// `timeOfDay` (the bubble's own "HH:mm") is only the fallback for a message that
// has no usable timestamp; the date is never invented.
inline QString copiedMessageHeader(const QString &author, qint64 timestampSecs,
                                   const QString &timeOfDay)
{
    const QString when = timestampSecs > 0
        ? QDateTime::fromSecsSinceEpoch(timestampSecs)
              .toString(QStringLiteral("yyyy-MM-dd HH:mm"))
        : timeOfDay;
    if (author.isEmpty())
        return QStringLiteral("[%1]").arg(when);
    return QStringLiteral("[%1, %2]").arg(author, when);
}

// The cheap form for text that is a single line of inline markup and is wanted
// on the PAINT path (a system row: "Alice added R&amp;D"): drop the tags, decode
// the four entities toHtmlEscaped emits. No document, so it is safe per paint.
// Not for a message body -- a code block's line breaks and a <pre>'s spacing need
// plainTextFromBodyHtml.
inline QString plainTextFromInlineHtml(QString html)
{
    static const QRegularExpression tagRe(QStringLiteral("<[^>]*>"));
    html.remove(tagRe);
    // &amp; LAST, or "&amp;lt;" (a typed "&lt;") would be decoded twice.
    html.replace(QLatin1String("&lt;"), QLatin1String("<"));
    html.replace(QLatin1String("&gt;"), QLatin1String(">"));
    html.replace(QLatin1String("&quot;"), QLatin1String("\""));
    html.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return html;
}

} // namespace talq
