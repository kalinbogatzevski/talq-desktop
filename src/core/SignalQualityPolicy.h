#pragma once

#include <cstdint>

// SignalQualityPolicy -- pure, per-remote-participant bucketing + hysteresis
// for the per-tile signal-quality bars glyph (Zoom/Teams-style). CallManager
// feeds it a WINDOWED inbound-RTP loss fraction + latest jitter sample once
// per 2 s updateCallStats tick (the same tick SubscriberStallPolicy already
// runs its frame-stall check on) -- see SubscribeWebrtcSrc::pollInboundRtp().
//
// It holds the previous tick's CUMULATIVE packets-lost/packets-received
// internally (mirrors SubscriberStallPolicy's m_prev baseline pattern) so it
// can compute a fraction over just the last ~2 s, not a lifetime-cumulative
// one (which would hide recovery -- a peer that lost packets an hour ago and
// has been clean ever since must show GOOD now, not a permanently-tainted
// average).
//
// Bucketing (4 levels; exact cutoffs are a tuning knob, not a hard
// requirement -- these mirror common Zoom/Meet-style heuristics). Loss and
// jitter are each bucketed independently against the same 4-level ladder,
// and the OVERALL bucket is the WORSE of the two:
//   GOOD  loss < 1%   , jitter < 30 ms   -> full bars
//   FAIR  loss < 3%   , jitter < 60 ms   -> 2/3 bars
//   POOR  loss < 8%   , jitter < 120 ms  -> 1/3 bars
//   LOST  anything worse, OR not connected / no stats yet -> greyed/none
// (An earlier drafted version combined the two with OR at the FAIR/POOR
// tiers -- e.g. "loss < 3% OR jitter < 60 ms" -- but that lets a catastrophic
// value in ONE metric hide behind a clean value in the OTHER: 50% packet
// loss with a low jitter sample would read as FAIR, which would materially
// mislead the viewer. Taking the worse of two independently-bucketed tiers
// keeps GOOD's original "both must be good" meaning while making a bad
// metric always visible, and is monotonic/simple to reason about.)
//
// Hysteresis: a DEGRADATION (worse bucket) commits immediately (1 tick) so a
// real problem shows up fast; an IMPROVEMENT requires the SAME better raw
// bucket for `holdTicks` consecutive ticks (default 2, ~4 s at the 2 s tick
// cadence) so a single clean sample right after a blip doesn't flicker the
// glyph back up and down.
//
// Pure logic (no Qt, no GStreamer) so bucketing/hysteresis is unit-testable
// without a live call -- see tests/signal_quality_policy_test.cpp.
// CallManager keeps one instance per subscribed peer (sessionId), alongside
// its SubscriberStallPolicy, and calls onTick() once per stats tick.

class SignalQualityPolicy {
public:
    enum Level { Lost = 0, Poor = 1, Fair = 2, Good = 3 };

    explicit SignalQualityPolicy(int holdTicks = 2) : m_holdTicks(holdTicks) {}

    // One call per stats tick for one peer's subscriber.
    //   connected       : the underlying stream is actually live (this
    //                     peer's CallParticipant::connState() == Connected).
    //                     false -> instant Lost (matches the existing
    //                     Reconnecting/Failed tile scrim semantics).
    //   statsValid      : at least one get-stats reply has been successfully
    //                     parsed for this peer since it (re)connected. false
    //                     (e.g. the very first tick right after connect, or a
    //                     libgstwebrtc stat-shape mismatch) holds whatever is
    //                     already committed rather than guessing -- avoids
    //                     flashing red before real data exists.
    //   packetsLost     : cumulative GST_WEBRTC_STATS_INBOUND_RTP "packets-lost".
    //   packetsReceived : cumulative GST_WEBRTC_STATS_INBOUND_RTP "packets-received".
    //   jitterMs        : latest "jitter" sample, milliseconds.
    // Returns the committed level (post-hysteresis).
    int onTick(bool connected, bool statsValid, uint64_t packetsLost,
               uint64_t packetsReceived, double jitterMs)
    {
        int raw;
        if (!connected) {
            raw = Lost;
            m_havePrev = false;   // re-baseline cleanly so a reconnect doesn't
                                   // compute a bogus delta against stale counters
        } else if (!statsValid) {
            raw = m_committed;    // no data (yet) -- hold, don't guess
        } else {
            uint64_t dLost = 0, dRecv = 0;
            if (m_havePrev && packetsLost >= m_prevLost && packetsReceived >= m_prevRecv) {
                dLost = packetsLost - m_prevLost;
                dRecv = packetsReceived - m_prevRecv;
            }
            // else: counters went backwards (a reconnect re-baselined the
            // underlying stream without us seeing `connected` flip, or a
            // stat-shape hiccup) -- treat this tick as "no loss data" rather
            // than underflow into a huge bogus delta.
            m_prevLost = packetsLost;
            m_prevRecv = packetsReceived;
            m_havePrev = true;

            const uint64_t total = dLost + dRecv;
            const double lossFrac = total > 0 ? double(dLost) / double(total) : 0.0;
            raw = bucketFor(lossFrac, jitterMs);
        }
        commit(raw);
        return m_committed;
    }

    void reset()
    {
        m_committed = Good;   // optimistic default: don't paint a fresh tile red
        m_pendingRaw = Good;
        m_holdCount = 0;
        m_havePrev = false;
        m_prevLost = 0;
        m_prevRecv = 0;
    }

    int committed() const { return m_committed; }

    // Overall bucket = the WORSE of the loss tier and the jitter tier (see
    // the class comment above for why this beats a literal OR combination).
    static int bucketFor(double lossFrac, double jitterMs)
    {
        const int lossTier   = tierFor(lossFrac, 0.01, 0.03, 0.08);
        const int jitterTier = tierFor(jitterMs, 30.0, 60.0, 120.0);
        return lossTier < jitterTier ? lossTier : jitterTier;
    }

private:
    // v < goodMax -> Good; < fairMax -> Fair; < poorMax -> Poor; else Lost.
    static int tierFor(double v, double goodMax, double fairMax, double poorMax)
    {
        if (v < goodMax) return Good;
        if (v < fairMax) return Fair;
        if (v < poorMax) return Poor;
        return Lost;
    }

    void commit(int raw)
    {
        if (raw == m_committed) {
            m_holdCount = 0;
            return;
        }
        if (raw < m_committed) {
            // Degrade instantly -- a real problem must show immediately.
            m_committed = raw;
            m_holdCount = 0;
            return;
        }
        // Improve only after `holdTicks` consecutive ticks at the SAME
        // better raw bucket, so one clean sample doesn't flicker the glyph.
        if (raw == m_pendingRaw) {
            ++m_holdCount;
        } else {
            m_pendingRaw = raw;
            m_holdCount = 1;
        }
        if (m_holdCount >= m_holdTicks) {
            m_committed = raw;
            m_holdCount = 0;
        }
    }

    int      m_holdTicks;
    int      m_committed  = Good;   // optimistic default until proven otherwise
    int      m_pendingRaw = Good;
    int      m_holdCount  = 0;
    bool     m_havePrev   = false;
    uint64_t m_prevLost   = 0;
    uint64_t m_prevRecv   = 0;
};
