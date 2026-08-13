#pragma once
// Aec3Processor — drive WebRTC's AudioProcessing (AEC3) DIRECTLY, instead of through
// the `webrtcdsp` GStreamer element.
//
// WHY, after five hypotheses were measured and eliminated (2026-08-11/12)
// ----------------------------------------------------------------------
//   429862e mic-capture low-latency  -> +1.25 dB, no effect
//   AGC re-amplifying residual echo  -> 1.00 dB, exonerated
//   Windows OS canceller (comms)     -> 21.7 dB WORSE, rejected
//   noise-suppression level 2 -> 1   -> +1.51 dB inside a 3.13 dB spread, exonerated
//   reference misalignment           -> DISPROVEN: flat across 0-240 ms, and the knob
//                                       was proven live (500 ms moves it +25.3 dB)
//
// What remains is measured and specific: the chain sends 11-12.5 dB BELOW its own idle
// noise floor while the far end is active. It is not just cancelling the echo, it is
// ducking the entire near-end signal. That is AEC3's residual echo suppressor, and
// `webrtcdsp` exposes NO property that touches it.
//
// HONEST LIMIT OF THIS CLASS — read before expecting a fix
// --------------------------------------------------------
// The suppressor's own thresholds live in EchoCanceller3Config::Suppressor
// (normal_tuning vs nearend_tuning masking thresholds). Reaching them needs
// EchoCanceller3Factory, which the shipped libwebrtc-audio-processing-1 does NOT put
// in its public headers — `EchoControlFactory` is an abstract interface with no public
// implementation. So this class CANNOT retune the suppressor. Doing that means
// rebuilding the library or vendoring AEC3, which is a much larger undertaking than
// this file.
//
// It does unlock three things webrtcdsp cannot do, and the first is why it exists:
//
//   1. export_linear_aec_output — WOULD have separated linear cancellation from
//      suppressor ducking, the decomposition this investigation has lacked. ⚠ IT
//      CRASHES: 0xC0000005 in ProcessStream on frame 0, verified as a defect in the
//      shipped library 1.3 (6/6 crash with the flag on, 6/6 clean with it off, across
//      int16/float and 16/32/48 kHz, driving AudioProcessing directly). It is now
//      FORCED OFF here. This capability is NOT available without rebuilding the
//      library.
//   2. set_stream_delay_ms — a REAL device delay from WASAPI, rather than
//      webrtcechoprobe's (running_time + pipeline_latency) estimate. Note the sweep
//      suggests alignment is NOT the limiting factor, so expectations here are low;
//      it is included because it is nearly free once the APM is ours.
//   3. mobile_mode — AECM, a different canceller with different suppression
//      behaviour. One flag, untested, and not reachable through webrtcdsp.
//
// ═══ OUTCOME, MEASURED 2026-08-12 — READ BEFORE REACHING FOR THIS ═══
// webrtcdsp WON. Do not switch the default.
//
//   baseline (webrtcdsp)  -13.77 dB cancellation   spread 5.40
//   aec3 (this class)      -9.23 dB                spread 3.76
//   aec3 cancels 4.54 dB LESS, and after the double-NS fix the paired sign is
//   CONSISTENT for the first time: 0 of 3 rounds favour aec3.
//
// AECM (mobile_mode) was also measured once its stream delay was wired: +3.59 dB
// LESS cancellation than baseline. Both alternatives lose to what already ships.
//
// This class is KEPT because the bench cannot reproduce the field problem — on it
// the SHIPPING chain already suppresses echo ~8-14 dB BELOW the room noise floor,
// which is inaudible. TALQ_AEC_ENGINE=aec3 exists so the two engines can be
// compared IN A REAL CALL, which is the only place they can meaningfully differ.
// It is a diagnostic, not a pending upgrade.
//
// SCOPE: opt-in via TALQ_AEC_ENGINE=aec3. Nothing changes by default.

#include <QDebug>
#include <QtGlobal>
#include <cstdint>
#include <memory>
#include <vector>

// The shipped MSYS2 package installs under this prefix; the include path is added by
// CMake only when TALQ_WITH_AEC3 is set, so a machine without the -devel package
// still builds.
#if defined(TALQ_WITH_AEC3)
#include "modules/audio_processing/include/audio_processing.h"

namespace talq {

// Fixed-point 10 ms frames at 48 kHz mono — the rate the publish chain already
// normalises to, and the only frame size AEC3 accepts.
class Aec3Processor
{
public:
    static constexpr int kRateHz      = 48000;
    static constexpr int kChannels    = 1;
    static constexpr int kFrameSamples = kRateHz / 100;   // 480

    // exportLinear is taken BY VALUE and forced false — see the refusal below.
    bool init(bool exportLinear, bool mobileMode, int nsLevel)
    {
        m_apm.reset(webrtc::AudioProcessingBuilder().Create());
        if (!m_apm) {
            qWarning() << "Aec3Processor: AudioProcessingBuilder().Create() returned null";
            return false;
        }

        webrtc::AudioProcessing::Config cfg;
        cfg.echo_canceller.enabled = true;
        cfg.echo_canceller.mobile_mode = mobileMode;

        // ⚠ export_linear_aec_output CRASHES THE PROCESS in the shipped
        // libwebrtc-audio-processing 1.3 — 0xC0000005 inside ProcessStream on FRAME 0.
        //
        // Verified as a library defect, not a wrapper defect: driving
        // webrtc::AudioProcessing directly with no TalQ code, a 2x3 matrix of
        // {int16, float} x {16, 32, 48} kHz crashes 6 of 6 with the flag ON and runs
        // 6 of 6 clean with it OFF. The obvious benign explanation — a linear buffer
        // sized for 16 kHz's 160 samples being overrun by a 480-sample write — was
        // refuted by the 16 kHz arm crashing too.
        //
        // It is FORCED OFF rather than merely defaulted off: this flag is the reason
        // the class was written (it would have separated linear cancellation from
        // suppressor ducking), so a future reader is likely to reach for it. Honouring
        // the request would kill the call. Getting it needs a rebuilt or vendored
        // webrtc-audio-processing — the same price already named for the suppressor
        // tunings.
        if (exportLinear)
            qWarning() << "Aec3Processor: export_linear_aec_output REFUSED — it segfaults "
                          "in libwebrtc-audio-processing 1.3 (verified library defect). "
                          "Linear/suppressor decomposition needs a rebuilt library.";
        cfg.echo_canceller.export_linear_aec_output = false;
        exportLinear = false;

        cfg.noise_suppression.enabled = nsLevel >= 0;
        switch (nsLevel) {
            case 0:  cfg.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kLow; break;
            case 1:  cfg.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kModerate; break;
            case 3:  cfg.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kVeryHigh; break;
            default: cfg.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh; break;
        }
        // AGC stays OFF here. It was measured inert for echo (1.00 dB) and leaving it
        // out keeps this path a clean comparison against the webrtcdsp chain.
        cfg.gain_controller1.enabled = false;
        cfg.gain_controller2.enabled = false;

        m_apm->ApplyConfig(cfg);
        m_exportLinear = exportLinear;
        m_linear.resize(kFrameSamples);

        qInfo().nospace() << "Aec3Processor: AEC3 direct engine up (48 kHz mono, "
                          << "mobile_mode=" << (mobileMode ? "true" : "false")
                          << ", linear_export=" << (exportLinear ? "true" : "false")
                          << ", ns_level=" << nsLevel << ")";
        return true;
    }

    // Far-end (render) frame. MUST be fed in lockstep with the near end, or AEC3 has
    // no reference to subtract — the failure mode that made every earlier number
    // uninterpretable.
    void pushFarEnd(const int16_t *frame)
    {
        if (!m_apm || !frame) return;
        const webrtc::StreamConfig sc(kRateHz, kChannels);
        m_apm->ProcessReverseStream(frame, sc, sc, m_scratch());
    }

    // Real device delay, from WASAPI rather than a pipeline estimate. Cheap, and the
    // one alignment lever webrtcdsp never had — though the reference sweep suggests
    // alignment is not what limits us, so this is not expected to be decisive.
    void setStreamDelayMs(int ms)
    {
        if (m_apm && ms >= 0) m_apm->set_stream_delay_ms(ms);
    }

    // Near-end (capture) frame, processed in place. Returns false if AEC3 rejected it.
    bool processNearEnd(int16_t *frame)
    {
        if (!m_apm || !frame) return false;
        const webrtc::StreamConfig sc(kRateHz, kChannels);
        const int err = m_apm->ProcessStream(frame, sc, sc, frame);
        if (err != 0) {
            if (++m_errCount <= 3)
                qWarning() << "Aec3Processor: ProcessStream returned" << err;
            return false;
        }
        return true;
    }

    // Only meaningful when exportLinear was requested. The caller compares the level
    // here against the processed frame: the gap between them IS the suppressor's
    // contribution, which is precisely what no previous measurement could isolate.
    const std::vector<int16_t> &linearOutput() const { return m_linear; }
    bool linearAvailable() const { return m_exportLinear; }

private:
    // ProcessReverseStream needs a destination even when the caller ignores it.
    int16_t *m_scratch()
    {
        if (m_scratchBuf.size() != (size_t)kFrameSamples) m_scratchBuf.resize(kFrameSamples);
        return m_scratchBuf.data();
    }

    std::unique_ptr<webrtc::AudioProcessing> m_apm;
    std::vector<int16_t> m_scratchBuf;
    std::vector<int16_t> m_linear;
    bool m_exportLinear = false;
    int  m_errCount = 0;
};

} // namespace talq

#endif // TALQ_WITH_AEC3
