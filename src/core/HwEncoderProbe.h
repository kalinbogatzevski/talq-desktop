#pragma once

// Feature #74 — cached OUT-OF-PROCESS hardware-encoder probe (RustDesk pattern).
//
// WHY: hardware H.264 encoders (nvh264enc / qsvh264enc / mfh264enc) sometimes
// take ~30-60s to (re)initialise, or collide on a rapid screen re-share, and
// stall video while the live pipeline blocks on encoder discovery. RustDesk
// avoids this by probing encoders in a CHILD PROCESS and caching the working
// ladder keyed by GPU signature, so the live path never blocks on encoder
// discovery (hwcodec.rs: start_check_process / check_available_hwcodec, cached
// by GPU signature; re-probed only on GPU change). This is the TalQ port.
//
// SHAPE:
//   • A short-lived child (`talq.exe --probe-hwcodec`) instantiates each hardware
//     encoder in a real 1280x720 pipeline, records which reach PLAYING, and
//     persists the verdict to QSettings keyed by a GPU signature.
//   • The live encoder selector (makeWebrtcVideoEncoder) reads that cache and
//     skips ONLY the hardware encoders a prior probe proved non-working on this
//     exact GPU. Everything else is attempted exactly as before.
//
// FAIL-OPEN CONTRACT (this ships to the fleet at the 0.59.0 beta — a wrong probe
// must NEVER break video):
//   • The cache can only ever REMOVE a KNOWN-BROKEN hardware encoder from the
//     ladder. It never restricts the ladder by default.
//   • x264enc (software) is NEVER a probe candidate and is NEVER excluded — the
//     ultimate software fallback is untouched.
//   • Missing / stale / unreadable / disabled cache, an unfinished probe, or an
//     unidentifiable GPU all behave EXACTLY as before this feature existed
//     (the full nv → qsv → mf → x264 ladder). No cache == current behaviour.
//   • Disable instantly with the TALQ_DISABLE_HWPROBE environment variable, or
//     persistently with the QSettings key Video/hwEncoderProbeEnabled = false.

// Child-process entry point. Called from main() when talq.exe is launched with
// the hidden --probe-hwcodec flag. Sets up GStreamer, computes the GPU signature,
// runs each hardware encoder candidate through a real 720p pipeline, and persists
// the result. Returns a process exit code. Runs BEFORE the normal startup
// (single-instance guard, log archive/truncate, splash, GUI) so it never disturbs
// the primary instance's log or lock.
int talqRunHwEncoderProbe(int argc, char **argv);

// Parent-side, call once at startup after QApplication exists. If the feature is
// enabled and no FRESH cache exists for the current GPU, launches
// `talq.exe --probe-hwcodec` DETACHED (async, never blocking). No-op when a fresh
// cache already exists, the feature is disabled, or the GPU can't be identified.
void talqMaybeLaunchHwEncoderProbe();

// Live-path reader used by makeWebrtcVideoEncoder(). Returns true ONLY when a
// VALID, FRESH probe cache for THIS GPU definitively marked `factory` as
// non-working (tested, never reached PLAYING). Returns false — "attempt it,
// exactly as before" — for EVERY other case (feature disabled, no/stale/
// unreadable cache, probe unfinished, GPU unidentifiable, or `factory` not a
// probed hardware candidate). This is the fail-open guarantee in one function.
bool talqHwEncoderProbeExcludes(const char *factory);

// Feature enable state: QSettings Video/hwEncoderProbeEnabled (default true) AND
// not overridden off by the TALQ_DISABLE_HWPROBE environment variable.
bool talqHwEncoderProbeEnabled();
