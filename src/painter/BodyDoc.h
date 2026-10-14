#pragma once

#include "BodyHtml.h"
#include "PainterTheme.h"

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <memory>

namespace talq {

// Is this text block a fenced code block (a <pre>)?
//
// Qt gives a <pre> a block-level background from the style sheet's `pre` rule;
// inline <code> sets only a CHARACTER background and prose has none. These were
// each measured on Qt 6.10.1: nonBreakableLines used to be the marker but is
// false now that pre wraps; BlockCodeFence / BlockCodeLanguage are never set by
// the HTML importer. The message HTML is colourless by contract, so the style
// sheet is the only thing that can put a fill on a block -- message content
// cannot fake a code block.
inline bool isCodeBlock(const QTextBlock &b)
{
    return b.isValid() && b.blockFormat().background().style() != Qt::NoBrush;
}

// The text of one code block as a person would copy it: entities decoded, line
// breaks as '\n', indentation and tabs exactly as written. (One exception, by
// Qt: a no-break space U+00A0 comes out as a plain space.)
//
// QTextDocumentFragment::toPlainText, not QTextCursor::selectedText(): a code
// block is ONE text block whose lines are QChar::LineSeparator, and
// selectedText() keeps U+2028 for each of them -- pasted into a terminal or an
// editor that is junk. toPlainText maps them to '\n' (measured).
inline QString codeBlockPlainText(const QTextBlock &b)
{
    QTextCursor c(b);
    c.movePosition(QTextCursor::StartOfBlock);
    c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    return c.selection().toPlainText();
}

// Build the QTextDocument a chat bubble paints its body from.
//
// This used to be a private static of LayoutEngine, which drags in the message
// model and the emoji tables -- so no test could call it, and the tests that
// wanted "what the chat shows" re-typed the recipe and quietly built a
// different document (one block per code LINE instead of the app's one block
// with line separators). A free, header-only function with a PainterTheme as
// its only dependency is what lets rich_text_contrast_test and the wrap/chip
// tests exercise the REAL thing.
inline std::shared_ptr<QTextDocument> createBodyDoc(
    const QString &html, qreal maxWidth, const PainterTheme &theme)
{
    auto doc = std::make_shared<QTextDocument>();
    doc->setDefaultFont(theme.bodyFont());
    doc->setTextWidth(maxWidth);
    doc->setDocumentMargin(0);

    // Theme colours for the formatted runs (<pre>, <code>, <a>, mentions).
    // The message HTML carries no colour of its own by contract (see
    // Message.cpp), so this is the only thing that inks them -- and it MUST
    // precede setHtml: Qt resolves the default style sheet while parsing, so
    // setting it afterwards silently does nothing. The same sheet is what makes
    // a <pre> wrap (white-space:pre-wrap, see PainterTheme::richTextStyleSheet).
    doc->setDefaultStyleSheet(theme.richTextStyleSheet());

    // Stored text carries real newlines; see layoutHtml() for what the recipe
    // does and why it is one function.
    doc->setHtml(layoutHtml(html));

    return doc;
}

} // namespace talq
