#pragma once

// Markdown -> the HTML a chat bubble is painted from.
//
// WHAT "FULL MARKDOWN" MEANS HERE. Talk's own client draws every message
// through NcRichText with remark-parse (CommonMark) + remark-gfm + remark-breaks
// (nextcloud-vue 9.9, `useExtendedMarkdown = message.markdown`): headings,
// emphasis, strikethrough, inline and fenced code, block quotes (nested), bullet
// and numbered lists (nested), task lists, tables, rules, [text](url) links,
// autolinked addresses, backslash escapes, and a single newline as a line break.
// This is the same grammar, parsed by md4c -- the CommonMark/GFM parser Qt itself
// uses for QTextDocument::setMarkdown -- and written out as the small HTML subset
// QTextDocument understands.
//
// THE CONTRACT with the rest of TalQ (see CLAUDE.md, Pitfall 15):
//
//  * The OUTPUT is the stored message HTML: escaped text plus semantic tags,
//    real '\n' for a line break (BodyHtml.h layoutHtml turns them into <br>),
//    and COLOURLESS -- no style=, no colour, ever. Colour comes from
//    PainterTheme::richTextStyleSheet(), per theme, at paint time.
//
//  * The INPUT is the RAW text the sender typed. Every character that reaches the
//    output goes through an escape here; there is no raw-HTML pass-through (the
//    text is untrusted: customers type it, bots forward it). `<b>x</b>` shows as
//    the six characters it is.
//
//  * Paragraphs carry no <p> tag. They are text joined by "\n\n", exactly the
//    look every plain message has always had, so a message with no markup is
//    byte for byte what it was. Only constructs that need a block get one:
//    <ul>/<ol>, <blockquote>, <h4>..<h6>, <hr>, <table>, <pre>.
//
//  * LINKS. Only http, https, mailto and tel are ever written as an <a>. Anything
//    else -- javascript:, file:, data:, a relative path -- is dropped to its plain
//    label, as Talk does (remark-unlink-protocols). `![alt](url)` is NOT fetched:
//    it becomes a link showing the alt text.
//
//  * PARKED BLOCKS. Message.cpp parks fenced code and unfenced code pastes under
//    sentinel tokens (\x01M<n>\x02) before this runs, because those rules are
//    TalQ's own and lenient (a fence typed on one line, a fence with no newline).
//    A token passes through here as ordinary text; Message.cpp swaps it for its
//    <pre> afterwards. Nothing in this file looks inside one.
//
// Pure Qt-Core plus md4c; one parse, one pass: a 32000-character message is a few
// milliseconds. Tests: markdown-html-test.

#include <QString>

namespace talq::markdown {

// The sentinel bracketing a parked block's index (see above).
inline constexpr char16_t kParkOpen = 0x01;
inline constexpr char16_t kParkClose = 0x02;

// May a link in a message be written as an <a> and opened by a click? http,
// https, mailto and tel, case-insensitive, and nothing else. This is the ONE
// list: the renderer uses it to decide what becomes a link, and the click
// handler uses it again before it hands anything to the operating system.
bool isOpenableLinkUrl(const QString &url);

// `text` (raw, sentinel tokens allowed) as stored message HTML.
QString toHtml(const QString &text);

} // namespace talq::markdown
