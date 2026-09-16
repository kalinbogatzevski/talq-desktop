#pragma once

#include <cstdint>

// SubscriberRxStallPolicy -- pure, per-peer detection of a subscriber whose
// inbound RTP has stopped: packets-received (summed over the subscriber's
// inbound-rtp stats, audio + video) no longer advances while the peer is
// unmuted and the subscriber is connected. The audio counterpart of
// SubscriberStallPolicy, which counts decoded VIDEO frames and so cannot see an
// audio-only peer at all.
//
// 2026-09-16 RCA, D8(d) / failure model B3: when an MCU proxy or a TURN relay
// dies under a subscriber, the HPB tells clients nothing and ICE may keep
// reporting connected (PublisherStallPolicy.h records the same for consent
// loss). A camera-off peer then goes silent with no recovery trigger.
//
// Why "no packets while unmuted" is not a false positive (verified 2026-09-16):
//  - TalQ senders: opusenc dtx=FALSE, cbr, and mute is a `volume` mute feeding
//    digital silence to the encoder (PublishPipeline.cpp:523-534, 1815-1837),
//    so audio RTP never pauses, muted or not.
//  - Talk web / mobile senders (libwebrtc): mute is track.enabled=false
//    (spreed TrackEnabler.js), which libwebrtc maps to SetAudioSend(enable=false)
//    -> MuteStream -> SetMuted; the muted frame is still encoded and sent
//    (rtp_sender.cc, webrtc_voice_engine.cc, channel_send.cc).
//  - Opus DTX is not negotiated: the HPB creates Janus rooms without opus_dtx
//    (nextcloud-spreed-signaling sfu/janus/janus.go createPublisherRoom), Janus
//    defaults it to false, and TalQ munges usedtx=0.
//  - TalQ never pauses a subscriber's audio (selectStream only picks a video
//    substream).
// The peer-muted gate is kept anyway, so a sender that does pause RTP on mute
// is covered as long as its mute reaches us. Limitation: because the counter is
// summed, a peer whose video still flows masks an audio-only leg failure -- a
// missed detection, never a false one.
//
// Pure logic (no Qt, no GStreamer) -- see tests/sub_rx_stall_policy_test.cpp.
// CallManager keeps one instance per subscribed peer and calls onTick() once
// per 2-second updateCallStats tick (which runs only while Active).

class SubscriberRxStallPolicy {
public:
    // stallTicks * 2 s tick = window. 5 -> ~10 s: longer than the video
    // frame-stall's 8 s (a short network hiccup must not rebuild an audio
    // feed), well under libnice's 30 s consent timeout.
    explicit SubscriberRxStallPolicy(int stallTicks = 5) : m_stallTicks(stallTicks) {}

    //   packetsReceived : summed inbound-rtp packets-received (one tick stale)
    //   statsValid      : at least one get-stats reply has been parsed
    //   connected       : the subscriber's ICE reached connected
    //   peerAudioMuted  : the peer signalled its microphone off (or is unknown)
    //   reofferPending  : a re-subscribe for this peer is already in flight
    // Returns true exactly once when packets stopped for stallTicks ticks after
    // having flowed; then resets so a later stall is detected fresh.
    bool onTick(uint64_t packetsReceived, bool statsValid, bool connected,
                bool peerAudioMuted, bool reofferPending)
    {
        if (!statsValid || !connected || peerAudioMuted || reofferPending) {
            // Pause, and re-baseline on resume, but remember that packets DID
            // flow: a dead feed that sees a transient gate blip must stay
            // recoverable (same rule as SubscriberStallPolicy).
            m_hasPrev = false;
            m_ticks = 0;
            return false;
        }
        if (!m_hasPrev || packetsReceived < m_prev) {
            // First observation, or the counter went backwards (a new stats
            // source): baseline only.
            m_prev = packetsReceived;
            m_hasPrev = true;
            m_ticks = 0;
            return false;
        }
        if (packetsReceived > m_prev) {
            m_prev = packetsReceived;
            m_seen = true;
            m_ticks = 0;
            return false;
        }
        if (!m_seen) return false;           // never received: not this watchdog's case
        if (++m_ticks >= m_stallTicks) { reset(); return true; }
        return false;
    }

    void reset() { m_hasPrev = false; m_prev = 0; m_seen = false; m_ticks = 0; }
    int  stallTicks() const { return m_stallTicks; }
    bool hasSeenPackets() const { return m_seen; }

private:
    int      m_stallTicks;
    bool     m_hasPrev = false;
    uint64_t m_prev = 0;
    bool     m_seen = false;   // packets advanced at least once after baseline
    int      m_ticks = 0;      // consecutive flat ticks after packets flowed
};
