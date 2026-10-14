#pragma once

// Finds UNFENCED code pasted into a chat message, so Message.cpp can box it.
//
// Why it exists. People paste indented code, pretty-printed JSON, a stack
// trace, a PowerShell block, and do not type a ``` fence around it -- a NOC
// team does this all day. Such a message is plain prose to the renderer, and
// that hurts twice: HTML collapses leading whitespace, so
//
//     def f(x):
//         if x:
//             return 1
//
// arrives as "def f(x):<LS>if x:<LS>return 1" with the structure gone; and not
// being a <pre> it gets none of what a code block has (monospace box, the copy
// chip). This turns a run of such lines into one <pre>, verbatim.
//
// CONSERVATIVE BY CONSTRUCTION. A false positive turns ordinary prose into a
// grey monospace box, which is worse than leaving a code paste unboxed, so
// every rule below errs towards "not code":
//
//  * Only lines indented by a TAB or two-plus spaces can be the body of a run,
//    and a run needs at least three of them. One or two indented lines are a
//    hanging indent or a quote, not a paste.
//  * A run containing a quote line ("&gt;") is never code.
//  * A run that is half bullets, numbered steps or ordinary sentences is a list
//    or an indented paragraph -- a pasted assistant transcript is exactly that,
//    two-space indented prose with bullets in it -- not code.
//  * The header of a snippet ("def f(x):", "{", "foo = [") is pulled into the
//    box, one line only and only when it ENDS like the start of a block;
//    ordinary prose such as "Here is the output:" stays outside. A trailing
//    ':', ',' or '(' only counts with a code token on the line: a '(' glued to
//    a name ("f("), a '{' that is not a {mention-...} placeholder, an '=' or a
//    comparison -- and for ':' also a bare block keyword ("else:", "class A:")
//    or a bare YAML/JSON key. So "Here is the output (from yesterday):", "Hi
//    {mention-user1}, here is the log:", "Hi team,", "try this:" and "for
//    example:" are prose and stay outside the box.
//  * A line is prose even if it carries a `quoted command`, a {mention-...}
//    placeholder or an @name: those are ordinary things to type in a sentence.
//
// WHAT IT DELIBERATELY STILL BOXES: three or more short indented lines under a
// bare label ("Servers:" over web1/web2/db1, an address, a signature) look
// exactly like the NOC output this exists for, and cannot be told apart from it
// by their characters. That is a documented trade-off, pinned by the tests as an
// accepted limitation: the text is verbatim in the box, nothing is lost, and the
// worst case is a grey box and a copy button on a short indented list.
//
// The input is the message text AFTER HTML-escaping (Message.cpp
// toHtmlEscaped). That matters: `>` arrives as "&gt;" (so "=>" is "=&gt;" and a
// quote line starts "&gt;"), `"` as "&quot;", and the checks below match the
// escaped spellings. The raw spellings are accepted too, so the function is
// also correct on unescaped text.
//
// Pure Qt-Core, no regular expressions: every pass is a single left-to-right
// scan, so a 32000-character message costs O(n) and cannot backtrack.
//
// Test: code-paste-heuristic-test.

#include <QChar>
#include <QLatin1String>
#include <QString>
#include <QStringView>
#include <QVector>

namespace talq::codepaste {

// A run's body must have this many non-blank lines.
constexpr int kMinCodeLines = 3;
// How many lines of bare closers ("}", "]);") may follow the body.
constexpr int kMaxCloserLines = 3;

// One detected run, as character offsets into the text it was found in:
// [begin, end) covers the whole lines, EXCLUDING the '\n' that follows the last
// one, so replacing it leaves every surrounding separator untouched.
struct Run {
    qsizetype begin = 0;
    qsizetype end = 0;
    int firstLine = 0;       // 0-based, inclusive, header included
    int lastLine = 0;        // 0-based, inclusive, closers included
    int codeLines = 0;       // non-blank body lines (what the thresholds judged)
    int listItems = 0;       // how many of those look like list items
    int proseLines = 0;      // how many of those are sentence-like (list items included)
    bool hasHeader = false;  // the line before the body was pulled in
    int closerLines = 0;     // bare-closer lines pulled in after the body
};

namespace detail {

inline bool isAsciiWord(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z')
        || (u >= u'0' && u <= u'9') || u == u'_';
}

inline bool isBlank(QStringView s)
{
    for (const QChar c : s)
        if (!c.isSpace()) return false;
    return true;
}

// A TAB, or at least two spaces, in column 0. (A single leading space is the
// ordinary hanging space of a wrapped sentence.)
inline bool isIndented(QStringView s)
{
    if (s.isEmpty()) return false;
    if (s[0] == QLatin1Char('\t')) return true;
    return s.size() >= 2 && s[0] == QLatin1Char(' ') && s[1] == QLatin1Char(' ');
}

inline QStringView lstrip(QStringView s)
{
    qsizetype i = 0;
    while (i < s.size() && s[i].isSpace()) ++i;
    return s.mid(i);
}

inline QStringView rstrip(QStringView s)
{
    while (!s.isEmpty() && s.back().isSpace()) s.chop(1);
    return s;
}

// "> quoted" -- in escaped text, "&gt; quoted".
inline bool isQuote(QStringView s)
{
    s = lstrip(s);
    return s.startsWith(QLatin1String("&gt;")) || s.startsWith(QLatin1Char('>'));
}

// ^\s*([-*+•–]|\d{1,3}[.)])\s+  -- a bullet or a numbered step.
inline bool isListItem(QStringView s)
{
    s = lstrip(s);
    if (s.isEmpty()) return false;
    const QChar c = s[0];
    qsizetype after = 0;
    if (c == QLatin1Char('-') || c == QLatin1Char('*') || c == QLatin1Char('+')
        || c == QChar(0x2022) || c == QChar(0x2013)) {
        after = 1;
    } else if (c.unicode() >= u'0' && c.unicode() <= u'9') {
        qsizetype digits = 0;
        while (digits < s.size() && s[digits].unicode() >= u'0' && s[digits].unicode() <= u'9')
            ++digits;
        if (digits > 3 || digits >= s.size()) return false;
        if (s[digits] != QLatin1Char('.') && s[digits] != QLatin1Char(')')) return false;
        after = digits + 1;
    } else {
        return false;
    }
    return after < s.size() && s[after].isSpace();
}

inline bool hasCallParen(QStringView s);   // defined below

// A sentence-like line: six or more tokens, at least 80% of them plain words
// (letters in any script, with internal apostrophes or hyphens, and
// quotes/brackets/".,;:!?" at the edges), and nothing code is made of: no
// brace, '=', angle bracket, backslash, pipe, '$', '#', '@', '`', and no
// call-shaped "name(". An indented paragraph or a pasted assistant transcript is
// made of these lines; a statement, a path, a flag list, a log record or a
// column of output is not.
//
// Evaluated on escaped text, so "&lt;" / "&gt;" are '<' / '>' (code), "&quot;"
// is a quote (prose), "&amp;" is an ampersand glued into a token (not a word).
inline bool isProseLine(QStringView s)
{
    s = rstrip(lstrip(s));
    // a bullet or number marker is not part of the sentence
    if (isListItem(s)) {
        qsizetype i = 0;
        while (i < s.size() && !s[i].isSpace()) ++i;
        s = lstrip(s.mid(i));
    }

    // Three things that look like code characters but are ordinary prose in a
    // chat, and so are read as a WORD:
    //  * an inline-code span `like this` -- a sentence with a command quoted in
    //    it is far likelier than code that contains backticks;
    //  * a rich-object placeholder {mention-user1} / {file}, which
    //    Message::fromJson substitutes with a name AFTER this runs (hasCodeBrace
    //    already treats them as prose; this line must agree);
    //  * @name, a mention typed out.
    // Without this, "{mention-user1} is on leave until Friday and back Monday"
    // failed the sentence test on its first character and a whole indented
    // paragraph of mentions was boxed as code.
    QString normalised;
    {
        bool changed = false;
        normalised.reserve(int(s.size()));
        for (qsizetype i = 0; i < s.size(); ++i) {
            const QChar c = s[i];
            if (c == QLatin1Char('`')) {
                const qsizetype close = s.indexOf(QLatin1Char('`'), i + 1);
                if (close > i + 1) {
                    normalised += QLatin1Char('x');
                    i = close;
                    changed = true;
                    continue;
                }
            } else if (c == QLatin1Char('{')) {
                qsizetype j = i + 1;
                while (j < s.size()
                       && (isAsciiWord(s[j]) || s[j] == QLatin1Char('-') || s[j] == QLatin1Char('.')))
                    ++j;
                if (j > i + 1 && j < s.size() && s[j] == QLatin1Char('}')) {
                    normalised += QLatin1Char('x');
                    i = j;
                    changed = true;
                    continue;
                }
            } else if (c == QLatin1Char('@') && i + 1 < s.size() && s[i + 1].isLetter()
                       && (i == 0 || s[i - 1].isSpace())) {
                changed = true;          // drop the '@': the name that follows is a word
                continue;
            }
            normalised += c;
        }
        if (changed)
            s = QStringView(normalised);
    }

    if (hasCallParen(s))
        return false;

    auto edgeLead = [](QChar c) {
        return c == QLatin1Char('(') || c == QLatin1Char('"') || c == QLatin1Char('\'')
            || c == QChar(0x201C) || c == QChar(0x2018) || c == QChar(0x201E);
    };
    auto edgeTrail = [](QChar c) {
        return c == QLatin1Char(',') || c == QLatin1Char('.') || c == QLatin1Char(';')
            || c == QLatin1Char(':') || c == QLatin1Char('!') || c == QLatin1Char('?')
            || c == QLatin1Char(')') || c == QLatin1Char('"') || c == QLatin1Char('\'')
            || c == QChar(0x201D) || c == QChar(0x2019) || c == QChar(0x2026)
            || c == QChar(0x2018) || c == QChar(0x201C);
    };

    int tokens = 0, words = 0;
    qsizetype i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i].isSpace()) ++i;
        if (i >= s.size()) break;
        qsizetype b = i;
        while (i < s.size() && !s[i].isSpace()) {
            switch (s[i].unicode()) {
            case u'{': case u'}': case u'=': case u'<': case u'>': case u'\\': case u'|':
            case u'$': case u'[': case u']': case u'#': case u'^': case u'~': case u'@':
            case u'`':
                return false;
            case u'&': {
                const QStringView rest = s.mid(i);
                if (rest.startsWith(QLatin1String("&lt;")) || rest.startsWith(QLatin1String("&gt;")))
                    return false;
                break;
            }
            default: break;
            }
            ++i;
        }
        QStringView tok = s.mid(b, i - b);
        // the escaped quote is punctuation at a token's edge, like '"'
        while (tok.startsWith(QLatin1String("&quot;"))) tok = tok.mid(6);
        while (tok.endsWith(QLatin1String("&quot;"))) tok.chop(6);
        while (!tok.isEmpty() && edgeLead(tok.front())) tok = tok.mid(1);
        for (;;) {
            if (tok.endsWith(QLatin1String("&quot;"))) { tok.chop(6); continue; }
            if (!tok.isEmpty() && edgeTrail(tok.back())) { tok.chop(1); continue; }
            break;
        }
        ++tokens;
        bool letter = false, ok = !tok.isEmpty();
        for (qsizetype k = 0; ok && k < tok.size(); ++k) {
            const QChar c = tok[k];
            if (c.isLetter()) letter = true;
            else if (c == QLatin1Char('\'') || c == QChar(0x2019)
                     || (c == QLatin1Char('-') && k > 0 && k + 1 < tok.size())) { /* ok inside */ }
            else ok = false;       // a digit, '_', '/', '.', ':' ... is not a word
        }
        if (ok && letter) ++words;
    }
    return tokens >= 6 && words * 5 >= tokens * 4;
}

// A line made of nothing but closing brackets: "}", "]);", "  ),".
inline bool isCloserOnly(QStringView s)
{
    s = rstrip(lstrip(s));
    qsizetype i = 0;
    while (i < s.size()
           && (s[i] == QLatin1Char(']') || s[i] == QLatin1Char(')') || s[i] == QLatin1Char('}')))
        ++i;
    if (i == 0) return false;
    if (i < s.size() && (s[i] == QLatin1Char(';') || s[i] == QLatin1Char(','))) ++i;
    return i == s.size();
}

// An identifier (or ')' / ']') glued to '(' -- a call or a signature, "f(x)",
// "class A(B)". A '(' after a space is punctuation in a sentence, "output (from
// yesterday):", and is deliberately NOT enough to pull a prose line into the box.
inline bool hasCallParen(QStringView s)
{
    for (qsizetype i = 1; i < s.size(); ++i) {
        if (s[i] != QLatin1Char('(')) continue;
        const QChar p = s[i - 1];
        if (isAsciiWord(p) || p == QLatin1Char(')') || p == QLatin1Char(']'))
            return true;
    }
    return false;
}

// A '{' that opens code. "{mention-user1}" / "{file}" are rich-object
// placeholders (Message::fromJson substitutes them AFTER this runs), and they
// sit in ordinary prose -- "Hi {mention-user1}, here is the log:" must not read
// as a block header.
inline bool hasCodeBrace(QStringView s)
{
    for (qsizetype i = 0; i < s.size(); ++i) {
        if (s[i] != QLatin1Char('{')) continue;
        qsizetype j = i + 1;
        while (j < s.size()
               && (isAsciiWord(s[j]) || s[j] == QLatin1Char('-') || s[j] == QLatin1Char('.')))
            ++j;
        const bool placeholder = j > i + 1 && j < s.size() && s[j] == QLatin1Char('}');
        if (!placeholder) return true;
    }
    return false;
}

inline bool startsWithBlockKeyword(QStringView s)
{
    static const QLatin1String kKeywords[] = {
        QLatin1String("def"),    QLatin1String("class"),   QLatin1String("if"),
        QLatin1String("elif"),   QLatin1String("else"),    QLatin1String("for"),
        QLatin1String("while"),  QLatin1String("try"),     QLatin1String("except"),
        QLatin1String("finally"),QLatin1String("with"),    QLatin1String("switch"),
        QLatin1String("case"),   QLatin1String("function"),QLatin1String("func"),
        QLatin1String("fn"),
    };
    s = lstrip(s);
    for (const QLatin1String kw : kKeywords) {
        if (!s.startsWith(kw)) continue;
        if (s.size() == kw.size() || !isAsciiWord(s[kw.size()]))
            return true;
    }
    return false;
}

// ^[\w.\-"']+:$  -- a YAML / JSON key on a line of its own. `s` ends with ':'.
// Quotes arrive escaped ("&quot;"), so that is accepted as a quote.
inline bool isBareKey(QStringView s)
{
    s.chop(1);
    if (s.isEmpty()) return false;
    qsizetype i = 0;
    while (i < s.size()) {
        const QChar c = s[i];
        if (isAsciiWord(c) || c == QLatin1Char('.') || c == QLatin1Char('-')
            || c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            ++i;
        } else if (c == QLatin1Char('&') && s.mid(i).startsWith(QLatin1String("&quot;"))) {
            i += 6;
        } else {
            return false;
        }
    }
    return true;
}

// Something on the line, besides how it ends, that says "this is code": a call
// or signature, a code brace, an assignment or a comparison. A greeting
// ("Hi team,") or a lead-in ("try this:") has none of these.
inline bool hasCodeToken(QStringView s)
{
    return hasCallParen(s) || hasCodeBrace(s) || s.contains(QLatin1Char('='))
        || s.contains(QLatin1String("&lt;")) || s.contains(QLatin1String("&gt;"));
}

// `else:` / `try:` / `finally:` on a line of their own, or `class Name:` /
// `def name:`. Anything else that merely STARTS with a block keyword -- "try
// this:", "for example:", "with this config:", "if you want:" -- is a sentence.
inline bool isBlockKeywordHeader(QStringView s)
{
    s = lstrip(s);
    static const QLatin1String kBare[] = {
        QLatin1String("else:"), QLatin1String("try:"), QLatin1String("finally:"),
    };
    for (const QLatin1String kw : kBare)
        if (s == kw) return true;

    static const QLatin1String kNamed[] = { QLatin1String("class"), QLatin1String("def") };
    for (const QLatin1String kw : kNamed) {
        if (!s.startsWith(kw) || s.size() <= kw.size() || !s[kw.size()].isSpace()) continue;
        QStringView rest = lstrip(s.mid(kw.size()));
        if (rest.size() < 2 || rest.back() != QLatin1Char(':')) continue;
        rest.chop(1);
        bool oneWord = true;
        for (const QChar c : rest) if (!isAsciiWord(c)) { oneWord = false; break; }
        if (oneWord) return true;
    }
    // A keyword with an expression after it: "if x > 0:", "for i in range(3):",
    // "while (a == b):". The expression is what proves it is code.
    return startsWithBlockKeyword(s) && hasCodeToken(s);
}

// Can `line`, sitting directly above an indented body, be the first line of it?
inline bool isHeaderLine(QStringView line)
{
    const QStringView s = rstrip(line);
    if (s.isEmpty() || isQuote(s)) return false;

    const QChar last = s.back();
    // Ends the way an unfinished statement / opened block does. '{', '[', '='
    // and a line continuation are code whatever precedes them.
    if (last == QLatin1Char('{') || last == QLatin1Char('[')
        || last == QLatin1Char('=') || last == QLatin1Char('\\'))
        return true;
    if (s.endsWith(QLatin1String("=&gt;")) || s.endsWith(QLatin1String("-&gt;"))
        || s.endsWith(QLatin1String("=>"))  || s.endsWith(QLatin1String("->")))
        return true;

    // A trailing ',' or '(' is NOT enough on its own: "Hi team," and "See the
    // manual (" end that way and are prose. Pulled into the box a greeting was
    // drawn in monospace and went onto the clipboard with the code. Require a
    // call/brace/assignment on the line ("foo(", "foo(a,", "x = 1,"), or a lone
    // "(" which opens a SQL or tuple block.
    if (last == QLatin1Char(',') || last == QLatin1Char('('))
        return hasCodeToken(s) || s == QLatin1String("(");

    // A trailing ':' opens a block only when something else says "code": a
    // call/signature, a brace, a block header, or a bare key. A sentence that
    // merely ends in a colon is prose.
    if (last != QLatin1Char(':')) return false;
    return hasCallParen(s) || hasCodeBrace(s) || isBlockKeywordHeader(s) || isBareKey(s);
}

} // namespace detail

// Every code run in `text` (already HTML-escaped), left to right, never
// overlapping. Empty when there is none.
inline QVector<Run> findRuns(const QString &text)
{
    using namespace detail;
    QVector<Run> runs;
    if (!text.contains(QLatin1Char('\n')))
        return runs;

    enum Kind : unsigned char { Blank, Indented, Plain };
    struct Line {
        qsizetype pos = 0;
        qsizetype len = 0;
        Kind kind = Plain;
        bool list = false;
        bool prose = false;     // sentence-like, or a list item
        bool quote = false;
    };

    const QStringView all(text);
    QVector<Line> lines;
    lines.reserve(int(text.count(QLatin1Char('\n'))) + 1);
    for (qsizetype pos = 0;;) {
        const qsizetype nl = text.indexOf(QLatin1Char('\n'), pos);
        const qsizetype end = nl < 0 ? text.size() : nl;
        const QStringView v = all.mid(pos, end - pos);
        Line l;
        l.pos = pos;
        l.len = end - pos;
        if (isBlank(v)) {
            l.kind = Blank;
        } else if (isIndented(v)) {
            l.kind = Indented;
            l.list = isListItem(v);
            l.prose = l.list || isProseLine(v);
            l.quote = isQuote(v);
        }
        lines.append(l);
        if (nl < 0) break;
        pos = nl + 1;
    }

    const int n = int(lines.size());
    auto view = [&](int i) { return all.mid(lines[i].pos, lines[i].len); };

    int lastConsumed = -1;      // the header of a later run must not steal a line
    int i = 0;
    while (i < n) {
        if (lines[i].kind == Plain) { ++i; continue; }

        // A maximal stretch of indented-or-blank lines: [s, e].
        const int s = i;
        while (i < n && lines[i].kind != Plain) ++i;
        const int e = i - 1;

        // Begins and ends with an indented line.
        int a = s;
        while (a <= e && lines[a].kind == Blank) ++a;
        int b = e;
        while (b >= a && lines[b].kind == Blank) --b;
        if (a > b) continue;

        int codeLines = 0, listItems = 0, proseLines = 0;
        bool quote = false;
        for (int k = a; k <= b; ++k) {
            if (lines[k].kind != Indented) continue;
            ++codeLines;
            if (lines[k].list) ++listItems;
            if (lines[k].prose) ++proseLines;
            if (lines[k].quote) quote = true;
        }
        // Half or more bullets / sentences is a list or an indented paragraph.
        if (codeLines < kMinCodeLines || quote || proseLines * 2 >= codeLines)
            continue;

        Run r;
        r.firstLine = a;
        r.lastLine = b;
        r.codeLines = codeLines;
        r.listItems = listItems;
        r.proseLines = proseLines;

        // The header: ONE immediately preceding line, never across a blank.
        if (a == s && a >= 1 && a - 1 > lastConsumed && lines[a - 1].kind == Plain
            && isHeaderLine(view(a - 1))) {
            r.firstLine = a - 1;
            r.hasHeader = true;
        }
        // The footer: up to three bare-closer lines, again never across a blank
        // (a blank after the body means b < e and we do not look further).
        if (b == e) {
            for (int j = b + 1; j < n && r.closerLines < kMaxCloserLines
                                && lines[j].kind == Plain && isCloserOnly(view(j)); ++j) {
                r.lastLine = j;
                ++r.closerLines;
            }
        }

        r.begin = lines[r.firstLine].pos;
        r.end = lines[r.lastLine].pos + lines[r.lastLine].len;
        // A CRLF body leaves the last line's '\r' in the run, which the layout
        // later turns into a <br> just before </pre>: an empty last line in the
        // box, and a trailing newline on the clipboard (a copied command that
        // then runs at once). The run ends at the last visible character.
        while (r.end > r.begin && text.at(r.end - 1) == QLatin1Char('\r'))
            --r.end;
        runs.append(r);
        lastConsumed = r.lastLine;
    }
    return runs;
}

// `text` with every run replaced by park(runText). `park` receives the run's
// lines verbatim (indentation, tabs and inner blank lines intact) and returns
// what stands in for them -- Message.cpp parks the finished <pre> under a
// sentinel so emphasis markers inside it are never re-parsed.
template <class Park>
QString wrapRuns(const QString &text, Park &&park)
{
    const QVector<Run> runs = findRuns(text);
    if (runs.isEmpty())
        return text;
    QString out;
    out.reserve(text.size() + 16 * int(runs.size()));
    qsizetype pos = 0;
    for (const Run &r : runs) {
        out += QStringView(text).mid(pos, r.begin - pos);
        out += park(text.mid(r.begin, r.end - r.begin));
        pos = r.end;
    }
    out += QStringView(text).mid(pos);
    return out;
}

} // namespace talq::codepaste
