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
    ProxyAuthRequired,    // terminal: the proxy demands credentials we cannot give
    WebSocketsBlocked,    // ordinary web works, every live connection is refused
    SignalingDown,        // calls will not connect
    CtiNeverConnected,    // never reached the daemon on this machine
    CtiLost,              // worked before, not now
};

struct ConnectionHealthInputs {
    // Whether the client is logged in and its background services have been
    // started. Defaults to FALSE, and that default is the point: the health
    // poll runs from the moment the window is constructed, long before anyone
    // has logged in, and at that moment every socket is legitimately down
    // while ordinary HTTP looks "reachable" (ApiClient assumes reachable until
    // something proves otherwise). Without this gate a fresh install sitting on
    // the login screen manufactures a fault out of nothing and, two minutes
    // later, tells the user to contact their IT department.
    bool servicesExpected = false;

    // Whether the SERVER told us these services exist. This is the difference
    // between "we cannot reach the call server" and "this deployment has no
    // call server", and it is not the same question as "has it ever connected":
    //   - a stock Nextcloud with no high-performance backend and no notify_push
    //     is a perfectly healthy, ordinary configuration, and must never be
    //     warned about;
    //   - a machine whose proxy blocks WebSockets learns the URLs fine over
    //     plain HTTP and then fails to connect -- configured, never connected,
    //     and exactly the case that must warn.
    // Keying on "ever connected" would get the second case backwards.
    bool signalingConfigured = false;
    bool pushConfigured = false;

    // Whether the server is not merely answering but actually well. A box in
    // maintenance mode answers every request with a 5xx -- reachable, and
    // refusing every WebSocket. From the client that is indistinguishable from
    // a proxy blocking live connections, so without this the whole office is
    // told to contact IT about an outage on our own side.
    bool serverHttpHealthy = true;

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

    // Push (notify_push) is tracked for ONE reason: it is the second witness.
    //
    // Signalling alone going quiet is ambiguous -- the call server could simply
    // be down. But signalling AND push are different services, on different
    // hosts, reached over different URLs. They share exactly one thing: both
    // are WebSockets. So when ordinary HTTP to the very same server is healthy
    // and BOTH of them are dead, the common factor is not any of those servers,
    // it is that this machine cannot open a live connection at all.
    //
    // That is a real diagnosis, and it is the one that took a week to reach by
    // hand: a desk logged in fine, listed rooms fine, fetched a 409 KB avatar
    // fine, and had every WebSocket refused. Encoding it here means the next
    // desk says so itself.
    bool pushUp = true;
    HealthMs pushDownMs = 0;

    // A proxy stands between this machine and the server, and it demanded
    // credentials we could not satisfy. Terminal like the other refusals: a 407
    // does not resolve itself, and for CALL MEDIA it cannot be worked around at
    // all -- libnice sends only preemptive Basic auth and treats any non-2xx
    // reply to its CONNECT as fatal, so an NTLM or Kerberos proxy ends the
    // media path outright rather than degrading it.
    bool proxyAuthFailed = false;

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
    // Nothing is expected to be up yet, so nothing can be wrong. Before login
    // every socket is down by design; reporting that as a fault is how a
    // warning system loses the user's trust on first run.
    if (!in.servicesExpected)
        return HealthFault::None;

    // Terminal refusals are reported even with no network, and that is not an
    // oversight: they can only have been learned over a socket that WAS working,
    // so they say something true and durable about the account, not about the
    // link. Everything below them is a statement about reachability, and a
    // statement about reachability is worthless when the machine has no network.
    if (in.ctiInUse && in.ctiRefusedNoExtension)
        return HealthFault::CtiNoExtension;
    if (in.ctiInUse && in.ctiRefusedUnauthorised)
        return HealthFault::CtiUnauthorised;

    // Also terminal, and NOT gated on reachability below: a proxy that
    // challenged us proves both that there is a network and that something on
    // it is refusing us. Waiting for the reachability threshold would only
    // delay a fault that will never clear by itself.
    if (in.proxyAuthFailed)
        return HealthFault::ProxyAuthRequired;

    // No network, or the network only just returned: say nothing. The calm
    // "Connecting…" strip already owns this story, and accusing a firewall
    // while the user's own Wi-Fi is off -- or one second after it comes back,
    // before a single reconnect has been attempted -- destroys the credibility
    // of every warning this file exists to raise.
    if (!in.serverReachable || in.serverReachableForMs <= in.warnAfterMs)
        return HealthFault::None;

    // BEFORE the per-service faults, because it explains them. If HTTP to the
    // server is working and every WebSocket is refused, reporting "the call
    // server is unreachable" is true but useless -- it sends the user to the
    // wrong place. The specific finding outranks the general symptom.
    // BOTH must be services this deployment actually has, or the "two
    // independent witnesses" argument collapses: on a server with no
    // notify_push, push is permanently "down" and would silently supply the
    // second witness for free, turning every ordinary call-server outage into
    // a firewall accusation.
    if (in.signalingConfigured && in.pushConfigured
        && in.serverHttpHealthy
        && !in.signalingUp && !in.pushUp
        && in.signalingDownMs > in.warnAfterMs
        && in.pushDownMs > in.warnAfterMs)
        return HealthFault::WebSocketsBlocked;

    // Only if the server said there IS one. A deployment without a
    // high-performance backend is not broken, it is just smaller.
    if (in.signalingConfigured && !in.signalingUp
        && in.signalingDownMs > in.warnAfterMs)
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
    case HealthFault::ProxyAuthRequired: return "proxy-auth";
    case HealthFault::WebSocketsBlocked: return "websockets-blocked";
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
