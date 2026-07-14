#include "BackgroundWorker.h"

#include "BackgroundCompositor.h"
#include "TfliteSegmenter.h"
#include "BackgroundEngine.h"   // for Mode enum

#include <QDebug>
#include <QElapsedTimer>
#include <QImage>
#include <QMutexLocker>
#include <QOffscreenSurface>

namespace {

// 0.60.5 COMPOSITE-RATE CAP — the background output is deliberately capped
// BELOW the 30 fps camera rate. Rationale (Kalin, 2026-07-14): the mailbox
// rework cut the per-frame cost, and the freed headroom was immediately spent
// raising the background rate from ~15 fps to ~27-28 fps — so on a MOVING
// scene the absolute process CPU stayed flat and only a static scene got
// cheaper. The complaint was CPU, not smoothness: 20 fps is visually fine for
// a webcam background (films are 24; Talk's own effect runs well below the
// camera rate on most laptops), and every skipped frame is one saved
// composite (texture upload + 4 GL passes + PBO readback) plus, every second
// skip, one saved inference. Frames skipped by the cap are still published:
// the producer re-serves the newest completed composite from the mailbox
// output slot — identical pixels cost the encoder an all-skip P-frame — and
// the wire frame rate stays at the camera's 30 fps. The cap must NEVER
// starve the output: a frame is only skipped when a current-epoch composite
// is already parked in the slot (see processMailbox), so a cold start, an
// epoch boundary or a failing compose always composes/attempts immediately.
constexpr qint64 kBgCompositeMinIntervalMs = 50;   // = 20 fps target

} // namespace

BackgroundWorker::BackgroundWorker(QOffscreenSurface *surface,
                                   std::shared_ptr<BgFrameMailbox> box,
                                   QObject *parent)
    : QObject(parent)
    , m_box(std::move(box))
    , m_surface(surface)
{}

BackgroundWorker::~BackgroundWorker() = default;
// m_compositor + m_segmenter: parented to this; Qt destroys them in the
// child cleanup. shutdown() releases GL resources first while the worker
// thread is still alive — never destruct GL objects from another thread.

void BackgroundWorker::applyMode(int modeInt)
{
    const auto m = static_cast<BackgroundEngine::Mode>(modeInt);
    if (m == BackgroundEngine::Mode::None) {
        // 0.60.2 (2026-07-13 field RCA) — turning the background OFF must
        // actually FREE the engine. This used to be a bare `return`, so the
        // ONLY teardown was ~BackgroundEngine at process exit: the ORT
        // session, the GL context, the FBOs and the textures all survived
        // the user's OFF. Measured on an Iris Xe box (0.60.1 field log,
        // controlled Off→Blur→Off): 256 MB steady → 401 MB with the engine
        // up, and only 29 MB came back on OFF — ~112 MB retained for the
        // rest of the session. Deliberate trade-off: re-enabling now pays
        // the cold GL + ORT init again (frames pass through raw for a few
        // seconds, exactly like the very first enable) — users toggle the
        // background rarely, and the alternative is carrying ~112 MB forever.
        releaseEngineResources();
        return;
    }

    if (!m_compositor) {
        m_compositor = new BackgroundCompositor(this);
        m_compositor->setExternalSurface(m_surface);
        // The compositor's lazy init runs on first compositeBlur/Image
        // call. We don't pre-warm here so a quick toggle ON→OFF in the
        // Settings dialog doesn't pay GL creation cost for nothing.
        //
        // 0.60.2 (2026-07-13 field RCA) — initFailed used to have ZERO
        // connects anywhere in the codebase: a GL init failure meant the
        // user's Blur/Image choice silently did nothing, with not one log
        // line saying why. Loud warning + forward as engineDisabled (the
        // engine → CallManager chain surfaces it on the in-call notice).
        connect(m_compositor, &BackgroundCompositor::initFailed, this,
                [this](const QString &reason) {
            qWarning() << "BackgroundWorker: compositor GL init FAILED —"
                       << reason
                       << "— background frames pass through unprocessed";
            emit engineDisabled(reason);
        });
    }
    if (!m_segmenter) {
        m_segmenter = new TfliteSegmenter(this);
        connect(m_segmenter, &TfliteSegmenter::unavailable, this,
                [this](const QString &reason) {
            emit engineDisabled(reason);
        });
    }
    // Compositor + segmenter constructed (ORT session is live). Tell the
    // engine it can stop passing frames through unmodified. release-store
    // pairs with the acquire-load in isReady() on the streaming thread.
    m_ready.store(true, std::memory_order_release);

    // 0.60.5 — and finish the job: GL context + shaders + FBOs + first-use
    // driver warm-up, at the production camera cap. Before this, applyMode
    // deliberately deferred GL init to the first composite ("a quick toggle
    // ON→OFF shouldn't pay GL creation") — but the first composite runs on
    // the CALL PATH, so every call started with 200 ms - 1 s of mosaic
    // (measured: fail-closed ~27 frames at call start, 0.60.5 live run).
    // The quick-toggle concern is obsolete: turning the background ON also
    // starts the Settings preview, which needs the GL chain immediately
    // anyway, and the whole warm-up runs on this worker thread — nothing
    // user-visible blocks. A caller that knows the real negotiated camera
    // size refines the FBO warm via prewarmResources(w, h).
    prewarmResources(1280, 720);
}

void BackgroundWorker::prewarmResources(int width, int height)
{
    if (!m_compositor || !m_segmenter) return;   // only meaningful while ON
    QElapsedTimer t;
    t.start();
    // GL context + shader compile + geometry. Idempotent; permanent-failure
    // latch inside makes repeat calls after a failed init instant no-ops.
    if (!m_compositor->ensureInitialised()) return;

    const QSize sz(width, height);
    if (sz.isEmpty() || m_prewarmedSize == sz) return;

    // One throwaway compose at the expected frame size: allocates the
    // frame-sized FBOs + the PBO pair and pays the first-use driver costs
    // (texture allocation, pipeline state build) OFF the call path. The
    // pixels are synthetic (flat grey frame, all-background mask). They
    // MUST NOT reach the wire: the returned composite is dropped on the
    // floor here — this function never touches the mailbox — and the copy
    // parked in the async-readback PBO is invalidated right after, so a
    // later real composite cannot re-serve it. If the real camera size
    // ends up different, the only extra cost is one FBO re-alloc on the
    // first real frame — same as before this warm-up existed.
    QImage dummy(sz, QImage::Format_RGB32);
    dummy.fill(0xFF808080);
    QImage mask(256, 256, QImage::Format_Grayscale8);
    mask.fill(0);
    const float radius = 10.0f * (sz.width() / 720.0f);
    (void) m_compositor->compositeBlur(dummy, mask, radius);
    m_compositor->discardPendingReadback();
    m_prewarmedSize = sz;

    qInfo() << "BackgroundWorker: GL chain prewarmed for" << sz
            << "in" << t.elapsed() << "ms (off the call path)";
}

void BackgroundWorker::invalidateImageCache()
{
    m_cachedBgScaled  = QImage();
    m_cachedBgPath.clear();
    m_cachedBgScaledSize = QSize();
    m_cachedBgFailed = false;
}

void BackgroundWorker::processMailbox()
{
    if (!m_box) return;

    // Take the newest frame. Anything older was already overwritten by the
    // producer — that IS the backpressure.
    QImage  rgba;
    int     mode = 0, blur = 10;
    QString path;
    quint64 seq = 0, epoch = 0;
    {
        QMutexLocker lk(&m_box->inMutex);
        if (m_box->inFrame.isNull()) return;   // a kick we already drained
        rgba = m_box->inFrame;
        m_box->inFrame = QImage();
        mode  = m_box->inMode;
        blur  = m_box->inBlur;
        path  = m_box->inPath;
        seq   = m_box->inSeq;
        epoch = m_box->inEpoch;
    }

    // 0.60.6 — the compositor's readback is pipelined one frame deep (see
    // BackgroundCompositor::takeReadback), which means it can be HOLDING the
    // previous composite. Two situations make that parked frame illegal to
    // return, both continuity breaks the mailbox's own gates cannot see
    // because the pixels would come out under a fresh seq + stamp:
    //   * epoch change — resetMailbox() ran (mode/image change, call
    //     teardown, camera enable): the parked composite is pre-boundary.
    //   * a >1 s submission gap — same wall-clock bound as the engine's
    //     kMaxStaleMs; without this, the last pre-gap composite (e.g. the
    //     final frame before a camera stall) would be published as the
    //     first post-gap frame, minutes old under a fresh timestamp.
    // The stale mask is dropped at an epoch break too: a fresh producer
    // session deserves a fresh inference, and one inference is cheap.
    if (m_compositor) {
        const bool gap = m_sinceLastCompose.isValid()
                      && m_sinceLastCompose.elapsed() > 1000;
        if (epoch != m_lastSeenEpoch || gap) {
            m_compositor->discardPendingReadback();
            m_lastMask  = QImage();
            m_motionRef = QImage();
            m_nextComposeDueMs = 0;   // a boundary never waits on the rate cap
        }
    }
    m_lastSeenEpoch = epoch;
    m_sinceLastCompose.restart();

    // 0.60.5 COMPOSITE-RATE CAP (rationale at kBgCompositeMinIntervalMs).
    // Skip this frame if the next composite isn't due yet AND the output slot
    // already holds a composite the producer can legally serve for it (same
    // epoch, actually published). Without the second condition the cap could
    // starve the slot — cold start, first frame after a boundary, or a
    // permanently failing compose — and every skipped frame would go out as
    // the mosaic instead of the effect. With it, a skip is invisible: the
    // producer re-pushes pixels at most ~100 ms old. The deadline follows an
    // accumulator (+50 per compose, clamped to now when we fall behind), so
    // the average rate is exactly the cap even though 30 fps frames can only
    // land on 33.3 ms boundaries.
    const qint64 nowMs = m_box->clock.elapsed();
    if (nowMs < m_nextComposeDueMs) {
        bool covered;
        {
            QMutexLocker lk(&m_box->outMutex);
            covered = m_box->outStampMs >= 0 && m_box->outEpoch == epoch;
        }
        if (covered) {
            talq::bg::Stats::noteRateCapped();
            return;   // frame discarded; its buffer unmaps with the last ref
        }
    }
    m_nextComposeDueMs = qMax(m_nextComposeDueMs + kBgCompositeMinIntervalMs,
                              nowMs);

    const QImage out = compose(rgba, mode, blur, path);

    // Log UNCONDITIONALLY, not only after a successful publish (2026-07-14
    // review): a permanently degraded box — GL init failed, ORT unavailable —
    // never publishes anything, and it is exactly the box the field evidence
    // has to come from. maybeLog is rate-limited internally.
    talq::bg::Stats::maybeLog();

    // FAIL-CLOSED. Two ways a raw camera frame could still escape into the
    // output slot, and hence to the encoder and every peer:
    //   1. compose() failed — it returns a NULL image, never the input.
    //   2. compose() succeeded but the COMPOSITOR passed the frame through.
    //      Since 2026-07-14 its failure paths return null too (they used to
    //      `return rgba`), so this arm is defence-in-depth against a future
    //      regression: the pass-through hands back the same implicitly-
    //      shared buffer, identifiable by cacheKey.
    // Neither is published. The engine covers the frame with the mosaic
    // instead. Do not "optimise" this check away.
    if (out.isNull() || out.cacheKey() == rgba.cacheKey())
        return;

    {
        QMutexLocker lk(&m_box->outMutex);
        // Epoch gate (2026-07-14): resetMailbox() ran between our take and
        // this publish — the frame belongs to the previous call / mode /
        // camera session. Publishing it would re-emit pre-boundary pixels
        // under a fresh timestamp; discard it instead. One worker period of
        // extra mosaic at the boundary is the whole cost.
        if (m_box->epoch.load(std::memory_order_acquire) != epoch)
            return;
        m_box->outFrame   = out;
        m_box->outSeq     = seq;
        m_box->outEpoch   = epoch;
        m_box->outStampMs = m_box->clock.elapsed();
    }
    m_box->completed.fetch_add(1, std::memory_order_relaxed);
}

QImage BackgroundWorker::compose(const QImage &rgba, int modeInt,
                                  int blurStrength, const QString &imagePath)
{
    const auto m = static_cast<BackgroundEngine::Mode>(modeInt);
    if (m == BackgroundEngine::Mode::None || rgba.isNull()) return QImage();

    // WorkerTotal is the WHOLE per-frame cost (0.60.6: no longer seg+comp —
    // decimation makes those two alternate, so their sum overstates a reused
    // frame and the per-frame truth is what decides the achievable fps).
    QElapsedTimer whole;
    whole.start();

    if (!m_compositor || !m_segmenter) {
        // 0.60.2 — do NOT reconstruct. The pre-0.60.2 code auto-constructed
        // here ("first frame before applyMode landed"). The way to land here
        // is a frame submitted in the same instant applyMode(None) released
        // everything. Reconstructing for that frame would silently resurrect
        // the ~145 MB engine right after the user turned the background OFF —
        // the exact retention bug the 2026-07-13 RCA measured.
        //
        // 0.60.5 — returns NULL, not `rgba`. This used to pass the raw camera
        // frame back to the caller, which pushed it to the encoder.
        return QImage();
    }

    // 0.60.6 — MASK DECIMATION + STATIC-SCENE GATE. Inference (input
    // downscale + ORT Run) was ~26 ms of the 55 ms worker total at 720p on
    // Kalin's zenbook (the 2026-07-13 "talq eats much more CPU" report),
    // and a silhouette does not change meaningfully in 33 ms — so:
    //   * under MOTION, segment() runs on every SECOND composed frame
    //     (~10 Hz now that composed frames are themselves capped at 20 fps —
    //     see kBgCompositeMinIntervalMs; the frame in between reuses the
    //     last mask, never older than kMaskMaxReuseMs);
    //   * while the scene is STATIC — this frame's 32×18 point-sampled
    //     signature matches the one taken when the mask was inferred —
    //     the mask is reused up to kStaticMaskMaxMs, cutting inference to
    //     ~2 Hz for the "sitting in a call" steady state that dominates
    //     real usage. The gate reads the CURRENT frame before every reuse,
    //     so the first frame with motion pays a fresh inference, and the
    //     reference is the INFERENCE-time frame, so slow drift accumulates
    //     until it trips instead of hiding under a per-frame threshold.
    // The COMPOSITE still runs every frame. Privacy bound: a stale mask
    // under fast motion leaves a strip of the REAL ROOM sharp at the
    // trailing silhouette edge — see the member note in BackgroundWorker.h
    // and the worst-case statement in the 0.60.6 release notes.
    //
    // A transient inference failure rides on the same bounds: if segment()
    // returns null but a reusable real mask exists, that mask covers the
    // frame — no worse than the deliberate reuse one frame earlier.
    // segment()'s internal failure latch still counts the failure either
    // way. With NO reusable mask, compose fails and the engine's mosaic
    // covers the frame (fail-closed, 2026-07-14: never a guessed mask, so
    // this reuse path can only serve masks a real inference produced).
    constexpr int    kMaskReuseFrames  = 1;    // infer, reuse, infer, reuse…
    constexpr qint64 kMaskMaxReuseMs   = 100;  // hard bound under motion
    constexpr qint64 kStaticMaskMaxMs  = 500;  // bound while provably static
    constexpr int    kSigW = 32, kSigH = 18;   // 576 sample points
    constexpr int    kSigLumaDelta   = 12;     // |ΔY| for a "changed" point
    constexpr int    kSigChangedMax  = 6;      // ~1 % of points may change

    // Point-sampled signature (FastTransformation = decimation, ~µs — no
    // full-image filter). Raw sensor noise passes through; a noisy camera
    // just keeps the gate open and degrades to plain N=2 decimation.
    const QImage sig = rgba.scaled(kSigW, kSigH, Qt::IgnoreAspectRatio,
                                   Qt::FastTransformation)
                           .convertToFormat(QImage::Format_RGB32);
    bool sceneStatic = false;
    if (!m_motionRef.isNull() && m_motionRef.size() == sig.size()) {
        int changed = 0;
        for (int y = 0; y < kSigH && changed <= kSigChangedMax; ++y) {
            const QRgb *a = reinterpret_cast<const QRgb *>(m_motionRef.constScanLine(y));
            const QRgb *b = reinterpret_cast<const QRgb *>(sig.constScanLine(y));
            for (int x = 0; x < kSigW; ++x) {
                const int ya = (qRed(a[x]) + 2 * qGreen(a[x]) + qBlue(a[x])) / 4;
                const int yb = (qRed(b[x]) + 2 * qGreen(b[x]) + qBlue(b[x])) / 4;
                if (qAbs(ya - yb) > kSigLumaDelta) ++changed;
            }
        }
        sceneStatic = changed <= kSigChangedMax;
    }

    const qint64 maskAge = (!m_lastMask.isNull() && m_lastMaskClock.isValid())
                         ? m_lastMaskClock.elapsed() : -1;
    const bool reusable = maskAge >= 0
        && (maskAge <= kMaskMaxReuseMs
            || (sceneStatic && maskAge <= kStaticMaskMaxMs));

    QImage mask;
    if (reusable && (sceneStatic || m_reusesSinceInfer < kMaskReuseFrames)) {
        mask = m_lastMask;
        m_reusesSinceInfer = qMin(m_reusesSinceInfer + 1, 1 << 20);
    } else {
        QElapsedTimer t;
        t.start();
        mask = m_segmenter->segment(rgba);
        // Segment is recorded per INFERENCE, not per frame — a reused-mask
        // frame spends ~0 here and recording that zero would halve the
        // metric's apparent cost without halving the work.
        talq::bg::Stats::record(talq::bg::Stats::Segment,
                                t.nsecsElapsed() / 1000);
        if (!mask.isNull()) {
            m_lastMask = mask;
            m_lastMaskClock.restart();
            m_reusesSinceInfer = 0;
            m_motionRef = sig;
        } else if (reusable) {
            mask = m_lastMask;          // transient failure, bounded cover
            m_reusesSinceInfer = qMin(m_reusesSinceInfer + 1, 1 << 20);
        }
    }

    if (mask.isNull()) {
        // Segmentation unavailable or failed (2026-07-14 review). NO mask
        // means we cannot know where the person ends and the room begins, so
        // the only frame we may emit is a fully-covered one — report failure
        // and let the engine's mosaic do that. Through 0.60.4 the segmenter
        // "helpfully" substituted a centred-gradient guess here, and the
        // resulting composite kept a sharp disc of the user's REAL ROOM in
        // the middle of the outgoing video on every ORT failure — precisely
        // on the weak machines the feature exists for.
        return QImage();
    }

    QElapsedTimer c;
    c.start();
    QImage out;

    switch (m) {
    case BackgroundEngine::Mode::Blur: {
        const float radius = static_cast<float>(blurStrength)
                              * (rgba.width() / 720.0f);
        out = m_compositor->compositeBlur(rgba, mask, radius);
        break;
    }
    case BackgroundEngine::Mode::Image: {
        // 0.60.5 — every `return rgba` in this block became `return QImage()`.
        // An Image-mode background with a missing plate must NOT resolve to
        // "show the real room"; it resolves to the mosaic, upstream.
        if (imagePath.isEmpty()) return QImage();
        if (m_cachedBgFailed && m_cachedBgPath == imagePath) return QImage();

        // 0.60.2 (2026-07-13 field RCA) — cache ONLY the camera-resolution
        // scaled plate, never the full decode. The bundled JPEGs are
        // 3200×1800 → ~23 MB as 32-bit pixels, and the old m_cachedBgRaw
        // held that for the whole session even though it was only ever
        // read to produce this scaled copy. Re-decoding from disk on the
        // two rare invalidation events (user picks a different image /
        // camera resolution change) costs one decode+scale — tens of ms,
        // on this worker thread, never the UI — instead of 23 MB forever.
        // The decode-failure latch (m_cachedBgFailed) is unchanged: a
        // missing file warns once and falls back until the path changes.
        if (m_cachedBgScaled.isNull()
            || m_cachedBgPath != imagePath
            || m_cachedBgScaledSize != rgba.size()) {
            QImage loaded(imagePath);
            if (loaded.isNull()) {
                qWarning() << "BackgroundWorker: background image missing:"
                           << imagePath
                           << "(one-shot warning; pick a different image to retry)";
                m_cachedBgFailed = true;
                m_cachedBgPath   = imagePath;
                m_cachedBgScaled = QImage();
                m_cachedBgScaledSize = QSize();
                emit backgroundImageFailed(imagePath);
                return QImage();
            }
            m_cachedBgScaled = loaded.scaled(
                rgba.size(),
                Qt::KeepAspectRatioByExpanding,
                Qt::SmoothTransformation);
            m_cachedBgScaledSize = rgba.size();
            m_cachedBgPath   = imagePath;
            m_cachedBgFailed = false;
            // `loaded` (the ~23 MB full-resolution decode) is freed HERE,
            // at end of scope. Note: each re-scale produces a fresh QImage
            // with a new cacheKey, which is exactly what makes the
            // compositor re-upload its m_texBg GPU texture (it keys on
            // cacheKey); on unchanged frames the same QImage is reused and
            // no re-upload happens.
        }
        out = m_compositor->compositeImage(rgba, mask, m_cachedBgScaled);
        break;
    }
    case BackgroundEngine::Mode::None:
    case BackgroundEngine::Mode::Video:
    case BackgroundEngine::Mode::VideoStream:
        return QImage();
    }

    talq::bg::Stats::record(talq::bg::Stats::Composite,
                            c.nsecsElapsed() / 1000);
    talq::bg::Stats::record(talq::bg::Stats::WorkerTotal,
                            whole.nsecsElapsed() / 1000);
    return out;
}

void BackgroundWorker::releaseEngineResources()
{
    // THREADING (the hazard here): this runs on the WORKER thread —
    // applyMode() and shutdown() are only ever reached via queued /
    // blocking-queued invocations onto this object, whose affinity is the
    // worker thread. That is the thread that owns the QOpenGLContext, so
    // ~BackgroundCompositor → releaseAll() → makeCurrent() + the
    // QOpenGLFramebufferObject / QOpenGLTexture destructors are legal
    // here; deleting them from any other thread is undefined. The
    // engine-owned QOffscreenSurface is deliberately NOT touched: it was
    // created on the GUI thread (QOffscreenSurface::create() requires
    // that on Windows — see BackgroundEngine's ctor comment) and moved
    // here exactly once; keeping it alive lets a later re-enable rebuild
    // the GL context on it with zero cross-thread surface gymnastics.
    if (!m_compositor && !m_segmenter) return;   // already off — idempotent

    // Drop the ready gate FIRST. processFrame() (streaming thread / UI
    // preview) checks isReady() before dispatching to us, so after this
    // release-store no NEW process() call can be queued with a stale ON
    // mode; anything already in the event queue ahead of us has run (FIFO).
    m_ready.store(false, std::memory_order_release);

    delete m_compositor; m_compositor = nullptr;   // GL ctx + FBOs + textures + shaders
    delete m_segmenter;  m_segmenter  = nullptr;   // ORT session + arenas + model copy
    invalidateImageCache();                        // scaled background plate
    m_lastMask  = QImage();                        // no mask outlives its session
    m_motionRef = QImage();
    m_reusesSinceInfer = 0;
    m_nextComposeDueMs = 0;                        // a re-enable composes immediately
    m_prewarmedSize = QSize();                     // and re-warms its GL sizing

    // 0.60.5 — and drop both mailbox slots. Each holds a full camera frame
    // (3.7 MB at 720p), and in the spirit of the 0.60.2 retention RCA, OFF
    // means OFF. It also guarantees no composite made before the release can
    // be served after it.
    if (m_box) {
        { QMutexLocker lk(&m_box->inMutex);  m_box->inFrame  = QImage(); }
        {
            QMutexLocker lk(&m_box->outMutex);
            m_box->outFrame   = QImage();
            m_box->outSeq     = 0;
            m_box->outStampMs = -1;
        }
    }

    talq::bg::Stats::logSummary("at background-engine release");

    qInfo() << "BackgroundWorker: background engine released (GL compositor"
               " + ONNX session freed; re-enable pays a cold init)";
}

void BackgroundWorker::shutdown()
{
    // Same teardown as applyMode(None); kept as a separate entry point
    // because the engine dtor calls it via BlockingQueuedConnection to
    // guarantee completion before quitting the thread.
    releaseEngineResources();
}
