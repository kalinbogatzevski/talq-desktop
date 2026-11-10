#include "core/MarkdownHtml.h"

#include "core/BareUrlLogic.h"

#include <md4c.h>

#include <QChar>
#include <QLatin1String>
#include <QStringView>
#include <QVector>

#include <algorithm>

namespace talq::markdown {

namespace {

// Nesting a hostile message can ask for is unbounded (">>>>>>..." 32000 deep);
// QTextDocument would build every level. Past these depths the tags are simply
// not written and the content flows on, still escaped and still readable.
constexpr int kMaxQuoteDepth = 6;
constexpr int kMaxListDepth = 8;

// How much STRUCTURE one message may build. Every list item, table row and cell
// is its own block in the QTextDocument, and layout cost goes with the block
// count: measured on Qt 6.10.1, a 32000-character message costs 92 ms to lay out
// as plain lines but 1.5 s as 8000 list items and ~1 s as a big table -- on the
// UI thread, per message and again on every resize. Nobody writes 400 list items
// or a 300-row table in a chat; a message that does is shown as plain text (all
// of it, links included), which costs what it always did.
constexpr int kMaxListItems = 400;
constexpr int kMaxTableRows = 300;
constexpr int kMaxTableCells = 1500;

// Talk draws # as h4: its headings are levelled down so user content never
// competes with the page's own h1..h3 (NcRichText.createElement). Here that
// keeps a "# Title" at the size of the message text, not twice it.
constexpr int kHeadingShift = 3;

// ---------------------------------------------------------------------------
// text helpers
// ---------------------------------------------------------------------------

void appendEscaped(QString &out, QStringView s)
{
    for (const QChar c : s) {
        switch (c.unicode()) {
        case u'&': out += QLatin1String("&amp;"); break;
        case u'<': out += QLatin1String("&lt;"); break;
        case u'>': out += QLatin1String("&gt;"); break;
        case u'"': out += QLatin1String("&quot;"); break;
        default:   out += c; break;
        }
    }
}

void appendEscapedUtf8(QString &out, const char *p, unsigned n)
{
    appendEscaped(out, QString::fromUtf8(p, qsizetype(n)));
}

struct NamedEntity { const char *name; char32_t cp; };

// The entities worth knowing in a chat message. Talk decodes every HTML entity
// (the browser does); this covers what people actually type. An entity not in
// the table is left exactly as typed, so nothing is lost.
constexpr NamedEntity kEntities[] = {
    {"amp", U'&'}, {"lt", U'<'}, {"gt", U'>'}, {"quot", U'"'}, {"apos", U'\''},
    {"nbsp", 0x00A0}, {"copy", 0x00A9}, {"reg", 0x00AE}, {"trade", 0x2122},
    {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"bull", 0x2022},
    {"middot", 0x00B7}, {"laquo", 0x00AB}, {"raquo", 0x00BB}, {"deg", 0x00B0},
    {"plusmn", 0x00B1}, {"times", 0x00D7}, {"divide", 0x00F7}, {"euro", 0x20AC},
    {"pound", 0x00A3}, {"yen", 0x00A5}, {"cent", 0x00A2}, {"sect", 0x00A7},
    {"para", 0x00B6}, {"frac12", 0x00BD}, {"larr", 0x2190}, {"rarr", 0x2192},
    {"uarr", 0x2191}, {"darr", 0x2193}, {"hearts", 0x2665}, {"check", 0x2713},
    {"ne", 0x2260}, {"le", 0x2264}, {"ge", 0x2265}, {"infin", 0x221E},
};

// "&copy;", "&#35;", "&#x23;" -> the character. Unknown named entities come back
// unchanged (and are escaped by the caller like any other text).
QString decodeEntity(QStringView e)
{
    if (e.size() < 3 || e.front() != QLatin1Char('&') || e.back() != QLatin1Char(';'))
        return e.toString();
    const QStringView body = e.mid(1, e.size() - 2);
    if (body.startsWith(QLatin1Char('#'))) {
        bool ok = false;
        const bool hex = body.size() > 1
            && (body[1] == QLatin1Char('x') || body[1] == QLatin1Char('X'));
        const uint cp = hex ? body.mid(2).toUInt(&ok, 16) : body.mid(1).toUInt(&ok, 10);
        // No control character comes out of an entity. Message.cpp parks code
        // blocks under U+0001 / U+0002 markers and strips the RAW ones before this
        // runs; "&#1;" is the other way to write one, and a message of "&#1;M0&#2;"
        // repeated 3000 times would have the swap-back write the same parked block
        // 3000 times. Tab and line feed are ordinary text.
        const bool control = (cp < 0x20 && cp != 0x09 && cp != 0x0A) || (cp >= 0x7F && cp <= 0x9F);
        if (!ok || cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || control)
            return QString(QChar(0xFFFD));
        const char32_t c32 = cp;
        return QString::fromUcs4(&c32, 1);
    }
    const QByteArray name = body.toLatin1();
    for (const NamedEntity &n : kEntities) {
        if (name == n.name) {
            const char32_t cp = n.cp;
            return QString::fromUcs4(&cp, 1);
        }
    }
    return e.toString();
}

// The text of an md4c attribute (a link target): backslash escapes are already
// gone, character references are in pieces.
QString attributeText(const MD_ATTRIBUTE &a)
{
    QString r;
    for (unsigned i = 0; a.substr_offsets[i] < a.size; ++i) {
        const unsigned from = a.substr_offsets[i];
        const unsigned to = a.substr_offsets[i + 1];
        const QString piece = QString::fromUtf8(a.text + from, qsizetype(to - from));
        switch (a.substr_types[i]) {
        case MD_TEXT_ENTITY:   r += decodeEntity(piece); break;
        case MD_TEXT_NULLCHAR: r += QChar(0xFFFD); break;
        default:               r += piece; break;
        }
    }
    return r;
}

// An address as it is written inside href='...'. The attribute is single-quoted
// and Qt's HTML reader treats \' as an escaped quote, so every character that
// could end or confuse the attribute is percent-encoded, and '&' is escaped.
QString hrefAttribute(const QString &url)
{
    QString out;
    out.reserve(url.size() + 8);
    for (const QChar c : url) {
        const char16_t u = c.unicode();
        // '{' and '}' too: a message's {mention-userN} placeholders are replaced
        // with markup AFTER the markdown is rendered (Message::fromJson), and one
        // sitting in a link target would have put a mention's <b class='mention'>
        // -- quotes and all -- inside the quoted href. Encoded, it is not a
        // placeholder any more (RFC 3986 does not allow a raw brace in an address
        // anyway). The link's visible text is untouched.
        if (u <= 0x20 || u == 0x7F || u == u'\'' || u == u'"' || u == u'\\' || u == u'<'
            || u == u'>' || u == u'`' || u == u'{' || u == u'}') {
            out += QLatin1Char('%');
            out += QString::number(int(u), 16).rightJustified(2, QLatin1Char('0')).toUpper();
        } else if (u == u'&') {
            out += QLatin1String("&amp;");
        } else {
            out += c;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// the renderer (md4c SAX callbacks)
// ---------------------------------------------------------------------------

struct Container {
    bool prevWasPara = false;   // the last block written in here was a paragraph
};

struct OpenSpan {
    bool isImage = false;
    bool emittedAnchor = false;
    qsizetype outAtStart = 0;
    QString url;                // an image's address, for the empty-alt fallback
};

struct Renderer {
    QString out;
    QVector<Container> containers;
    QVector<bool> boxEmitted;   // per QUOTE/UL/OL entered: was its tag written?
    QVector<qsizetype> listTag; // per UL/OL entered: where its tag starts in `out`, or -1
    QVector<OpenSpan> spans;    // A and IMG, innermost last
    QString pending;            // plain text not yet written (see flush)
    int quoteDepth = 0;
    int listDepth = 0;
    int imageDepth = 0;         // >0: inside an image's alt text, no inline tags
    int linkDepth = 0;          // >0: inside a link
    bool inCodeBlock = false;
    int items = 0;              // list items, table rows and cells seen (the budget)
    int rows = 0;
    int cells = 0;
    // md4c 0.5.2's md_parse returns 0 even when a callback aborts the parse
    // (checked), so "over budget" cannot be read from its return value.
    bool overBudget = false;
    qsizetype codeStart = 0;    // where the current code block's content starts in `out`
    QString codeInfo;           // the fence's info string, trimmed
    bool codeNeedNewline = false;   // the info string was shown as the first line
};

// A fence's info string is a LANGUAGE TAG when it is one word of the characters a
// language name has ("python", "c#", "vb.net"). Anything else typed after a fence
// is the author's own text.
bool isLanguageTag(const QString &info)
{
    if (info.isEmpty())
        return false;
    for (const QChar c : info) {
        const char16_t u = c.unicode();
        const bool ok = (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z')
            || (u >= u'0' && u <= u'9') || u == u'_' || u == u'+' || u == u'#' || u == u'.'
            || u == u'-';
        if (!ok)
            return false;
    }
    return true;
}

Renderer &R(void *u) { return *static_cast<Renderer *>(u); }

// Plain text is held back until the next event of any other kind and written
// here, escaped, with any bare address linkified. Held back because md4c may hand
// one address over in several pieces, and because md4c does NOT autolink an
// address that has a parenthesis in it ("/wiki/Tuple_(film)" -- Talk does): what
// md4c leaves as text gets the same GFM tail rule as the rest
// (BareUrlLogic.h). Never inside a link, an image's alt text or code, which do
// not come through here.
void flush(Renderer &r)
{
    if (r.pending.isEmpty())
        return;
    QString escaped;
    escaped.reserve(r.pending.size() + 16);
    appendEscaped(escaped, r.pending);
    r.out += (r.linkDepth == 0 && r.imageDepth == 0) ? talq::bareurl::linkify(escaped)
                                                     : escaped;
    r.pending.clear();
}

// Any block other than a paragraph starting inside the current container.
void noteNonParagraph(Renderer &r)
{
    if (!r.containers.isEmpty())
        r.containers.last().prevWasPara = false;
}

const char *alignAttr(MD_ALIGN a)
{
    switch (a) {
    case MD_ALIGN_CENTER: return "center";
    case MD_ALIGN_RIGHT:  return "right";
    default:              return "left";
    }
}

int enterBlock(MD_BLOCKTYPE type, void *detail, void *user)
{
    Renderer &r = R(user);
    flush(r);
    switch (type) {
    case MD_BLOCK_DOC:
        r.containers.push_back({});
        break;

    case MD_BLOCK_QUOTE: {
        noteNonParagraph(r);
        const bool written = r.quoteDepth < kMaxQuoteDepth;
        if (written) {
            // QTextDocument draws no bar for a <blockquote> and cannot be taught
            // to; a table cell is the one thing in its HTML subset that takes a
            // left border. So a quote is a one-cell table (the theme styles
            // td.q: a bar and muted ink), and a nested quote a table in a cell.
            // No width: a short quote must not stretch its bubble to full width.
            r.out += QLatin1String("<table class='q' cellspacing='0' "
                                   "cellpadding='0'><tr><td class='q'>");
            ++r.quoteDepth;
        }
        r.boxEmitted.push_back(written);
        r.containers.push_back({});
        break;
    }

    case MD_BLOCK_UL: {
        noteNonParagraph(r);
        const bool written = r.listDepth < kMaxListDepth;
        r.listTag.push_back(written ? r.out.size() : -1);
        if (written) { r.out += QLatin1String("<ul>"); ++r.listDepth; }
        r.boxEmitted.push_back(written);
        break;
    }

    case MD_BLOCK_OL: {
        noteNonParagraph(r);
        const bool written = r.listDepth < kMaxListDepth;
        r.listTag.push_back(written ? r.out.size() : -1);
        if (written) {
            const unsigned start = static_cast<MD_BLOCK_OL_DETAIL *>(detail)->start;
            if (start != 1)
                r.out += QLatin1String("<ol start='") + QString::number(start)
                    + QLatin1String("'>");
            else
                r.out += QLatin1String("<ol>");
            ++r.listDepth;
        }
        r.boxEmitted.push_back(written);
        break;
    }

    case MD_BLOCK_LI: {
        if (++r.items > kMaxListItems) {
            r.overBudget = true;
            return 1;                       // over budget: toHtml falls back to plain text
        }
        const auto *d = static_cast<MD_BLOCK_LI_DETAIL *>(detail);
        if (r.boxEmitted.isEmpty() || r.boxEmitted.last())
            r.out += QLatin1String("<li>");
        else
            r.out += QLatin1String("\n- ");           // list too deep: flatten
        if (d->is_task) {
            // A list of check boxes has no bullets (Talk: ul.contains-task-list).
            // The tag was written before this item was seen, so mark it now.
            if (!r.listTag.isEmpty() && r.listTag.last() >= 0) {
                const qsizetype gt = r.out.indexOf(QLatin1Char('>'), r.listTag.last());
                if (gt > 0 && !QStringView{r.out}.mid(r.listTag.last(),
                                                      gt - r.listTag.last()).contains(QLatin1String("class")))
                    r.out.insert(gt, QLatin1String(" class='task'"));
            }
            // A check box drawn as text: QTextDocument has no <input>. The ticked
            // one is U+2611 followed by U+FE0E (text presentation): U+2611 is in
            // the Dingbats range, so without it TalQ's own emoji drawing swaps in
            // Twemoji's blue emoji box while the empty one (U+2610) stays a plain
            // outline (see EmojiClusterLogic.h).
            if (d->task_mark == 'x' || d->task_mark == 'X') {
                r.out += QChar(0x2611);
                r.out += QChar(0xFE0E);
            } else {
                r.out += QChar(0x2610);
            }
            r.out += QLatin1Char(' ');
        }
        r.containers.push_back({});
        break;
    }

    case MD_BLOCK_HR:
        noteNonParagraph(r);
        r.out += QLatin1String("<hr>");
        break;

    case MD_BLOCK_H: {
        noteNonParagraph(r);
        const int level = std::min<int>(static_cast<MD_BLOCK_H_DETAIL *>(detail)->level
                                            + kHeadingShift, 6);
        // Always an <h6>, sized by class: QTextDocument ignores the style
        // sheet's font-size on h4 and h5 (its own tag default wins; measured on
        // Qt 6.10.1) and honours it on h6.
        r.out += QLatin1String("<h6 class='h") + QString::number(level) + QLatin1String("'>");
        break;
    }

    case MD_BLOCK_CODE: {
        noteNonParagraph(r);
        r.out += QLatin1String("<pre>");
        r.inCodeBlock = true;
        r.codeStart = r.out.size();
        // The info string is metadata ONLY when it is a language tag; Message.cpp
        // already took every closed ``` fence, so what reaches here is a ~~~ fence
        // or a fence that was never closed ("```hello" typed and sent). Read as
        // CommonMark that line is all info string and the block comes out empty --
        // the words gone. So text that is not a language tag is shown as the first
        // line of the block, and a tag with nothing after it is shown too (leaveBlock).
        r.codeInfo = attributeText(static_cast<MD_BLOCK_CODE_DETAIL *>(detail)->info).trimmed();
        r.codeNeedNewline = false;
        if (!r.codeInfo.isEmpty() && !isLanguageTag(r.codeInfo)) {
            appendEscaped(r.out, r.codeInfo);
            r.codeNeedNewline = true;
        }
        break;
    }

    case MD_BLOCK_HTML:
        // Raw HTML is disabled (MD_FLAG_NOHTML); if md4c ever reports one, its
        // text still arrives as text and is escaped like any other.
        break;

    case MD_BLOCK_P:
        if (!r.containers.isEmpty() && r.containers.last().prevWasPara)
            r.out += QLatin1String("\n\n");
        break;

    case MD_BLOCK_TABLE:
        noteNonParagraph(r);
        r.out += QLatin1String("<table border='1' cellspacing='0' cellpadding='4'>");
        break;
    case MD_BLOCK_THEAD:
    case MD_BLOCK_TBODY:
        break;
    case MD_BLOCK_TR:
        if (++r.rows > kMaxTableRows) {
            r.overBudget = true;
            return 1;
        }
        r.out += QLatin1String("<tr>");
        break;
    case MD_BLOCK_TH:
        if (++r.cells > kMaxTableCells) {
            r.overBudget = true;
            return 1;
        }
        r.out += QLatin1String("<th align='")
            + QLatin1String(alignAttr(static_cast<MD_BLOCK_TD_DETAIL *>(detail)->align))
            + QLatin1String("'>");
        break;
    case MD_BLOCK_TD:
        if (++r.cells > kMaxTableCells) {
            r.overBudget = true;
            return 1;
        }
        r.out += QLatin1String("<td align='")
            + QLatin1String(alignAttr(static_cast<MD_BLOCK_TD_DETAIL *>(detail)->align))
            + QLatin1String("'>");
        break;
    }
    return 0;
}

int leaveBlock(MD_BLOCKTYPE type, void *, void *user)
{
    Renderer &r = R(user);
    flush(r);
    switch (type) {
    case MD_BLOCK_DOC:
        break;

    case MD_BLOCK_QUOTE: {
        if (!r.containers.isEmpty()) r.containers.pop_back();
        const bool written = !r.boxEmitted.isEmpty() && r.boxEmitted.takeLast();
        if (written) { r.out += QLatin1String("</td></tr></table>"); --r.quoteDepth; }
        break;
    }

    case MD_BLOCK_UL: {
        if (!r.listTag.isEmpty()) r.listTag.pop_back();
        const bool written = !r.boxEmitted.isEmpty() && r.boxEmitted.takeLast();
        if (written) { r.out += QLatin1String("</ul>"); --r.listDepth; }
        break;
    }
    case MD_BLOCK_OL: {
        if (!r.listTag.isEmpty()) r.listTag.pop_back();
        const bool written = !r.boxEmitted.isEmpty() && r.boxEmitted.takeLast();
        if (written) { r.out += QLatin1String("</ol>"); --r.listDepth; }
        break;
    }

    case MD_BLOCK_LI:
        if (!r.containers.isEmpty()) r.containers.pop_back();
        if (r.boxEmitted.isEmpty() || r.boxEmitted.last())
            r.out += QLatin1String("</li>");
        break;

    case MD_BLOCK_HR:
        break;

    case MD_BLOCK_H:
        r.out += QLatin1String("</h6>");
        break;

    case MD_BLOCK_CODE:
        // The block's own final newline is not content.
        while (r.out.size() > r.codeStart && r.out.endsWith(QLatin1Char('\n')))
            r.out.chop(1);
        // A fence with a one-word tag and NOTHING after it ("```hello"): the word
        // is all the author wrote. Show it rather than an empty box.
        if (r.out.size() == r.codeStart && !r.codeInfo.isEmpty())
            appendEscaped(r.out, r.codeInfo);
        r.out += QLatin1String("</pre>");
        r.inCodeBlock = false;
        break;

    case MD_BLOCK_HTML:
        break;

    case MD_BLOCK_P:
        if (!r.containers.isEmpty()) r.containers.last().prevWasPara = true;
        break;

    case MD_BLOCK_TABLE: r.out += QLatin1String("</table>"); break;
    case MD_BLOCK_THEAD:
    case MD_BLOCK_TBODY: break;
    case MD_BLOCK_TR:    r.out += QLatin1String("</tr>"); break;
    case MD_BLOCK_TH:    r.out += QLatin1String("</th>"); break;
    case MD_BLOCK_TD:    r.out += QLatin1String("</td>"); break;
    }
    return 0;
}

int enterSpan(MD_SPANTYPE type, void *detail, void *user)
{
    Renderer &r = R(user);
    flush(r);

    if (type == MD_SPAN_IMG) {
        OpenSpan s;
        s.isImage = true;
        s.url = attributeText(static_cast<MD_SPAN_IMG_DETAIL *>(detail)->src);
        s.outAtStart = 0;
        // An image is never fetched. If its address is one we may open it
        // becomes a link that shows the alt text.
        if (r.imageDepth == 0 && isOpenableLinkUrl(s.url)) {
            r.out += QLatin1String("<a href='") + hrefAttribute(s.url) + QLatin1String("'>");
            s.emittedAnchor = true;
        }
        s.outAtStart = r.out.size();
        r.spans.push_back(s);
        ++r.imageDepth;
        return 0;
    }

    if (r.imageDepth > 0)
        return 0;               // inside an alt text: the words, no tags

    switch (type) {
    case MD_SPAN_EM:     r.out += QLatin1String("<i>"); break;
    case MD_SPAN_STRONG: r.out += QLatin1String("<b>"); break;
    case MD_SPAN_DEL:    r.out += QLatin1String("<s>"); break;
    case MD_SPAN_CODE:   r.out += QLatin1String("<code>"); break;
    case MD_SPAN_A: {
        OpenSpan s;
        const QString url = attributeText(static_cast<MD_SPAN_A_DETAIL *>(detail)->href);
        if (isOpenableLinkUrl(url)) {
            r.out += QLatin1String("<a href='") + hrefAttribute(url) + QLatin1String("'>");
            s.emittedAnchor = true;
        }
        r.spans.push_back(s);
        ++r.linkDepth;
        break;
    }
    default:
        break;                  // U, LATEXMATH, WIKILINK: flags are off
    }
    return 0;
}

int leaveSpan(MD_SPANTYPE type, void *, void *user)
{
    Renderer &r = R(user);
    flush(r);

    if (type == MD_SPAN_IMG) {
        --r.imageDepth;
        if (r.spans.isEmpty()) return 0;
        const OpenSpan s = r.spans.takeLast();
        // ![](url) has no words to show: show the address instead of nothing.
        if (s.emittedAnchor && r.out.size() == s.outAtStart)
            appendEscaped(r.out, s.url);
        if (s.emittedAnchor)
            r.out += QLatin1String("</a>");
        return 0;
    }

    if (r.imageDepth > 0)
        return 0;

    switch (type) {
    case MD_SPAN_EM:     r.out += QLatin1String("</i>"); break;
    case MD_SPAN_STRONG: r.out += QLatin1String("</b>"); break;
    case MD_SPAN_DEL:    r.out += QLatin1String("</s>"); break;
    case MD_SPAN_CODE:   r.out += QLatin1String("</code>"); break;
    case MD_SPAN_A:
        if (r.linkDepth > 0) --r.linkDepth;
        if (!r.spans.isEmpty()) {
            const OpenSpan s = r.spans.takeLast();
            if (s.emittedAnchor)
                r.out += QLatin1String("</a>");
        }
        break;
    default:
        break;
    }
    return 0;
}

int onText(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *user)
{
    Renderer &r = R(user);
    if (type == MD_TEXT_NORMAL) {
        r.pending += QString::fromUtf8(text, qsizetype(size));
        return 0;
    }
    flush(r);
    switch (type) {
    case MD_TEXT_NULLCHAR:
        r.out += QChar(0xFFFD);
        break;
    case MD_TEXT_BR:
    case MD_TEXT_SOFTBR:
        // remark-breaks: a single newline is a line break, in Talk and here.
        r.out += QLatin1Char('\n');
        break;
    case MD_TEXT_ENTITY:
        appendEscaped(r.out, decodeEntity(QString::fromUtf8(text, qsizetype(size))));
        break;
    default:    // CODE, HTML, LATEXMATH
        if (r.inCodeBlock && r.codeNeedNewline) {
            r.out += QLatin1Char('\n');         // the shown info string was line one
            r.codeNeedNewline = false;
        }
        appendEscapedUtf8(r.out, text, size);
        break;
    }
    return 0;
}

} // namespace

bool isOpenableLinkUrl(const QString &url)
{
    const QString u = url.trimmed();
    const qsizetype colon = u.indexOf(QLatin1Char(':'));
    if (colon <= 0)
        return false;                       // relative, "#frag", "//host/x"
    const QString scheme = u.left(colon).toLower();
    for (const QChar c : u) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7F)
            return false;
    }
    const QStringView rest = QStringView{u}.mid(colon + 1);
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        return rest.startsWith(QLatin1String("//")) && rest.size() > 2;
    if (scheme == QLatin1String("mailto") || scheme == QLatin1String("tel"))
        return !rest.isEmpty();
    return false;
}

QString toHtml(const QString &text)
{
    if (text.isEmpty())
        return text;

    const QByteArray utf8 = text.toUtf8();

    Renderer r;
    r.out.reserve(text.size() + text.size() / 4 + 16);

    MD_PARSER parser;
    parser.abi_version = 0;
    parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS
        | MD_FLAG_PERMISSIVEURLAUTOLINKS | MD_FLAG_PERMISSIVEWWWAUTOLINKS
        | MD_FLAG_PERMISSIVEEMAILAUTOLINKS
        | MD_FLAG_NOHTML                    // no raw HTML: the text is untrusted
        | MD_FLAG_NOINDENTEDCODEBLOCKS;     // an indented run is CodePasteHeuristic's
    parser.enter_block = &enterBlock;
    parser.leave_block = &leaveBlock;
    parser.enter_span = &enterSpan;
    parser.leave_span = &leaveSpan;
    parser.text = &onText;
    parser.debug_log = nullptr;
    parser.syntax = nullptr;

    if (md_parse(utf8.constData(), MD_SIZE(utf8.size()), &parser, &r) != 0 || r.overBudget) {
        // A parser failure, or a message over the structure budget (above), must
        // never cost anybody their message: it is shown as the plain text it is,
        // addresses linked.
        return talq::bareurl::linkify(text.toHtmlEscaped());
    }
    return r.out;
}

} // namespace talq::markdown
