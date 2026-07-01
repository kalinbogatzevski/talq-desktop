#pragma once

#include <cstdint>

// BweAutoCap — pure decision logic for the publisher's sustained-low BWE
// auto-cap (PublishPipeline::onGccBitrate). Extracted so it can be unit-tested
// with injected timestamps (the real path uses a monotonic clock).
//
// Behaviour:
//   - Evaluate the RAW (un-clamped) GCC estimate. If it stays below
//     originalMax/2 for kLowMs, latch: cap the publisher max to estimate*1.2.
//   - Once capped, RESTORE to originalMax only after the RAW estimate holds
//     above originalMax*0.7 for kRestoreMs (a dwell, so a flapping link can't
//     restore→re-saturate→re-cap on a loop).
//
// THE BUG this guards against (0.52.3): the old inline version evaluated the
// CLAMPED estimate. Once capped, every sample was clamped to the low max, so it
// could never exceed the restore threshold → the cap latched for the whole call
// even after the link recovered (field: 1080p → low → stuck low to a distant
// peer). Evaluating the RAW estimate is what makes restore reachable. This helper
// + bwe_auto_cap_test pin the restore LOGIC itself; the un-clamped WIRING at the
// onGccBitrate call site (passing rawBps, captured before the clamp) is verified
// by build + review, not by this unit test.

namespace talq {

class BweAutoCap {
public:
    // kLowMs / kRestoreMs default to production; the ctor lets tests pin them.
    explicit BweAutoCap(std::int64_t lowMs = 15000, std::int64_t restoreMs = 4000)
        : m_lowMs(lowMs), m_restoreMs(restoreMs) {}

    // Feed the RAW estimate (bps) + the originally-chosen ceiling (bps) + a
    // monotonic timestamp (ms). Returns the max bitrate to apply: the capped
    // value while latched, else originalMax.
    int onTick(int rawBps, int originalMax, std::int64_t nowMs) {
        if (originalMax <= 0) return originalMax;
        const int lo = originalMax / 2;
        const int hi = originalMax * 7 / 10;
        if (rawBps < lo) {
            m_highSinceMs = -1;                       // not recovering
            if (m_lowSinceMs < 0) m_lowSinceMs = nowMs;
            if (!m_active && nowMs - m_lowSinceMs > m_lowMs) {
                m_active = true;
                m_capped = rawBps * 6 / 5;
            }
        } else {
            m_lowSinceMs = -1;
            if (m_active && rawBps > hi) {
                if (m_highSinceMs < 0) m_highSinceMs = nowMs;
                if (nowMs - m_highSinceMs > m_restoreMs) {
                    m_active = false;                 // restore
                    m_highSinceMs = -1;
                }
            } else {
                m_highSinceMs = -1;                   // dipped back below hi → reset dwell
            }
        }
        return m_active ? m_capped : originalMax;
    }

    bool active() const { return m_active; }
    void reset() { m_active = false; m_lowSinceMs = m_highSinceMs = -1; m_capped = 0; }

private:
    std::int64_t m_lowMs, m_restoreMs;
    bool         m_active     = false;
    std::int64_t m_lowSinceMs = -1;
    std::int64_t m_highSinceMs = -1;
    int          m_capped     = 0;
};

} // namespace talq
