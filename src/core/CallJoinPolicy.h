#pragma once

#include <cstdint>

// CallJoinPolicy -- pure decisions for joining a call: when STUN/TURN must be
// (re)fetched, when media may start, and what a failed call-join POST means.
// No Qt, no GStreamer -- see tests/call_join_policy_test.cpp. CallManager owns
// the state and the side effects (joinCallOnServer, refreshIceServers,
// maybeStartCallMedia, reconcileCallJoin).
//
// Field incident 2026-09-16 (full RCA "D3/D5"). Talk sends its HPB backend
// notification synchronously inside POST call/{token}; one that drew a dead HPB
// held the request 90 s (3 x 30 s) and then threw -- 500 for POST/DELETE, 400
// for PUT -- AFTER in_call may or may not have been committed, depending on
// which notification hung (spreed 24.0.4 BackendNotifier.php, and
// ParticipantService.php where call_joined is posted before setInCall). TalQ's
// 30 s transfer timeout turned that into status 0.
//  - STUN/TURN were fetched only in the POST's success callback, so a committed
//    join whose response hung left every subscriber offer parked and the
//    publisher unbuilt (63 s in the field, then a 16-pipeline burst).
//  - status 0 was re-POSTed blindly. In one call the re-POST landed after the
//    other side had left and started a NEW ringing call; in another a failure
//    that arrived after hang-up still raised "Couldn't start the call" and no
//    leave was sent (a ghost participant for 30 s).
//  - m_stunServer was never cleared, so after the first successful join of a
//    process every later call reused that fetch -- including TURN REST
//    credentials that spreed issues for 24 h (lib/Config.php getTurnSettings).

namespace talq {

// ── STUN/TURN cache ──────────────────────────────────────────────────────────
// Refresh well before the 24 h TURN credential lifetime; stop USING a list
// before it can expire mid-call-setup.
constexpr int64_t kIceRefreshAfterMs = 6LL * 60 * 60 * 1000;    // 6 h
constexpr int64_t kIceUsableForMs    = 20LL * 60 * 60 * 1000;   // 20 h (< 24 h)
// After this many failed fetches with nothing usable cached, fall back to the
// public STUN server with no TURN so a directly-reachable call can still start
// (what TalQ always did on a failed fetch). Relay-only networks cannot connect
// on the fallback; the fetch keeps retrying in the background.
constexpr int     kIceFetchFailuresBeforeFallback = 3;
constexpr int     kIceFetchMaxAttempts = 10;

struct IceServerCache {
    int64_t fetchedAtMs = 0;     // wall clock of the last SUCCESSFUL fetch; 0 = never
    int     fetchedForCall = -1; // call generation that fetch belonged to
    bool    fallback = false;    // public-STUN fallback is installed (not a real fetch)
    int     fallbackForCall = -1; // call generation that installed the fallback
};

// May pipelines be built on the cached servers right now? The public-STUN
// fallback has no TURN, so it counts only for the call whose failed fetches
// installed it: a later call waits for its own fetch instead of building
// pipelines a relay-only network cannot connect on.
inline bool iceServersUsable(const IceServerCache &c, int64_t nowMs, int callGen)
{
    if (c.fallback) return c.fallbackForCall == callGen;
    if (c.fetchedAtMs <= 0) return false;
    const int64_t age = nowMs - c.fetchedAtMs;
    return age >= 0 && age < kIceUsableForMs;
}

// Should a fetch start now? Always once per call (config or credentials may
// have changed since the last call), when stale, when forced (signaling session
// reset), and never while one is already in flight.
inline bool shouldFetchIceServers(const IceServerCache &c, int64_t nowMs, int callGen,
                                  bool inFlight, bool force)
{
    if (inFlight) return false;
    if (force) return true;
    if (c.fetchedAtMs <= 0) return true;
    if (c.fetchedForCall != callGen) return true;
    const int64_t age = nowMs - c.fetchedAtMs;
    return age < 0 || age >= kIceRefreshAfterMs;
}

inline bool shouldUsePublicStunFallback(int failedAttempts, bool usableCache)
{
    return !usableCache && failedAttempts >= kIceFetchFailuresBeforeFallback;
}

inline bool shouldRetryIceFetch(int failedAttempts)
{
    return failedAttempts < kIceFetchMaxAttempts;
}

// 1 s, 2 s, 4 s, ... capped at 30 s. `failedAttempts` counts from 1.
inline int iceFetchRetryDelayMs(int failedAttempts)
{
    if (failedAttempts <= 1) return 1000;
    if (failedAttempts >= 6) return 30000;
    const int ms = 1000 << (failedAttempts - 1);
    return ms > 30000 ? 30000 : ms;
}

// ── media start barrier ─────────────────────────────────────────────────────
// The publisher (and the peer-discovery poll) start exactly once, when BOTH our
// join is confirmed (REST 200, HPB self in-call, or a reconcile) AND ICE servers
// are usable -- in whichever order those two arrive.
inline bool shouldStartCallMedia(bool joinConfirmed, bool iceUsable, bool alreadyStarted,
                                 bool callLive)
{
    return callLive && joinConfirmed && iceUsable && !alreadyStarted;
}

// ── call-join POST failure ──────────────────────────────────────────────────
enum class JoinFailureAction {
    IgnoreStale,     // the call this POST belonged to is gone -- no dialog, no teardown
    TreatAsJoined,   // we are demonstrably in the call already -- keep going
    Reconcile,       // ask the server whether the join committed before re-POSTing
    VerifyRejection, // a 4xx after HPB-only confirmation: GET call/{token} decides
    Fail,            // tell the user, tear down
};

// No response (transfer timeout / transport) or a 5xx: the server may have
// committed the join anyway, or may still commit it.
inline bool isTransientJoinStatus(int status)
{
    return status == 0 || (status >= 500 && status <= 599);
}

// Is a join POST's failure still about getting INTO the call? A signaling
// session reset during Connecting moves the call to Reconnecting before the
// join is confirmed; that join is still pending (review CR-7).
inline bool isStillJoining(bool joiningPhase, bool reconnecting, bool joinConfirmed)
{
    return joiningPhase || (reconnecting && !joinConfirmed);
}

struct JoinFailure {
    int  status = 0;          // HTTP status, 0 = no response (transfer timeout / transport)
    bool stale = false;       // call generation changed (hung up, torn down, new call)
    bool stillTrying = true;  // isStillJoining()
    bool alreadyJoined = false; // HPB self in-call (or an earlier confirmation) seen
    int  attempts = 0;        // reconcile/re-POST rounds already spent
    int  maxAttempts = 2;
};

inline JoinFailureAction decideJoinFailure(const JoinFailure &f)
{
    if (f.stale) return JoinFailureAction::IgnoreStale;
    const bool transient = isTransientJoinStatus(f.status);
    if (f.alreadyJoined) {
        // A server that already lists us in the call committed the join, whatever
        // a late 0/5xx says (Talk throws those AFTER the write). Tearing down here
        // killed working calls. A 4xx is a real rejection unless REST shows us in
        // the call: the HPB confirmation may be a late update from a previous call
        // in the same room (review CR-4).
        return transient ? JoinFailureAction::TreatAsJoined : JoinFailureAction::VerifyRejection;
    }
    if (!f.stillTrying) return JoinFailureAction::IgnoreStale;
    if (transient && f.attempts < f.maxAttempts) return JoinFailureAction::Reconcile;
    return JoinFailureAction::Fail;
}

// ── reconcile result (GET call/{token}) ─────────────────────────────────────
// GET call/{token} (spreed CallController::getPeersForCall) is a plain SELECT of
// sessions with in_call != 0 and last_ping >= now - 30 s; it fires no backend
// notification, so it answers promptly even while notifications are hanging.
// An HPB session's last_ping is refreshed only by its HPB's 10 s backend ping
// (nextcloud-spreed-signaling server/room.go updateActiveSessionsInterval), so a
// POP whose backend requests stall drops its sessions from that list while they
// are still in the call. "Nobody left" therefore needs the HPB to agree.
enum class JoinReconcileAction { TreatAsJoined, RePost, CallGone, Fail };

struct JoinReconcile {
    bool restOk = false;      // the GET itself succeeded
    bool selfListed = false;  // a row maps to OUR signaling session
    int  othersInCall = 0;    // rows for anyone other than our own user
    bool outgoing = false;    // we placed this call (startCall), not accepted it
    bool alreadyJoined = false; // HPB confirmed while the GET was in flight
    bool verifyingRejection = false; // this GET checks a 4xx that followed HPB-only confirmation
    // HPB view of the call's room. Known = our signaling is in that room, the
    // join is acked, and at least one participants update was applied since
    // joining it (joining a room delivers no in-call list, room.go AddSession).
    bool hpbRoomKnown = false;
    int  hpbOthersInCall = 0; // other sessions whose last update had the in-call flag
};

inline JoinReconcileAction decideJoinReconcile(const JoinReconcile &r)
{
    if (r.verifyingRejection) {
        // Only a successful GET that does not list us proves the rejection; the
        // HPB confirmation is the evidence in doubt, so it cannot settle it.
        return (r.restOk && !r.selfListed) ? JoinReconcileAction::Fail
                                           : JoinReconcileAction::TreatAsJoined;
    }
    if (r.alreadyJoined || (r.restOk && r.selfListed)) return JoinReconcileAction::TreatAsJoined;
    // Accepting a call that has nobody left in it: a re-POST would START a new
    // call and ring the caller back (field, 2026-09-16).
    if (r.restOk && !r.outgoing && r.othersInCall == 0
        && r.hpbRoomKnown && r.hpbOthersInCall == 0)
        return JoinReconcileAction::CallGone;
    return JoinReconcileAction::RePost;
}

// ── call end: what the server may still hold for this device ────────────────
// Talk writes in_call only AFTER the call_joined system message's backend
// notification returns (ParticipantService::changeInCall dispatches
// BeforeParticipantModifiedEvent before setInCall; SystemMessage/Listener posts
// call_joined there), and BackendNotifier::doRequest retries 3 times and on the
// last 5xx RETURNS instead of throwing. A join POST can therefore commit ~90 s
// after it was sent -- after a leave sent at hang-up, which then did nothing
// (leaveCall runs changeInCall(DISCONNECTED) even when not in the call).
constexpr int64_t kLateJoinCommitCheckAfterSendMs = 95000;
constexpr int64_t kLateJoinCommitCheckMinDelayMs  = 1000;

// Per call: the join POSTs (initial, re-POSTs, the session-reset re-register).
struct JoinPostLedger {
    bool    sent = false;        // at least one POST call/{token} went out
    int     inFlight = 0;        // POSTs with no response yet
    bool    unresolved = false;  // a POST ended 0/5xx: it may still commit server-side
    int64_t lastSentAtMs = 0;

    void noteSent(int64_t nowMs) { sent = true; ++inFlight; lastSentAtMs = nowMs; }
    void noteResult(bool ok, int status)
    {
        if (inFlight > 0) --inFlight;
        if (!ok && isTransientJoinStatus(status)) unresolved = true;
    }
};

struct CallEndPlan {
    bool    sendLeave = false;              // DELETE call/{token} (must-complete)
    bool    revertStatusAfterLeave = false; // run the client-side "stuck call status" revert
    bool    lateCommitCheck = false;        // re-check the call list once the server hold is over
    int64_t lateCommitCheckDelayMs = 0;
};

inline CallEndPlan planCallEnd(const JoinPostLedger &l, bool joinConfirmed, int64_t nowMs)
{
    CallEndPlan p;
    // Any join POST may have committed unless proven otherwise; a leave for a
    // session that is not in the call is harmless. Review CR-1: gating on
    // "confirmed or in flight" sent nothing during the reconcile window.
    p.sendLeave = joinConfirmed || l.sent;
    // Only a device that joined or tried to can have set the account-wide "In a
    // call" status (spreed Status/Listener.php); a ring that timed out, was
    // declined or was answered elsewhere must leave it alone (RCA D12).
    p.revertStatusAfterLeave = p.sendLeave;
    p.lateCommitCheck = l.inFlight > 0 || l.unresolved;
    if (p.lateCommitCheck) {
        const int64_t due = l.lastSentAtMs + kLateJoinCommitCheckAfterSendMs - nowMs;
        p.lateCommitCheckDelayMs = due > kLateJoinCommitCheckMinDelayMs ? due : kLateJoinCommitCheckMinDelayMs;
    }
    return p;
}

enum class LateCommitAction { Nothing, Leave };

// The timed check after the hold: GET call/{token} for the call that ended.
// `rejoined` = this device is joining or in that same call again. A failed GET
// still leaves: a leave while not in the call is a server-side no-op.
inline LateCommitAction decideLateJoinCommitCheck(bool restOk, bool selfListed, bool rejoined)
{
    if (rejoined) return LateCommitAction::Nothing;
    return (!restOk || selfListed) ? LateCommitAction::Leave : LateCommitAction::Nothing;
}

// A join POST's response that arrives after its call ended. 200 committed; 0/5xx
// may have (leave now, the timed check covers a later write); 4xx did not.
inline LateCommitAction decideLateJoinResult(bool ok, int status, bool rejoined)
{
    if (rejoined) return LateCommitAction::Nothing;
    return (ok || isTransientJoinStatus(status)) ? LateCommitAction::Leave : LateCommitAction::Nothing;
}

} // namespace talq
