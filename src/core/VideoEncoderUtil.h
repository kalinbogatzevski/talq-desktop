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
// live rate at runtime. `forceSoftware` skips the hardware ladder for
// THIS encoder only (screen re-share HW-session-collision fallback)
// without touching the process-global sticky latches below. Returns the
// encoder (floating ref; caller adds to the bin) or nullptr on total
// failure.

#include <QString>
#include <QDebug>
#include <atomic>
#include <gst/gst.h>

// Runtime latch: "NVENC can't open a session on THIS machine." Some 2-GPU
// (Optimus) laptops register nvh264enc, but the process — pinned to the Intel
// iGPU — can't open an NVENC session (CUDA_ERROR_NO_DEVICE), so it dies the
// instant the camera starts and used to drop the whole call. We can't reliably
// PREDICT this (a throwaway-pipeline probe can't replicate the real GPU-memory
// upload path nvenc needs), so we DETECT it from the real pipeline's failure and
// set this latch; makeWebrtcVideoEncoder then skips nvh264enc and uses Intel QSV
// (or software x264). CallManager persists it to QSettings Video/avoidNvenc so a
// machine that fails once never tries NVENC again. Atomic: read on the GStreamer
// build thread, set from the bus handler / loaded on the Qt thread.
inline std::atomic<bool> &talqAvoidNvenc()
{
    static std::atomic<bool> v{false};
    return v;
}

// Second-stage latch: "no hardware H264 encoder works here — use software
// x264." Set only if the call ALREADY skipped NVENC and the next hardware
// encoder (Intel QSV / MediaFoundation) ALSO failed to open. x264 is software
// and always works, so this is the guaranteed floor that gets video back on a
// machine where every hardware encoder is unusable. Also persisted by
// CallManager (QSettings Video/forceSoftwareVideo).
inline std::atomic<bool> &talqForceSoftwareVideo()
{
    static std::atomic<bool> v{false};
    return v;
}

// B1/B4 (0.53.x robustness) — force software video DECODE on this machine. The
// receive-side mirror of talqForceSoftwareVideo(): some GPUs expose a HW H264
// decoder (d3d11/nvdec/qsv) whose video-device interface is broken
// (E_NOINTERFACE 0x80004002, "Interface not supported") — it builds and even
// emits frames but drops them constantly, with NO fallback (unlike the encode
// ladder). When detected we set this latch, demote the HW decoder factory ranks
// so decodebin/decodebin3 re-plug the software decoder, and persist to QSettings
// Video/forceSoftwareDecode (restored at startup). Process-global + sticky by
// design ("detect once, demote, persist"). Atomic: set from a GStreamer thread.
inline std::atomic<bool> &talqForceSoftwareDecode()
{
    static std::atomic<bool> v{false};
    return v;
}

// B4 — armed by the GStreamer log probe (main.cpp) the instant the d3d11
// video-device-interface failure is seen. The actual demote + persist + subscriber
// rebuild is done on the Qt thread (CallManager::onLoadTick) since registry
// mutation and pipeline rebuilds must not run on a streaming thread.
inline std::atomic<bool> &talqHwDecodeFaultDetected()
{
    static std::atomic<bool> v{false};
    return v;
}

// Demote every hardware H264/VP8/VP9 decoder factory to GST_RANK_NONE so
// autoplugging (decodebin / webrtcsrc's internal decodebin3) selects the
// software decoder (avdec_h264 / openh264dec / vp8dec). Idempotent; call AFTER
// gst_init (needs the registry loaded). Process-global and sticky by design.
inline void talqDemoteHwVideoDecoders()
{
    // SAFETY: only demote if a SOFTWARE decoder exists to take over — otherwise
    // decodebin would be left with no H264 decoder at all (worse than a flaky HW
    // one). Bump the software decoder to PRIMARY+ so autoplug definitely picks it.
    GstElementFactory *sw = gst_element_factory_find("avdec_h264");
    if (!sw) sw = gst_element_factory_find("openh264dec");
    if (!sw) {
        qWarning() << "VideoDecoder: HW decode faulty but NO software H264 decoder present — keeping HW";
        return;
    }
    gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(sw), GST_RANK_PRIMARY + 1);
    gst_object_unref(sw);

    static const char *hwDecoders[] = {
        "d3d11h264dec", "d3d11vp8dec", "d3d11vp9dec",
        "nvh264dec", "nvh264sldec", "nvvp8dec", "nvvp9dec",
        "qsvh264dec", "qsvvp8dec", "qsvvp9dec",
        "msdkh264dec", nullptr
    };
    for (int i = 0; hwDecoders[i]; ++i) {
        if (GstElementFactory *f = gst_element_factory_find(hwDecoders[i])) {
            gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(f), GST_RANK_NONE);
            gst_object_unref(f);
        }
    }
    qWarning() << "VideoDecoder: demoted hardware H264/VP8 decoders -> software";
}

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
                                          QString *outDesc,
                                          bool *outIsSoftware = nullptr,
                                          bool forceSoftware = false)
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
    // Camera GOP: 30 frames (~1 s at 30 fps) instead of 60 (~2 s).
    // Tied to the deterministic caller-side chop: a fresh subscribe
    // against a just-up publisher reliably eats an initial-burst packet
    // loss, and the receiver's decoder concealment continues until the
    // next I-frame arrives. At GOP=60 that's a ~2 s blocky window the
    // user reads as persistent chop; the callee, subscribing against a
    // mature publisher, doesn't see the burst at all → asymmetric.
    // GOP=30 caps the worst-case recovery at ~1 s (and PLI on the
    // receive side cuts it further). Screen stays at GOP=120 — its
    // mostly-static content benefits from long GOPs and isn't subject
    // to the same burst-loss-on-subscribe condition.
    const guint gop  = screen ? 120u : 30u;

    const char *order[] = { "nvh264enc", "qsvh264enc", "mfh264enc",
                            "x264enc", nullptr };
    // Camera = CBR (stable rate for continuous motion video). Screen =
    // VBR: mostly-static content costs almost nothing, and the encoder
    // bursts up to the peak only for detail/motion/scrolling. `kbps` is
    // the target; for screen the peak is allowed up to the full ceiling.
    const char *rc       = screen ? "vbr" : "cbr";
    const guint peakKbps = kbps;                       /* configured ceiling */
    const guint avgKbps  = screen ? (kbps / 3 ? kbps / 3 : 1) : kbps;
    // #android-static A/B (2026-06-02): force the software x264enc -- the
    // documented known-good encoder for browser/libwebrtc H264 interop -- to test
    // whether the HW (qsv) bitstream is what strict decoders snow on.
    // `forceSoftware` is the per-pipeline caller request (screen re-share
    // HW-session-collision fallback) — one build only, never persisted.
    const bool forceX264 = qEnvironmentVariableIsSet("TALQ_TEST_X264")
                           || talqForceSoftwareVideo().load()
                           || forceSoftware;
    for (int i = 0; order[i]; ++i) {
        if (forceX264 && g_strcmp0(order[i], "x264enc") != 0) continue;
        // Skip NVENC entirely once it has been shown to be unusable on this
        // machine (a 2-GPU/Optimus laptop where the process can't open an NVENC
        // session). Without this, every call would pick nvh264enc, fail at
        // camera-on, and have to recover — this latch makes it go straight to
        // Intel QSV / software. Latched by CallManager on the first failure and
        // restored from QSettings on startup.
        if (!g_strcmp0(order[i], "nvh264enc") && talqAvoidNvenc().load())
            continue;
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
            // Screen = TU1 (best quality): fine text/UI detail is what a share
            // is FOR, the content is mostly static (low effective fps) so QSV
            // has ample headroom — TU4 "balanced" visibly mushed small fonts at
            // a native-res share with a healthy 8-11 Mbps rate (Kalin field
            // 2026-07-08). Camera stays TU4: continuous 30 fps + simulcast.
            setIfExists(enc, "target-usage", screen ? 1u : 4u);
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
                // NOTE: a prior revision rejected mfh264enc here when it
                // could not prove bframes==0, falling back to software
                // x264enc. That rested on a B-frame hypothesis later
                // DISPROVEN — a controlled real-MCU reproduction with
                // B-frames did NOT reproduce the symptom, and the affected
                // peer's stream was baseline yet still choppy. The fallback
                // also forced capable hardware onto software x264enc that a
                // modest CPU cannot sustain at 720p30, itself producing a
                // low-fps stream. So: keep the HARDWARE encoder. bframes=0
                // remains best-effort (setIfExists above) but is no longer a
                // reason to abandon hardware encoding.
            }
        } else { /* x264enc — software fallback */
            // 0.41.0-beta — psy-tune is the single biggest visible-quality
            // delta at the same bitrate. `film` (= --psy-rd 1.0:0.15) gives
            // sharper motion edges for camera; `animation` is the OBS
            // community recipe for screen content (preserves text edges +
            // handles scroll-jumps better than `stillimage` while staying
            // compatible with `tune=zerolatency`). qp-min keeps "easy"
            // frames from being over-quantised below visible loss; qp-max
            // caps the upper bound on hard frames.
            setArg(enc, "tune", "zerolatency");
            setArg(enc, "speed-preset", "veryfast");
            setArg(enc, "psy-tune", screen ? "animation" : "film");
            setIfExists(enc, "bitrate", avgKbps);
            setIfExists(enc, "key-int-max", gop);
            setIfExists(enc, "bframes", 0);
            setIfExists(enc, "byte-stream", FALSE);
            setIfExists(enc, "qp-min", screen ? 14u : 18u);
            setIfExists(enc, "qp-max", 42u);
            setIfExists(enc, "vbv-buf-capacity", 1000u);   // ms
            // NO intra-refresh. It emits a rolling intra-refresh band INSTEAD
            // of IDR keyframes — but every SFU (Janus) subscriber that joins
            // mid-stream (each screen re-share makes a fresh subscriber) needs
            // a real IDR to begin decoding, and the SFU asks for one via PLI.
            // With intra-refresh the new subscriber decoded nothing but GREEN
            // until (if ever) the band swept the frame — the "green garbage"
            // on a SOFTWARE screen re-share (Kalin dev-build 2026-07-08). The
            // HW encoders (qsv/mf) never set it, so hardware shares were clean.
            // key-int-max (above) + PLI-forced IDRs give decodable keyframes.
        }
        GstElement *parse = gst_element_factory_make("h264parse", nullptr);
        if (!parse) { gst_object_unref(enc); continue; }
        *outParser = parse;
        *outUseH264 = true;
        if (outIsSoftware) *outIsSoftware = !hw;   // x264enc is the only software H264 here
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
        if (outIsSoftware) *outIsSoftware = true;   // vp8 software fallback
        if (outDesc) *outDesc = QStringLiteral("VP8 · vp8enc · sw");
        qWarning() << "VideoEncoder: no H264 encoder available — "
                      "VP8 software fallback";
    }
    return vp8;
}
