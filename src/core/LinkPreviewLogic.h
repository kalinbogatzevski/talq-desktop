#pragma once

// Pure decisions behind link previews in the chat ("unfurling" a pasted URL).
//
// The server does the fetching: Nextcloud's Reference API
// (GET /ocs/v2.php/references/resolve?reference=<url>, the call Talk's own web
// client makes) loads the page, reads its OpenGraph tags and answers with a
// title, a description and a thumbnail. Three things follow from that and shape
// every rule below:
//
//   * The linked site never sees the reader's IP -- only the Nextcloud server
//     does. That is the reason this is done through the server and not by
//     fetching the page in the client, and it must stay that way.
//   * The answer is DATA FROM AN ARBITRARY WEB PAGE. It is painted as plain
//     text (never HTML), it is cleaned and length-capped here, and the card
//     opens the URL the SENDER typed, never a link the page or the server
//     supplied.
//   * The thumbnail is fetched with the user's credentials. It is therefore
//     only ever fetched from the SAME ORIGIN as the Nextcloud server and only
//     from the references/preview endpoint (thumbPathFor).
//
// Qt-Core only (no widgets, no network) so the decisions are testable without
// a server: test: link-preview-logic-test.

#include "models/CodePasteHeuristic.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace talq {
namespace linkpreview {

// A message shows at most this many cards. Each card is a request to the server,
// and a chat that pastes a list of links should not turn into a wall of boxes.
constexpr int kMaxCardsPerMessage = 2;

constexpr int kMaxUrlLength   = 2048;
constexpr int kMaxTitleChars  = 160;
constexpr int kMaxDescChars   = 400;

// What a card shows. `url` is ALWAYS the address the sender typed.
struct CardData {
    QString url;
    QString title;
    QString description;
    QString site;        // host of `url` without "www.", derived here, never taken from the page
    QString thumbUrl;    // as the server sent it; only usable through thumbPathFor()
    bool valid() const { return !url.isEmpty() && !title.isEmpty(); }
};

// A card the server has answered for. `hasThumb` is decided by whoever holds
// the server URL (thumbPathFor accepted the picture's address).
struct ReadyCard {
    CardData data;
    bool hasThumb = false;
};

namespace detail {

// Characters that may trail a URL in prose without being part of it.
inline bool isTrailingPunct(QChar c)
{
    return c == QLatin1Char('.') || c == QLatin1Char(',') || c == QLatin1Char(';')
        || c == QLatin1Char(':') || c == QLatin1Char('!') || c == QLatin1Char('?')
        || c == QLatin1Char('*') || c == QLatin1Char('_') || c == QLatin1Char('~')
        || c == QLatin1Char('\'') || c == QLatin1Char('"');
}

inline int countOf(const QString &s, QChar c)
{
    int n = 0;
    for (const QChar x : s) if (x == c) ++n;
    return n;
}

// Closing bracket belongs to the URL only when the URL opened one itself
// ("https://en.wikipedia.org/wiki/Foo_(bar)"); "(see https://x.com)" must not
// take the ")" with it.
inline bool isUnbalancedCloser(const QString &s, QChar c)
{
    QChar open;
    if (c == QLatin1Char(')'))      open = QLatin1Char('(');
    else if (c == QLatin1Char(']')) open = QLatin1Char('[');
    else if (c == QLatin1Char('}')) open = QLatin1Char('{');
    else return false;
    return countOf(s, c) > countOf(s, open);
}

inline QString trimTrailing(QString u)
{
    for (;;) {
        if (u.isEmpty()) return u;
        const QChar last = u.back();
        if (isTrailingPunct(last) || isUnbalancedCloser(u, last))
            u.chop(1);
        else
            return u;
    }
}

// Code is not prose: a URL inside a fence or `inline code` is an example, a log
// line or a command, and previewing it would be wrong and noisy.
inline QString withoutCode(QString text)
{
    static const QRegularExpression fenceRx(QStringLiteral(R"(```[\s\S]*?```)"));
    static const QRegularExpression inlineRx(QStringLiteral(R"(`[^`\n]+`)"));
    text.replace(fenceRx, QStringLiteral(" "));
    // A fence that was opened and never closed: everything after it is code.
    const qsizetype dangling = text.indexOf(QLatin1String("```"));
    if (dangling >= 0)
        text.truncate(dangling);
    text.replace(inlineRx, QStringLiteral(" "));
    return text;
}

inline bool looksLikeIpv4(const QString &host)
{
    static const QRegularExpression rx(QStringLiteral(R"(^\d{1,3}(\.\d{1,3}){3}$)"));
    return rx.match(host).hasMatch();
}

} // namespace detail

// Whether `url` is worth asking the server about. Cheap and conservative: a
// refusal here only means no card; the server applies its own (stricter) rules
// on top, including a block on local and private addresses.
inline bool isPreviewableUrl(const QString &url)
{
    if (url.isEmpty() || url.size() > kMaxUrlLength)
        return false;
    // Tolerant: a pasted address with an unencoded accent or a stray "|" is
    // still a page, and refusing it here would only mean a missing card.
    const QUrl u(url, QUrl::TolerantMode);
    if (!u.isValid())
        return false;
    const QString scheme = u.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return false;
    // Credentials in an address are a secret that has no business travelling to
    // the server (and it is the classic way to disguise where a link goes).
    if (!u.userInfo().isEmpty())
        return false;
    const QString host = u.host().toLower();
    if (host.isEmpty())
        return false;
    // An IP literal or a one-label name is an internal address far more often
    // than a page somebody wants unfurled.
    if (host.contains(QLatin1Char(':')) || detail::looksLikeIpv4(host))
        return false;
    if (!host.contains(QLatin1Char('.')))
        return false;
    static const char *const kInternalSuffix[] = {
        ".local", ".localhost", ".internal", ".lan", ".home", ".corp", ".intranet" };
    for (const char *suf : kInternalSuffix)
        if (host.endsWith(QLatin1String(suf)))
            return false;
    return true;
}

// What the chat draws as CODE rather than prose, blanked out of `rawText`:
// fences and `inline code` (detail::withoutCode) and -- exactly as Message.cpp
// decides for the body -- an unfenced indented paste, which the renderer boxes as
// a code block. Only for a message the server marked markdown: the code styling
// does not exist otherwise, and a backtick is then just a backtick.
//
// The paste heuristic reads HTML-ESCAPED text (that is what Message.cpp hands it),
// so the text is escaped for it and put back afterwards; the escape is exactly
// invertible (& < > and the double quote only).
inline QString proseOf(const QString &rawText, bool markdown)
{
    if (!markdown)
        return rawText;
    QString text = rawText;
    if (!text.contains(QLatin1String("```"))) {
        QString esc = text.toHtmlEscaped();
        esc = talq::codepaste::wrapRuns(esc, [](const QString &) { return QStringLiteral(" "); });
        esc.replace(QLatin1String("&lt;"), QString(QChar(0x3C)));
        esc.replace(QLatin1String("&gt;"), QString(QChar(0x3E)));
        esc.replace(QLatin1String("&quot;"), QString(QChar(0x22)));
        esc.replace(QLatin1String("&amp;"), QString(QChar(0x26)));
        text = esc;
    }
    return detail::withoutCode(text);
}

// The URLs of `rawText` (the message as typed / as the server stores it, NOT the
// rendered HTML) that should get a card, in order, de-duplicated, at most
// `maxUrls`. `markdown` is the message's own flag (Message::markdown).
inline QStringList extractPreviewUrls(const QString &rawText, int maxUrls = kMaxCardsPerMessage,
                                      bool markdown = true)
{
    QStringList out;
    if (rawText.isEmpty() || maxUrls <= 0 || !rawText.contains(QLatin1String("http"), Qt::CaseInsensitive))
        return out;

    const QString prose = proseOf(rawText, markdown);
    // \x22 \x27 \x60 are " ' ` -- spelled as escapes because a header that moc
    // reads (this one is pulled in by LinkPreviewService.h) must not contain a
    // quote character inside a raw string. The rest are the Unicode quotes,
    // brackets and full stops that end a sentence typed on a "smart" or CJK
    // keyboard and are never part of an address. \s is Unicode white space
    // (UseUnicodePropertiesOption), so a no-break or ideographic space ends the
    // address too.
    static const QRegularExpression urlRx(
        QStringLiteral(R"(https?://[^\s<>\x22\x27\x60\x{2018}\x{2019}\x{201C}\x{201D}\x{2026})"
                       R"(\x{3001}\x{3002}\x{300C}-\x{300F}\x{3010}\x{3011})"
                       R"(\x{FF01}\x{FF08}\x{FF09}\x{FF0C}\x{FF1A}\x{FF1B}\x{FF1F}]+)"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);

    auto it = urlRx.globalMatch(prose);
    while (it.hasNext() && out.size() < maxUrls) {
        QString cand = it.next().captured(0);
        // Bound the work BEFORE trimming: a message can be 32000 characters, and
        // a run of closing brackets is trimmed one character at a time.
        if (cand.size() > kMaxUrlLength + 64)
            continue;
        // "[text](https://a)" and "[https://a](https://a)": the link text and the
        // target run together with no space, and the seam is "](".
        const qsizetype seam = cand.indexOf(QLatin1String("]("));
        if (seam > 0)
            cand.truncate(seam);
        const QString url = detail::trimTrailing(cand);
        if (!isPreviewableUrl(url) || out.contains(url))
            continue;
        out.append(url);
    }
    return out;
}

// "https://www.Example.com/a?b" -> "example.com"
inline QString siteLabel(const QString &url)
{
    QString host = QUrl(url).host().toLower();
    if (host.startsWith(QLatin1String("www.")))
        host.remove(0, 4);
    return host;
}

// Text from a web page, made safe to paint on one or two lines: control
// characters and line breaks become spaces, runs of white space collapse, the
// bidirectional overrides that make text read as something else are dropped,
// and it is cut to `maxChars` with an ellipsis.
inline QString cleanText(const QString &in, int maxChars)
{
    QString s;
    s.reserve(qMin<qsizetype>(in.size(), maxChars * 2));
    bool lastSpace = true;                       // swallows leading white space
    for (const QChar c : in) {
        const ushort u = c.unicode();
        const bool bidi = (u >= 0x202A && u <= 0x202E) || (u >= 0x2066 && u <= 0x2069)
                          || u == 0x200E || u == 0x200F || u == 0x061C;
        if (bidi || u == 0xFEFF || u == 0x200B)
            continue;
        if (c.isSpace() || c.category() == QChar::Other_Control) {
            if (!lastSpace) s.append(QLatin1Char(' '));
            lastSpace = true;
            continue;
        }
        s.append(c);
        lastSpace = false;
    }
    while (s.endsWith(QLatin1Char(' ')))
        s.chop(1);
    if (s.size() > maxChars) {
        s.truncate(maxChars - 1);
        while (s.endsWith(QLatin1Char(' '))) s.chop(1);
        s.append(QChar(0x2026));
    }
    return s;
}

// The server's answer for `url` (the `ocs.data` object of references/resolve)
// as a card, or an invalid CardData when there is nothing worth showing.
//
// Every failure looks the same on purpose -- "no card" -- so a page that is
// private, empty, blocked or simply slow cannot be told apart from one with no
// metadata. A reference the server marks inaccessible is treated as nothing.
inline CardData parseResolveResponse(const QJsonObject &data, const QString &url)
{
    CardData card;
    const QJsonObject refs = data.value(QStringLiteral("references")).toObject();
    QJsonValue v = refs.value(url);
    if (!v.isObject() && refs.size() == 1)
        v = refs.begin().value();                // the server may normalise the key
    if (!v.isObject())
        return card;
    const QJsonObject ref = v.toObject();
    if (!ref.value(QStringLiteral("accessible")).toBool(true))
        return card;

    QJsonObject og = ref.value(QStringLiteral("openGraphObject")).toObject();
    if (og.isEmpty() && ref.value(QStringLiteral("richObjectType")).toString()
                            == QLatin1String("open-graph"))
        og = ref.value(QStringLiteral("richObject")).toObject();
    if (og.isEmpty())
        return card;

    card.title = cleanText(og.value(QStringLiteral("name")).toString(), kMaxTitleChars);
    if (card.title.isEmpty())
        return CardData();
    card.description = cleanText(og.value(QStringLiteral("description")).toString(), kMaxDescChars);
    card.thumbUrl    = og.value(QStringLiteral("thumb")).toString().trimmed();
    card.site        = siteLabel(url);
    card.url         = url;                      // what the sender typed, never og["link"]
    return card;
}

// The path (relative to the Nextcloud base URL) to fetch a card's thumbnail
// from, or an empty string when it must not be fetched. The fetch carries the
// user's credentials, so it is allowed ONLY for
//   * the same scheme, host and port as the server, and
//   * the references/preview endpoint under it.
// Anything else -- another host, another path, a ".." walk, userinfo -- is
// refused and the card simply has no picture.
inline QString thumbPathFor(const QString &thumb, const QString &serverUrl)
{
    if (thumb.isEmpty() || thumb.size() > kMaxUrlLength)
        return {};
    const QUrl base(serverUrl);
    if (!base.isValid() || base.host().isEmpty())
        return {};

    QUrl t(thumb, QUrl::StrictMode);
    if (!t.isValid())
        return {};
    if (!t.userInfo().isEmpty() || !t.fragment().isEmpty())
        return {};

    if (!t.isRelative()) {
        if (t.scheme().compare(base.scheme(), Qt::CaseInsensitive) != 0
            || t.host().compare(base.host(), Qt::CaseInsensitive) != 0
            || t.port(t.scheme() == QLatin1String("https") ? 443 : 80)
                   != base.port(base.scheme() == QLatin1String("https") ? 443 : 80))
            return {};
    } else if (!thumb.startsWith(QLatin1Char('/')) || thumb.startsWith(QLatin1String("//"))) {
        return {};
    }

    QString path = t.path(QUrl::FullyEncoded);
    if (path.contains(QLatin1String("..")) || path.contains(QLatin1Char('\\')))
        return {};
    // m_serverUrl already ends with the install's web root; the server writes
    // absolute URLs that include it, so drop it before joining again.
    QString root = base.path(QUrl::FullyEncoded);
    while (root.endsWith(QLatin1Char('/'))) root.chop(1);
    if (!root.isEmpty() && path.startsWith(root + QLatin1Char('/')))
        path.remove(0, root.size());

    // index.php/ is optional in front of core/...
    QString tail = path;
    if (tail.startsWith(QLatin1String("/index.php/")))
        tail.remove(0, 10);
    if (!tail.startsWith(QLatin1String("/core/references/preview/")))
        return {};
    const QString id = tail.mid(QString::fromLatin1("/core/references/preview/").size());
    if (id.isEmpty() || id.contains(QLatin1Char('/')))
        return {};

    QString out = path;
    if (t.hasQuery())
        out += QLatin1Char('?') + t.query(QUrl::FullyEncoded);
    return out;
}

} // namespace linkpreview
} // namespace talq
