#pragma once

#include <QString>

// Single source of truth for the encode-load device-tier cap: maps the detected
// GPU acceleration tier (CallManager::gpuAccelStatus) to the camera SEND
// resolution ceiling + whether to shed the HIGH simulcast layer, so a publisher
// can't saturate a weak / iGPU / software encoder (the field freeze 2026-06-05:
// 3 simulcast layers up to 1080p starved audio + choked decode on an Intel-only
// laptop). Used by BOTH PublishPipeline::start() (to APPLY the cap) and the Home
// screen (to SHOW it) — keeping them in lockstep.
//
// Tiers:
//   "NVIDIA NVDEC" (discrete dGPU)         -> no cap, all 3 layers (full quality)
//   "Intel DXVA" / "DXVA (H264 only)"      -> cap 480p, keep 3 layers
//   "Software only" OR unknown/undetected  -> cap 480p AND shed HIGH (send l+m)
// The 480p iGPU ceiling matches the official Talk web client, whose own top
// quality tier is only 720x540@30 (it never sends 1080p, and drops fps as
// quality falls) -- so 480p@30 is a comfortably-within-spec send ceiling for a
// shared-media-engine iGPU. fps is NOT capped here: the shared caps stay 30.
// "When in doubt" (empty/unrecognised tier) is treated as the WEAKEST: if we
// can't confirm hardware acceleration, assume there is none.

namespace talq {

struct EncodeTierCap {
    int     maxSendHeight = 0;     // 0 = no cap (discrete GPU); else 720 / 480
    bool    shedHighLayer = false; // true = send l+m only (no HW accel)
    QString homeText;              // short Home-screen note ("" = no restriction)
};

inline EncodeTierCap encodeTierCap(const QString &gpuAccel)
{
    if (gpuAccel == QLatin1String("NVIDIA NVDEC"))
        return { 0, false, QString() };   // discrete dGPU: full quality, no note

    if (gpuAccel.isEmpty() || gpuAccel == QLatin1String("Software only"))
        return { 480, true,
            QStringLiteral("Camera video is sent at up to 480p — no hardware "
                           "video acceleration was detected on this device.") };

    // Intel iGPU (Intel DXVA / DXVA H264-only): keep 3 layers, cap resolution
    // to 480p (matches the official Talk web client's send ceiling; HW encode
    // handles three light 180/360/480 layers without saturating the engine).
    return { 480, false,
        QStringLiteral("Camera video is sent at up to 480p — this device uses "
                       "integrated graphics.") };
}

// Screen-share device-tier ceiling. A screen share adds a SECOND video encode
// on top of the camera's simulcast layers; on a weak / iGPU encoder the two
// together saturate the single fixed-function media engine (field freeze
// 2026-06-08: Intel UHD 620 software-fed 1440p30 share + 3 camera layers ->
// gradual lock-up). Mirrors the camera cap: clamp the screen capture height by
// tier. 0 = no clamp (discrete GPU keeps the user's quality choice).
inline int screenTierMaxHeight(const QString &gpuAccel)
{
    if (gpuAccel == QLatin1String("NVIDIA NVDEC"))
        return 0;                                  // discrete dGPU: no clamp
    if (gpuAccel.isEmpty() || gpuAccel == QLatin1String("Software only"))
        return 540;                                // no HW encode: 540p ceiling
    return 720;                                    // Intel iGPU: 720p (max HD)
}

} // namespace talq
