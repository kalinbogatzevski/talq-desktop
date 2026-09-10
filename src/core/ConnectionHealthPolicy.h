#pragma once

// Which connection fault, if any, is worth telling the user about right now.
//
// Extracted from the widget for the usual reason: this is a decision, and a
// decision that lives inside a paint path cannot be tested and tends to grow
// special cases nobody can see. It is deliberately free of Qt widgets, of
// CtiClient, and of any knowledge of how the answer is displayed -- callers map
// their live state onto Inputs and render the verdict however they like.
//
// The rules encode two things learned the hard way:
//
//   1. A TERMINAL refusal needs no waiting period. The server has already given
//      a verdict that will never change on its own, so holding it back for two
//      minutes only delays the fix.
//   2. Everything else must OUTLIVE a threshold. TalQ already has a calm,
//      Telegram-style "Connecting…" strip for ordinary transience; a second,
//      louder warning that fired on every Wi-Fi roam would be noise, and noise
//      is how the last warning nobody read got ignored.
//
// The ordering between faults matters too: a definitive refusal outranks an
// unreachable service, because "your account has no extension" is actionable
// and "we cannot reach the server" is not, for the same user.

// Deliberately no Qt: this is a decision over plain data, so its test builds
// and runs in well under a second like the other policy suites.
#include <cstdint>

namespace talq {

using HealthMs = std::int64_t;

enum class HealthFault {
    None,
    CtiNoExtension,       // terminal: paired, but the account has no extension
    CtiUnauthorised,      // terminal: token bad, revoked or expired
    SignalingDown,        // calls will not connect
    CtiNeverConnected,    // never reached the daemon on this machine
    CtiLost,              // worked before, not now
};

struct ConnectionHealthInputs {
    // CTI is only judged when the site actually uses it. An install with the
    // feature switched off, or never configured, must never be warned about it
    // -- a permanent scold about a feature you do not use is worse than
    // silence, because it teaches people to ignore the strip.
    bool ctiInUse = false;
    bool ctiConnected = false;
    bool ctiEverConnected = false;
    bool ctiRefusedNoExtension = false;
    bool ctiRefusedUnauthorised = false;
    HealthMs ctiUnhealthyMs = 0;

    bool signalingUp = true;
    HealthMs signalingDownMs = 0;

    // Whether the ORDINARY web connection to the server is working, and for how
    // long it has been working without interruption.
    //
    // Both are needed, and for different bugs:
    //
    //  - `serverReachable` false means the machine has no usable network at all
    //    (Wi-Fi off, lid closed, on a train). Signaling and the screen-pop are
    //    of course down too, but blaming a firewall for the user's own Wi-Fi is
    //    worse than saying nothing -- and TalQ already shows its calm
    //    "Connecting…" strip for exactly this. Two contradictory banners, one of
    //    them accusing IT, is the failure mode this guards.
    //
    //  - `serverReachableForMs` guards the moment the network COMES BACK. The
    //    fault clocks have been running the whole time the lid was shut, so
    //    without this the warning fires the instant Wi-Fi returns -- before the
    //    client has had even one reconnect attempt, whose backoff can be a
    //    minute. The threshold has to measure time since the network returned,
    //    not time since the fault began.
    // Defaults fail SAFE, not convenient: a caller who forgets to fill these in
    // gets silence rather than a false accusation aimed at somebody's IT
    // department. Callers that mean "long-established connection" must say so.
    bool serverReachable = true;
    HealthMs serverReachableForMs = 0;

    // Long enough that a blip, a laptop waking from sleep, or a Wi-Fi roam
    // never trips it; short enough that somebody still finds out the same
    // morning. Overridable so the tests do not have to sleep.
    HealthMs warnAfterMs = 120000;
};

inline HealthFault decideHealthFault(const ConnectionHealthInputs &in)
{
    // Terminal refusals are reported even with no network, and that is not an
    // oversight: they can only have been learned over a socket that WAS working,
    // so they say something true and durable about the account, not about the
    // link. Everything below them is a statement about reachability, and a
    // statement about reachability is worthless when the machine has no network.
    if (in.ctiInUse && in.ctiRefusedNoExtension)
        return HealthFault::CtiNoExtension;
    if (in.ctiInUse && in.ctiRefusedUnauthorised)
        return HealthFault::CtiUnauthorised;

    // No network, or the network only just returned: say nothing. The calm
    // "Connecting…" strip already owns this story, and accusing a firewall
    // while the user's own Wi-Fi is off -- or one second after it comes back,
    // before a single reconnect has been attempted -- destroys the credibility
    // of every warning this file exists to raise.
    if (!in.serverReachable || in.serverReachableForMs <= in.warnAfterMs)
        return HealthFault::None;

    if (!in.signalingUp && in.signalingDownMs > in.warnAfterMs)
        return HealthFault::SignalingDown;

    if (in.ctiInUse && !in.ctiConnected && in.ctiUnhealthyMs > in.warnAfterMs)
        return in.ctiEverConnected ? HealthFault::CtiLost
                                   : HealthFault::CtiNeverConnected;

    return HealthFault::None;
}

// A stable id for the fault, used to remember which warning the user closed.
// Keyed by FAULT, never by "the strip" as a whole: dismissing "signaling is
// down" must not also silence a later, different problem, or the close button
// quietly recreates the blindness this whole strip exists to end.
inline const char *healthFaultKey(HealthFault f)
{
    switch (f) {
    case HealthFault::None:              return "";
    case HealthFault::CtiNoExtension:    return "cti-no-extension";
    case HealthFault::CtiUnauthorised:   return "cti-unauthorised";
    case HealthFault::SignalingDown:     return "signaling-down";
    case HealthFault::CtiNeverConnected: return "cti-never";
    case HealthFault::CtiLost:           return "cti-lost";
    }
    return "";
}

// Whether the warning carries a "take me to the fix" button. Only where the
// user can actually do something in TalQ: a signaling outage and a missing ERP
// attribute are somebody else's job, and offering a button that leads nowhere
// useful is how a warning loses its credibility.
inline bool healthFaultHasAction(HealthFault f)
{
    return f == HealthFault::CtiUnauthorised
        || f == HealthFault::CtiNeverConnected
        || f == HealthFault::CtiLost;
}

} // namespace talq
