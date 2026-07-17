#pragma once

// #20 0.40.1 hotfix — owns BackgroundCompositor + TfliteSegmenter on a
// dedicated QThread so the GL + ORT work that BackgroundEngine::processFrame
// performs runs OFF the Qt main thread.
//
// The 0.40.0 stable shipped with BackgroundEngine doing its GL work on Qt
// main, and PublishPipeline's onBgSample hopped from the GStreamer
// streaming thread to Qt main via Qt::BlockingQueuedConnection. Any call
// from the streaming thread starving Qt main (or Qt main doing anything
// that needed the GST stream lock while the streaming thread was parked
// on it) would freeze the UI. With this worker, the streaming thread
// blocks only on a thread dedicated to GL work — Qt main is never
// involved in the per-frame critical path.
//
// Threading contract:
//   * Constructed on the Qt main thread.
//   * The owning BackgroundEngine calls `moveToThread(workerThread)` on
//     this worker (and on its QOffscreenSurface) before processFrame is
//     ever invoked. After that, every method runs on the worker thread.
//   * `processMailbox()` is the invokable entry point, reached via a
//     QUEUED invokeMethod (0.60.5 — it was a BlockingQueuedConnection
//     `process(...)` through 0.60.4, which is what pinned the camera
//     pad's stream lock for 58 ms a frame; see BackgroundEngine.h).
//     Nobody ever waits for it. It takes the newest frame out of the
//     shared mailbox, composites it, and publishes the result back into
//     the mailbox's output slot.
//   * `shutdown()` releases GL resources on the worker thread; the
//     engine calls it via BlockingQueuedConnection before quitting the
//     thread.

#include <QElapsedTimer>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSize>
#include <QString>

#include <atomic>
#include <memory>

class BackgroundCompositor;
class TfliteSegmenter;
class QOffscreenSurface;

// Defined in BackgroundEngine.h (the facade). Forward-declared with its fixed
// underlying type so this header — included before BackgroundEngine.h in
// BackgroundWorker.cpp — can name it in the failCompose() signature without a
// full include. BgFrameMailbox stores it as a plain int to stay independent.
enum class BgFailReason : int;

// 0.60.5 — the frame mailbox shared between the camera streaming thread
// (producer, via BackgroundEngine::processFrame) and the worker thread
// (consumer). Two independent one-slot buffers, each with its own mutex,
// so the producer never waits on the consumer:
//
//   INPUT  — drop-oldest. A frame submitted while the worker is still busy
//            OVERWRITES the one waiting. This is the first real backpressure
//            this path has ever had: the appsink's drop=TRUE/max-buffers=2
//            (PublishPipeline) cannot drop anything, because we pull
//            synchronously inside `new-sample` on the producing thread, so
//            its queue never reaches its bound. It was always false comfort.
//
//   OUTPUT — most recently COMPLETED composite, plus the sequence number of
//            the frame it came from, the WALL-CLOCK time it was published,
//            and the mailbox epoch it belongs to. Only ever written with a
//            REAL composite; see the fail-closed rule in
//            BackgroundWorker::processMailbox.
//
// STALENESS + EPOCH (2026-07-14 adversarial review). The original design
// aged the output slot in SUBMISSIONS ("30 frames ≈ 1 s at 30 fps"), which
// is only true while submissions are continuous. Across a GAP they are not:
// CallManager keeps ONE engine alive across calls, so the slot still held
// the FINAL COMPOSITE OF THE PREVIOUS CALL when the next one started — the
// first frame of call 2 submitted seq N+1 against outSeq N, the "age" came
// out as 1, and pixels captured for one audience (the user's sharp face and
// whatever the mask classed as person) were transmitted to a different one
// with no consent event in between. Mute→unmute had the same shape. Hence:
//   * outStampMs — publish time on the mailbox's own monotonic clock; the
//     consumer refuses anything older than a wall-clock bound, gap or not.
//   * epoch — bumped by BackgroundEngine::resetMailbox() at every producer
//     continuity break (mode/image change, call teardown, camera enable,
//     preview stop). The worker echoes the epoch it took from the input
//     slot; a publish whose epoch is no longer current is discarded, so a
//     compose in flight ACROSS the boundary cannot resurrect pre-boundary
//     pixels with a fresh timestamp.
//
// Held by shared_ptr on both sides — the engine dtor deliberately leaks a
// worker whose thread refuses to exit, and that worker must not be left
// holding a dangling mailbox.
struct BgFrameMailbox
{
    // Monotonic timebase for outStampMs. Started ONCE here and never
    // restarted — QElapsedTimer::elapsed() is safe for concurrent readers
    // only while nobody re-arms it.
    BgFrameMailbox() { clock.start(); }
    QElapsedTimer clock;

    QMutex  inMutex;
    QImage  inFrame;              // newest submitted frame; null = nothing pending
    int     inMode = 0;           // BackgroundEngine::Mode as int
    int     inBlur = 10;
    QString inPath;
    quint64 inSeq  = 0;           // sequence number of inFrame
    quint64 inEpoch = 0;          // mailbox epoch captured at submit time

    QMutex  outMutex;
    QImage  outFrame;             // newest completed composite; null until the first lands
    quint64 outSeq = 0;           // inSeq of the frame outFrame was composited from
    qint64  outStampMs = -1;      // clock.elapsed() at publish; -1 = never published
    quint64 outEpoch = 0;         // epoch outFrame was composited under

    // Current epoch. Written only by BackgroundEngine::resetMailbox();
    // read by the producer (submit) and the worker (publish gate).
    std::atomic<quint64> epoch{0};

    std::atomic<quint64> completed{0};
    std::atomic<quint64> dropped{0};

    // 0.60.6 P0-d (Petia's 0.60.5 foggy-window RCA) — content-free mosaics the
    // WORKER published into the output slot while composeLatched (see below).
    // NOT counted in `completed`: a mosaic is a cover, not a composite, so the
    // zero-composite detector still reads structural failure. Proves the camera
    // thread stopped recomputing the inline mosaic (ground-truth #7).
    std::atomic<quint64> mosaicked{0};

    // 0.60.6 (Petia's 0.60.5 foggy-window RCA) — the persistent-failure
    // ladder's shared state. lastFailReason holds the BgFailReason (as int)
    // of the most recent compose() outcome; composeLatched flips true once the
    // worker has given up after a run of consecutive STRUCTURAL failures.
    // Worker writes, engine + CallManager read; relaxed atomics, off the
    // camera hot path (the camera thread only swaps the in/out slots).
    std::atomic<int>  lastFailReason{0};   // BgFailReason::None
    std::atomic<bool> composeLatched{false};
};

class BackgroundWorker : public QObject
{
    Q_OBJECT

public:
    // Constructed on the Qt main thread. The surface is created on Qt
    // main by BackgroundEngine (QOffscreenSurface::create() requires the
    // GUI thread on most platforms) and then moved to the worker
    // thread together with this worker. `box` is the mailbox shared with
    // the engine (see BgFrameMailbox above).
    explicit BackgroundWorker(QOffscreenSurface *surface,
                              std::shared_ptr<BgFrameMailbox> box,
                              QObject *parent = nullptr);
    ~BackgroundWorker() override;

public slots:
    // 0.60.5 — asynchronous entry point, reached via a QUEUED invokeMethod.
    // Nobody waits for it. Takes the newest frame from the mailbox's input
    // slot, composites it, and publishes the result into the output slot.
    // Extra kicks that find an empty slot return in microseconds, so a
    // backlog of kicks from a producer running ahead of us is self-limiting.
    void processMailbox();

    // Called on EVERY engine setMode(). OFF→ON lazy-constructs compositor +
    // segmenter on the worker thread so their GL/ORT resources are born
    // with the worker affinity. ON→OFF (Mode::None) RELEASES them again
    // (0.60.2, 2026-07-13 field RCA): the old code early-returned on None,
    // so the ORT session + GL context + FBOs/textures lived until process
    // exit — measured ~112 MB retained after the user turned the
    // background off (256 MB steady → 401 MB with the engine up, only
    // 29 MB returned on OFF).
    // 0.60.5 — an ON applyMode also FINISHES the cold init right here
    // (prewarmResources with the production-cap default size) instead of
    // leaving the GL context + shader compile + FBO alloc + first-use
    // driver work to the first camera frame. Constructing the objects but
    // deferring their GL init was the missing half of the 0.60.5 prewarm:
    // the ORT session was warm before the first frame, but the first
    // composite still paid 200 ms - 1 s of GL cold start ON the call path
    // (~27 frames served as the mosaic at every call start).
    void applyMode(int modeInt);

    // 0.60.5 — build EVERYTHING the first composite needs, now: GL context,
    // shader compile, geometry, the frame-sized FBOs + PBO pair, and one
    // throwaway compose to warm first-use driver paths. Idempotent per
    // size; ~0 cost when already warm. The dummy pixels are synthetic and
    // are NEVER published: the compose result is discarded and the parked
    // async readback is dropped (discardPendingReadback), so they cannot
    // resurface as a "previous composite" later. Reached from applyMode at
    // 1280×720 (the camera menu negotiates ≤720p, so that IS the production
    // frame size in the common case; a smaller negotiated mode costs one
    // FBO re-alloc on the first real frame — a few ms, same as before this
    // existed). Kept invokable so a future caller that knows the real
    // negotiated size can refine the warm.
    void prewarmResources(int width, int height);

    // Drop the image cache when the user picks a different path (so a
    // same-pixel-size replacement still re-reads from disk).
    void invalidateImageCache();

    // Release GL + ORT resources on the worker thread, called from the
    // engine destructor via BlockingQueuedConnection before quitting.
    // Same teardown applyMode(None) performs (releaseEngineResources).
    void shutdown();

public:
    // Cross-thread readable flag, set true by applyMode after the
    // compositor + segmenter are fully constructed (the ORT session in
    // particular is the multi-second cold-init).
    //
    // 0.60.5 — this is now a pure diagnostic. It used to gate
    // BackgroundEngine::processFrame, which PASSED THE RAW CAMERA FRAME
    // THROUGH while we were still initialising (0.40.9). That was framed as
    // "the user briefly sees raw camera in the PiP" — but the frame goes
    // through the tee to the encoder, so for the whole 200 ms - 1.07 s
    // cold-init window every peer received a sharp, fully-decodable image of
    // the room the user had turned blur on to hide. The engine no longer
    // consults this to decide whether to reveal anything: it composites, or
    // it falls back to the content-free mosaic. Nothing else.
    bool isReady() const { return m_ready.load(std::memory_order_acquire); }

signals:
    // Re-emitted by the engine. Cross-thread connection, queued to UI.
    void engineDisabled(const QString &reason);
    void backgroundImageFailed(const QString &path);

private:
    // Free the compositor (GL context, FBOs, textures, shaders) + the
    // segmenter (ORT session) + the image cache ON THE WORKER THREAD.
    // Called by applyMode(Mode::None) and shutdown(). Idempotent. The
    // engine-owned QOffscreenSurface is deliberately left alive — see the
    // body for the threading rationale.
    void releaseEngineResources();

    // The composite itself. Returns a NULL QImage on every failure path —
    // never the input frame. Callers must treat null as "cover this frame",
    // not as "send it as-is". Runs on the worker thread.
    QImage compose(const QImage &rgba, int modeInt,
                   int blurStrength, const QString &imagePath);

    // 0.60.6 — record a compose failure and return a NULL QImage (so a call
    // site can `return failCompose(reason)`): stash the reason on the mailbox
    // for the ladder's detector, advance the structural-failure streak, and at
    // kComposeFailLatch latch the effect off (stop the doomed ORT+GL work and
    // emit engineDisabled once). Worker thread only.
    QImage failCompose(BgFailReason reason);

    std::shared_ptr<BgFrameMailbox> m_box;            // shared with the engine
    QOffscreenSurface     *m_surface     = nullptr;   // not owned (engine)
    BackgroundCompositor  *m_compositor  = nullptr;   // owned (parent=this)
    TfliteSegmenter       *m_segmenter   = nullptr;   // owned (parent=this)

    // Image-mode cache. Lives here because only the worker reads/writes
    // it — the engine never touches QImage state directly anymore.
    // 0.60.2 (2026-07-13 field RCA): only the camera-resolution SCALED
    // plate is cached. The full-resolution decode (bundled JPEGs are
    // 3200×1800 → ~23 MB as 32-bit pixels) used to be retained here
    // (m_cachedBgRaw) for the whole session even though it was only ever
    // read to produce the scaled copy; we now re-decode from disk on the
    // rare invalidation events (new image picked / camera resolution
    // change) instead of carrying 23 MB.
    QString m_cachedBgPath;
    QImage  m_cachedBgScaled;
    QSize   m_cachedBgScaledSize;
    bool    m_cachedBgFailed = false;

    // 0.60.6 — mask decimation state (see compose()). A silhouette does
    // not change meaningfully in 33 ms, so inference runs on every SECOND
    // composed frame and the frame in between reuses the last mask; the
    // composite itself still runs every frame. Reuse is bounded BOTH ways:
    // at most kMaskReuseFrames consecutive reuses AND never a mask older
    // than kMaskMaxReuseMs of wall clock — the frame-count bound alone
    // would be the same "counted, not timed" mistake the 2026-07-14
    // staleness review threw out of the mailbox. m_lastMask only ever
    // holds a REAL segmenter output (never a guess), so reuse can not
    // weaken the fail-closed contract: no mask ever segmented ⇒ nothing
    // to reuse ⇒ compose fails ⇒ mosaic. Worker thread only.
    //
    // m_motionRef extends reuse for a STATIC scene: a point-sampled 32×18
    // signature of the frame the mask was inferred from. Each frame's own
    // signature is compared against it BEFORE deciding to reuse, so the
    // very frame where motion appears pays a fresh inference — and because
    // the comparison is against the inference-time reference (not the
    // previous frame), slow drift ACCUMULATES until it trips the gate
    // instead of sneaking under a per-frame threshold. This is what stops
    // inference burning ~0.5 of a core while the user just sits in a call
    // (Kalin's 2026-07-13 zenbook report).
    QImage        m_lastMask;              // 256×256 Grayscale8, EMA-smoothed
    QElapsedTimer m_lastMaskClock;         // age of m_lastMask
    int           m_reusesSinceInfer = 0;
    QImage        m_motionRef;             // 32×18 signature at last inference
    quint64       m_lastSeenEpoch    = 0;  // pipeline-flush edge detector
    QElapsedTimer m_sinceLastCompose;      // >1 s gap ⇒ flush pending readback

    // 0.60.5 COMPOSITE-RATE CAP (see kBgCompositeMinIntervalMs in the .cpp).
    // Mailbox-clock deadline before which a frame is NOT composed but
    // discarded — the producer re-serves the parked composite from the
    // mailbox, which is exactly what the mailbox exists for. 0 = compose
    // immediately (cold start, and reset at every epoch break / gap /
    // release so a boundary never waits on the cap). Worker thread only.
    qint64 m_nextComposeDueMs = 0;

    // 0.60.5 — the frame size prewarmResources last warmed the FBO chain
    // for. Invalid until the first warm; reset on release so a re-enable
    // re-warms. Worker thread only.
    QSize m_prewarmedSize;

    // 0.60.6 (Petia's 0.60.5 foggy-window RCA) — persistent-compose-failure
    // latch, modelled on TfliteSegmenter's m_runFailStreak. A compositor that
    // is structurally dead on this driver (old UHD 620 GL, a wedged context)
    // returns null from EVERY compose; through 0.60.5 the engine then served
    // the obscuring mosaic for the whole call while ORT inference + the doomed
    // GL passes kept burning CPU. m_structuralFailStreak counts CONSECUTIVE
    // structural nulls (CompositorNull/NotReady/Image-plate faults — NOT a
    // transient SegmenterNull, which rung 1 already covers); at
    // kComposeFailLatch it sets m_composeLatchedOff, which short-circuits
    // compose() before segment()/GL so the doomed work stops, and emits
    // engineDisabled once. A real success or a producer-boundary/epoch change
    // resets both (the effect gets a fresh attempt). Worker thread only.
    int  m_structuralFailStreak = 0;
    bool m_composeLatchedOff    = false;
    int  m_latchedReason        = 0;   // BgFailReason of the latch

    // Fault-injection seam (TALQ_BG_FORCE_COMPOSE_FAIL, worker-thread getenv,
    // same idiom as TfliteSegmenter's TALQ_BG_SEG_FAIL). This dev box
    // composites fine, so Petia's structural GL failure cannot be reproduced
    // organically — the seam forces compose() to return null so every ladder
    // rung is reachable here. `transient` fails the first N composes then
    // recovers (exercises rung 1 + recovery WITHOUT latching); `persistent`
    // never recovers (exercises the rung-3 latch). Counter is the remaining
    // transient failures. Worker thread only.
    int m_injectFailRemaining = 0;

    std::atomic<bool> m_ready{false};   // see isReady()
};
