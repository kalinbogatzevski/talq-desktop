#pragma once

#include <array>

// 0.61.0 "Blue Fiesta Week 2" — slow-start fps ramp for the weak-tier single
// stream. Pure logic, no Qt/GStreamer. Fed one encodeUsage sample per ~1 s tick.
//
// WHY a separate policy: MediaLoadController models "start at FULL quality, shed
// down a ladder under load" (its lowest fps rung is 15). A weak box must do the
// OPPOSITE — start at a safe 10 fps and climb ONLY when there is sustained encode
// headroom — so a call-start 30 fps spike can't freeze it while the camera,
// encoder and scaler are all warming up. This ramp is the fps authority for solo
// mode; the CallManager tick applies min(ramp, controllerFps) so real overload
// still pulls below the ramp.
//
//   ladder = {10, 15, 20, 24, 30}  (index 0..4)
//   headroom (usage < low) for upTicks consecutive ticks -> +1 rung
//   overload (usage > high)                              -> -2 rungs (fast), floored
//   deadband (low <= usage <= high)                      -> hold (reset the run)

namespace talq {

class FpsRampPolicy {
public:
    explicit FpsRampPolicy(int upTicks = 3, double highUsage = 0.85, double lowUsage = 0.50)
        : m_upTicks(upTicks), m_high(highUsage), m_low(lowUsage) {}

    // Advance one tick; returns the fps to apply.
    int onTick(double encodeUsage) {
        if (encodeUsage > m_high) {
            m_idle = 0;
            m_idx = (m_idx >= 2) ? m_idx - 2 : 0;   // drop fast, floor at 0
        } else if (encodeUsage < m_low) {
            if (m_idx < kMax && ++m_idle >= m_upTicks) { ++m_idx; m_idle = 0; }
        } else {
            m_idle = 0;                              // deadband: hold
        }
        return fps();
    }

    int  fps() const { return kLadder[m_idx]; }
    void reset() { m_idx = 0; m_idle = 0; }

private:
    static constexpr int kMax = 4;
    static constexpr std::array<int, kMax + 1> kLadder = { 10, 15, 20, 24, 30 };
    int    m_upTicks;
    double m_high, m_low;
    int    m_idx  = 0;   // start at the 10 fps floor
    int    m_idle = 0;   // consecutive headroom ticks
};

} // namespace talq
