#pragma once

#include <cstdint>

// CallRecoveryPolicy -- pure decisions for mid-call recovery: when an in-flight
// publisher rebuild stops blocking the next one, when a rebuild may actually
// build, and what a subscriber failure during Reconnecting turns into. No Qt, no
// GStreamer -- see tests/call_recovery_policy_test.cpp. CallManager owns the
// state, the timers and the side effects (recoverPublisher,
// rebuildPublisherAndReoffer, recoverSubscriber, the sessionReset handler).
//
// 2026-09-16 RCA, defect D8 (failure model B1-B4). Code paths, not exercised in
// the field logs:
//  - MT-13: m_pubRebuildInFlight was cleared only by publisher ICE
//    connected/completed/failed, a start() failure or teardown. The rebuilt
//    offer goes out through SignalingClient::sendSessionMessage, which returns
//    silently while unauthenticated (SignalingClient.cpp:1262). A rebuild fired
//    inside the ~46 s signaling reconnect got no answer, its ICE never left
//    "new", and recoverPublisher -- also the one sessionReset calls -- returned
//    early forever: the call stayed Reconnecting.
//  - MT-12: recoverSubscriber returned unless Connecting/Active, so a subscriber
//    failure during Reconnecting was dropped, and the dead pipeline stayed in
//    m_subscribePipelines, which also hid the peer from the onLoadTick
//    subscribe-reconcile sweep.
//  - sessionReset rebuilt the publisher on the STUN/TURN list cached at join.

namespace talq {

// ── in-flight publisher rebuild ─────────────────────────────────────────────
// How long a rebuild may stay unconcluded before it stops blocking the next.
// The slowest legitimate answer: the HPB stalls publisher creation on a dead
// proxy for its 20 s proxytimeout before moving to the next proxy, and
// cross-POP lookups wait its 25 s mcu timeout (both verified live on all three
// POPs, 2026-09-16). ICE then needs its connectivity checks on top. A rebuild
// with no conclusion after this is not coming back.
constexpr int64_t kPublisherRebuildDeadlineMs = 45000;

class PublisherRebuildGate {
public:
    void begin(int64_t nowMs) { m_inFlight = true; m_startedMs = nowMs; }
    // ICE connected/completed/failed, start() failure, session reset, teardown.
    void conclude() { m_inFlight = false; m_startedMs = 0; }
    bool inFlight() const { return m_inFlight; }
    bool expired(int64_t nowMs) const
    {
        if (!m_inFlight) return false;
        const int64_t age = nowMs - m_startedMs;
        return age >= kPublisherRebuildDeadlineMs;   // a backwards clock never expires it
    }
    // A rebuild blocks another only while it can still conclude on its own.
    bool blocksNewRebuild(int64_t nowMs) const { return m_inFlight && !expired(nowMs); }

private:
    bool    m_inFlight = false;
    int64_t m_startedMs = 0;
};

// ── may a (re)build build right now? ────────────────────────────────────────
// After a session reset the forced room re-join and the forced STUN/TURN
// re-fetch are both REST calls; the room join can hang on Talk's synchronous
// HPB notifier (RCA A3), so both waits are capped. Signaling being down is not
// capped: an offer cannot leave the client then.
constexpr int64_t kRebuildRoomRejoinWaitMaxMs = 10000;
constexpr int64_t kRebuildIceFetchWaitMaxMs   = 3000;
constexpr int     kRebuildWaitPollMs          = 500;

enum class PublisherRebuildStep {
    Build,
    WaitForSignaling,    // hello not authenticated: the offer would be dropped
    WaitForRoomRejoin,   // fresh session not yet back in the room
    WaitForIceServers,   // a STUN/TURN refresh is about to land
};

struct PublisherRebuildInputs {
    bool    signalingUp = true;         // SignalingClient::isConnected()
    bool    awaitingRoomRejoin = false; // a session reset's forced room join is not acked yet
    int64_t roomRejoinWaitedMs = 0;     // since that session reset
    bool    iceFetchInFlight = false;   // refreshIceServers() still running
    int64_t iceFetchWaitedMs = 0;       // since this rebuild first found it running
};

inline PublisherRebuildStep decidePublisherRebuildStep(const PublisherRebuildInputs &in)
{
    if (!in.signalingUp) return PublisherRebuildStep::WaitForSignaling;
    if (in.awaitingRoomRejoin && in.roomRejoinWaitedMs < kRebuildRoomRejoinWaitMaxMs)
        return PublisherRebuildStep::WaitForRoomRejoin;
    if (in.iceFetchInFlight && in.iceFetchWaitedMs < kRebuildIceFetchWaitMaxMs)
        return PublisherRebuildStep::WaitForIceServers;
    return PublisherRebuildStep::Build;
}

// ── subscriber failure by call phase ────────────────────────────────────────
enum class SubscriberRecoveryAction {
    Ignore,          // not in a call (Idle/Ending) or not yet in media (Outgoing/Incoming)
    RecoverNow,      // Connecting/Active: tear down + re-request
    DeferToActive,   // Reconnecting: tear down now, re-request on the next Active
};

inline SubscriberRecoveryAction decideSubscriberRecovery(bool connectingOrActive, bool reconnecting)
{
    if (connectingOrActive) return SubscriberRecoveryAction::RecoverNow;
    if (reconnecting) return SubscriberRecoveryAction::DeferToActive;
    return SubscriberRecoveryAction::Ignore;
}

// ── the re-subscribe replay on the next Active ──────────────────────────────
// Deferred subscriber failures (above) and a session reset both replay a
// requestoffer for peers without a subscriber when the call is Active again.
// A peer that publishes nothing (in the call with no mic and no camera) has no
// offer to give: asking anyway runs the 8 s requestoffer retry for minutes, or
// its not_allowed escalation parks the tile on Reconnecting -> Failed. Skip it,
// with the same evidence the JOINED handler subscribes on.
//  - The HPB lists the session in the call: trust its media flags (Talk
//    Participant flags: 1 in call, 2 with audio, 4 with video). The tile's
//    muted state also follows mute messages, which do not stop a publisher.
//  - Not listed (the map was cleared by a session reset's room re-join and no
//    update has arrived yet): fall back to the tile's view, like the
//    onLoadTick subscribe-reconcile sweep. The re-join's JOINED edges request
//    any peer skipped here once their flags arrive.
inline bool resubscribeOnActive(int hpbCallFlags, bool tileAudioMuted, bool tileVideoMuted)
{
    constexpr int kInCall = 1, kWithAudio = 2, kWithVideo = 4;
    if (hpbCallFlags & kInCall)
        return (hpbCallFlags & (kWithAudio | kWithVideo)) != 0;
    return !tileAudioMuted || !tileVideoMuted;
}

} // namespace talq
