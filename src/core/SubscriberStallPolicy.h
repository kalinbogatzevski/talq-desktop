#pragma once

// SubscriberStallPolicy -- pure, per-peer detection of a "frozen subscriber":
// a remote feed that WAS delivering decoded video frames but has stopped, while
// the peer is still in-call and has NOT muted video. This is the signaling-
// independent backstop for the publisher-reconnect peer-freeze (#bug2): when a
// publisher reconnects it re-mints all its SSRCs, but the MCU keeps the old
// subscriber PeerConnection's ICE/DTLS alive -- so no ICE-failed / session-ended
// fires; the old RTP simply stops and the remote tile freezes with no recovery
// trigger. frameCount() is the one signal that is always present when the bug
// manifests, so the watchdog keys on it.
//
// Pure logic (no Qt, no GStreamer) so the decision is unit-testable without a
// real call -- see tests/sub_stall_policy_test.cpp. CallManager keeps one
// instance per subscribed peer (sessionId) and calls onTick() once per 2-second
// updateCallStats tick.
//
// Rule: once a feed has delivered at least one frame (its count has advanced),
// if the count then fails to advance for `stallTicks` consecutive ticks it is
// stale -> recover. A feed that has NEVER advanced is not treated as a stall
// (it may be a camera-off / not-yet-flowing peer, so recovering would be
// wasteful); videoMuted or an in-flight re-offer suppress and reset the tracker.

class SubscriberStallPolicy {
public:
    // stallTicks * updateCallStats interval (2 s) = stall window. 4 -> ~8 s.
    explicit SubscriberStallPolicy(int stallTicks = 4) : m_stallTicks(stallTicks) {}

    // One call per stats tick for one peer's subscriber.
    //   frameCount     : current VideoFrameProvider::frameCount() for this feed
    //   videoMuted     : peer has signalled video off (don't expect frames)
    //   reofferPending : a re-subscribe for this feed is already in flight
    // Returns true exactly once when the feed has been frozen for stallTicks
    // ticks; then resets itself so a later stall is detected fresh.
    bool onTick(int frameCount, bool videoMuted, bool reofferPending)
    {
        if (videoMuted || reofferPending) {
            // Pause detection, but do NOT forget that this feed HAS delivered
            // frames: a still-frozen feed that sees a transient mute / in-flight
            // re-offer blip must remain recoverable. Clearing m_seen here would
            // permanently suppress the recovery this watchdog exists to provide.
            m_prev  = -1;   // re-baseline cleanly on resume
            m_ticks = 0;
            return false;
        }
        if (m_prev < 0) { m_prev = frameCount; return false; }   // first observation: baseline only
        if (frameCount > m_prev) { m_prev = frameCount; m_seen = true; m_ticks = 0; return false; }
        m_prev = frameCount;                 // no advance this tick
        if (!m_seen) return false;           // never delivered a frame -> not a stall (camera-off etc.)
        if (++m_ticks >= m_stallTicks) { reset(); return true; }
        return false;
    }

    void reset() { m_prev = -1; m_seen = false; m_ticks = 0; }
    int stalledTicks() const { return m_ticks; }       // for tests / telemetry
    bool hasSeenFrames() const { return m_seen; }

private:
    int  m_stallTicks;
    int  m_prev = -1;     // last observed frame count; -1 = no baseline yet
    bool m_seen = false;  // have we ever seen the count advance after baseline?
    int  m_ticks = 0;     // consecutive ticks with no advance (after first frame)
};
