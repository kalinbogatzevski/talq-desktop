#pragma once

#include <array>

// Pure decision logic for the dynamic encode/decode load controller (0.51.x
// "Bafana Bafana"). Fed one LoadSample per ~1 s tick; returns the LoadCaps to
// apply. No Qt, no GStreamer — so it is unit-tested in isolation (the dev box
// has an NVENC GPU and will NOT reproduce real overload, so the controller's
// behaviour is validated by feeding synthetic samples; see
// tests/media_load_controller_test.cpp and the TALQ_TEST_ENCODE_USAGE seam).
//
// Design: docs/superpowers/specs/2026-06-10-encode-load-controller-design.md
//   - load = encodeUsage + decodeUsage (combined demand on the media engine; on
//     the single-engine iGPU we protect, encode and decode share one engine).
//   - the DOMINANT side (higher usage) is degraded first ("biggest-cost first").
//   - one monotonic ladder per dominant side; the level moves UP fast on
//     sustained overload (or a dropped-frame burst), DOWN slowly on sustained
//     headroom, with a cooldown that blocks the OPPOSITE direction after a move.
//   - the controller only ever REDUCES; the actuators apply min-of-ceilings, so
//     it can never raise quality above the device-tier / network / tile-size
//     ceilings — every axis is a ceiling, effective = the min.

namespace talq {

struct LoadSample {
    double encodeUsage = 0.0;   // 0..1+  send cost  = sum(encode_time)/frame_interval
    double decodeUsage = 0.0;   // 0..1+  receive cost = aggregate decode time/interval
    bool   dropBurst   = false; // a burst of dropped frames this tick → fast-trip
};

struct LoadCaps {
    int  sendLayerCeiling = 2;    // 2 = l/m/h, 1 = l/m, 0 = l only
    int  sendFps          = 30;   // 30 / 20 / 15
    int  recvSubstreamCap = 2;    // max substream for NON-focused tiles (2 hi … 0 lo)
    bool recvCapFocused   = false;// also cap the focused/largest tile (top rungs only)

    bool operator==(const LoadCaps &o) const {
        return sendLayerCeiling == o.sendLayerCeiling && sendFps == o.sendFps
            && recvSubstreamCap == o.recvSubstreamCap && recvCapFocused == o.recvCapFocused;
    }
    bool operator!=(const LoadCaps &o) const { return !(*this == o); }
};

class MediaLoadController {
public:
    // Thresholds default to the spec; ctor params let tests pin shorter timings.
    explicit MediaLoadController(int upTicks = 2, int downTicks = 12,
                                 int cooldownTicks = 5,
                                 double highUsage = 0.85, double lowUsage = 0.50)
        : m_upTicks(upTicks), m_downTicks(downTicks),
          m_cooldownTicks(cooldownTicks), m_high(highUsage), m_low(lowUsage) {}

    // Advance one ~1 s tick with a fresh load sample; returns the caps to apply.
    LoadCaps onTick(const LoadSample &s) {
        if (m_cooldown > 0) --m_cooldown;

        const double load = s.encodeUsage + s.decodeUsage;
        // Re-evaluated every tick so a call shifting from 1:1 (send-heavy) to
        // group (decode-heavy) retargets which side gets cut.
        m_dom = (s.decodeUsage > s.encodeUsage) ? Receive : Send;

        if (s.dropBurst) {
            // Panic: a cliff can't wait for the sustain window — jump up to two
            // rungs at once, bypassing the sustain count and any cooldown.
            stepUp(2);
            return caps();
        }

        if (load > m_high) {
            ++m_over; m_under = 0;
            if (m_over >= m_upTicks && upAllowed() && m_level < kMaxLevel)
                stepUp(1);
        } else if (load < m_low) {
            ++m_under; m_over = 0;
            if (m_under >= m_downTicks && downAllowed() && m_level > 0)
                stepDown();
        } else {
            // Deadband: a step requires SUSTAINED over/under, so a brief
            // excursion shouldn't count — reset both run-length counters.
            m_over = 0; m_under = 0;
        }
        return caps();
    }

    int      loadLevel() const { return m_level; }
    LoadCaps caps()      const { return capsFor(m_level, m_dom); }
    static constexpr int maxLevel() { return kMaxLevel; }

private:
    enum Dominant { Send, Receive };
    enum Move { None, Up, Down };
    static constexpr int kMaxLevel = 6;

    // Cooldown blocks only the OPPOSITE direction of the last move: shed fast
    // (consecutive up-steps allowed, gated only by the sustain count), but don't
    // reverse quickly (no down-step for cooldownTicks after an up, and vice-versa).
    bool upAllowed()   const { return !(m_cooldown > 0 && m_lastMove == Down); }
    bool downAllowed() const { return !(m_cooldown > 0 && m_lastMove == Up); }

    void stepUp(int n) {
        if (m_level >= kMaxLevel) return;
        m_level = (m_level + n > kMaxLevel) ? kMaxLevel : m_level + n;
        m_over = 0; m_under = 0;
        m_lastMove = Up; m_cooldown = m_cooldownTicks;
    }
    void stepDown() {
        if (m_level <= 0) return;
        --m_level;
        m_over = 0; m_under = 0;
        m_lastMove = Down; m_cooldown = m_cooldownTicks;
    }

    static LoadCaps capsFor(int level, Dominant dom) {
        if (level < 0) level = 0;
        if (level > kMaxLevel) level = kMaxLevel;
        // Hand-authored monotonic ladders (index = level 0..kMaxLevel).
        // SEND-dominant (1:1): degrade own send first, the remote tile last.
        static const std::array<LoadCaps, kMaxLevel + 1> kSend = {{
            /*L0*/ {2, 30, 2, false},   // full
            /*L1*/ {2, 20, 2, false},   // fps 30→20
            /*L2*/ {1, 20, 2, false},   // shed h
            /*L3*/ {1, 15, 2, false},   // fps 20→15
            /*L4*/ {0, 15, 2, false},   // shed m (l only)
            /*L5*/ {0, 15, 1, true},    // cap the remote tile → med
            /*L6*/ {0, 15, 0, true},    // cap the remote tile → low
        }};
        // RECEIVE-dominant (group): thumbnails → own send → focused tile last.
        static const std::array<LoadCaps, kMaxLevel + 1> kRecv = {{
            /*L0*/ {2, 30, 2, false},   // full
            /*L1*/ {2, 30, 1, false},   // thumbnails → med (focused exempt)
            /*L2*/ {2, 30, 0, false},   // thumbnails → low (focused exempt)
            /*L3*/ {2, 20, 0, false},   // cut own send fps
            /*L4*/ {1, 20, 0, false},   // shed h
            /*L5*/ {0, 15, 0, false},   // shed m + fps 15
            /*L6*/ {0, 15, 0, true},    // cap the focused tile too
        }};
        return (dom == Receive) ? kRecv[level] : kSend[level];
    }

    int    m_upTicks, m_downTicks, m_cooldownTicks;
    double m_high, m_low;
    int    m_level    = 0;
    int    m_over     = 0;
    int    m_under    = 0;
    int    m_cooldown = 0;
    Move   m_lastMove = None;
    Dominant m_dom    = Send;
};

} // namespace talq
