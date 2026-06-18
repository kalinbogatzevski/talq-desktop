#pragma once

// SubstreamPolicy — the rule that maps "how big is this remote tile" to
// "which simulcast layer should we ask the SFU to forward". Pure function,
// no Qt deps (takes a plain double for height), so the harness / unit-test
// target can include it without dragging in QWidget / CallManager.
//
// Layer numbering matches PublishPipeline's m_layers:
//   0 = LOW  / 180p
//   1 = MED  / 360p
//   2 = HIGH / 720p
//
// Thresholds match upstream spreed's tile-size policy and the prior
// inline logic in CallStage::updateStreamQualities:
//   stage tile OR tile height >= 480px  -> HIGH
//   tile height >= 240px                -> MED
//   smaller                             -> LOW
//
// The manual override (-1 = auto, 0/1/2 = forced) from the #8 Quality
// chip wins over tile size — the user's pick is sticky for as long as
// the chip is set to anything but Auto.

inline int pickSubstream(int qualityOverride, double tileHeight, bool isStage)
{
    if (qualityOverride >= 0 && qualityOverride <= 2)
        return qualityOverride;
    if (isStage || tileHeight >= 480.0) return 2;
    if (tileHeight >= 240.0)            return 1;
    return 0;
}

// Hysteretic variant of the AUTO selection. Same UP thresholds (480 -> HIGH,
// 240 -> MED), but once a tile is at a layer it does NOT drop to a lower one
// until the tile shrinks past a ~10% band below the boundary. A tile parked on
// a threshold (a window drag, a grid reflow, a 1px relayout jitter) would
// otherwise flip the requested substream on every relayout — and each flip is a
// real cost: the SFU switches the forwarded layer and the encoder must emit a
// keyframe, so the remote video visibly hitches. Raising is immediate (the user
// grew the tile, they want more detail now); only the DROP is damped.
//   `prev` = the layer last chosen for this tile (-1 = none yet / first pick).
// The manual override still wins outright (sticky user pick), same as above.
inline int pickSubstreamHysteretic(int qualityOverride, double tileHeight,
                                   bool isStage, int prev)
{
    if (qualityOverride >= 0 && qualityOverride <= 2) return qualityOverride;
    if (isStage) return 2;
    int up;
    if      (tileHeight >= 480.0) up = 2;
    else if (tileHeight >= 240.0) up = 1;
    else                          up = 0;
    if (prev < 0 || up >= prev) return up;            // first pick, or rising
    // Falling: hold the current (higher) layer until well past the boundary.
    if (prev == 2 && tileHeight >= 432.0) return 2;   // 480 - 10%
    if (prev == 1 && tileHeight >= 216.0) return 1;   // 240 - 10%
    return up;
}
