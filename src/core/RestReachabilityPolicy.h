#pragma once

// RestReachabilityPolicy -- pure decision behind ApiClient's "is the Nextcloud
// server reachable" flag, applied once per finished reply. No Qt: ApiClient maps
// the QNetworkReply onto ReplyKind and applies the result. See
// tests/rest_reachability_policy_test.cpp.
//
// Field incident 2026-09-16 11:37:40: two call-flags PUTs held server-side for
// 90 s by Talk's synchronous HPB notifier hit the 30 s transfer timeout back to
// back. Qt reports that as QNetworkReply::TimeoutError with no HTTP status
// (qnetworkreplyhttpimpl.cpp _q_transferTimedOut -> abortImpl(TimeoutError),
// "Operation timed out"), which counted as a miss exactly like "connection
// refused", so the second one flipped the app OFFLINE while the server was
// answering everything else. A request that hangs says the server is slow FOR
// THAT REQUEST; it does not say the server is unreachable. So a transfer timeout
// on an ordinary request only asks the /status.php probe to confirm; the probe's
// own outcome (and hard transport errors) still count.

namespace talq {

enum class ReplyKind {
    Cancelled,         // deliberately aborted (logout, context death) -- not an outage
    HttpAnswered,      // any HTTP status, even 4xx/5xx: the box is reachable
    TransferTimeout,   // no bytes for the transfer timeout (QNetworkReply::TimeoutError)
    TransportFailure,  // refused / DNS / no route / TLS / reset before any HTTP status
};

struct ReachabilityDecision {
    int  misses = 0;            // new consecutive-miss count
    bool setOnline = false;
    bool setOffline = false;
    bool scheduleProbe = false; // confirm with /status.php
    bool serverAnswered = false;
};

inline ReachabilityDecision decideReachability(int misses, ReplyKind kind, bool fromProbe,
                                               bool probeInFlight, int offlineMisses = 2)
{
    ReachabilityDecision d;
    d.misses = misses;
    switch (kind) {
    case ReplyKind::Cancelled:
        return d;
    case ReplyKind::HttpAnswered:
        d.misses = 0;
        d.setOnline = true;
        d.serverAnswered = true;
        return d;
    case ReplyKind::TransferTimeout:
        if (!fromProbe) {
            d.scheduleProbe = !probeInFlight;
            return d;
        }
        [[fallthrough]];
    case ReplyKind::TransportFailure:
        d.misses = misses + 1;
        d.scheduleProbe = (d.misses == 1) && !probeInFlight;
        d.setOffline = d.misses >= offlineMisses;
        return d;
    }
    return d;
}

} // namespace talq
