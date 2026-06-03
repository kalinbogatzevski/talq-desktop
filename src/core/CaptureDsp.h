#pragma once
#include <gst/gst.h>

// Capture-leg (send) WebRTC DSP configuration, split out of PublishPipeline so
// the EXACT production configuration can be exercised by talq-agc-test without
// standing up a full webrtcbin pipeline or a live call. The caller decides the
// two booleans (from QSettings / the TALQ_FORCE_AGC env override); this only
// applies them to the element's properties.
//
// Property names, ranges and the gain-control-mode enum were verified against
// the deployed webrtcdsp (gst-inspect): target-level-dbfs 0-31 (default 3),
// compression-gain-db 0-90 (default 9), gain-control-mode adaptive-digital(1)
// / fixed-digital(2). AGC values follow the 2026-05-28 call-quality spec.
namespace talq {

inline void configureCaptureDsp(GstElement *dsp, bool nsEnabled, bool agcEnabled,
                                bool aecEnabled = false,
                                const char *aecProbeName = "talq-aec-probe")
{
    if (!dsp)
        return;

    // echo-cancel (AEC). When enabled, the named webrtcechoprobe MUST already
    // exist in the process-global registry (brought up by SharedFarEndBus
    // BEFORE this publisher starts) — otherwise gst_webrtc_dsp_start fails
    // ("No echo probe found") and the call drops. The caller (CallManager)
    // guarantees that ordering and only passes aecEnabled=true once the bus is
    // PLAYING. delay-agnostic lets the APM self-estimate the cross-pipeline
    // capture↔playback delay (auto/harmless on AEC3 wap 1.x; load-bearing on
    // the legacy 0.3.x library). See docs/aec-design.md.
    if (aecEnabled) {
        g_object_set(dsp,
                     "echo-cancel",             TRUE,
                     "probe",                   aecProbeName,
                     "delay-agnostic",          TRUE,
                     "extended-filter",         TRUE,
                     "noise-suppression",       nsEnabled ? TRUE : FALSE,
                     "noise-suppression-level", 2,            // high
                     "high-pass-filter",        TRUE,          // voice-friendly rumble cut
                     "voice-detection",         FALSE,
                     nullptr);
    } else {
        // echo-cancel OFF — the safe default when no probe is available.
        g_object_set(dsp,
                     "echo-cancel",             FALSE,
                     "noise-suppression",       nsEnabled ? TRUE : FALSE,
                     "noise-suppression-level", 2,            // high
                     "high-pass-filter",        TRUE,          // voice-friendly rumble cut
                     "voice-detection",         FALSE,
                     nullptr);
    }

    if (agcEnabled) {
        // Bring quiet speakers up toward a consistent peak target without
        // clipping loud ones. adaptive-digital (the plugin default) ties the
        // gain to the signal envelope; target-level-dbfs=3 is a -3 dBFS peak
        // target (leaves headroom); compression-gain-db=15 caps the digital
        // boost; the hard limiter clamps anything that would exceed target.
        g_object_set(dsp,
                     "gain-control",        TRUE,
                     "target-level-dbfs",   3,
                     "compression-gain-db", 15,
                     "limiter",             TRUE,
                     nullptr);
        gst_util_set_object_arg(G_OBJECT(dsp),
                                "gain-control-mode", "adaptive-digital");
    } else {
        g_object_set(dsp, "gain-control", FALSE, nullptr);
    }
}

} // namespace talq
