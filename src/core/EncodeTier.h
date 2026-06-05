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
//   "Intel DXVA" / "DXVA (H264 only)"      -> cap 720p, keep 3 layers
//   "Software only" OR unknown/undetected  -> cap 480p AND shed HIGH (send l+m)
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

    // Intel iGPU (Intel DXVA / DXVA H264-only): keep 3 layers, cap resolution.
    return { 720, false,
        QStringLiteral("Camera video is sent at up to 720p — this device uses "
                       "integrated graphics.") };
}

} // namespace talq
