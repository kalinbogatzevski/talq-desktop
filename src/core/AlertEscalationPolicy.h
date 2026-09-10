#pragma once

// How loudly a connection fault should be reported, and when.
//
// This file exists to hold a line that was already drawn once and must not be
// crossed again. In 0.51.6 TalQ shipped a red danger banner plus an OS/tray
// notification on every server-reachability transition. It was withdrawn one
// day later, in 0.51.7, because it read as "the app is broken" for what was
// usually a Wi-Fi blip or a laptop waking up. The replacement -- a quiet 26px
// "Connecting…" strip with animated dots, and NO notification at all -- is
// still the right answer for that case and is not revisited here.
//
// What 0.51.7 did not cover is the other kind of fault: the one that is still
// there tomorrow. A desk sat for days with every live connection refused while
// chat worked perfectly, and nothing ever said so, because the only louder
// surface had been removed and the quieter one lives in a column the user does
// not always look at.
//
// So the rule is not "quiet" or "loud", it is WHICH FAULT:
//
//   * A transient reachability dip never reaches this file at all --
//     decideHealthFault() returns None while the network is down or has only
//     just returned, so there is no fault to escalate and the calm strip keeps
//     the story. That is structural, not a promise.
//   * A fault that OUTLIVES the threshold, or that the server has already given
//     a terminal verdict on, escalates: strip, then a notification that shows
//     even when TalQ is minimised, then a tray icon that stays coloured until
//     it is actually fixed.
//
// And one inversion that matters more than any of the timings: dismissing the
// banner hides THE BANNER. It does not cancel the notification and it does not
// clear the tray. A warning you can make disappear by clicking X is exactly the
// warning that went unread for a week.

#include <cstdint>

namespace talq {

using AlertMs = std::int64_t;

struct AlertEscalationInputs {
    // Whether decideHealthFault() found anything. A transient dip is already
    // None by the time it gets here.
    bool faultPresent = false;

    // The server has answered and the answer will not change on its own (no
    // extension linked, authorisation revoked, proxy demanding credentials we
    // cannot produce). There is nothing to wait for, so no threshold applies.
    bool faultIsTerminal = false;

    // How long THIS fault has been continuously present. Reset when the fault
    // changes or clears -- never a running total across different problems.
    AlertMs faultHeldMs = 0;

    // Whether a notification has already gone out for this same fault. Keyed by
    // fault, so a new and different problem still gets its own, but a reconnect
    // loop cannot toast every twenty seconds.
    bool alreadyNotified = false;

    // The user clicked X on the strip. Affects the strip and nothing else.
    bool bannerDismissed = false;

    // Long enough that nothing routine reaches it. The notification threshold
    // matches the health policy's own warn threshold; the tray waits longer
    // still, because a coloured tray icon is a standing accusation and should
    // only appear once the problem has clearly failed to fix itself.
    AlertMs notifyAfterMs = 120000;
    AlertMs trayAfterMs   = 300000;
};

struct AlertPlan {
    bool showBanner = false;   // the in-window strip
    bool sendNotification = false;   // one OS notification, once per fault
    bool trayAlarm = false;    // tray icon stays coloured until the fault clears
};

inline AlertPlan decideAlert(const AlertEscalationInputs &in)
{
    AlertPlan plan;
    if (!in.faultPresent)
        return plan;   // nothing anywhere: the calm strip owns this moment

    // The banner is the only surface the close button reaches.
    plan.showBanner = !in.bannerDismissed;

    // A terminal verdict skips both clocks. Holding back a notification for two
    // minutes when the server has ALREADY said "this account has no extension"
    // only delays the fix; there is nothing in flight that might still succeed.
    const bool ripe = in.faultIsTerminal || in.faultHeldMs > in.notifyAfterMs;

    plan.sendNotification = ripe && !in.alreadyNotified;
    plan.trayAlarm = in.faultIsTerminal || in.faultHeldMs > in.trayAfterMs;
    return plan;
}

} // namespace talq
