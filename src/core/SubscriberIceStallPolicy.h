#pragma once

#include <cstdint>

// SubscriberIceStallPolicy -- pure, per-peer detection of a subscriber that was
// handed an offer but whose ICE never left "none"/"new" because it was never
// given a single remote candidate.
//
// Why it exists (field 2026-10-06, Ivan <-> Kalin): the HPB delivered an offer
// (0.86 s after requestOffer, like a healthy call) and then trickled NO
// candidate at all, so webrtcbin had nothing to check and ICE sat at "new". A
// healthy subscriber gets its candidates in the same millisecond as the offer
// and goes new -> checking -> connected in ~0.15 s. Nothing in the client timed
// out ICE "new": the only watchdog that saw it, the never-decoded one in
// SubscriberStallPolicy, waits ~12 s and is gated on the peer's VIDEO being on,
// so an audio-only / camera-off peer in the same state was never rebuilt.
//
// This one keys on ICE alone (no video input at all), so it also covers
// audio-only peers. It fires a bounded number of times per call and is re-armed
// only by the subscriber reaching ICE connected, so it cannot hot-loop. Each fire
// goes through recoverSubscriber, so it spends at most maxFires of that function's
// kMaxSubRecoveries budget per ICE-connect cycle.
//
// WHAT IT DOES NOT COVER:
//  - it asks for a rebuild, i.e. a fresh requestoffer. When the server never
//    answers that request (the 2026-10-06 dead-subscriber-entry bug in the
//    signaling server) a rebuild cannot produce an offer, so this watchdog would
//    NOT have rescued that call; it only shortens the time to the first re-request
//    from ~12 s to ~4 s where the server does answer;
//  - candidates that were given but did not lead anywhere ("checking" forever);
//  - an offer that never reached webrtcsrc (parse failure, webrtcsrc never
//    started): offerAgeMs stays < 0, so nothing fires;
//  - candidates counted by addIceCandidate() while they only sit in the queue
//    because webrtcbin never became ready: they count as "given".
//
// Pure logic (no Qt, no GStreamer) -- see tests/sub_ice_stall_policy_test.cpp.
// CallManager keeps one instance per subscribed peer and calls onTick() once per
// 1-second onLoadTick for every live camera subscriber.

struct SubscriberIceInput {
    int64_t offerAgeMs = -1;        // ms since the offer reached the subscriber; < 0 = not delivered yet
    bool    iceNewOrNone = false;   // webrtcbin ICE state is "new", or none was ever reported
    bool    iceConnected = false;   // "connected" or "completed"
    int     remoteCandidatesGiven = 0;  // addIceCandidate() calls this subscriber has received
};

class SubscriberIceStallPolicy {
public:
    // graceMs: how long a healthy subscriber is allowed to sit without candidates
    //          (healthy = ~1 ms; 4 s tolerates a slow link without hair-triggering).
    // maxFires: rebuilds this policy may request before ICE connects again.
    explicit SubscriberIceStallPolicy(int64_t graceMs = 4000, int maxFires = 2)
        : m_graceMs(graceMs), m_maxFires(maxFires) {}

    // Returns true when the subscriber should be rebuilt NOW.
    bool onTick(const SubscriberIceInput &in)
    {
        if (in.iceConnected) {          // it works: the budget starts afresh
            m_fires = 0;
            return false;
        }
        if (!in.iceNewOrNone) return false;             // checking / failed / ...: other watchdogs own those
        if (in.remoteCandidatesGiven > 0) return false; // it has something to check
        if (in.offerAgeMs < m_graceMs) return false;    // not delivered yet, or still inside the grace
        if (m_fires >= m_maxFires) return false;        // spent: the retry net and never-decoded watchdog remain
        ++m_fires;
        return true;
    }

    void reset() { m_fires = 0; }
    int  firesUsed() const { return m_fires; }
    int  maxFires() const { return m_maxFires; }
    int64_t graceMs() const { return m_graceMs; }

private:
    int64_t m_graceMs;
    int     m_maxFires;
    int     m_fires = 0;
};
