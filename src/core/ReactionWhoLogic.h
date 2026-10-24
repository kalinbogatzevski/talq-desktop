#pragma once

// Who reacted -- the pure half (test: reaction-who-logic-test).
//
// A reaction pill shows an emoji and a count; the people behind the count come
// from one extra call, GET apps/spreed/api/v1/reaction/{token}/{messageId}. Its
// answer is {"<emoji>": [{actorType, actorId, actorDisplayName, timestamp}, ...]}
// (spreed ResponseDefinitions TalkReaction). `actorType` is "users" or "guests" in
// the documentation, but the reaction is stored as a Nextcloud comment, so any
// actor type of Talk can appear (emails, bots, bridged, federated_users) and a
// deleted account comes back as "deleted_users" with an empty name. Any other string
// is carried, not rejected.
//
// Everything that decides what the tooltip may SAY lives here, because a display
// name is text from another person's account:
//   * it is cleaned before it is stored (control characters, line breaks and
//     bidirectional overrides, length) with the same helper link previews use;
//   * a tooltip is RICH TEXT to Qt (anything that looks like markup is parsed as
//     HTML), so every piece of server text is HTML-escaped on the way in and a
//     name such as "<img src=x>" is shown as those characters, never run;
//   * the token that goes into the request path is checked against Talk's own
//     route pattern, so a malformed token cannot walk the path.
//
// The network half is ReactionWhoService; ChatPainter owns the hover and the
// tooltip. Nothing here touches a widget, a font or a socket.

#include "core/LinkPreviewLogic.h"
#include "core/TalkToken.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace talq::reactionwho {

// Names listed in the tooltip; the rest are counted ("and 3 more").
constexpr int kMaxNamesShown = 10;
// Parse bound per emoji: the answer is read on the GUI thread.
constexpr int kMaxActorsPerEmoji = 500;
// A name is cut to this many characters (an ellipsis marks the cut).
constexpr int kMaxNameChars = 40;
// How long a fetched list is trusted before a hover asks again, even when the pill
// looks the same: two people swapping a reaction leaves every count unchanged.
constexpr qint64 kFreshMs = 20 * 1000;
// After a failed fetch, how long hovering does not ask again.
constexpr qint64 kRetryAfterFailureMs = 10 * 1000;

struct Actor {
    QString type;               // "users" | "guests" | other (federated ...)
    QString id;
    QString name;               // cleaned; empty when the server sent none
    qint64  timestamp = 0;      // seconds, UTC
};

using ByEmoji = QHash<QString, QVector<Actor>>;

// The words the tooltip uses. The caller supplies them (tr() needs a QObject),
// so this header stays free of translation and the tests can use plain English.
struct Labels {
    QString you;                // "You"
    QString guest;              // "Guest"
    QString deletedUser;        // "Deleted user"
    QString andMore;            // "and %1 more"
    QString loading;            // "Loading…"
    QString unavailable;        // "Couldn't load who reacted" (the request failed)
    QString noNames;            // "No names to show" (the request worked and the list is empty)
};

// The request path for a message's reactors, or an empty string when the token or
// the id could not be a real one. Talk's route is token [a-z0-9]{4,30}; the check
// is anchored with \A..\z because `$` would also accept a trailing newline.
inline QString reactorsPath(const QString &token, int messageId)
{
    if (messageId <= 0 || !isTalkToken(token))
        return {};
    return QStringLiteral("apps/spreed/api/v1/reaction/") + token + QLatin1Char('/')
         + QString::number(messageId);
}

// `ocs.data` of the GET as emoji -> reactors, oldest first. Defensive on every
// level: a value that is not an array, an entry that is not an object, or an
// entry without an actor id is skipped, never an error. An answer that is not an
// object at all (ApiClient hands an empty one) parses to nothing.
inline ByEmoji parseReactors(const QJsonObject &data)
{
    ByEmoji out;
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        if (!it.value().isArray())
            continue;
        QVector<Actor> list;
        for (const QJsonValue &v : it.value().toArray()) {
            if (list.size() >= kMaxActorsPerEmoji)
                break;
            if (!v.isObject())
                continue;
            const QJsonObject o = v.toObject();
            Actor a;
            a.type = o.value(QStringLiteral("actorType")).toString();
            a.id   = o.value(QStringLiteral("actorId")).toString();
            if (a.id.isEmpty())
                continue;
            a.name = linkpreview::cleanText(o.value(QStringLiteral("actorDisplayName")).toString(),
                                            kMaxNameChars);
            a.timestamp = qint64(o.value(QStringLiteral("timestamp")).toDouble());
            list.append(a);
        }
        // Stable, so two reactions in the same second keep the server's order.
        std::stable_sort(list.begin(), list.end(),
                         [](const Actor &x, const Actor &y) { return x.timestamp < y.timestamp; });
        out.insert(it.key(), list);
    }
    return out;
}

// True when `a` is the signed-in user. Nextcloud user ids are not case-sensitive at
// login, so the comparison is not either; a guest or federated actor is never "you".
inline bool isMe(const Actor &a, const QString &myUserId)
{
    return !myUserId.isEmpty() && a.type == QLatin1String("users")
        && a.id.compare(myUserId, Qt::CaseInsensitive) == 0;
}

// Does a name show anything to a reader? cleanText drops line breaks, control and
// bidi characters, but a name made only of invisible FORMAT characters (U+2060 word
// joiner, ...) or of characters that render as blanks (U+3164 Hangul filler, U+2800
// braille blank, ...) survives it and would show as an empty line. Used only to decide
// the fallback; the stored name is left alone, so a zero-width joiner inside a real
// name (an emoji sequence) is untouched.
inline bool hasVisibleChar(const QString &s)
{
    for (const QChar c : s) {
        const ushort u = c.unicode();
        if (c.isSpace() || c.category() == QChar::Other_Format
            || c.category() == QChar::Other_Control)
            continue;
        if (u == 0x3164 || u == 0x115F || u == 0x1160 || u == 0xFFA0 || u == 0x2800 || u == 0x034F)
            continue;
        return true;
    }
    return false;
}

// What to call one reactor: the cleaned display name, else a label for the actor
// types that can come back nameless (a deleted account, a guest), else the cleaned
// account id.
inline QString displayNameOf(const Actor &a, const Labels &l)
{
    if (hasVisibleChar(a.name))
        return a.name;
    if (a.type == QLatin1String("deleted_users"))
        return l.deletedUser;          // its id is the literal "deleted_users" for every one of them
    if (a.type == QLatin1String("guests"))
        return l.guest;
    const QString id = linkpreview::cleanText(a.id, kMaxNameChars);
    return hasVisibleChar(id) ? id : l.guest;
}

// The lines of the list, in order: "You" first, then everyone else oldest-first,
// cut to kMaxNamesShown. `more` receives how many were cut. `forceMe` lists "You"
// even when the signed-in user is not among `actors` -- for a list that was cut at
// kMaxActorsPerEmoji, where the viewer's own reaction may be in the part not kept.
inline QStringList nameLines(const QVector<Actor> &actors, const QString &myUserId,
                             const Labels &l, int *more = nullptr, bool forceMe = false)
{
    QStringList lines;
    bool me = forceMe;
    for (const Actor &a : actors)
        if (isMe(a, myUserId)) { me = true; break; }
    if (me)
        lines << l.you;
    for (const Actor &a : actors)
        if (!isMe(a, myUserId))
            lines << displayNameOf(a, l);
    int cut = 0;
    if (lines.size() > kMaxNamesShown) {
        cut = int(lines.size()) - kMaxNamesShown;
        lines = lines.mid(0, kMaxNamesShown);
    }
    if (more)
        *more = cut;
    return lines;
}

// ── Tooltip text ──
// Rich text on purpose (a header line over a list), and therefore escaped
// everywhere a server string goes in. `white-space:nowrap` keeps a name on one
// line; the 40-character cap keeps the tip a sensible width.

inline QString headerHtml(const QString &emoji, const QString &count)
{
    return QStringLiteral("<b>%1</b>&nbsp;&nbsp;%2")
        .arg(emoji.toHtmlEscaped(), count.toHtmlEscaped());
}

inline QString wrapHtml(const QString &inner)
{
    return QStringLiteral("<div style='white-space:nowrap'>%1</div>").arg(inner);
}

// A header over a one-line note: "Loading…", or why there are no names.
inline QString noteTipHtml(const QString &emoji, const QString &count, const QString &note)
{
    return wrapHtml(headerHtml(emoji, count) + QStringLiteral("<br>") + note.toHtmlEscaped());
}

// The finished tooltip.
//
// The header counts the PEOPLE LISTED, not the pill's number: the pill can lag the
// server by up to ~30 s (another person's reaction reaches the row on the long-poll,
// the list is read straight from the server), and a header that says 2 over three
// names reads as a bug. The one exception is a list cut at kMaxActorsPerEmoji, which
// is incomplete by construction: there the pill's count is the only complete number,
// "You" comes from the viewer's own reaction (`iReacted`, from reactionsSelf) instead
// of from the part of the list that was kept, and "and N more" is whatever the pill
// says is not shown.
//
// An empty list is "No names to show", not "Couldn't load": the request worked, and
// the pill is simply behind (the only reactor took theirs back a moment ago).
inline QString namesTipHtml(const QString &emoji, const QString &pillCount,
                            const QVector<Actor> &actors, const QString &myUserId,
                            const Labels &l, bool iReacted = false)
{
    if (actors.isEmpty())
        return noteTipHtml(emoji, pillCount, l.noNames);
    const bool cut = actors.size() >= kMaxActorsPerEmoji;
    int more = 0;
    const QStringList lines = nameLines(actors, myUserId, l, &more, cut && iReacted);
    int total = int(lines.size()) + more;               // everyone the list names
    if (cut)
        total = qMax(total, pillCount.toInt());         // ... and the pill knows of more
    more = total - int(lines.size());
    QString html = headerHtml(emoji, QString::number(total));
    for (const QString &line : lines)
        html += QStringLiteral("<br>") + line.toHtmlEscaped();
    if (more > 0)
        html += QStringLiteral("<br><i>") + l.andMore.arg(more).toHtmlEscaped()
              + QStringLiteral("</i>");
    return wrapHtml(html);
}

// ── Freshness ──

// What a pill LOOKS like, as one string: the "emoji count" text plus the user's own
// reactions. A fetched list is only valid for the signature it was fetched under.
inline QString signatureOf(const QString &reactions, const QStringList &self)
{
    return reactions + QChar(0x1F) + self.join(QChar(0x1E));
}

// Whether a list fetched at `fetchedAtMs` under `fetchedSig` may still be used
// without asking again. A clock that went backwards makes it not fresh.
inline bool isFresh(const QString &fetchedSig, const QString &currentSig,
                    qint64 fetchedAtMs, qint64 nowMs)
{
    const qint64 age = nowMs - fetchedAtMs;
    return fetchedSig == currentSig && age >= 0 && age < kFreshMs;
}

// Whether hovering may ask again after a failure at `failedAtMs`.
inline bool mayRetryAfterFailure(qint64 failedAtMs, qint64 nowMs)
{
    const qint64 age = nowMs - failedAtMs;
    return age < 0 || age >= kRetryAfterFailureMs;
}

} // namespace talq::reactionwho
