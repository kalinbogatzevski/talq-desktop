// BweLayerGate.h — pure, header-only decision for whether a simulcast layer
// should be active under a fluctuating bandwidth estimate.
//
// Extracted from PublishPipeline so the ANTI-FLAP dwell logic is unit-testable
// without GStreamer/HW/network (see tests/bwe_layer_gate_test.cpp). The behavior
// is deliberately ASYMMETRIC:
//   * a layer DROPS the instant the estimate falls to/below its close threshold
//     (protect the link — congestion response must stay fast), but
//   * a layer is only RE-ADDED after the estimate has stayed above the re-open
//     threshold (close + hysteresis) continuously for reopenDwellMs.
// Without the dwell, an estimate hovering near a threshold makes a layer flip
// ACTIVE/MUTED every few seconds (each flip costs a keyframe + visible quality
// jump) — the "camera quality keeps changing, mostly down, rarely up" field
// report (Ilko, 2026-06-18, ~1.9 Mbps congested link).
#pragma once

namespace talq {

// Per-layer dwell state carried across ticks by the caller.
struct BweGateState {
    bool active = false;
    long long reopenSinceMs = -1;  // when the estimate first cleared re-open; -1 = not pending
};

// Decide a single layer's next active state.
//   s              persistent per-layer state (active + re-open dwell start)
//   estimateBps    current GCC bandwidth estimate
//   closeBps       drop-below threshold (the layer turns OFF at/below this)
//   hysteresisBps  added to closeBps to form the re-open threshold
//   ceilingAllows  tier / load / screen-share ceiling permits this layer at all
//   nowMs          monotonic clock (ms)
//   reopenDwellMs  estimate must stay above the re-open threshold this long
//                  before the layer is re-added
inline bool bweGateLayer(BweGateState &s, int estimateBps, int closeBps,
                         int hysteresisBps, bool ceilingAllows,
                         long long nowMs, long long reopenDwellMs)
{
    if (!ceilingAllows) {            // a ceiling veto forces the layer off now
        s.reopenSinceMs = -1;
        s.active = false;
        return false;
    }
    if (s.active) {
        // Drop fast: at/below the close threshold the layer goes off immediately.
        if (estimateBps <= closeBps) {
            s.reopenSinceMs = -1;
            s.active = false;
        }
        return s.active;
    }
    // Inactive: re-add only after a SUSTAINED estimate above the re-open level.
    const int reopenBps = closeBps + hysteresisBps;
    if (estimateBps <= reopenBps) {
        s.reopenSinceMs = -1;       // fell back below re-open — reset the dwell
        return false;
    }
    if (s.reopenSinceMs < 0)
        s.reopenSinceMs = nowMs;    // first tick above re-open — start the dwell
    if (nowMs - s.reopenSinceMs >= reopenDwellMs) {
        s.active = true;
        s.reopenSinceMs = -1;
    }
    return s.active;
}

} // namespace talq
