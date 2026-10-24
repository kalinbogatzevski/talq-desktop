#pragma once

// A Talk conversation token, the way the server's own routes accept it.
//
// One definition, because a token ends up INSIDE a request path (reaction lists, room
// pictures) and every place that builds such a path must refuse the same things. The
// pattern is Talk's route requirement: [a-z0-9]{4,30}. It is anchored with \A..\z, never
// ^..$: `$` also matches before a trailing newline, so "abcd\n" would pass.

#include <QRegularExpression>
#include <QString>

namespace talq {

inline bool isTalkToken(const QString &token)
{
    static const QRegularExpression re(QStringLiteral("\\A[a-z0-9]{4,30}\\z"));
    return re.match(token).hasMatch();
}

} // namespace talq
