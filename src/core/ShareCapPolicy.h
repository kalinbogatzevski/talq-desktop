#pragma once

// Pure decision logic for the screen-share encode-resolution cap: quality
// level (0..3) -> pre-encode downscale cap fed to
// ScreenSharePipeline::setQualityCap(). No Qt, no GStreamer — unit-tested in
// tests/share_cap_policy_test.cpp. The SINGLE source of truth for BOTH the
// share build (buildAndStartSharePipeline) and the LIVE in-place re-cap
// (setScreenShareQuality): the two used to carry hand-copied duplicates, and
// the live path silently missed the software-encoder 1080p clamp.
//
// The cap is a CEILING: a smaller monitor frame passes through untouched, a
// larger one is downscaled to fit. The level applies to WINDOW shares as well
// as MONITOR shares, and the ceiling semantics already give a window its
// native resolution for free: a window smaller than the cap negotiates
// through videoscale untouched (Kalin 2026-06-04: "window app shares must be
// in native resolutions and only after that downscale the stream itself"). We
// used to force windowHandle != 0 to 3840x2160, which collapsed all four
// levels onto a ceiling no real window ever reaches — so the quality picker
// was a silent no-op for window shares while the pill, the persisted setting
// and the log all reported success (0.60.1 field report). Only a window
// LARGER than the chosen cap is downscaled, which is exactly what lowering
// the quality asks for; level 3 ("Native") remains the effectively-uncapped
// choice. This function is deliberately windowHandle-agnostic.

namespace talq {

struct ShareCap {
    int w = 1920;
    int h = 1080;
    // Which clamps actually bit — drives CallManager's per-clamp qInfo lines.
    // Mutually exclusive: the tier ceilings (720/540) sit below the software
    // clamp's 1080, so a tier-clamped frame never reaches the software clamp.
    bool tierClamped = false;   // weak/iGPU GPU-tier ceiling applied
    bool swClamped   = false;   // software-encoder 1080p ceiling applied
};

// Clamp a capture size DOWN to maxH, preserving aspect with an even width
// (encoders reject odd widths). maxH <= 0 = no clamp. A frame already at or
// below maxH passes through untouched, so re-applying a clamp to its own
// output never shrinks it further.
inline void clampShareHeightTo(int &w, int &h, int maxH)
{
    if (maxH <= 0 || h <= maxH) return;
    w = ((w * maxH) / h) & ~1;
    h = maxH;
}

// level:          0=720p 1=1080p 2=1440p 3=Native (out of range -> 1080p)
// tierMaxHeight:  talq::screenTierMaxHeight(gpuClass) — 720p iGPU / 540p
//                 software class / 0 = no clamp on a discrete GPU
// forceSwEncoder: THIS share runs the x264 software fallback (confirm-timeout
//                 retry; see #reshare-hw-collision in CallManager)
inline ShareCap screenShareCap(int level, int tierMaxHeight, bool forceSwEncoder)
{
    int cw = 1920, ch = 1080;
    switch (level) {
        case 0: cw = 1280; ch = 720;  break;
        case 1: cw = 1920; ch = 1080; break;
        case 2: cw = 2560; ch = 1440; break;
        case 3: cw = 3840; ch = 2160; break;   // "High" = up to 4K
        default: break;
    }
    // Hard 4K ceiling. The hardware H264 encoder (Intel QSV / qsvh264enc)
    // tops out around 4K (4096x2304); a native 8K monitor frame produced
    // ZERO encoded output, so the share's outbound RTP never confirmed and
    // it silently failed + wedged the subsystem (field report 2026-06-01).
    // Clamp every tier so a big/8K monitor downscales instead of feeding the
    // encoder a resolution it cannot handle.
    if (cw > 3840) cw = 3840;
    if (ch > 2160) ch = 2160;

    ShareCap cap;
    // Weak/iGPU encoder: cap to the GPU tier's ceiling (720p/540p) so the
    // share can't saturate a shared/iGPU/software media engine (EncodeTier.h).
    if (tierMaxHeight > 0 && ch > tierMaxHeight) {
        clampShareHeightTo(cw, ch, tierMaxHeight);
        cap.tierClamped = true;
    }
    // Software-fallback shares cap at 1080p: realtime x264 above that
    // (1440p / native-res window shares up to 4K) costs more CPU than a
    // laptop can hide mid-call. 1080p keeps slides/code readable, and it
    // applies only to THIS share — the next hardware share restores the
    // user's cap.
    if (forceSwEncoder && ch > 1080) {
        clampShareHeightTo(cw, ch, 1080);
        cap.swClamped = true;
    }
    cap.w = cw;
    cap.h = ch;
    return cap;
}

// Effective share quality for ONE share, from the user's stored quality level
// and whether Presentation mode was ticked for this share.
//
// Default (presentation=false) deliberately clamps to 1080p/15. The share
// default used to be level 2 (1440p) at 30 fps while the CAMERA defaulted to
// 720p. That asymmetry starved a full-screen 2K scroll of bits at the ~12 Mbps
// ceiling and showed up as smearing (field 2026-07-28) — the artifacts were bit
// starvation, not latency. Static content (slides, code, documents) is
// indistinguishable at 15 fps and gets roughly twice the bits per frame, which
// is exactly what a scroll needs.
//
// The stored level is NEVER rewritten by the toggle: Presentation lifts the
// clamp for this share only, so the user's preference survives it.
struct ShareQuality {
    int level;   // effective level to feed screenShareCap()
    int fps;     // capture + encode framerate
};

inline ShareQuality shareQualityFor(int storedLevel, bool presentation)
{
    int lvl = storedLevel;
    if (lvl < 0) lvl = 0;
    if (lvl > 3) lvl = 3;
    if (presentation)
        return ShareQuality{ lvl, 30 };
    return ShareQuality{ lvl < 1 ? lvl : 1, 15 };
}

} // namespace talq
