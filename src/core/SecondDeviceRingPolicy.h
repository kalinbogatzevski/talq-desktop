#pragma once

// SecondDeviceRingPolicy -- pure decisions for a user signed in on several
// devices: do not ring (or stop ringing) for a call our own user is already in
// on another device, and never revert the account-wide "In a call" status from
// a device that did not join the call. No Qt -- see
// tests/second_device_ring_policy_test.cpp. CallManager gathers the evidence
// (HPB session maps, GET call/{token}) and owns the ring/teardown side effects.
//
// 2026-09-16 RCA, defect D12 (a user's second device, not the one in the call):
//  - two calls detected by the conversation poll rang for 60 s although the
//    user was in both on another device. The poll's self-ring guard reads
//    `participantInCallFlags`, a field Talk 24.0.4 does not send
//    (RoomFormatter.php sends `participantFlags`, and only for the REQUESTING
//    session), so it never fired. This device's signaling was in another room,
//    so only REST can tell.
//  - a third call rang; the user's own other session JOINED 8 s later (HPB
//    update, this device still Incoming) and the ring ran on for another 53 s.
//    Own-user joins were only ignored while Idle.
//  - Every ring timeout ends in teardown -> leaveCallOnServer(wasJoined=false),
//    which acks immediately -> callServerLeaveAcked -> revertStuckCall(). Talk
//    sets messageId "call" when an attendee's session enters a call (spreed
//    lib/Status/Listener.php), so the ringing device reverted the status of the
//    device that WAS in the call.

namespace talq {

// Before ringing, wait at most this long for GET call/{token}. That GET is a
// plain SELECT (no backend notification) and normally answers in one RTT; the
// cap keeps a hung request (RCA A3) from delaying a real call. A reply that
// lands after the cap can still stop the ring.
constexpr int kRingPrecheckMaxWaitMs = 2500;
// While ringing, re-check for our own user joining elsewhere this often (the
// only signal when our signaling is in a different room). Every ringing device
// that is not viewing the call's room sends these, so a group call fans them
// out across the room's members; 10 s keeps a ring answered elsewhere short
// (5 s on average) at half the requests of 5 s. A tick is skipped while the
// previous GET is still outstanding (CallManager::checkRingAnsweredElsewhere).
constexpr int kRingingSelfCheckIntervalMs = 10000;

enum class OwnPresence {
    Unknown,           // no usable evidence
    NotInCall,         // the server's call list has no other session of ours
    InCallElsewhere,   // another session of our user is in this call
};

// One row of GET call/{token} (spreed CallController::getPeersForCall:
// actorType, actorId, sessionId, lastPing), pre-classified by the caller.
struct CallPeerRow {
    bool actorIsUser = false;     // actorType == "users"
    bool actorIsSelfUser = false; // actorId == our Nextcloud user id
    bool isThisDevice = false;    // the row's Nextcloud session maps to our own HPB session
};

template <class Rows>
OwnPresence ownPresenceFromCallPeers(bool restOk, bool selfUserKnown, const Rows &rows)
{
    if (!restOk || !selfUserKnown) return OwnPresence::Unknown;
    for (const CallPeerRow &r : rows)
        if (r.actorIsUser && r.actorIsSelfUser && !r.isThisDevice)
            return OwnPresence::InCallElsewhere;
    return OwnPresence::NotInCall;
}

// HPB evidence: in the room our signaling is joined to, how many sessions of
// our user other than our own carry an in-call flag. Only positive evidence
// counts -- a participants update is applied entry by entry, so a peer's JOINED
// can be handled before our sibling's entry in the same update.
inline OwnPresence ownPresenceFromHpb(bool roomMatches, bool selfUserKnown, int otherOwnSessionsInCall)
{
    if (!roomMatches || !selfUserKnown) return OwnPresence::Unknown;
    return otherOwnSessionsInCall > 0 ? OwnPresence::InCallElsewhere : OwnPresence::Unknown;
}

enum class IncomingRingAction { Ring, SuppressInCallElsewhere };

// Unknown rings: a spurious ring beats a missed call.
inline IncomingRingAction decideIncomingRing(OwnPresence p)
{
    return p == OwnPresence::InCallElsewhere ? IncomingRingAction::SuppressInCallElsewhere
                                             : IncomingRingAction::Ring;
}

inline bool shouldStopRinging(OwnPresence p) { return p == OwnPresence::InCallElsewhere; }

// An HPB JOINED edge while ringing: is it our own user on another session?
inline bool isOwnOtherSession(bool selfUserKnown, bool sameUser, bool sameSession)
{
    return selfUserKnown && sameUser && !sameSession;
}

// Where a ring's own-user evidence comes from. When our signaling is joined
// (acked) to the call's room, the HPB own-session JOINED edge already stops a
// ring answered elsewhere, so REST is neither waited on (HPB-detected rings)
// nor polled while ringing (review CR-9: every ring waited up to 2.5 s and
// every ringing device sent ~12 GETs). What the HPB maps cannot show is a
// sibling that was already in the call before our signaling entered the room
// (joining a room delivers no in-call list, spreed-signaling room.go
// AddSession), hence one non-blocking GET once an HPB-detected ring starts.
// (The account-status revert rule for a device that never joined lives in
// talq::planCallEnd, core/CallJoinPolicy.h.)
struct RingEvidencePlan {
    bool waitForRest = false;          // hold the ring for GET call/{token}, capped
    bool deferOneTurn = false;         // re-check the HPB maps after the current update is applied
    bool restCheckAtRingStart = false; // one GET after ringing starts; a hit stops the ring
    bool pollRestWhileRinging = false; // GET every kRingingSelfCheckIntervalMs
};

inline RingEvidencePlan planRingEvidence(bool fromHpbEvent, bool signalingInCallRoomAcked)
{
    RingEvidencePlan p;
    if (fromHpbEvent && signalingInCallRoomAcked) {
        p.deferOneTurn = true;
        p.restCheckAtRingStart = true;
        return p;
    }
    p.waitForRest = true;
    p.pollRestWhileRinging = !signalingInCallRoomAcked;
    return p;
}

// An incoming call detected while an earlier detection's evidence is still
// being gathered. The earlier one keeps its check and rings first; a different
// call is the second call and takes the busy path (notification + 1:1
// auto-reply), exactly as when the first had already started ringing. Letting
// it replace the pending check silently dropped the first call: no ring, no
// busy reply (the conversation poll reports each call start only once).
enum class RingDetection {
    StartCheck,       // nothing pending: gather evidence for this call
    AlreadyChecking,  // the same call again (poll and HPB both saw it)
    SecondCall,       // another call's check is pending: reply busy
};

inline RingDetection classifyRingDetection(bool checkPending, bool sameCall)
{
    if (!checkPending) return RingDetection::StartCheck;
    return sameCall ? RingDetection::AlreadyChecking : RingDetection::SecondCall;
}

// When a ring starts, which in-call sessions of the call's room (HPB maps) go
// into the call's participant list. The evidence gathering can hold the ring
// for an event-loop turn or a REST round trip, so the JOINED edges of the
// caller and of anyone else already in the call were handled while this device
// was still Idle and registered nobody. Without this the caller's tile had no
// name and camera-off/muted defaults, and a second in-call peer was never
// subscribed after Accept. Our own session and our own user's other sessions
// are not participants of the call ringing here.
inline bool seedsRingParticipant(bool isOwnSession, bool isOwnUserSession, int hpbCallFlags)
{
    constexpr int kInCall = 1;
    return !isOwnSession && !isOwnUserSession && (hpbCallFlags & kInCall) != 0;
}

} // namespace talq
