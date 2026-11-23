#pragma once

// CallPeerFlagsPolicy -- pure decisions about the OTHER person in a call:
//
//  1. decideRegisteredPeerFlags: which media flags a "peer joined the call"
//     report may write onto that person's tile (audio/video mute state), and
//     the flags that report may then act on (subscribablePeerFlags,
//     knownPeerFlags).
//  2. shouldAdoptPeerWhileActive: when a call that is already Active must still
//     adopt its 1:1 peer, so the REST discovery poll can stop.
//  3. callPeerPollIntervalMs: how often that poll asks while nobody is adopted
//     (a group call that went Active first is never adopted).
//
// No Qt -- see tests/call_peer_flags_policy_test.cpp. CallManager gathers the
// facts (the poll row, the signaling server's flag cache, the subscriber maps)
// and owns the side effects.
//
// 2026-11-02 RCA (a tester as callee, TalQ 0.79.2; the same signature is in 0.78.0
// logs). The tester's TalQ received and decoded the caller's video for the whole ~410 s
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
    // `flags` is the poll's guess with nothing else known (a NEW tile, the
    // signaling server's cache empty -- e.g. just cleared by a call or room
    // (re)join). The guess carries no video bit, yet the peer's camera may well
    // be on: the tile's video is then UNKNOWN (CallParticipant::markVideoUnknown),
    // not "Camera off". A real report clears it.
    bool videoUnknown = false;
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
//     peer is first subscribed) with its video marked unknown, an EXISTING
//     tile is left alone.
inline PeerFlagsDecision decideRegisteredPeerFlags(bool flagsSynthesized, bool participantExisted,
                                                   int flags, int hpbFlags)
{
    if (!flagsSynthesized) return { true, flags };
    if (hpbFlags != 0)     return { true, hpbFlags };
    if (!participantExisted) return { true, flags, /*videoUnknown*/true };
    return { false, 0 };
}

// The flags a report may SUBSCRIBE on (the group fallthrough in
// handleParticipantJoined): what the tile was just given. An invented report
// that left an existing tile alone starts nothing new; the periodic
// subscribe-reconcile sweep still subscribes any tile that shows media. A NEW
// tile seeded from the guess does subscribe: for a peer the signaling server
// never announced, that guess is the only discovery there is.
inline int subscribablePeerFlags(const PeerFlagsDecision &d)
{
    return d.apply ? d.flags : 0;
}

// The flags a report may MOVE A 1:1 CALL'S IDENTITY on (the #bug4 sibling
// re-adopt): only what the peer or the signaling server actually said. The poll
// invents audio for every row, an idle device of the same person included, so
// its guess cannot tell a sibling that publishes from one that never will; with
// the server silent it yields 0 (no media known). Reachable inside ONE poll
// answer: the first row adopts, the second row (the same person's other device)
// arrives before the first has a subscriber.
inline int knownPeerFlags(bool flagsSynthesized, int flags, int hpbFlags)
{
    return flagsSynthesized ? hpbFlags : flags;
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

// The REST call-list poll stops once a peer is adopted. A GROUP call that reached
// Active before the poll's first tick (rung from the conversation list, with a
// fast publisher) never adopts anyone (shouldAdoptPeerWhileActive is 1:1 only),
// so the poll asked every 3 s for the whole call: about 20 GETs a minute. Its
// own job is to turn a row the signaling server never announced into a tile;
// once every other row has one, later joiners come with the server's join
// events, and the poll stays only as a slow backup for one that does not.
//
//  groupCall       a 1:1 keeps the fast rate: its peer is adopted on the next
//                  tick after the stream request, and a peer returning from a
//                  drop must be found inside the grace window.
//  callActive      Outgoing/Connecting adopt the first joiner and stop the poll.
//  restPromoted    the REST-evidence backstop is waiting for the HPB session and
//                  times out a vanished peer after 10 s without a fresh tick.
//  otherRows       rows of the call list that are not our own account.
//  registeredRows  of those, rows that resolved to a signaling session with a tile.
constexpr int kCallPeerPollFastMs = 3000;
constexpr int kCallPeerPollSlowMs = 15000;

inline int callPeerPollIntervalMs(bool groupCall, bool callActive, bool restPromoted,
                                  int otherRows, int registeredRows)
{
    const bool settled = groupCall && callActive && !restPromoted
                      && otherRows > 0 && registeredRows >= otherRows;
    return settled ? kCallPeerPollSlowMs : kCallPeerPollFastMs;
}

} // namespace talq
