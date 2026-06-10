#pragma once

#include <QString>
#include <QStringList>

// Single source of truth for the encode-load device caps. The camera SEND
// resolution + screen-share resolution are capped on weak encoders so a publisher
// can't saturate a shared/iGPU/software media engine (the field freeze 2026-06-05:
// 3 camera simulcast layers + a 1440p screen on a single fixed-function engine).
//
// The cap follows ENCODE capability, classified into GpuClass:
//   Capable  -- discrete dGPU OR a capable iGPU (Intel Iris/Iris Xe/Arc, AMD
//               Radeon, NVIDIA GeForce/RTX/Quadro): NO cap, native screen share.
//   WeakIgpu -- a weak integrated GPU WITH hardware encode (Intel UHD/HD 6xx, or
//               any unrecognised HW-encode GPU): camera 480p, screen 720p, 3 layers.
//   Software -- NO hardware H.264 encoder at all (x264/openh264 only): camera 480p
//               AND shed the HIGH layer (send l+m), screen 540p.
//
// IMPORTANT: this is classified from the GPU ENCODER capability + GPU MODEL NAME,
// NOT from which video DECODER factory exists (the old proxy mislabelled capable
// iGPUs like Iris Xe as weak and throttled them to 480p). Screen-share suppression
// of the camera simulcast (camera -> single LOW layer while sharing) applies to
// ALL classes regardless, so even a Capable box sends native screen + ONE low
// camera, never 3 camera layers + screen. The 480p iGPU ceiling matches the
// official Talk web client's 720x540@30 send tier.

namespace talq {

enum class GpuClass {
    Capable,    // no cap, native screen share
    WeakIgpu,   // HW encode but weak iGPU -> protective caps
    Software,   // no HW encode -> hardest cap + shed HIGH
};

// User override (Settings -> Video/gpuClassOverride). Auto wins to the classifier.
enum class GpuClassOverride { Auto = 0, AlwaysFull = 1, AlwaysProtected = 2 };

// Classify ENCODE capability from: whether a hardware H.264 encoder exists, the
// GPU adapter model name(s) (talq::gpuAdapterNames()), and the user override.
inline GpuClass gpuClassFromSignals(bool hwH264Encoder,
                                    const QStringList &adapterNames,
                                    GpuClassOverride override)
{
    if (override == GpuClassOverride::AlwaysFull)      return GpuClass::Capable;
    if (override == GpuClassOverride::AlwaysProtected) return GpuClass::WeakIgpu;

    if (!hwH264Encoder)
        return GpuClass::Software;   // x264/openh264 only -- protect hardest

    // HW encode present: capable vs weak by GPU model. A Capable match on ANY
    // enumerated adapter wins (hybrid iGPU + dGPU laptops use the better one; the
    // manual override corrects the rare mismatch). An unrecognised HW-encode GPU
    // stays WeakIgpu -- conservative "when in doubt, protect" (override to lift).
    static const char *kCapable[] = {
        "iris", "arc", "radeon", "rx ", "vega",
        "geforce", "rtx", "gtx", "quadro", "titan",
    };
    for (const QString &raw : adapterNames) {
        const QString n = raw.toLower();
        for (const char *p : kCapable)
            if (n.contains(QLatin1String(p)))
                return GpuClass::Capable;
    }
    return GpuClass::WeakIgpu;
}

inline QString gpuClassName(GpuClass cls)
{
    switch (cls) {
    case GpuClass::Capable:  return QStringLiteral("Capable");
    case GpuClass::WeakIgpu: return QStringLiteral("WeakIgpu");
    case GpuClass::Software: return QStringLiteral("Software");
    }
    return QStringLiteral("Software");
}

struct EncodeTierCap {
    int     maxSendHeight = 0;     // 0 = no cap (Capable); else 480
    bool    shedHighLayer = false; // true = send l+m only (no HW encode)
    QString homeText;              // short Home-screen note ("" = no restriction)
};

inline EncodeTierCap encodeTierCap(GpuClass cls)
{
    switch (cls) {
    case GpuClass::Capable:
        return { 0, false, QString() };   // full quality, no note
    case GpuClass::WeakIgpu:
        return { 480, false,
            QStringLiteral("Camera video is sent at up to 480p — this device uses "
                           "integrated graphics.") };
    case GpuClass::Software:
        break;
    }
    return { 480, true,
        QStringLiteral("Camera video is sent at up to 480p — no hardware video "
                       "acceleration was detected on this device.") };
}

// Screen-share capture height ceiling by class. 0 = no clamp (native).
inline int screenTierMaxHeight(GpuClass cls)
{
    switch (cls) {
    case GpuClass::Capable:  return 0;     // native, no clamp
    case GpuClass::WeakIgpu: return 720;   // Intel iGPU: 720p (max HD)
    case GpuClass::Software: break;
    }
    return 540;                            // no HW encode: 540p ceiling
}

} // namespace talq
