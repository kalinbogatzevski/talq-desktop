#include "BackgroundEngine.h"
#include "BackgroundWorker.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QImage>
#include <QMutexLocker>
#include <QOffscreenSurface>
#include <QSurfaceFormat>
#include <QThread>

#include <algorithm>
#include <cstdlib>
#include <vector>

// 0.40.1 — the engine is a thin facade; the heavy lifting (BackgroundCompositor
// + TfliteSegmenter, both with thread-affine GL / ORT state) lives on a
// dedicated QThread inside BackgroundWorker. See BackgroundEngine.h for the
// threading rationale and for the 0.60.5 non-blocking / fail-closed contract.

// ---------------------------------------------------------------------------
// 0.60.5 instrumentation. See the Stats note in BackgroundEngine.h.
// ---------------------------------------------------------------------------
namespace talq::bg {
namespace {

// 150 samples ≈ 5 s at 30 fps — long enough for a stable p95, short enough
// that a stall shows up in the next summary instead of being averaged away.
constexpr int    kWindow        = 150;
constexpr double kFrameBudgetMs = 33.3;    // 30 fps, the production cap
constexpr int    kSummaryMs     = 5000;
constexpr int    kWarnMs        = 30000;   // over-budget warnings, rate-limited

const char *const kNames[Stats::MetricCount] = {
    "cameraHold", "bridgeTotal", "segment", "composite", "workerTotal"
};

struct Ring {
    qint64 v[kWindow] = {};
    int    next   = 0;
    int    filled = 0;
    void push(qint64 us) {
        v[next] = us;
        next = (next + 1) % kWindow;
        if (filled < kWindow) ++filled;
    }
};

QMutex               g_mutex;
Ring                 g_rings[Stats::MetricCount];
std::atomic<quint64> g_dropped{0};
std::atomic<quint64> g_failClosed{0};
std::atomic<quint64> g_rateCapped{0};
QElapsedTimer        g_sinceSummary;
QElapsedTimer        g_sinceWarn;
bool                 g_timersStarted = false;
bool                 g_warnedOnce    = false;

// Percentile in MILLISECONDS from a ring of microsecond samples.
// Caller holds g_mutex.
double pctMs(const Ring &r, double p)
{
    if (r.filled == 0) return 0.0;
    std::vector<qint64> s(r.v, r.v + r.filled);
    std::sort(s.begin(), s.end());
    auto idx = static_cast<size_t>(p * (s.size() - 1));
    return double(s[idx]) / 1000.0;
}

// Caller holds g_mutex.
QString summaryLine()
{
    QString out;
    for (int m = 0; m < Stats::MetricCount; ++m) {
        const Ring &r = g_rings[m];
        if (r.filled == 0) continue;
        out += QStringLiteral("%1 p50 %2 p95 %3 max %4 | ")
                   .arg(QLatin1String(kNames[m]))
                   .arg(pctMs(r, 0.50), 0, 'f', 1)
                   .arg(pctMs(r, 0.95), 0, 'f', 1)
                   .arg(pctMs(r, 1.00), 0, 'f', 1);
    }
    out += QStringLiteral("dropped %1 | capped %2 | fail-closed %3 (ms; window %4 frames)")
               .arg(g_dropped.load(std::memory_order_relaxed))
               .arg(g_rateCapped.load(std::memory_order_relaxed))
               .arg(g_failClosed.load(std::memory_order_relaxed))
               .arg(g_rings[Stats::CameraHold].filled);
    return out;
}

} // namespace

bool Stats::verbose()
{
    static const bool on = [] {
        const char *e = std::getenv("TALQ_DEBUG_BG");
        return e && *e == '1';
    }();
    return on;
}

void Stats::record(Metric m, qint64 us)
{
    if (m < 0 || m >= MetricCount) return;
    QMutexLocker lk(&g_mutex);
    if (!g_timersStarted) {
        g_sinceSummary.start();
        g_sinceWarn.start();
        g_timersStarted = true;
    }
    g_rings[m].push(us);
}

void Stats::noteDropped()    { g_dropped.fetch_add(1, std::memory_order_relaxed); }
void Stats::noteFailClosed() { g_failClosed.fetch_add(1, std::memory_order_relaxed); }
void Stats::noteRateCapped() { g_rateCapped.fetch_add(1, std::memory_order_relaxed); }
quint64 Stats::dropped()     { return g_dropped.load(std::memory_order_relaxed); }
quint64 Stats::failClosed()  { return g_failClosed.load(std::memory_order_relaxed); }
quint64 Stats::rateCapped()  { return g_rateCapped.load(std::memory_order_relaxed); }

void Stats::maybeLog()
{
    QString periodic;
    QString overBudget;
    {
        QMutexLocker lk(&g_mutex);
        if (!g_timersStarted) return;

        if (verbose() && g_sinceSummary.elapsed() >= kSummaryMs) {
            g_sinceSummary.restart();
            periodic = summaryLine();
        }

        // Loud, rate-limited, and NOT gated on TALQ_DEBUG_BG: a shipped build
        // that misses the frame budget has to leave evidence in the log
        // without the user first knowing to set an env var. Two distinct
        // failures, deliberately worded differently:
        //   cameraHold over budget => the blocking hop is BACK (a freeze bug).
        //   workerTotal over budget => the background frame rate sags (a
        //   perf bug: correct output, fewer updates). Only the first is a
        //   regression of the 0.60.5 fix.
        //
        // The FIRST warning fires as soon as there is a full window to judge
        // (~5 s of video), not kWarnMs later: a 30 s wait would have meant a
        // 20 s call that stuttered the whole way through left nothing in the
        // log at all. Subsequent ones are rate-limited so a persistently slow
        // box doesn't flood it.
        const bool firstJudgeable = !g_warnedOnce
                                 && g_rings[WorkerTotal].filled >= kWindow;
        if (firstJudgeable || g_sinceWarn.elapsed() >= kWarnMs) {
            // Mark judged + re-arm even when nothing is wrong, so the healthy
            // path doesn't re-sort both rings on every single frame.
            g_warnedOnce = true;
            g_sinceWarn.restart();

            const double holdP95   = pctMs(g_rings[CameraHold],  0.95);
            const double workerP95 = pctMs(g_rings[WorkerTotal], 0.95);
            if (holdP95 > 10.0) {
                overBudget = QStringLiteral(
                    "BG: the camera streaming thread is BLOCKING for %1 ms p95 "
                    "(expected ~2 ms). The non-blocking mailbox is not doing its "
                    "job - this is the 0.40.7 / 0.60.4 FREEZE signature. %2")
                    .arg(holdP95, 0, 'f', 1).arg(summaryLine());
            } else if (workerP95 > kFrameBudgetMs) {
                overBudget = QStringLiteral(
                    "BG: worker over the %1 ms frame budget (p95 %2 ms) - the "
                    "background updates slower than the camera and frames fall back "
                    "to the last good composite. Video is fine; the effect is just "
                    "coarser. NOT a freeze. %3")
                    .arg(kFrameBudgetMs, 0, 'f', 1)
                    .arg(workerP95, 0, 'f', 1).arg(summaryLine());
            }
        }
    }
    if (!periodic.isEmpty())   qInfo().noquote()    << "BG timing:" << periodic;
    if (!overBudget.isEmpty()) qWarning().noquote() << overBudget;
}

void Stats::logSummary(const char *why)
{
    QString line;
    {
        QMutexLocker lk(&g_mutex);
        if (g_rings[CameraHold].filled == 0 && g_rings[BridgeTotal].filled == 0)
            return;   // background never ran this call — nothing to say
        line = summaryLine();
    }
    qInfo().noquote() << "BG timing" << (why ? why : "") << "-" << line;
}

void Stats::reset()
{
    QMutexLocker lk(&g_mutex);
    for (auto &r : g_rings) r = Ring{};
    g_dropped.store(0, std::memory_order_relaxed);
    g_failClosed.store(0, std::memory_order_relaxed);
    g_rateCapped.store(0, std::memory_order_relaxed);
}

} // namespace talq::bg

BackgroundEngine::BackgroundEngine(QObject *parent)
    : QObject(parent)
{
    // Pre-create the QOffscreenSurface on whatever thread constructed
    // the engine. Qt requires QOffscreenSurface::create() to be called
    // on the GUI thread on most platforms (Windows, Wayland), so we do
    // it here BEFORE moving the surface to the worker thread. The
    // QOpenGLContext built on top of this surface inside the worker can
    // then call makeCurrent() from the worker thread without issue.
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setMajorVersion(3);
    fmt.setMinorVersion(3);
    fmt.setDepthBufferSize(0);
    fmt.setStencilBufferSize(0);

    m_surface = new QOffscreenSurface;
    m_surface->setFormat(fmt);
    m_surface->create();
    if (!m_surface->isValid()) {
        qWarning() << "BackgroundEngine: QOffscreenSurface::create() failed"
                      " — background effects will be no-ops this session";
        delete m_surface; m_surface = nullptr;
        return;
    }

    // m_thread is NOT parented to `this`. With QObject parent ownership,
    // ~QObject would delete the thread as part of child cleanup AFTER
    // our dtor body finishes — and if wait() timed out the thread would
    // still be running, tripping the "QThread: Destroyed while thread
    // is still running" UB warning. We own m_thread manually instead.
    m_thread = new QThread;
    m_thread->setObjectName(QStringLiteral("bg-engine-worker"));

    m_box = std::make_shared<BgFrameMailbox>();

    m_worker = new BackgroundWorker(m_surface, m_box);   // constructed on caller's thread
    m_worker->moveToThread(m_thread);
    m_surface->moveToThread(m_thread);

    // Forward signals from worker → engine on the engine's thread.
    connect(m_worker, &BackgroundWorker::engineDisabled,
            this, &BackgroundEngine::engineDisabled);
    connect(m_worker, &BackgroundWorker::backgroundImageFailed,
            this, &BackgroundEngine::backgroundImageFailed);

    // We do not rely on QThread::finished + deleteLater for worker
    // teardown: deleteLater() posts onto the worker thread's event
    // loop, which has just finished, so the event would never be
    // processed and the worker would leak. The dtor below deletes
    // m_worker explicitly after wait() returns.

    m_thread->start();
}

BackgroundEngine::~BackgroundEngine()
{
    if (m_thread) {
        if (m_worker) {
            // Release GL + ORT resources on the worker thread BEFORE the
            // thread exits. BlockingQueuedConnection so we wait for the
            // shutdown to complete.
            QMetaObject::invokeMethod(m_worker, "shutdown",
                                       Qt::BlockingQueuedConnection);
        }
        m_thread->quit();
        const bool clean = m_thread->wait(5000);
        if (!clean) {
            // The worker thread is still alive. We can't safely delete
            // the worker, the surface, or the thread itself — touching
            // any of them from this thread risks UAF. Leak deliberately
            // and surface the failure; a clean shutdown should always
            // complete inside 5 s.
            qCritical() << "BackgroundEngine: worker thread did not exit"
                           " within 5 s — leaking worker + surface to"
                           " avoid use-after-free";
            m_thread = nullptr;     // intentional leak; thread still owns its objects
            m_worker  = nullptr;
            m_surface = nullptr;
            return;
        }
        delete m_worker;  m_worker  = nullptr;
        delete m_thread;  m_thread  = nullptr;
    }
    // Surface lives on a now-stopped thread; move it back to ours so
    // ~QObject runs on the right thread. Stale affinity here would
    // trip a Qt warning on Windows debug builds.
    if (m_surface) {
        m_surface->moveToThread(QThread::currentThread());
        delete m_surface;
        m_surface = nullptr;
    }
}

bool BackgroundEngine::isReady() const
{
    return m_worker != nullptr && m_thread && m_thread->isRunning();
}

void BackgroundEngine::setMode(Mode m)
{
    const Mode prev = m_modeAtomic.exchange(m, std::memory_order_relaxed);
    if (prev != m) {
        // Drop the completed composite. It was made under the OLD mode, and
        // the output slot is what processFrame hands to the encoder: without
        // this, one frame of the previous background (or, after an Off→On
        // toggle, a composite from minutes ago) would go out on the wire
        // before the first new composite lands. Clearing costs one mosaic
        // frame instead. resetMailbox also bumps the epoch, so a compose
        // already in flight under the old mode is discarded at publish.
        resetMailbox();
    }
    if (!m_worker) return;
    // Kick the worker so the compositor + segmenter exist before the
    // first frame arrives. Queued so the caller (Qt main) never blocks
    // on the worker for a settings change.
    QMetaObject::invokeMethod(m_worker, "applyMode",
                              Qt::QueuedConnection,
                              Q_ARG(int, int(m)));
}

void BackgroundEngine::prewarm()
{
    const Mode m = m_modeAtomic.load(std::memory_order_relaxed);
    if (m == Mode::None || !m_worker) return;
    // applyMode is idempotent — if the compositor + segmenter are already up
    // this is a no-op on the worker's event loop. Since 0.60.5 it also runs
    // the full GL warm-up (context, shaders, FBOs at the production cap, one
    // throwaway compose) — see BackgroundWorker::prewarmResources.
    QMetaObject::invokeMethod(m_worker, "applyMode",
                              Qt::QueuedConnection,
                              Q_ARG(int, int(m)));
}

quint64 BackgroundEngine::framesComposited() const
{
    return m_box ? m_box->completed.load(std::memory_order_relaxed) : 0;
}

quint64 BackgroundEngine::framesDropped() const
{
    return m_box ? m_box->dropped.load(std::memory_order_relaxed) : 0;
}

void BackgroundEngine::setBlurStrength(int s)
{
    m_blurStrengthAtomic.store(qBound(1, s, 20), std::memory_order_relaxed);
}

void BackgroundEngine::setImagePath(const QString &p)
{
    {
        QMutexLocker locker(&m_pathMutex);
        if (p == m_imagePath) return;
        m_imagePath = p;
    }
    // Same reason as setMode: the composite sitting in the output slot was
    // made against the PREVIOUS plate, and it would be pushed once more
    // before the first composite against the new one lands.
    resetMailbox();
    if (!m_worker) return;
    QMetaObject::invokeMethod(m_worker, "invalidateImageCache",
                              Qt::QueuedConnection);
}

void BackgroundEngine::resetMailbox()
{
    if (!m_box) return;
    // Bump the epoch FIRST: a worker publish that raced past the slot-clear
    // below still fails its epoch gate (BackgroundWorker::processMailbox),
    // so pre-boundary pixels cannot be resurrected under a fresh stamp.
    m_box->epoch.fetch_add(1, std::memory_order_acq_rel);
    { QMutexLocker lk(&m_box->inMutex);  m_box->inFrame = QImage(); }
    {
        QMutexLocker lk(&m_box->outMutex);
        m_box->outFrame   = QImage();
        m_box->outSeq     = 0;
        m_box->outStampMs = -1;
    }
}

QString BackgroundEngine::imagePath() const
{
    QMutexLocker locker(&m_pathMutex);
    return m_imagePath;
}

QImage BackgroundEngine::mosaicFallback(const QImage &rgba)
{
    // Downscale by 16, then smooth-scale back. Everything that identifies a
    // room — faces, screens, text, the whiteboard behind you — is gone at
    // 1/16 linear resolution, and the two Qt scales cost ~1 ms at 720p with
    // no GL context and no ORT session, so this works on the streaming thread
    // on the very first frame. That is the whole point: it is the ONE
    // fallback that is always available, so no failure anywhere in the engine
    // ever has an excuse to reveal the real background.
    const int w = qMax(1, rgba.width()  / 16);
    const int h = qMax(1, rgba.height() / 16);
    return rgba.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
               .scaled(rgba.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QImage BackgroundEngine::processFrame(const QImage &rgba)
{
    // Off-mode: verbatim pass-through, no worker, no copy. The user asked for
    // no background effect, so the real frame IS the correct output.
    // PublishPipeline short-circuits this case before calling us; the
    // Settings preview (BgPreviewSource) still arrives here.
    const Mode m = m_modeAtomic.load(std::memory_order_relaxed);
    if (m == Mode::None || rgba.isNull()) return rgba;

    // From here on the mode is ON, which means: whatever happens below, we do
    // not return `rgba`. Not on cold init, not on a dead worker, not on an
    // engine that never came up. See the privacy note in BackgroundEngine.h.
    QElapsedTimer hold;
    hold.start();

    if (!m_worker || !m_box) {
        // The engine never came up at all (QOffscreenSurface::create() failed
        // in the ctor — no GL platform). Mode is ON, so cover the frame.
        talq::bg::Stats::noteFailClosed();
        const QImage safe = mosaicFallback(rgba);
        talq::bg::Stats::record(talq::bg::Stats::CameraHold, hold.nsecsElapsed() / 1000);
        return safe;
    }

    // ---- Submit. Drop-oldest: a frame the worker never got to is simply
    // overwritten by the newer one. We never wait for it to catch up. ----
    quint64 seq = 0;
    quint64 submitEpoch = 0;
    {
        QMutexLocker lk(&m_box->inMutex);
        if (!m_box->inFrame.isNull()) {
            // The worker is still busy with an earlier frame. Overwrite.
            m_box->dropped.fetch_add(1, std::memory_order_relaxed);
            talq::bg::Stats::noteDropped();
        }
        // Shallow copy — QImage is implicitly shared with an atomic refcount,
        // and the worker only ever READS this buffer. 0.60.6: the caller
        // (PublishPipeline) no longer detaches from the GstBuffer — the
        // QImage OWNS the mapped sample via a cleanup function, so the
        // pixels stay valid for exactly as long as any ref (this slot, the
        // worker's in-flight frame) exists, and the camera buffer is
        // returned the moment the last ref drops.
        m_box->inFrame = rgba;
        m_box->inMode  = int(m);
        m_box->inBlur  = m_blurStrengthAtomic.load(std::memory_order_relaxed);
        {
            QMutexLocker plk(&m_pathMutex);
            m_box->inPath = m_imagePath;
        }
        seq = m_box->inSeq = ++m_submitSeq;
        submitEpoch = m_box->inEpoch =
            m_box->epoch.load(std::memory_order_acquire);
    }
    // Queued, never blocking. A kick that finds the slot already drained
    // returns immediately, so kicks cannot pile up faster than the worker
    // discards them.
    QMetaObject::invokeMethod(m_worker, "processMailbox", Qt::QueuedConnection);

    // ---- Take whatever is READY. Wait for nothing. ----
    QImage  out;
    quint64 outSeq = 0;
    quint64 outEpoch = 0;
    qint64  outStampMs = -1;
    {
        QMutexLocker lk(&m_box->outMutex);
        out        = m_box->outFrame;
        outSeq     = m_box->outSeq;
        outEpoch   = m_box->outEpoch;
        outStampMs = m_box->outStampMs;
    }

    // Re-pushing the last good composite is the DESIGNED behaviour when the
    // worker is merely behind: the encoder sees an identical frame and emits
    // an all-skip P-frame (near-zero bitrate), and the output frame rate stays
    // flat, which rtpgccbwe and the receiver's jitter buffer both prefer to a
    // sagging one.
    //
    // But a composite older than the staleness bound means the worker is not
    // behind, it is STUCK (wedged GL driver, hung ORT) — or, worse, that the
    // producer PAUSED and resumed. Past the bound we switch to the mosaic,
    // which is live, content-free, and computed right here without the
    // worker's help.
    //
    // Staleness is WALL-CLOCK, not submissions (2026-07-14 review). The
    // original bound was "30 submissions ≈ 1 s at 30 fps", an equivalence
    // that is false across any gap in submissions — and gaps are real:
    // CallManager keeps one engine across CALLS, video mute stops the camera
    // mid-call, the Settings preview stops and the call starts. Counted in
    // submissions, the last composite of the previous call aged exactly 1 at
    // the start of the next one and went out on the wire to a different
    // audience. The submission bound stays as a second opinion (it also
    // catches a wedged worker under continuous submission); the epoch check
    // makes "this composite belongs to the current producer session"
    // structural rather than statistical.
    constexpr quint64 kMaxStaleFrames = 30;    // secondary bound, see above
    constexpr qint64  kMaxStaleMs     = 1000;  // THE bound: 1 s of wall time

    // outSeq can legitimately be NEWER than the frame we just submitted: the
    // Settings dialog deliberately shares one engine between the preview and
    // the call (SettingsDialog.h), so two threads submit into the same mailbox
    // and the worker may already have published the other one's later frame.
    // That composite is fresher, not staler — age 0. Computing `seq - outSeq`
    // unsigned in that case would underflow to ~2^64 and silently force the
    // mosaic on a perfectly good frame.
    const quint64 age   = (outSeq >= seq) ? 0 : (seq - outSeq);
    const qint64  ageMs = (outStampMs >= 0)
                        ? (m_box->clock.elapsed() - outStampMs) : -1;

    const bool usable = !out.isNull()
                     && outSeq != 0
                     && out.size() == rgba.size()          // resolution changed mid-call
                     && age <= kMaxStaleFrames
                     && ageMs >= 0 && ageMs <= kMaxStaleMs
                     && outEpoch == submitEpoch;           // same producer session
    if (!usable) {
        talq::bg::Stats::noteFailClosed();
        out = mosaicFallback(rgba);
    }

    talq::bg::Stats::record(talq::bg::Stats::CameraHold, hold.nsecsElapsed() / 1000);
    return out;
}
