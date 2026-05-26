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
