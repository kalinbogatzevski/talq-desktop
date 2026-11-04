#pragma once

// NotificationCoalesceLogic -- the one rule behind NotificationStack's "N new message(s)" merge: a
// popup folds into one already on screen only when both belong to the same chat and the one on
// screen was last shown no more than the coalescing window ago. Qt Core only, no widgets. See
// tests/notification_coalesce_test.cpp.
//
// A popup with an EMPTY token belongs to no chat (a saved download, a call notice, an
// update-ready notice, every notifySilently notice), so there is nothing for it to merge with.
// Before this header an empty token compared equal to any other empty token, and two such notices
// within the window were rewritten into "2 new messages".
//
// The window is inclusive: an elapsed time of exactly windowMs still coalesces (the behaviour the
// stack always had).

#include <QString>
#include <QtGlobal>

namespace talq {

inline bool canCoalesce(const QString &existingToken, const QString &newToken,
                        qint64 elapsedMs, int windowMs)
{
    if (newToken.isEmpty())
        return false;
    if (existingToken != newToken)
        return false;
    return elapsedMs <= windowMs;
}

} // namespace talq
