#pragma once

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
// code block's spacing), and the newline that touches a <pre> is dropped as in
// layoutHtml so a fence adds no phantom blank line.
//
// QTextDocument::toPlainText maps U+2028 and U+2029 to '\n' (QTextCursor::
// selectedText() does not -- it leaves both in).
//
// Builds a QTextDocument per call: fine on a click or once per arriving
// message, NOT for a paintEvent -- use plainTextFromInlineHtml there.
inline QString plainTextFromBodyHtml(const QString &html)
{
    QString s = html;
    s.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    s.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    s.replace(QLatin1String("\n<pre>"), QLatin1String("<pre>"));
    s.replace(QLatin1String("</pre>\n"), QLatin1String("</pre>"));

    QTextDocument doc;
    doc.setHtml(QLatin1String("<div style='white-space:pre-wrap'>") + s
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
