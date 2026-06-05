#pragma once

#include <cstdint>

// PublisherStallPolicy -- pure, signaling-independent detection of a "dead
// publish leg": the LOCAL publisher WAS sending RTP (outbound packets-sent
// rising) but has STOPPED while it should still be sending. This is the
// publish-side analogue of SubscriberStallPolicy.
//
// Why it exists (field freeze 2026-06-05): when the remote peer vanishes
// WITHOUT a clean leave (its process was killed), libnice revokes "consent to
// send" -- but that does NOT move webrtcbin's ice-connection-state (it stayed
// "completed" the whole time), and the consent warning is a libnice g_warning
// that never reaches the GStreamer bus, so pollBus can't see it either. So
// nothing in TalQ observed the dead leg while the camera + H264 encoder +
// nicesink hot-looped "Consent to send has been revoked" ~89x/second for
// minutes -- pegging CPU/GPU/disk until the laptop froze entirely. The ONE
// publisher-observable signal that genuinely drops on consent revocation is
// outbound-RTP packets-sent (libnice stops actually sending). This watchdog
// keys on it, exactly as SubscriberStallPolicy keys on frameCount for the
// receive-side zombie-feed (#bug2). On fire, CallManager routes it through the
// EXISTING recoverPublisher (publisher-only rebuild) -- which tears down the
// flooding pipeline and stops the flood, and NEVER drops the call.
//
// Pure logic (no Qt, no GStreamer) so the decision is unit-testable without a
// real call -- see tests/pub_stall_policy_test.cpp. CallManager keeps one
// instance and calls onTick() once per 2-second updateCallStats tick.
//
// Rule: once the publisher has sent (packets-sent advanced past baseline), if
// the count then fails to advance for `stallTicks` consecutive ticks WHILE the
// publisher is expected to be sending, the leg is dead -> recover. A publisher
// that has never sent is not a stall (the call may just be starting).
// `expectedToSend == false` (an intentionally RTP-silent publisher: camera off
// AND muted, whose dummy-halt closes the video valves and whose audio is
// silenced) suppresses detection and re-baselines WITHOUT forgetting the leg
// has sent -- the analogue of SubscriberStallPolicy's videoMuted suppressor.
//
// KNOWN LIMITATIONS (reviewed 2026-06-05, accepted):
//  - The no-false-positive property relies on audio being a CONSTANT RTP
//    carrier: opusenc runs dtx=FALSE + fmtp usedtx=0, so audio packets keep
//    rising even when the camera is off. A future DTX change must revisit the
//    `expectedToSend` input.
//  - A consent-revoked flood occurring while the local user is camera-OFF AND
//    muted (expectedToSend=false) is NOT detected here, since libnice
//    consent-freshness STUN can hot-loop independent of app media. Uncovered
//    edge -- the common case (camera on or unmuted) is fully covered.

class PublisherStallPolicy {
public:
    // stallTicks * updateCallStats interval (2 s) = stall window. 4 -> ~8 s.
    explicit PublisherStallPolicy(int stallTicks = 4) : m_stallTicks(stallTicks) {}

    //   packetsSent    : summed outbound-rtp packets-sent (audio + all video layers)
    //   expectedToSend : the publisher should currently be emitting RTP
    //                    (camera on OR not muted). false = intentionally silent.
    // Returns true exactly once when the publisher has been stalled for
    // stallTicks ticks; then resets itself so a later stall is detected fresh.
    bool onTick(uint64_t packetsSent, bool expectedToSend)
    {
        if (!expectedToSend) {
            // Intentionally RTP-silent: pause detection, but do NOT forget the
            // leg HAS sent -- a still-dead leg seeing a transient silence must
            // remain recoverable. Clearing m_seen here would permanently
            // suppress the recovery this watchdog exists to provide.
            m_hasBaseline = false;   // re-baseline cleanly on resume
            m_ticks = 0;
            return false;
        }
        if (!m_hasBaseline) { m_prev = packetsSent; m_hasBaseline = true; return false; }
        if (packetsSent > m_prev) { m_prev = packetsSent; m_seen = true; m_ticks = 0; return false; }
        m_prev = packetsSent;                // no advance this tick
        if (!m_seen) return false;           // never sent -> not a stall (call just starting)
        if (++m_ticks >= m_stallTicks) { reset(); return true; }
        return false;
    }

    void reset() { m_hasBaseline = false; m_seen = false; m_ticks = 0; m_prev = 0; }
    int stalledTicks() const { return m_ticks; }   // for tests / telemetry
    bool hasSent() const { return m_seen; }

private:
    int      m_stallTicks;
    bool     m_hasBaseline = false;  // have we taken a baseline yet?
    uint64_t m_prev = 0;             // last observed packets-sent
    bool     m_seen = false;         // has the count advanced past baseline?
    int      m_ticks = 0;            // consecutive ticks with no advance (after first send)
};
