#pragma once

// 0.61.0 "Blue Fiesta Week 2" — the single-stream target for a weak encode tier.
// Pure data, no Qt/GStreamer, so it is unit-tested in isolation. A weak box (no
// HW H.264 encoder, an older iGPU, or a low-core CPU) publishes ONE stream at
// this target instead of the 3-layer simulcast ladder, so it never runs multiple
// encoders. HD-off = 360p (the default); HD-on = 480p (reuses the existing 480
// bitrate bucket from PublishPipeline's ladder). Bitrate ~ 0.10 bpp @ 30 fps.
// fps starts LOW (10) and the CallManager tick ramps it UP into headroom.

namespace talq {

struct SoloVideoTarget {
    int w, h;
    int nominalBitrate;   // encoder target (bits/s)
    int initBitrate;      // GCC start
    int maxBitrate;       // GCC ceiling
    int startFps;         // first-frame send fps (ramps up from here)
    int minFps;           // ramp floor
    int maxFps;           // ramp ceiling
};

inline SoloVideoTarget soloVideoTarget(bool hdEnabled)
{
    if (hdEnabled)
        return { 852, 480, 1'200'000, 1'200'000, 2'000'000, 10, 10, 30 };
    return     { 640, 360,   700'000,   700'000, 1'200'000, 10, 10, 30 };
}

} // namespace talq
