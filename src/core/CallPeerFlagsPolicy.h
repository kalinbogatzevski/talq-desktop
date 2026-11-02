#pragma once

// CallPeerFlagsPolicy -- two pure decisions about the OTHER person in a call:
//
//  1. decideRegisteredPeerFlags: which media flags a "peer joined the call"
//     report may write onto that person's tile (audio/video mute state).
//  2. shouldAdoptPeerWhileActive: when a call that is already Active must still
//     adopt its 1:1 peer, so the REST discovery poll can stop.
//
// No Qt -- see tests/call_peer_flags_policy_test.cpp. CallManager gathers the
// facts (the poll row, the signaling server's flag cache, the subscriber maps)
// and owns the side effects.
//
// 2026-11-02 RCA (Ivan as callee, TalQ 0.79.2; the same signature is in 0.78.0
// logs). Ivan's TalQ received and decoded the caller's video for the whole ~410 s
// call (about 20 fps for the first minute, about 7 fps after the caller switched
// his camera off and on -- a separate, unsolved send-rate problem) and his screen
// showed the caller's avatar with "Camera off" for all but a few seconds:
//  - His ring came from the conversation list (no caller session id), so nothing
//    recorded who the peer was. The only other chance to adopt the peer is the
//    REST poll's first tick 1.2 s after the publisher starts, and the adopt gate
//    only opens in Outgoing/Connecting. His publisher connected in 0.4 s, so the
//    call was already Active and the poll never stopped (137 ticks in 7 minutes).
//  - Every tick replays "peer joined" through the same handler the signaling
//    server's join event uses. Talk's REST call list carries NO inCall field, so
//    the poll invents IN_CALL|WITH_AUDIO (no video bit) and the handler wrote
//    "video muted" onto the caller's tile every 3 s, against the server's truth
//    (flags 7). Reconstructed from the log (that build logged no tile state), the
//    tile showed live video for only a few seconds.
//  - Collateral: no hang-up hint to the caller, no peer-left/grace handling (both
//    keyed on the adopted session), and a real mic mute undone within 3 s.

namespace talq {

struct PeerFlagsDecision {
    bool apply = false;   // write `flags` onto the tile's audio/video mute state
    int  flags = 0;
};

// flagsSynthesized: the flags are not a report from the peer. The REST poll
//                   invents them when the call list has no inCall field.
// participantExisted: the tile already existed before this report.
// flags:            what the report says (a real report, or the invented ones).
// hpbFlags:         the signaling server's last reported flags for this session,
//                   0 when it has none (it prunes a session on leave).
//
// A real report (the server's join/flag event, or a REST row that does carry
// inCall) is written as it always was. An invented one must never overwrite what
// is already known:
//   - the signaling server's flags win, because they track the peer's camera and
//     microphone;
//   - with none known, a NEW tile is seeded from the guess (audio is how a
//     peer is first subscribed), an EXISTING tile is left alone.
inline PeerFlagsDecision decideRegisteredPeerFlags(bool flagsSynthesized, bool participantExisted,
                                                   int flags, int hpbFlags)
{
    if (!flagsSynthesized) return { true, flags };
    if (hpbFlags != 0)     return { true, hpbFlags };
    if (!participantExisted) return { true, flags };
    return { false, 0 };
}

// A call that reached Active without ever recording its 1:1 peer (a ring with no
// session id and a fast publisher) must adopt the peer it is already
// subscribed to; adopting is what stops the REST poll and enables the hang-up
// hint and the peer-left/grace handling.
//
//  callActive           the call is Active (Outgoing/Connecting have their own gate).
//  oneToOne             a group call is NOT adopted: the adopted session is "the
//                       peer" whose departure ends the call, which for the first
//                       joiner of a group would end it for everyone else.
//  remotePeerUnset      no peer recorded yet.
//  peerGraceActive      a peer is mid-return from a drop; that path adopts it.
//  joinerIsOwnUser      the joining session belongs to our own account (another
//                       device of ours) -- never the peer.
//  joinerHasSubscriber  we already hold, or have asked for, that session's stream:
//                       the session we are subscribing to. (The poll asks for every
//                       row it resolves, so this narrows the choice but does not by
//                       itself pick between two live sessions of the same person;
//                       the #bug4 sibling re-adopt covers that case.)
inline bool shouldAdoptPeerWhileActive(bool callActive, bool oneToOne, bool remotePeerUnset,
                                       bool peerGraceActive, bool joinerIsOwnUser,
                                       bool joinerHasSubscriber)
{
    return callActive && oneToOne && remotePeerUnset && !peerGraceActive
        && !joinerIsOwnUser && joinerHasSubscriber;
}

} // namespace talq
