#pragma once

// #20 Phase 2e — person-segmentation backend.
//
// Encapsulates the model load + per-frame inference that produces a
// single-channel mask (white = person, black = background) from an RGBA
// camera frame.
//
// The class name is historical: the bundled model started as TFLite, but
// runtime inference uses ONNX Runtime (TFLite has no usable prebuilt
// Windows mingw64 binary; the model is converted .tflite → .onnx in dev,
// and the patched .onnx is bundled at :/bg/models/selfie_segmenter.onnx).
// The header keeps the same shape so callers don't move.
//
// At construction (when TALQ_BG_ORT is defined):
//   1. Reads :/bg/models/selfie_segmenter.onnx from qrc into RAM.
//   2. Builds an ORT session on a hidden Ort::Env — DirectML EP first
//      (0.60.5: GPU inference on NVIDIA/AMD/Intel via D3D12; chosen over
//      CUDA precisely because the worst-performing boxes are Intel-only
//      laptops), with a GRACEFUL fallback to the CPU EP when DML is
//      unavailable (CPU-only onnxruntime.dll, missing DirectML.dll, no
//      D3D12 device, broken driver) or TALQ_BG_ORT_CPU=1 forces it. The
//      ready log states which EP the session actually runs on.
//   3. One warm-up Run with zeros to JIT-compile internals (and, under
//      DML, to pay the first-dispatch GPU cost off the call path).
// On any failure the segmenter stays !isReady() and segment() returns a
// NULL mask — see the fail-closed note below.
//
// Per-frame: downscale RGBA to 256×256 (model expects NHWC float32 [0,1]),
// run the session, return the 256×256×1 confidence mask as Grayscale8.
//
// 0.60.5 — the mask is returned at the MODEL's 256×256, not scaled up to the
// camera resolution. The consumer (BackgroundCompositor) uploads it as a
// Linear-filtered texture and samples it by normalised UV into a frame-sized
// FBO, so the GPU's bilinear tap does the upscale for free inside a pass we
// already run. Doing it on the CPU first cost 6.2 ms/frame at 720p — a fifth
// of the whole 33.3 ms budget — and changed nothing about the result. Any new
// consumer must therefore NOT assume mask.size() == frame.size().
//
// 0.60.5 FAIL-CLOSED (2026-07-14 adversarial review) — segment() returns a
// NULL QImage on EVERY failure path. It used to return a "best effort"
// centred radial-gradient ellipse instead (the 0.39.x stub), and that mask
// is fail-OPEN: white at frame centre means the compositor keeps a sharp,
// fully-decodable ~576 px disc of the REAL ROOM at 720p (whatever is behind
// the user's head — whiteboard, monitor, an open door) and ships it to every
// peer and recorder, every frame, for the whole call — while the rim looks
// convincingly blurred, the fail-closed counters read 0, and the user
// believes the room is hidden. It fired on exactly the machines the feature
// matters most on: any ORT session-build failure (missing/version-skewed
// onnxruntime.dll — the Windows-inbox 1.17.1 shadowing our 1.20.1 is one
// documented trigger — model load fault, unsupported CPU) or any per-frame
// Run() throw. A guessed mask is a licence to transmit the middle of the
// frame sharp; NO mask is the only honest answer. Callers (BackgroundWorker
// ::compose) treat a null mask as a composite failure, which routes to the
// engine's content-free mosaic — the frame is covered, never revealed.

#include <QByteArray>
#include <QImage>
#include <QObject>

#include <atomic>
#include <memory>

#ifdef TALQ_BG_ORT
// Forward decls of the ORT C++ types we keep as members. The full header
// is heavy; the impl includes it.
namespace Ort {
class Env;
class Session;
class SessionOptions;
class MemoryInfo;
}
#endif

class TfliteSegmenter : public QObject
{
    Q_OBJECT

public:
    explicit TfliteSegmenter(QObject *parent = nullptr);
    ~TfliteSegmenter() override;

    // True when the ONNX session loaded and the warm-up Run succeeded.
    // False after any construction-time failure (qrc missing, ORT
    // failed to compile, etc.); segment() then returns null masks and
    // the pipeline fails closed upstream.
    bool isReady() const;

    // Process-wide live-instance count (ctor increments, dtor decrements);
    // readable from any thread. Companion to
    // BackgroundCompositor::liveInstanceCount() — lets
    // background_engine_test prove setMode(None) really destroys the ORT
    // session holder, and that one enable builds exactly ONE session (the
    // 0.60.2 Settings RCA found the mode-combo slot building two engines
    // in the same slot invocation).
    static int liveInstanceCount();

    // Compute a person mask for `rgba`. Returns an 8-bit Grayscale8 QImage:
    // 255 = full person, 0 = full background — or a NULL QImage on ANY
    // failure (not ready, downscale fault, empty inference output, Run()
    // threw, built without TALQ_BG_ORT). Never a guessed mask; see the
    // fail-closed note in the header comment.
    //
    // NOTE (0.60.5): the mask is the MODEL's native 256×256 on the normal
    // path — NOT the size of `rgba`. Consumers must sample it by normalised
    // coordinates. See the header note above for why.
    //
    // TEST SEAM: env var TALQ_BG_SEG_FAIL = nosession | throw | emptyout
    // forces the corresponding still-live failure path (checked per call, so
    // one test process can exercise all three). Follows the TALQ_DEBUG_* /
    // TALQ_PUB_TESTSRC pattern; costs one getenv per inference (~20 Hz).
    // Without it the failure paths are unreachable on a healthy box and the
    // fail-closed contract would be untestable — which is how the gradient
    // leak survived review in the first place.
    QImage segment(const QImage &rgba);

signals:
    // Fires once when ORT initialisation failed at construction (model
    // load fault, missing DLL, GPU device lost on first warm-up), and
    // once more if per-frame Run() failures latch the session off (see
    // m_runFailStreak). The segmenter then returns null masks for the
    // lifetime of this instance — frames are covered upstream, not
    // revealed. BackgroundEngine forwards this onto its own
    // engineDisabled signal so the UI can toast + flip the QSetting.
    void unavailable(const QString &reason);

private:
    static std::atomic<int> s_liveInstances;   // see liveInstanceCount()

    bool m_ready = false;
    // One-shot guard: the per-frame Run failure path qWarning's once
    // then stays quiet. At 30 fps a persistent fault (GPU device lost,
    // ORT internal panic) used to flood the log with thousands of
    // identical warnings per minute.
    std::atomic<bool> m_runFailedOnce{false};
    // 0.60.5 (2026-07-14 review) — consecutive per-frame Run/output
    // failures. A success resets it; at kRunFailureLatch the session is
    // torn down and `unavailable` fires. Through 0.60.4 Run errors were
    // deliberately NOT latched ("most are transient") — defensible only
    // while a failure cost nothing; in reality each one shipped the
    // fail-OPEN gradient composite, so a persistent fault leaked the room
    // for the entire call. Now each failure costs a fully-obscured frame
    // (no leak), so the latch is about honesty + CPU: after ~3 s of
    // continuous failure the fault is not transient, stop paying ~15 ms
    // of doomed inference per frame and tell the user. Worker-thread only.
    int m_runFailStreak = 0;

    // Temporal EMA on the segmentation mask. At 30 fps the raw sigmoid
    // output flickers at the silhouette edge by a couple of pixels
    // per frame even with the GPU bilateral pass downstream - the
    // bilateral cleans spatial noise but can't reduce frame-to-frame
    // shimmer. Holding the previous frame's 256x256 mask and blending
    // (alpha = 0.4) lights the edge in time as well as space. Always
    // 256x256 Grayscale8 (model output size, decoupled from camera
    // resolution) so a camera-mode change does NOT invalidate it.
    // Empty only on first frame; the impl seeds it from the new mask.
    QImage m_prevMask;

#ifdef TALQ_BG_ORT
    // Bump m_runFailStreak; at kRunFailureLatch tear the session down and
    // emit `unavailable` once — after first attempting a CPU-EP rebuild if
    // the failing session was on DirectML (0.60.5: a GPU/driver that dies
    // mid-call must degrade to slow-but-working, not to a latched-off
    // mosaic). Shared by the Run-throw and empty-output failure paths in
    // segment(). Worker thread only.
    void noteRunFailure(const QString &what);

    // 0.60.5 — build (or rebuild) m_sessOpts + m_session and run the zeros
    // warm-up. withDml=true appends the DirectML EP (resolved at RUNTIME
    // from the loaded onnxruntime.dll — never a static import, so a
    // CPU-only DLL on the search path degrades instead of killing the
    // loader). Returns false with `whyNot` filled on ANY failure, leaving
    // m_session null; never throws. Worker thread only (ctor +
    // noteRunFailure).
    bool buildSession(bool withDml, QString *whyNot);

    // True while m_session runs on the DirectML EP (set by buildSession).
    // Drives the mid-call CPU fallback in noteRunFailure and the EP line
    // in the ready log — the log ATTESTS the EP actually in use; nothing
    // downstream assumes.
    bool m_dmlActive = false;

    // Owned. Constructed in the ctor when the model and ORT both succeed,
    // destroyed in the dtor. unique_ptr so the heavy ORT header stays
    // out of consumers' translation units.
    std::unique_ptr<Ort::Env>            m_env;
    std::unique_ptr<Ort::SessionOptions> m_sessOpts;
    std::unique_ptr<Ort::Session>        m_session;
    std::unique_ptr<Ort::MemoryInfo>     m_memInfo;
    // Keep the model bytes alive for the lifetime of the session — the
    // session references them through Ort::Session's internal copy, but
    // holding the source buffer makes debugging easier and is cheap
    // (~450 KB).
    QByteArray m_modelBytes;
#endif
};
