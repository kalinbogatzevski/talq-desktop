#pragma once

// Shared WebRTC video-encoder selection for the publisher pipelines
// (camera = PublishPipeline, screen = ScreenSharePipeline). One place so
// the two paths never diverge: prefer HARDWARE H264 (NVIDIA NVENC → Intel
// QuickSync → MediaFoundation), then software x264, then software VP8 as
// a last resort so a call still works on a box with no H264 encoder.
// All configured for low-latency realtime CBR. The `screen` flag tunes
// GOP for mostly-static screen content. For H264 it also creates the
// h264parse the RTP payloader needs (SPS/PPS repeated for late SFU
// subscribers). `bitrateBps` is the start bitrate; rtpgccbwe drives the
// live rate at runtime. Returns the encoder (floating ref; caller adds
// to the bin) or nullptr on total failure.

#include <QString>
#include <gst/gst.h>

// Transport-Wide Congestion Control. webrtcbin builds the offer's
// a=extmap lines from the transceiver codec-preferences caps, so the
// SAME id must be (a) put in those caps as `extmap-<id>` and (b) set on
// the rtphdrexttwcc added to the external payloader, or the two disagree
// and Janus never sends transport-wide RTCP feedback (rtpgccbwe starves —
// the 0.29.3 black-video regression). Shared here so camera and screen
// negotiate TWCC identically. Low ids (1-4) are left free for the MID
// extension webrtcbin auto-adds under max-bundle.
static constexpr int kTwccExtId = 5;
static constexpr char kTwccUri[] =
    "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01";

// Live bitrate update driven by rtpgccbwe's estimate. `bps` is bits/s.
// H264 encoders (nv/qsv/mf/x264) take `bitrate` in kbit/s; vp8enc takes
// `target-bitrate` in bit/s. Property presence is probed so this is safe
// across the whole encoder fallback ladder. Called from both publisher
// paths' notify::estimated-bitrate handler so they never diverge.
inline void setWebrtcVideoEncoderBitrate(GstElement *enc, bool h264, guint bps)
{
    if (!enc || bps == 0) return;
    auto has = [enc](const char *p) {
        return g_object_class_find_property(G_OBJECT_GET_CLASS(enc), p) != nullptr;
    };
    if (h264) {
        const guint kbps = (bps / 1000) ? (bps / 1000) : 1u;
        if (has("bitrate")) g_object_set(enc, "bitrate", kbps, nullptr);
    } else if (has("target-bitrate")) {
        g_object_set(enc, "target-bitrate", (gint)bps, nullptr);
    }
}

inline GstElement *makeWebrtcVideoEncoder(bool screen, int bitrateBps,
                                          bool *outUseH264,
                                          GstElement **outParser,
                                          QString *outDesc)
{
    auto setIfExists = [](GstElement *e, const char *prop, auto value) {
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(e), prop))
            g_object_set(e, prop, value, nullptr);
    };
    // Enum/flags integer values vary across GStreamer versions — set them
    // by nick string (version-robust) and guard on property existence.
    auto setArg = [](GstElement *e, const char *prop, const char *nick) {
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(e), prop))
            gst_util_set_object_arg(G_OBJECT(e), prop, nick);
    };
    const guint kbps = (guint)(bitrateBps / 1000);
    const guint gop  = screen ? 120u : 60u;

    const char *order[] = { "nvh264enc", "qsvh264enc", "mfh264enc",
                            "x264enc", nullptr };
    // Camera = CBR (stable rate for continuous motion video). Screen =
    // VBR: mostly-static content costs almost nothing, and the encoder
    // bursts up to the peak only for detail/motion/scrolling. `kbps` is
    // the target; for screen the peak is allowed up to the full ceiling.
    const char *rc       = screen ? "vbr" : "cbr";
    const guint peakKbps = kbps;                       /* configured ceiling */
    const guint avgKbps  = screen ? (kbps / 3 ? kbps / 3 : 1) : kbps;
    for (int i = 0; order[i]; ++i) {
        GstElement *enc = gst_element_factory_make(order[i], nullptr);
        if (!enc) continue;
        const bool hw = g_strcmp0(order[i], "x264enc") != 0;
        if (!g_strcmp0(order[i], "nvh264enc")) {
            setArg(enc, "preset", "low-latency-hq");
            setArg(enc, "rc-mode", rc);
            setIfExists(enc, "bitrate", avgKbps);       /* kbit/s */
            setIfExists(enc, "max-bitrate", peakKbps);
            setIfExists(enc, "gop-size", (gint)gop);
            setIfExists(enc, "bframes", 0u);
            setIfExists(enc, "b-frames", 0u);           /* older name */
            setIfExists(enc, "zerolatency", TRUE);
        } else if (!g_strcmp0(order[i], "qsvh264enc")) {
            setArg(enc, "rate-control", rc);
            setIfExists(enc, "low-latency", TRUE);
            setIfExists(enc, "bitrate", avgKbps);
            setIfExists(enc, "max-bitrate", peakKbps);
            setIfExists(enc, "gop-size", gop);
            setIfExists(enc, "b-frames", 0u);
            setIfExists(enc, "target-usage", 4u);       /* balanced */
        } else if (!g_strcmp0(order[i], "mfh264enc")) {
            setArg(enc, "rc-mode", rc);
            setIfExists(enc, "low-latency", TRUE);
            setIfExists(enc, "bitrate", avgKbps);
            setIfExists(enc, "max-bitrate", peakKbps);
            setIfExists(enc, "gop-size", (gint)gop);
            setIfExists(enc, "bframes", 0u);
            // mfh264enc's rc-mode is "conditionally available" and defaults
            // to uvbr (Unconstrained VBR). On some MF hardware the property
            // is absent / doesn't stick, leaving the camera encoder in
            // UVBR: it holds image quality and collapses the frame rate,
            // ignoring the bitrate entirely (the confirmed "perfect image,
            // ~1 fps, ~300 kbps while GCC is asking for 2.5-4 Mbps" bug).
            // A realtime camera needs a rate-targeting mode; if this MFT
            // cannot be constrained, reject mfh264enc and fall through to
            // x264enc (software, reliably CBR, cheap at 720p30). Screen
            // content is fine in UVBR, so only gate the camera path.
            if (!screen) {
                gint rcm = 2 /* uvbr */;
                if (g_object_class_find_property(
                        G_OBJECT_GET_CLASS(enc), "rc-mode"))
                    g_object_get(enc, "rc-mode", &rcm, nullptr);
                if (rcm != 0 /* cbr */ && rcm != 1 /* pcvbr */) {
                    qWarning() << "VideoEncoder: mfh264enc stuck in UVBR on "
                                  "this hardware (cannot rate-target) — "
                                  "using software x264enc for the camera";
                    gst_object_unref(enc);
                    continue;
                }
            }
        } else { /* x264enc — software fallback */
            setArg(enc, "tune", "zerolatency");
            setArg(enc, "speed-preset", "veryfast");
            setIfExists(enc, "bitrate", avgKbps);
            setIfExists(enc, "key-int-max", gop);
            setIfExists(enc, "bframes", 0);
            setIfExists(enc, "byte-stream", FALSE);
        }
        GstElement *parse = gst_element_factory_make("h264parse", nullptr);
        if (!parse) { gst_object_unref(enc); continue; }
        *outParser = parse;
        *outUseH264 = true;
        if (outDesc)
            *outDesc = QStringLiteral("H264 · %1 · %2")
                           .arg(QString::fromUtf8(order[i]),
                                hw ? QStringLiteral("hw") : QStringLiteral("sw"));
        qInfo().nospace() << "VideoEncoder: " << order[i]
                          << (hw ? " [hw]" : " [sw]") << " H264, " << kbps
                          << " kbps start, gop " << gop;
        return enc;
    }

    GstElement *vp8 = gst_element_factory_make("vp8enc", nullptr);
    if (vp8) {
        g_object_set(vp8, "deadline", (gint64)1, "target-bitrate", bitrateBps,
                     "cpu-used", 6, "threads", 4,
                     "end-usage", screen ? 0 /* VBR */ : 1 /* CBR */,
                     "keyframe-max-dist", screen ? 120 : 30,
                     "error-resilient", 1, nullptr);
        *outUseH264 = false;
        *outParser  = nullptr;
        if (outDesc) *outDesc = QStringLiteral("VP8 · vp8enc · sw");
        qWarning() << "VideoEncoder: no H264 encoder available — "
                      "VP8 software fallback";
    }
    return vp8;
}
