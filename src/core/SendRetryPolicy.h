#pragma once

// SendRetryPolicy -- the pure decisions behind re-sending a chat message that
// failed to send. No Qt: MessageListModel maps the network outcome onto these
// primitives and applies the result. See tests/send_retry_policy_test.cpp.
//
// Why this is a policy and not a one-line "resend on reconnect" (RCA
// 2026-10-03, every claim below verified against the Talk 24.0.4 source):
//
//  * "failed" means UNKNOWN, not "undelivered". ChatManager::sendMessage saves
//    the comment (ChatManager.php:456) BEFORE a synchronous notify to the HPB
//    (:511). A hang there trips our timeout; an exception there becomes a
//    400 {error:'message'} (ChatController.php:452-454). Either way the message
//    is already stored.
//  * The server does NOT deduplicate on referenceId. Re-POSTing with the same id
//    stores a second message. So a re-send must be CHECK-THEN-RESEND: ask the
//    server for recent history, and POST again only when our message is
//    provably not in it.
//  * "Provably" cannot be read off the number of messages on the page. Talk
//    applies `limit` to the RAW comments and then drops the ones the caller may
//    not see (a /command reply, a private reply, an expired message), so a
//    window that is completely full can come back short
//    (ChatController.php:1124-1133, and its own doc: "could be lower if some
//    messages are not visible"). What the server does tell us exactly is
//    X-Chat-Last-Given: the id of the OLDEST RAW comment in the window
//    (ChatController.php:1212-1214). Everything newer than that id is in the
//    window. Our message, if it was saved, got an id newer than any message we
//    knew about when we sent it -- so the window covers it iff
//    lastGiven <= (newest id known at send time) + 1. An empty room answers 304
//    with no body, which is conclusive on its own.
//  * The 20 s "failed" display flip is shorter than the 30 s transfer timeout
//    and ApiClient::post has no handle to abort, so the original POST can still
//    be live. An attempt is "in flight" until its reply callback fires, however
//    the row looks.
//  * The status code is the only discriminator we get (the OCS error body is
//    dropped by ApiClient). 403/404/412/413 are raised before the save and will
//    never succeed on a retry; 401 must not be hammered (core brute-force
//    throttle on a shared office NAT address).
//  * Precedent for NOT treating status 0 as "not delivered": CallJoinPolicy.h
//    (2026-09-16), where a blind status-0 re-POST started a second ringing call.
//
// The history probe doubles as the connectivity test: when it succeeds the
// connection is demonstrably back, so no reachability edge is needed (the edge
// misses timeouts, HTTP errors and a single failed request after a wake).

namespace talq::sendretry {

// ---- failure classification --------------------------------------------------

enum class Failure {
    Retryable,   // outcome unknown or transient: reconcile with the server, then re-send
    Permanent,   // the server refused it for a reason a retry cannot change
};

// `status` is what ApiClient hands the callback: the HTTP status, or 0 for a
// transport failure (reset, timeout, refused, DNS, TLS), or the OCS status.
inline Failure classifyFailure(int status)
{
    if (status == 0)                  return Failure::Retryable;  // unknown
    if (status >= 500)                return Failure::Retryable;  // may follow a committed save
    if (status == 408 || status == 429) return Failure::Retryable;
    // 400 {error:'message'} is the post-save catch-all (retryable-by-reconcile)
    // but 400 {error:'reply-to'} is permanent, and the body that tells them
    // apart is dropped. The tight budget below bounds the cost of guessing.
    if (status == 400)                return Failure::Retryable;
    // A 2xx we could not use (empty data, unparsable body): the server accepted
    // it, so the reconcile will find it.
    if (status >= 200 && status < 300) return Failure::Retryable;
    return Failure::Permanent;        // 401 403 404 412 413 422 ...
}

// Total POSTs (the first send included) before we stop trying on our own. A
// cap matters because a persistent post-save failure (e.g. a rejected HPB
// secret) stores the message on EVERY attempt.
inline int postAttemptBudget(int status)
{
    return status == 400 ? 2 : 5;
}

// ---- timing ------------------------------------------------------------------

// Wait before the next probe+re-send, after the Nth failed POST (N >= 1). Also
// the settle time that lets a server request that outlived our client finish
// its save before we look for it. (The budget gives up after the 5th POST, so
// the 5-minute step is only reached by a caller that raises the budget.)
inline long long retryDelayMs(int postAttemptsDone)
{
    switch (postAttemptsDone < 1 ? 1 : postAttemptsDone) {
    case 1:  return 5000;
    case 2:  return 15000;
    case 3:  return 45000;
    case 4:  return 120000;
    default: return 300000;
    }
}

// Wait before probing again when the probe itself could not reach the server.
// Does not consume the POST budget: a long outage must not use it up.
inline long long probeDelayMs(int probeFailures)
{
    long long d = 5000;
    for (int i = 1; i < probeFailures && d < 60000; ++i) d *= 2;
    return d > 60000 ? 60000 : d;
}

// An attempt whose callback never fires (should be impossible under the 30 s
// transfer timeout) is released after this long so the entry cannot wedge.
constexpr long long kInFlightWatchdogMs = 120000;

// A recovery signal may pull the next try forward, but never closer than this
// to the last activity on the entry (flapping links fire the edge repeatedly).
constexpr long long kMinSpacingMs = 3000;

inline long long kickedNextTryMs(long long nextTryMs, long long nowMs, long long lastActivityMs)
{
    const long long earliest = lastActivityMs + kMinSpacingMs;
    const long long target = earliest > nowMs ? earliest : nowMs;
    return nextTryMs < target ? nextTryMs : target;
}

// The deadlines are wall-clock. After a resume the clock can be stepped BACK
// (RTC / NTP correction), which would leave a deadline hours away. No schedule
// above waits longer than this, so a deadline further out than that is a clock
// step, not a plan: bring it back to a short wait.
constexpr long long kMaxPlannedWaitMs = 6 * 60 * 1000;

inline long long sanitizeNextTryMs(long long nextTryMs, long long nowMs)
{
    return nextTryMs > nowMs + kMaxPlannedWaitMs ? nowMs + 5000 : nextTryMs;
}

// ---- what happened to a POST -------------------------------------------------

struct FailureOutcome {
    bool      giveUp = false;     // stop auto-retrying; the user can still retry by hand
    long long nextTryDelayMs = 0;
};

// A request that was in flight this long before it failed was almost certainly
// held by a stalled server -- the ~90 s signaling-notify hang that a Talk server
// with an unreachable signaling backend shows is the case in point. Under that
// load the request may still be QUEUED there, not yet saved, when a probe 5 s
// later is served: the
// probe reads "absent", the re-POST goes out, and two copies are stored once the
// queue drains. The first check after a slow failure therefore waits long enough
// for the original to have run. (A fast failure -- refused, reset on a dead
// pooled connection -- means the request did not get that far, so the short
// settle is enough. A slow request that is still queued after even this wait is
// the remaining hole in "exactly one copy".)
constexpr long long kSlowFailureMs = 25000;
constexpr long long kSlowFailureSettleMs = 120000;

// inFlightMs: how long the attempt was outstanding before it failed.
inline FailureOutcome onPostFailed(int status, int postAttemptsDone, long long inFlightMs = 0)
{
    FailureOutcome o;
    if (classifyFailure(status) == Failure::Permanent
        || postAttemptsDone >= postAttemptBudget(status)) {
        o.giveUp = true;
        return o;
    }
    o.nextTryDelayMs = retryDelayMs(postAttemptsDone);
    if (inFlightMs >= kSlowFailureMs && o.nextTryDelayMs < kSlowFailureSettleMs)
        o.nextTryDelayMs = kSlowFailureSettleMs;
    return o;
}

// ---- the history probe -------------------------------------------------------

enum class ProbeVerdict {
    Delivered,      // our referenceId is in the page: it landed
    Absent,         // the window provably covers the send and our id is not in it
    Inconclusive,   // the window may not reach back far enough to say
};

// referenceIdFound  our referenceId is on the page
// emptyHistory      the room has no comments at all (HTTP 304, no body)
// lastGivenId       X-Chat-Last-Given: the oldest RAW comment id in the window;
//                   0 when the header is missing
// newestKnownId     the newest server id we knew in this room when we sent; 0
//                   when we knew none (so no coverage can be proven by id)
inline ProbeVerdict probeVerdict(bool referenceIdFound, bool emptyHistory,
                                 long long lastGivenId, long long newestKnownId)
{
    if (referenceIdFound) return ProbeVerdict::Delivered;
    if (emptyHistory)     return ProbeVerdict::Absent;
    if (lastGivenId > 0 && newestKnownId > 0 && lastGivenId <= newestKnownId + 1)
        return ProbeVerdict::Absent;
    return ProbeVerdict::Inconclusive;
}

enum class AfterProbe { MarkDelivered, Resend, GiveUp };

// everPosted: a POST for this message has actually been started. One that has
// not (it was queued behind an older message) cannot be on the server, so the
// window proves nothing about it and there is no duplicate to guard against.
// manualForce: the user pressed Retry on this very message. Inconclusive then
// means "resend anyway" -- otherwise a message in a very busy room could never
// be re-sent at all. The caller consumes the flag when the forced POST starts.
inline AfterProbe afterProbe(ProbeVerdict v, bool manualForce, bool everPosted)
{
    if (v == ProbeVerdict::Delivered) return AfterProbe::MarkDelivered;
    if (!everPosted)                  return AfterProbe::Resend;
    switch (v) {
    case ProbeVerdict::Delivered:    return AfterProbe::MarkDelivered;
    case ProbeVerdict::Absent:       return AfterProbe::Resend;
    case ProbeVerdict::Inconclusive: return manualForce ? AfterProbe::Resend
                                                        : AfterProbe::GiveUp;
    }
    return AfterProbe::GiveUp;
}

enum class ProbeFailure { Retry, GiveUp };

// The probe could not read history. Unreachable -> keep waiting. The server
// answering "no" (revoked credentials, not a participant, room gone) means the
// message can never be delivered from here.
inline ProbeFailure classifyProbeFailure(int httpStatus)
{
    return (httpStatus == 401 || httpStatus == 403 || httpStatus == 404)
        ? ProbeFailure::GiveUp : ProbeFailure::Retry;
}

// ---- ordering and display ----------------------------------------------------

// A token's queue is strictly in send order: only its OLDEST still-active entry
// may start a probe. Entries behind it share its fate, so a retry can never
// leapfrog an older message that is still waiting.
inline bool headReady(bool inFlight, bool permanent, long long nextTryMs, long long nowMs)
{
    return !permanent && !inFlight && nowMs >= nextTryMs;
}

// A NEW send queues behind an entry that is waiting (neither delivered, given
// up, nor currently in flight): sending it straight away would land it ahead
// of the older message that is about to be retried.
inline bool blocksNewSend(bool inFlight, bool permanent)
{
    return !inFlight && !permanent;
}

enum class Display { Sending, Failed };

// What the bubble shows. `slow` is the 20 s "this is taking too long" flag of
// the CURRENT attempt. A message whose POST has not even been started (queued
// behind an older one) reads as Sending; one that has been POSTed and is between
// tries reads as Failed, because as far as we know it is not on the server.
inline Display displayState(bool inFlight, bool slow, bool everPosted, bool permanent)
{
    if (permanent) return Display::Failed;
    if (inFlight)  return slow ? Display::Failed : Display::Sending;
    return everPosted ? Display::Failed : Display::Sending;
}

} // namespace talq::sendretry
