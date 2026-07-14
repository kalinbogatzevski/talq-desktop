#include "TfliteSegmenter.h"

#include <QDebug>
#include <QFile>
#include <QImage>

#include <cstdlib>
#include <cstring>

#ifdef TALQ_BG_ORT
// ORT headers use MSVC SAL annotations that mingw's <sal.h> doesn't
// fully stub. Define the missing ones as no-ops BEFORE including the
// ORT headers. Each #ifndef so an upgraded mingw or alt toolchain that
// provides them keeps working unchanged.
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#ifndef _In_reads_bytes_
#define _In_reads_bytes_(s)
#endif
#ifndef _Out_writes_bytes_
#define _Out_writes_bytes_(s)
#endif
#include <onnxruntime_cxx_api.h>
#include <array>
#include <vector>
#ifdef _WIN32
// For the DirectML EP runtime resolution (GetModuleHandle/GetProcAddress/
// LoadLibrary — see the DML notes in the ctor). LEAN_AND_MEAN keeps
// rpcndr.h out, which would otherwise #define `small` and break the local
// variable of that name in segment(). NOMINMAX for qMin/std::min safety.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#endif

// Phase 2e — real selfie segmentation via ONNX Runtime.
//
// Bundle: `:/bg/models/selfie_segmenter.onnx` is MediaPipe's Selfie
// Segmenter (256×256 input, 256×256×1 sigmoid mask) converted .tflite →
// .onnx via tf2onnx (opset 18). One TFLite-only fused op
// (`Convolution2DTransposeBias`) was hand-replaced post-conversion by
// the mathematically equivalent ConvTranspose+Add pair so the model
// loads with stock ORT.
//
// When TALQ_BG_ORT is not defined segment() returns null masks and the
// whole background feature fails closed to the engine's mosaic. Through
// 0.60.4 it "degraded" to a centred-gradient stub mask instead — deleted
// 2026-07-14 because that mask is fail-OPEN: white-at-centre means the
// compositor keeps the middle of the REAL ROOM sharp while the rim looks
// convincingly blurred. See the fail-closed note in TfliteSegmenter.h.

namespace {

#ifdef TALQ_BG_ORT
constexpr int kInputW = 256;
constexpr int kInputH = 256;
constexpr int kInputC = 3;

// After this many CONSECUTIVE per-frame failures (Run threw or produced no
// tensor) the session is torn down and `unavailable` fires. ~3 s at the
// worker's measured ~20 fps — far above any transient blip (momentary OOM
// recovers within a frame or two), far below "the whole call was a mosaic
// and nobody said why". See m_runFailStreak in the header.
constexpr int kRunFailureLatch = 60;

// TALQ_BG_SEG_FAIL test seam — see segment()'s doc in the header. Parsed
// per call so one test process can walk all three failure modes.
enum class SegFailInject { None, NoSession, EmptyOut, Throw };
SegFailInject segFailInject()
{
    const char *e = std::getenv("TALQ_BG_SEG_FAIL");
    if (!e || !*e) return SegFailInject::None;
    if (std::strcmp(e, "nosession") == 0) return SegFailInject::NoSession;
    if (std::strcmp(e, "emptyout")  == 0) return SegFailInject::EmptyOut;
    if (std::strcmp(e, "throw")     == 0) return SegFailInject::Throw;
    return SegFailInject::None;
}
#endif

} // namespace

std::atomic<int> TfliteSegmenter::s_liveInstances{0};

int TfliteSegmenter::liveInstanceCount()
{
    return s_liveInstances.load(std::memory_order_acquire);
}

TfliteSegmenter::TfliteSegmenter(QObject *parent)
    : QObject(parent)
{
    s_liveInstances.fetch_add(1, std::memory_order_acq_rel);
#ifdef TALQ_BG_ORT
    // Load the bundled .onnx from qrc into a QByteArray we own for the
    // session lifetime. ORT's session can also load from a file path,
    // but qrc:// isn't a normal filesystem path on Windows, and the
    // model is small (~450 KB) so the in-memory buffer is fine.
    QFile f(QStringLiteral(":/bg/models/selfie_segmenter.onnx"));
    if (!f.open(QIODevice::ReadOnly)) {
        const QString reason = QStringLiteral(
            "bundled selfie_segmenter.onnx missing from qrc - "
            "background frames will fail closed (fully obscured)");
        qWarning() << "TfliteSegmenter:" << reason;
        // Deferred emit: the connect in BackgroundEngine happens after
        // the segmenter is constructed, so a synchronous emit here
        // would be dropped. Queue it for the next event-loop spin.
        QMetaObject::invokeMethod(this, [this, reason]() {
            emit unavailable(reason);
        }, Qt::QueuedConnection);
        return;
    }
    m_modelBytes = f.readAll();
    f.close();

    try {
        // Pin ORT to the highest C-API version the bundled onnxruntime.dll actually
        // supports, BEFORE creating any Ort object. The headers request
        // ORT_API_VERSION (e.g. 20); if the shipped DLL is older (1.17.x → max API
        // 17), the header's static GetApi(ORT_API_VERSION) returned nullptr and the
        // first Ort:: call would dereference it and take the app down. Re-negotiate
        // down once so a version-skewed DLL just runs at its own API level. All of
        // our calls (Env/SessionOptions/Session/Run/CreateTensor/MemoryInfo) are
        // API-1 surface, so a lower struct is ABI-safe here.
        if (Ort::Global<void>::api_ == nullptr) {
            const OrtApiBase *base = OrtGetApiBase();
            for (uint32_t v = ORT_API_VERSION; v >= 1; --v)
                if (const OrtApi *a = base->GetApi(v)) { Ort::Global<void>::api_ = a; break; }
        }
        // ORT_LOGGING_LEVEL_WARNING — quiet on the happy path, but if
        // the GPU/CPU EP needs to fall back, we surface the warning.
        m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "talq-bg");

        m_memInfo = std::make_unique<Ort::MemoryInfo>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));
        // ^ stays a CPU MemoryInfo under BOTH EPs: our input tensors are
        // built over CPU float buffers either way; the DML EP copies them
        // to the GPU internally. (256×256×3 floats = 768 KB/inference —
        // an IOBinding to skip that copy is not worth the complexity at
        // this size.)

        // 0.60.5 — DirectML first, CPU as the always-works fallback. The
        // escape hatch TALQ_BG_ORT_CPU=1 pins the CPU EP (support tool for
        // a box where DML inits fine but misbehaves — wrong output, driver
        // hangs — and the A/B seam the before/after numbers came from).
        QString whyNotDml;
        bool built = false;
        if (qEnvironmentVariableIsSet("TALQ_BG_ORT_CPU")) {
            whyNotDml = QStringLiteral("TALQ_BG_ORT_CPU=1 (forced)");
        } else {
            built = buildSession(/*withDml*/ true, &whyNotDml);
        }
        if (!built) {
            qInfo().noquote()
                << "TfliteSegmenter: DirectML EP not used —" << whyNotDml
                << "— falling back to the CPU EP (slower, always works)";
            QString whyNotCpu;
            if (!buildSession(/*withDml*/ false, &whyNotCpu)) {
                // Both EPs failed — same terminal handling the old
                // CPU-only path had.
                const QString reason = QStringLiteral(
                    "ORT initialisation failed: %1 - background frames will"
                    " fail closed (fully obscured)").arg(whyNotCpu);
                qWarning() << "TfliteSegmenter:" << reason;
                m_sessOpts.reset();
                m_env.reset();
                m_memInfo.reset();
                QMetaObject::invokeMethod(this, [this, reason]() {
                    emit unavailable(reason);
                }, Qt::QueuedConnection);
                return;
            }
        }
        m_ready = true;
        // This line is the EP attestation — it reports what the session was
        // actually BUILT with (a DML session whose device could not be
        // created throws at build time and lands on the CPU line instead).
        qInfo().noquote()
            << "TfliteSegmenter: ONNX Runtime session ready (model"
            << (m_modelBytes.size() / 1024) << "KB, input 256x256 NHWC"
            << "float32, EP:" << (m_dmlActive ? "DirectML/GPU" : "CPU") << ")";
    } catch (const Ort::Exception &e) {
        // Env / MemoryInfo construction failed — pre-session, both EPs moot.
        const QString reason = QStringLiteral(
            "ORT initialisation failed: %1 - background frames will fail"
            " closed (fully obscured)")
            .arg(QString::fromLatin1(e.what()));
        qWarning() << "TfliteSegmenter:" << reason;
        m_session.reset();
        m_sessOpts.reset();
        m_env.reset();
        m_memInfo.reset();
        QMetaObject::invokeMethod(this, [this, reason]() {
            emit unavailable(reason);
        }, Qt::QueuedConnection);
    }
#else
    qInfo() << "TfliteSegmenter: built without TALQ_BG_ORT — "
               "using centred-gradient stub";
#endif
}

TfliteSegmenter::~TfliteSegmenter()
{
    // The ORT members (unique_ptrs) are destroyed right after this body on
    // the same thread — an observer reading 0 may race that by microseconds
    // but the destruction is already unconditional at that point.
    s_liveInstances.fetch_sub(1, std::memory_order_acq_rel);
}

bool TfliteSegmenter::isReady() const
{
    return m_ready;
}

QImage TfliteSegmenter::segment(const QImage &rgba)
{
    // FAIL CLOSED (2026-07-14 review): every failure return in this function
    // is a NULL QImage, and it must stay that way. Each of these paths used
    // to return fallbackGradientMask() — a centred ellipse of "person" — and
    // the compositor then happily produced a fresh, cacheKey-distinct
    // composite that sailed past every downstream leak guard while keeping a
    // sharp disc of the user's real room dead-centre of the outgoing video.
    // A null mask instead makes BackgroundWorker::compose() report failure,
    // nothing is published, and the engine covers the frame with the mosaic.
#ifdef TALQ_BG_ORT
    const SegFailInject inject = segFailInject();
    if (!m_ready || !m_session || inject == SegFailInject::NoSession) {
        return QImage();
    }

    // 1. Downscale the camera frame to 256×256 in RGB float32 NHWC.
    //    SmoothTransformation is good enough at this size; the model's
    //    input is already lossy to scale changes.
    //
    //    0.60.5 — SCALE FIRST, convert second. This used to convert the whole
    //    921 k-pixel frame to RGBA8888 (a 3.7 MB format conversion) and only
    //    then throw 98.6 % of it away in the downscale to 256². Doing it in
    //    the other order converts 64 k pixels instead of 921 k. Measured
    //    2.9 ms/frame saved at 720p (6.2 ms at 1080p) on the bench.
    //
    //    Byte order is NOT affected — verified on Qt 6.10.1, not assumed:
    //    QImage::scaled(Smooth) preserves the source format for both formats
    //    that reach here (RGB32 from the GStreamer BGRx bridge, RGBA8888 from
    //    the tests), so converting after the scale yields the same R,G,B,A
    //    scanline bytes the loop below indexes. (Getting this wrong would
    //    silently feed the model BGR and quietly degrade every mask.)
    QImage small = rgba.scaled(kInputW, kInputH,
                               Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation)
                       .convertToFormat(QImage::Format_RGBA8888);
    if (small.width() != kInputW || small.height() != kInputH) {
        return QImage();
    }

    std::vector<float> nhwc(kInputH * kInputW * kInputC);
    for (int y = 0; y < kInputH; ++y) {
        const uchar *row = small.constScanLine(y);
        float *dst = nhwc.data() + y * kInputW * kInputC;
        for (int x = 0; x < kInputW; ++x) {
            // Format_RGBA8888 byte order is R,G,B,A. The model takes
            // RGB; drop alpha. Normalise to [0,1].
            dst[x * 3 + 0] = row[x * 4 + 0] / 255.0f;
            dst[x * 3 + 1] = row[x * 4 + 1] / 255.0f;
            dst[x * 3 + 2] = row[x * 4 + 2] / 255.0f;
        }
    }

    // 2. Run inference.
    try {
        if (inject == SegFailInject::Throw)
            throw Ort::Exception("TALQ_BG_SEG_FAIL=throw (test injection)",
                                 ORT_FAIL);
        const std::array<int64_t, 4> shape = {1, kInputH, kInputW, kInputC};
        Ort::Value in = Ort::Value::CreateTensor<float>(
            *m_memInfo, nhwc.data(), nhwc.size(),
            shape.data(), shape.size());
        const char *inputNames[]  = {"input_1"};
        const char *outputNames[] = {"activation_10"};
        auto outs = m_session->Run(Ort::RunOptions{nullptr},
                                   inputNames,  &in, 1,
                                   outputNames, 1);
        if (outs.empty() || !outs[0].IsTensor()
            || inject == SegFailInject::EmptyOut) {
            noteRunFailure(QStringLiteral("inference produced no tensor"));
            return QImage();
        }
        const float *mask = outs[0].GetTensorData<float>();
        m_runFailStreak = 0;   // a real mask came back — fault (if any) was transient

        // 3. Convert 256x256x1 sigmoid output -> Grayscale8 256x256.
        //    Temporal EMA: blend with the previous frame's mask before
        //    upsampling so frame-to-frame shimmer at the silhouette
        //    edge is reduced. Higher alpha tracks motion better but
        //    flickers more. Empty m_prevMask on first frame -> seed it
        //    from the new mask directly.
        //
        //    0.60.6 — alpha 0.4 → 0.6 when inference dropped to ~15 Hz
        //    (mask decimation), keeping the mask's mean lag ≈ the shipped
        //    0.4 @ 30 Hz (~50 ms). Mask lag is not just cosmetic — a
        //    trailing mask keeps a strip of the REAL ROOM sharp at the
        //    silhouette edge.
        //    0.60.5 composite-rate cap — inference under motion is now
        //    ~10 Hz (20 fps composed frames, every second one inferred), so
        //    0.6 would mean (1-a)/a * 100 ms ≈ 67 ms mean lag. Bumped to
        //    0.7 → ≈ 43 ms, back on par with what shipped; the further-
        //    reduced update rate keeps damping the shimmer the EMA exists
        //    for.
        constexpr float kEmaAlpha = 0.7f;
        QImage maskSmall(kInputW, kInputH, QImage::Format_Grayscale8);
        const bool haveHistory = !m_prevMask.isNull()
                              && m_prevMask.width()  == kInputW
                              && m_prevMask.height() == kInputH
                              && m_prevMask.format() == QImage::Format_Grayscale8;
        for (int y = 0; y < kInputH; ++y) {
            uchar *row = maskSmall.scanLine(y);
            const float *src = mask + y * kInputW;
            const uchar *prevRow = haveHistory ? m_prevMask.constScanLine(y) : nullptr;
            for (int x = 0; x < kInputW; ++x) {
                float v = src[x];
                if (v < 0.0f) v = 0.0f;
                else if (v > 1.0f) v = 1.0f;
                uchar curr = static_cast<uchar>(v * 255.0f + 0.5f);
                if (prevRow) {
                    const float blended =
                        kEmaAlpha * curr + (1.0f - kEmaAlpha) * prevRow[x];
                    row[x] = static_cast<uchar>(blended + 0.5f);
                } else {
                    row[x] = curr;
                }
            }
        }
        // Keep a copy of the smoothed mask for the next frame's EMA.
        m_prevMask = maskSmall;

        // 0.60.5 — return the 256×256 mask AS IS. This used to smooth-scale it
        // up to full frame here, on the CPU, every single inference: a ~1 Mpx
        // resample costing 6.2 ms/frame at 720p (13.9 ms at 1080p) — measured,
        // and by itself a fifth of the 33.3 ms frame budget.
        //
        // The upscale was redundant, not merely expensive. BackgroundCompositor
        // uploads this mask as a texture with Linear min/mag filtering, and
        // bg_mask_refine.frag samples it by NORMALISED UV (`texture(u_segMask,
        // coord)`) while rendering into a FRAME-sized FBO. So the GPU's
        // bilinear tap already performs exactly this upscale, for free, as part
        // of a pass we run anyway. We were paying Qt to do the GPU's work.
        // The refine pass's ±3-texel offsets are computed in FRAME texels,
        // which at 720p are sub-texel on a 256² mask — i.e. they were already
        // landing inside a single mask texel after the CPU upscale, so the
        // sampled neighbourhood is unchanged.
        //
        // Bonus: the mask texture upload (and the mirrored() copy inside it)
        // drops from ~900 KB to 64 KB per frame, and m_texMask stops being
        // reallocated whenever the camera resolution changes.
        return maskSmall;
    } catch (const Ort::Exception &e) {
        // Warn once, then quiet. At 30 fps a persistent fault would
        // otherwise log thousands of identical lines per minute.
        if (!m_runFailedOnce.exchange(true, std::memory_order_relaxed)) {
            qWarning() << "TfliteSegmenter: ORT Run failed (first occurrence) -"
                       << e.what()
                       << "- further per-frame failures suppressed for this session";
        }
        // A single transient failure (momentary OOM) recovers on its own —
        // the null return costs one mosaic-covered frame, nothing more. A
        // STREAK means the session is dead; noteRunFailure latches it off
        // after kRunFailureLatch so the user is told instead of spending the
        // call as an unexplained mosaic. (Pre-0.60.5 this path deliberately
        // never latched — and every failure leaked the gradient composite.)
        noteRunFailure(QString::fromLatin1(e.what()));
        return QImage();
    }
#else
    Q_UNUSED(rgba);
    return QImage();
#endif
}

#ifdef TALQ_BG_ORT
bool TfliteSegmenter::buildSession(bool withDml, QString *whyNot)
{
    m_session.reset();      // rebuild-safe: noteRunFailure reuses this
    m_dmlActive = false;
    try {
        m_sessOpts = std::make_unique<Ort::SessionOptions>();
        m_sessOpts->SetIntraOpNumThreads(2);   // small model, 2 threads is
                                               // enough; under DML this only
                                               // serves CPU-assigned nodes
        m_sessOpts->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // MEASURED (2026-07-14, Iris Xe, 720p motion scene): ORT's intra-op
        // pool BUSY-SPINS between Runs by default. At our ~10 inferences/s
        // duty cycle the two spinning threads burned ~40% of a core doing
        // nothing — more CPU than the inferences themselves. Off, the pool
        // parks on a kernel wait between Runs; the wake-up adds ~µs to a
        // ~100 ms-spaced inference, irrelevant here. Applies to both EPs
        // (DML still schedules its CPU-assigned nodes on this pool).
        m_sessOpts->AddConfigEntry("session.intra_op.allow_spinning", "0");

        if (withDml) {
#ifndef _WIN32
            *whyNot = QStringLiteral("DirectML is Windows-only");
            return false;
#else
            // The DML EP requires these two (onnxruntime docs, "DirectML
            // Execution Provider — configuration"): memory-pattern
            // optimisation assumes CPU allocators, and DML kernels are not
            // re-entrant, so execution must be sequential. Both are
            // per-session and cost nothing measurable on this 1-input
            // 256×256 graph.
            m_sessOpts->DisableMemPattern();
            m_sessOpts->SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);

            // Resolve the append entry point from the ALREADY-LOADED
            // onnxruntime.dll at runtime. NEVER a static import: talq.exe
            // links only OrtGetApiBase, so if some machine resolves
            // onnxruntime.dll to the Windows-inbox CPU-only 1.17.1 (the
            // known System32 shadow for non-co-located exes), the app still
            // LOADS and lands here, where the missing export simply routes
            // to the CPU EP. A static import of the DML symbol would have
            // been an "entry point not found" loader abort — a dead app.
            using DmlAppendFn = OrtStatus *(ORT_API_CALL *)(OrtSessionOptions *, int);
            const HMODULE ortMod = GetModuleHandleW(L"onnxruntime.dll");
            const auto dmlAppend = reinterpret_cast<DmlAppendFn>(
                ortMod ? reinterpret_cast<void *>(
                             GetProcAddress(ortMod,
                                 "OrtSessionOptionsAppendExecutionProvider_DML"))
                       : nullptr);
            if (!dmlAppend) {
                *whyNot = QStringLiteral(
                    "onnxruntime.dll exports no DML entry point (CPU-only "
                    "build on the search path?)");
                return false;
            }
            // Preflight the DELAY-LOADED runtime pieces. onnxruntime.dll's
            // DML build delay-loads DirectML.dll + d3d12.dll; a missing
            // module at delay-load time raises an SEH exception MinGW's
            // try/catch cannot see. Loading them here first (search order
            // starts at the exe dir, so the DEPLOYED DirectML.dll wins over
            // any older System32 inbox copy) turns "would crash" into
            // "falls back with a reason". The handles are deliberately kept
            // (pinned) — the session needs them resident anyway.
            const HMODULE dmlMod = LoadLibraryW(L"DirectML.dll");
            if (!dmlMod) {
                *whyNot = QStringLiteral(
                    "DirectML.dll not found next to the app (deploy must "
                    "carry it)");
                return false;
            }
            if (!GetProcAddress(dmlMod, "DMLCreateDevice1")) {
                *whyNot = QStringLiteral(
                    "DirectML.dll too old (no DMLCreateDevice1) — an inbox "
                    "System32 copy shadowed the deployed one?");
                return false;
            }
            if (!LoadLibraryW(L"d3d12.dll")) {
                *whyNot = QStringLiteral("no d3d12.dll on this system");
                return false;
            }
            // Device 0 = default adapter. On error ORT hands back an
            // OrtStatus; release it via the pinned C API.
            if (OrtStatus *st = dmlAppend(*m_sessOpts, /*device_id*/ 0)) {
                const OrtApi &api = Ort::GetApi();
                *whyNot = QString::fromUtf8(api.GetErrorMessage(st));
                api.ReleaseStatus(st);
                return false;
            }
#endif // _WIN32
        }

        m_session = std::make_unique<Ort::Session>(
            *m_env,
            m_modelBytes.constData(),
            static_cast<size_t>(m_modelBytes.size()),
            *m_sessOpts);

        // Warm-up Run with zeros. JITs internal kernels (CPU) / pays the
        // shader+pipeline first-dispatch cost (DML) so the first real frame
        // doesn't. Also the attestation that the chosen EP can actually
        // EXECUTE this graph, not merely initialise — a DML device that
        // dies on dispatch fails here and falls back instead of failing on
        // frame one of the call.
        std::vector<float> dummy(kInputH * kInputW * kInputC, 0.0f);
        const std::array<int64_t, 4> shape = {1, kInputH, kInputW, kInputC};
        Ort::Value in = Ort::Value::CreateTensor<float>(
            *m_memInfo, dummy.data(), dummy.size(),
            shape.data(), shape.size());
        const char *inputNames[]  = {"input_1"};
        const char *outputNames[] = {"activation_10"};
        auto outs = m_session->Run(Ort::RunOptions{nullptr},
                                   inputNames,  &in, 1,
                                   outputNames, 1);
        if (outs.empty()) {
            *whyNot = QStringLiteral("warm-up Run produced no output");
            m_session.reset();
            return false;
        }
        m_dmlActive = withDml;
        return true;
    } catch (const Ort::Exception &e) {
        // Session build or warm-up threw — with DML appended this is the
        // "no D3D12 device / driver refused" path (headless box, remote
        // desktop session, ancient driver).
        *whyNot = QString::fromLatin1(e.what());
        m_session.reset();
        return false;
    }
}

void TfliteSegmenter::noteRunFailure(const QString &what)
{
    if (++m_runFailStreak < kRunFailureLatch || !m_session)
        return;
    // 0.60.5 — if the dying session was on DirectML, this is a GPU/driver
    // fault (device removed, TDR loop), not a model fault: rebuild on the
    // CPU EP and keep going. The user gets slower inference, not a latched
    // -off feature. Only when the CPU EP fails too is the fault terminal.
    if (m_dmlActive) {
        qWarning() << "TfliteSegmenter: DML session failed" << kRunFailureLatch
                   << "times in a row (last:" << what
                   << ") - rebuilding on the CPU EP";
        QString whyNot;
        if (buildSession(/*withDml*/ false, &whyNot)) {
            m_runFailStreak = 0;
            m_runFailedOnce.store(false, std::memory_order_relaxed);
            qWarning() << "TfliteSegmenter: CPU EP rebuild succeeded -"
                          " background continues (slower, no GPU)";
            return;
        }
        qWarning() << "TfliteSegmenter: CPU EP rebuild ALSO failed:" << whyNot;
    }
    const QString reason = QStringLiteral(
        "ORT Run failed %1 times in a row (last: %2) - session latched off,"
        " background frames stay fully obscured")
        .arg(kRunFailureLatch).arg(what);
    qWarning() << "TfliteSegmenter:" << reason;
    // Free the session: the !m_session fast-path at the top of segment()
    // then skips the doomed 256² preprocess + Run per frame. The connects
    // exist by now (we are per-frame, long past construction), so unlike
    // the ctor this emit needs no queued deferral.
    m_session.reset();
    m_ready = false;
    emit unavailable(reason);
}
#endif
