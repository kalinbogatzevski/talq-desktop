#pragma once

// Where to fetch a person's or a group's picture at full size for the picture viewer,
// and what to call it when it is saved (test: avatar-paths-test).
//
// Both ids come from the server and end up in a request path that carries the user's own
// credentials, so a path is only built from an id that could really name the thing:
// a user id is percent-encoded and refused when it could walk the path (a slash, a
// backslash, a control character, or only dots); a bot's actor id carries a "bots/" prefix,
// contains a slash, and has no picture file, so it is refused too. A room token must match
// Talk's own route pattern (TalkToken.h).
//
// Sizes: a room's picture is served at 512 px (spreed AvatarService::getAvatar,
// getFile(512)); Nextcloud serves a user's at whatever size the path names.

#include "core/TalkToken.h"

#include <QSize>
#include <QString>
#include <QUrl>

namespace talq::avatarpaths {

// Asked for when a person's picture is shown large. The viewer fits it to its window.
constexpr int kUserZoomPx = 1024;
// An id longer than this is not a Nextcloud user id.
constexpr int kMaxIdChars = 256;

// "/index.php/avatar/<id>/<px>", or an empty string when `userId` cannot name a user.
inline QString userPath(const QString &userId, int px = kUserZoomPx)
{
    if (userId.isEmpty() || userId.size() > kMaxIdChars || px <= 0)
        return {};
    bool onlyDots = true;
    for (const QChar c : userId) {
        if (c.unicode() < 0x20 || c == QLatin1Char('/') || c == QLatin1Char('\\'))
            return {};
        if (c != QLatin1Char('.'))
            onlyDots = false;
    }
    if (onlyDots)
        return {};                     // "." and ".." are path segments, not names
    return QStringLiteral("/index.php/avatar/")
         + QString::fromLatin1(QUrl::toPercentEncoding(userId))
         + QLatin1Char('/') + QString::number(px);
}

// The room picture's path, or an empty string when `token` is not a Talk token.
// `nowSecs` is a cache-buster: a fixed address would let any cache on the way keep
// serving the OLD picture after it was changed (the painters do the same).
inline QString roomPath(const QString &token, qint64 nowSecs)
{
    if (!isTalkToken(token))
        return {};
    return QStringLiteral("/ocs/v2.php/apps/spreed/api/v1/room/") + token
         + QStringLiteral("/avatar?v=") + QString::number(nowSecs);
}

// The size to rasterise a VECTOR picture at. A group without a custom picture is served as
// an SVG whose own size is 22x22 (viewBox 44, height="22"); decoded as it stands it would
// be a 22-pixel image blown up in the viewer. The long edge is made `longEdge` and the
// shape is kept; an intrinsic size that is missing or silly gives a square.
inline QSize rasterSizeForVector(const QSize &intrinsic, int longEdge = 512)
{
    if (intrinsic.width() <= 0 || intrinsic.height() <= 0 || longEdge <= 0)
        return QSize(longEdge, longEdge);
    const double k = double(longEdge) / double(qMax(intrinsic.width(), intrinsic.height()));
    return QSize(qMax(1, qRound(intrinsic.width() * k)), qMax(1, qRound(intrinsic.height() * k)));
}

// A file name for "Save as" from a person's or room's name: what Windows refuses in a
// name becomes '_', it is trimmed and capped, and it ends in ".png". Never empty.
inline QString fileNameFor(const QString &title)
{
    QString s;
    s.reserve(title.size());
    for (const QChar c : title) {
        const bool bad = c.unicode() < 0x20 || QStringLiteral("\\/:*?\"<>|").contains(c);
        s.append(bad ? QLatin1Char('_') : c);
    }
    s = s.trimmed();
    while (s.endsWith(QLatin1Char('.')))
        s.chop(1);                      // Windows drops a trailing dot, which would rename the file
    if (s.size() > 80)
        s.truncate(80);
    s = s.trimmed();
    if (s.isEmpty())
        s = QStringLiteral("picture");
    return s + QStringLiteral(".png");
}

} // namespace talq::avatarpaths
