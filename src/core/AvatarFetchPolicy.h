#pragma once

// When an avatar is downloaded again, and what a download may do to the picture
// already on screen (test: avatar-fetch-policy-test).
//
// WHY THIS EXISTS. The sidebar, the chat header and the chat bubbles each keep a
// cache of avatars and each marked a picture "download in flight" until that download
// FINISHED, refusing to ask again meanwhile. The downloads had no timeout (every other
// request has 30 s), so one that never finished -- seen on a real laptop after the
// server closed the HTTP/2 connection (GOAWAY) at the moment the window was activated
// -- froze that picture for the life of the process: eleven pictures, including a group
// whose picture had just been changed, were last refreshed together and never again
// over three days, while every other row refreshed on every focus. The header, which
// has its own cache and was not stuck, showed the new picture beside a stale chat list.
//
// So: a download gets a transfer timeout; an "in flight" marker is only believed for a
// minute; and a FAILED refresh keeps the picture that is already there instead of
// replacing it with nothing (a timeout is a normal outcome now, and an offline blip
// must not blank every avatar).
//
// Pure decisions, no Qt beyond integers; the painters own the HTTP.

#include <QtGlobal>

namespace talq::avatarfetch {

// A cached picture is asked for again once it is this old (a changed picture then
// shows up without a restart); a window activation also marks every one stale.
constexpr qint64 kTtlMs = 15 * 60 * 1000;
// A missing picture (its last download failed) is asked for again after this.
constexpr qint64 kErrorTtlMs = 60 * 1000;
// Qt's inactivity timeout for one download: a stalled transfer errors out, a
// progressing one never trips it.
constexpr int kTransferTimeoutMs = 20 * 1000;
// An "in flight" marker older than this is treated as a download that is never
// coming back, and a new one may start. Longer than the transfer timeout, so a slow
// but live download is never doubled.
constexpr qint64 kPendingGiveUpMs = 60 * 1000;

// Is a cached picture due for a refresh? `fetchedMs` is when it was last stamped
// (0 = never, or marked stale by a window activation). `isNull` is true for a
// picture that failed to download. A clock that went backwards (negative age) makes
// it due rather than frozen until the clock catches up.
inline bool due(bool isNull, qint64 fetchedMs, qint64 nowMs)
{
    const qint64 age = nowMs - fetchedMs;
    return age < 0 || age >= (isNull ? kErrorTtlMs : kTtlMs);
}

// May a download for this picture start? `inFlight` says a marker exists and
// `startedMs` is when that download began.
inline bool mayStart(bool inFlight, qint64 startedMs, qint64 nowMs)
{
    if (!inFlight)
        return true;
    const qint64 age = nowMs - startedMs;
    return age < 0 || age >= kPendingGiveUpMs;
}

// The fetched-at stamp to store after a FAILED download. A picture we already hold
// is kept (`hadImage`), stamped so that it is due again after kErrorTtlMs rather than
// after the full kTtlMs: a failure should be retried soon, but not shown as a blank.
// With nothing to keep, the plain "now" stamp does the same through the null entry's
// own kErrorTtlMs.
inline qint64 failedStampMs(bool hadImage, qint64 nowMs)
{
    return hadImage ? nowMs - (kTtlMs - kErrorTtlMs) : nowMs;
}

} // namespace talq::avatarfetch
