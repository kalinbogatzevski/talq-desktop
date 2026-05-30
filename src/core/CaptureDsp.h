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

inline void configureCaptureDsp(GstElement *dsp, bool nsEnabled, bool agcEnabled)
{
    if (!dsp)
        return;

    // echo-cancel stays OFF: webrtcdsp's AEC needs a webrtcechoprobe present at
    // pipeline start, but the only far-end tap (SubscribeWebrtcSrc playback)
    // does not exist until a subscriber connects. echo-cancel=TRUE therefore
    // made gst_webrtc_dsp_start fail and dropped every call. AEC needs a
    // separate shared-probe design (tracked elsewhere).
    g_object_set(dsp,
                 "echo-cancel",             FALSE,
                 "noise-suppression",       nsEnabled ? TRUE : FALSE,
                 "noise-suppression-level", 2,            // high
                 "high-pass-filter",        TRUE,          // voice-friendly rumble cut
                 "voice-detection",         FALSE,
                 nullptr);

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
