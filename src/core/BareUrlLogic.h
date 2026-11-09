#pragma once

// Where a bare web address in a chat message ENDS.
//
// Why it exists. "(see https://x.y)" used to open "https://x.y)" and "go to
// https://x.y." linked the full stop: the old linkifier took everything up to
// the next space, so the punctuation of the sentence around an address became
// part of the address and the link was dead.
//
// The rule is the one Talk's own client applies to every markdown message.
// NcRichText hands the text to remark-gfm, whose "autolink literal" extension
// (GFM spec, "Autolinks (extension)") drops the trailing characters
//
//     !  "  '  )  *  ,  .  :  ;  <  ?  ]  _  ~
//
// from the link, and keeps a closing ) ] } only when it has a partner INSIDE the
// address (a Wikipedia URL ending "_(film)" stays whole, "(see https://x.y)"
// does not take the bracket). Checked against nextcloud-vue 9.9
// (NcRichText.vue: "remark-gfm has its own autolink parser which can not be
// disabled").
//
// The input is the candidate AS IT SITS IN THE STORED HTML, i.e. HTML-escaped by
// Message.cpp: a typed `&` is "&amp;", a typed `<` is "&lt;". Those spellings
// matter here:
//   * "&lt;", "&gt;" and "&quot;" are a typed < > " -- characters that END an
//     address (the raw regex always excluded them; escaping turned them into
//     ordinary-looking characters and they were swept into the link, so
//     `see "https://x.y" ok` linked  https://x.y&quot;  ).
//   * a trailing "&amp;" is a typed lone `&`, punctuation like any other.
//
// Pure Qt-Core, one left-to-right pass plus a bounded trim: a 32000-character
// message costs O(n). Test: bare-url-logic-test.

#include <QChar>
#include <QLatin1String>
#include <QRegularExpression>
#include <QString>
#include <QStringView>

namespace talq::bareurl {

namespace detail {

inline bool isTrailingPunct(QChar c)
{
    switch (c.unicode()) {
    case u'!': case u'"': case u'\'': case u'*': case u',': case u'.':
    case u':': case u';': case u'?': case u'_': case u'~':
    case u'\\':   // Qt's HTML reader treats \' as an escaped quote inside href='...'
        return true;
    default:
        return false;
    }
}

inline qsizetype count(QStringView s, QChar c)
{
    qsizetype n = 0;
    for (const QChar x : s)
        if (x == c) ++n;
    return n;
}

// A closer (")", "]", "}") belongs to the address only if the address itself
// opened one more than it has closed.
inline bool closerIsUnbalanced(QStringView s, QChar closer)
{
    const QChar opener = closer == QLatin1Char(')') ? QLatin1Char('(')
                       : closer == QLatin1Char(']') ? QLatin1Char('[')
                                                    : QLatin1Char('{');
    return count(s, closer) > count(s, opener);
}

} // namespace detail

// How many characters of `candidate` (which starts "http://" or "https://" and
// runs to the next space / '<' / quote) are the ADDRESS. The rest is the
// sentence the address was typed in. 0 when nothing but the scheme is left.
inline qsizetype linkLength(QStringView candidate)
{
    // An escaped < > " ends the address, wherever it is.
    for (const QLatin1String stop : {QLatin1String("&lt;"), QLatin1String("&gt;"),
                                     QLatin1String("&quot;")}) {
        const qsizetype at = candidate.indexOf(stop);
        if (at >= 0) candidate = candidate.left(at);
    }

    for (;;) {
        if (candidate.isEmpty())
            break;
        if (candidate.endsWith(QLatin1String("&amp;"))) {   // a typed lone '&'
            candidate.chop(5);
            continue;
        }
        const QChar last = candidate.back();
        if (detail::isTrailingPunct(last)) {
            candidate.chop(1);
            continue;
        }
        if ((last == QLatin1Char(')') || last == QLatin1Char(']') || last == QLatin1Char('}'))
            && detail::closerIsUnbalanced(candidate, last)) {
            candidate.chop(1);
            continue;
        }
        break;
    }

    // "https://" alone is not an address.
    const qsizetype schemeEnd = candidate.indexOf(QLatin1String("://"));
    if (schemeEnd < 0 || candidate.size() <= schemeEnd + 3)
        return 0;
    return candidate.size();
}

// `escaped` (HTML-escaped text with no tags in it) with every bare http(s)
// address written as <a href='...'>...</a>, per linkLength() above.
//
// Two callers: the linkifier for a message that is not markdown, and the
// markdown renderer for the plain text it sees between its own links -- md4c
// does not autolink an address with a parenthesis in it ("/wiki/Tuple_(film)"),
// which Talk does, so what md4c leaves as text is linkified here by the same
// rule. Text inside a link, a code span or an image's alt text never comes here.
//
// The regex finds where a candidate STARTS and runs to the next space, '<' or
// quote; linkLength() decides where the ADDRESS ends. The href is written inside
// single quotes and Qt's HTML parser reads \' as an escaped quote, so a trailing
// backslash is kept out of the link by linkLength(); one in the MIDDLE is
// untouched.
inline QString linkify(const QString &escaped)
{
    // \x01 and \x02 are the markers Message.cpp parks a code block under; an
    // address must end at one, or the block's <pre> would be written into the
    // href when the marker is swapped back.
    static const QRegularExpression urlRx(
        QStringLiteral(R"(https?://[^\s<>"'\x01\x02]+)"),
        QRegularExpression::CaseInsensitiveOption);

    if (!escaped.contains(QLatin1String("://")))
        return escaped;

    QString out;
    qsizetype pos = 0;      // everything before this is already in `out`
    qsizetype from = 0;     // where the next search starts
    for (;;) {
        const auto m = urlRx.match(escaped, from);
        if (!m.hasMatch())
            break;
        const qsizetype n = linkLength(m.capturedView());
        if (n <= 0) {                        // "http://" and nothing after it
            from = m.capturedEnd();
            continue;
        }
        out += QStringView{escaped}.mid(pos, m.capturedStart() - pos);
        const QStringView url = m.capturedView().left(n);
        out += QLatin1String("<a href='");
        // A brace in the TARGET is percent-encoded: a {mention-userN} placeholder
        // is replaced with markup after this runs (Message::fromJson), and must not
        // be able to land inside the quoted href. The visible text keeps it.
        for (const QChar c : url) {
            if (c == QLatin1Char('{'))      out += QLatin1String("%7B");
            else if (c == QLatin1Char('}')) out += QLatin1String("%7D");
            else                            out += c;
        }
        out += QLatin1String("'>");
        out += url;
        out += QLatin1String("</a>");
        // The tail (")" "." "&quot;" ...) stays plain text, and the search
        // resumes INSIDE it: after a cut at "&lt;" another address may follow.
        pos = from = m.capturedStart() + n;
    }
    if (pos == 0)
        return escaped;
    out += QStringView{escaped}.mid(pos);
    return out;
}

} // namespace talq::bareurl
