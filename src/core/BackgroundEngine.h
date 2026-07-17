#pragma once

// #20 — Per-frame background blur / replace engine.
//
// Scope: takes RGBA frames from PublishPipeline's shared chain, runs
// person segmentation (ONNX Runtime + selfie_segmenter), composites the
// person over a blurred-self or chosen-image background via the three
// GLSL shaders ported verbatim from Talk's WebGLCompositor.js, and hands
// the composited RGBA back to the encoder branches.
//
// 0.40.1 threading model: the engine itself is a thin facade that lives
// on whatever thread constructed it (typically Qt main). All actual GL
// + ORT work runs on a dedicated worker QThread owned by the engine —
// the worker is BackgroundWorker, a separate QObject with affinity to
// that thread.
//
// The 0.40.0 stable shipped with this work on Qt main; under Blur/Image
// mode the streaming thread parked on Qt main while the GL compositor
// cold-started, and any Qt-main code path that touched the GST stream
// lock deadlocked the UI. See task #32.
//
// 0.60.5 THREADING FIX — the freeze Kalin hit on 0.60.4 with blur on
// both ends (video started late, high CPU, app froze intermittently).
// 0.40.1 moved the work off Qt main but kept processFrame() BLOCKING on
// the worker (Qt::BlockingQueuedConnection). The caller is a GstAppSink
// callback running on the camera's streaming thread WHILE HOLDING that
// pad's GST_PAD_STREAM_LOCK — so the stream lock stayed pinned for the
// full segment()+composite round trip. Measured on Kalin's Iris Xe at
// the production cap of 720p30 (bench, 240 frames, machine IDLE):
//
//     camera-thread hold   p50 58.1 ms   p95 84.9 ms   max 578.9 ms
//     (frame budget at 30 fps: 33.3 ms)
//
// A stream lock held for 58 ms every frame transitively parks Qt main:
// every gst_element_set_state() on the camera (enableCamera, mute /
// unmute, the stall watchdog's re-arm, BgPreviewSource::stop()'s
// set_state(NULL) on the UI thread) blocks on GST_STATE_LOCK, which the
// state-change waits on the stream lock to acquire. That is the same
// lock-pin the 0.40.7 report named and the 0.40.9 isReady() gate only
// papered over for the COLD-INIT case — nothing guarded steady-state
// slowness, and steady state was never fast.
//
// processFrame() is now NON-BLOCKING and waits for nothing:
//   * submit the frame into a one-slot, DROP-OLDEST input mailbox,
//   * take whatever composite is already sitting in the output slot,
//   * return immediately.
// The camera thread's cost collapses to two mutex swaps (~ms), and the
// stream lock is released long before ORT is touched. If the worker
// falls behind, the output slot simply doesn't advance and we re-push
// the last composited pixels with the CURRENT buffer's PTS: an identical
// frame costs the encoder an all-skip P-frame (near-zero bitrate) and
// keeps the output frame rate constant, which rtpgccbwe and the
// receiver's jitter buffer both prefer to a sagging one. Content goes
// stale under load; it is never revealed. Cost: one frame (33 ms) of
// added video latency, invisible on a webcam.
//
// PRIVACY (the reason the fallback is what it is): while a background is
// ON, this class must NEVER hand back the real camera frame. Through
// 0.60.4 it did — BackgroundEngine.cpp's cold-init gate, the
// invokeMethod-failed path, and four paths in BackgroundWorker/
// PublishPipeline all "passed the frame through unmodified" on failure.
// That frame does not stop at the local PiP: it goes through the tee to
// the encoder and out to every peer and every recorder. The cold-init
// window alone was measured at 200 ms - 1.07 s, and since 0.60.2 frees
// the engine on OFF, every re-enable paid it again. Every one of those
// paths now falls back to a CONTENT-FREE mosaic of the frame instead.
// A privacy feature must never fail toward "reveals the thing".
//
// The 2026-07-14 adversarial review found two more ways this contract was
// being broken, both fixed with it:
//   1. FAIL-OPEN SEGMENTATION — TfliteSegmenter substituted a centred-
//      gradient "person" guess on every ORT failure, so the compositor kept
//      the middle of the REAL ROOM sharp while claiming success (composites
//      counted, fail-closed counters read 0). segment() now returns a null
//      mask on every failure and the ladder covers the frame. See the
//      fail-closed note in TfliteSegmenter.h.
//   2. CROSS-BOUNDARY REPLAY — the output mailbox aged in SUBMISSIONS, not
//      wall-clock, and was never invalidated at producer boundaries, so the
//      final composite of one call was emitted at the start of the NEXT
//      call ("age 1", hours later). Staleness is now wall-clock-bounded and
//      resetMailbox() invalidates the mailbox at every continuity break.
//      See BgFrameMailbox in BackgroundWorker.h.

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QMutex>

#include <atomic>
#include <memory>

class BackgroundWorker;
class QOffscreenSurface;
class QThread;
struct BgFrameMailbox;

// 0.60.6 (Petia's 0.60.5 foggy-window field RCA) — a compose() that cannot
// produce a frame used to collapse to a bare null QImage on all six of its
// return paths, indistinguishable from each other. The engine then served the
// content-free mosaic FOREVER with no way to tell "no image was ever chosen"
// (a misconfiguration) from "the GL compositor is structurally dead on this
// driver" (the old-UHD-620 case) from "one transient inference throw". The
// reason code makes the six paths distinguishable so the persistent-failure
// ladder can pick the right rung (config → pass-through vs structural →
// keep-obscuring-and-stop-the-doomed-work). Stored as an int on the shared
// mailbox (worker writes, engine/CallManager read); one relaxed atomic.
enum class BgFailReason : int {
    None = 0,           // last compose succeeded
    EmptyImagePath,     // Image mode, no image chosen — no concealment intent
    ImageDecodeFailed,  // Image mode, chosen plate won't decode
    CachedImageFailed,  // Image mode, plate previously latched as unreadable
    SegmenterNull,      // segmentation returned no mask (transient — rung 1)
    CompositorNull,     // GL compose produced no frame (structural — rung 3)
    NotReady,           // worker has no compositor/segmenter while mode is ON
};

namespace talq::bg {

// 0.60.5 — per-frame cost instrumentation for the background path.
//
// Why this exists: through 0.60.4 there was NOT ONE timer anywhere between
// the camera appsink and the encoder, so "background blur makes calls
// freeze" was argued from theory for three releases. The bench
// (tests/bg_bench.cpp) finally measured it on Kalin's Iris Xe box at the
// production cap of 1280x720@30 (33.3 ms budget): the camera streaming
// thread was blocking p50 58 ms / p95 85 ms / max 579 ms per frame — 1.7x
// the budget on an IDLE machine — and it did so while holding the camera
// pad's GST_PAD_STREAM_LOCK. This class makes that number visible in the
// field, so the NEXT report has data in it instead of a hypothesis.
//
// The metric that proves the 0.60.5 fix works is CameraHold: it must sit
// at ~1-2 ms. If it is ever tens of ms again, the blocking hop is back.
//
// Cost: two stores + a mutex per frame at 30 Hz. Percentiles are computed
// only when a summary is emitted, never per frame.
class Stats
{
public:
    enum Metric {
        CameraHold,    // BackgroundEngine::processFrame — the camera thread's hold (THE number)
        BridgeTotal,   // PublishPipeline::onBgSample end-to-end (map + copy + hold + push)
        Segment,       // TfliteSegmenter::segment — worker thread
        Composite,     // BackgroundCompositor::composite* — worker thread
        WorkerTotal,   // Segment + Composite: the ceiling on the BACKGROUND frame rate
        MetricCount
    };

    // TALQ_DEBUG_BG=1 turns on the periodic (~5 s) summary. The end-of-call
    // summary and the over-budget warning fire regardless — a shipped build
    // that freezes must leave evidence in the log without the user first
    // knowing to set an env var.
    static bool verbose();

    static void record(Metric m, qint64 microseconds);
    static void noteDropped();      // input slot overwritten before the worker took it
    static void noteFailClosed();   // frame served by the content-free fallback
    static void noteRateCapped();   // frame skipped by the composite-rate cap
                                    // (deliberate; the producer re-serves the
                                    // parked composite — NOT a worker-behind drop)

    static quint64 dropped();
    static quint64 failClosed();
    static quint64 rateCapped();

    static void maybeLog();                    // rate-limited; safe to call per frame
    static void logSummary(const char *why);   // one line, unconditional
    static void reset();
};

} // namespace talq::bg

class BackgroundEngine : public QObject
{
    Q_OBJECT

public:
    // The four modes mirror Talk's `BACKGROUND_TYPE` enum (constants.ts
    // L411-420). VIDEO / VIDEO_STREAM (Talk's animated backgrounds) are
    // explicitly out of scope; the enum value is reserved so a future
    // session can add them without renumbering.
    enum class Mode {
        None = 0,
        Blur = 1,
        Image = 2,
        Video = 3,         // reserved
        VideoStream = 4,   // reserved
    };
    Q_ENUM(Mode)

    explicit BackgroundEngine(QObject *parent = nullptr);
    ~BackgroundEngine() override;

    // Settings — live-applied. Setters are safe to call from any thread;
    // they update an atomic snapshot read by processFrame and forward an
    // applyMode/invalidate kick to the worker via a queued connection.
    // Direct calls from Qt main are the common case.
    // 0.60.2 (2026-07-13 field RCA): setMode(Mode::None) RELEASES the
    // worker's GL + ORT resources (~112 MB measured retained before this);
    // setting a non-None mode again rebuilds them lazily (cold init, frames
    // pass through raw until ready — same as the very first enable).
    void setMode(Mode m);
    Mode mode() const { return m_modeAtomic.load(std::memory_order_relaxed); }

    // 1..20, matches Talk's blurStrength. The scaling onto Talk's GLSL
    // `backgroundBlurValue = strength * width/720` happens inside the
    // engine, NOT here.
    void setBlurStrength(int strength);
    int  blurStrength() const { return m_blurStrengthAtomic.load(std::memory_order_relaxed); }

    // For Image mode. Empty path = fall back to None (matches Talk's
    // behaviour when an image is missing).
    void setImagePath(const QString &absolutePath);
    QString imagePath() const;

    // Per-frame entry point. Thread-safe and NON-BLOCKING (0.60.5): callable
    // from Qt main (Settings preview) or from a GStreamer streaming thread
    // (PublishPipeline's BG bridge, holding the camera pad's stream lock).
    // Submits `rgba` to the worker and returns the most recently COMPLETED
    // composite — it never waits for the worker, for GL, or for ORT. See the
    // threading + privacy note at the top of this header.
    //
    // Return contract while mode != None: NEVER the input frame — either a
    // real composite or the content-free mosaic fallback. The ONE exception
    // (0.60.6, Petia's foggy-window RCA): Image mode with NO image chosen is a
    // misconfiguration, not a concealment intent, so it passes the real frame
    // through and surfaces backgroundPassthroughNoImage. Blur and
    // Image-with-a-chosen-plate never pass through.
    QImage processFrame(const QImage &rgba);

    // Build the compositor + ORT session + the ENTIRE GL chain NOW rather
    // than on the first frame. The cold init is 200 ms - 1.07 s (bench);
    // paying it while the pipeline is still negotiating means the user is
    // not a mosaic for the first second of the call. Idempotent, queued,
    // never blocks the caller.
    // 0.60.5 — "the entire chain" now includes the GL context, the shader
    // compile, the frame-sized FBOs/PBOs (at the 1280×720 production cap;
    // the camera menu negotiates ≤720p) and one throwaway compose, so the
    // first real frame finds everything hot. See
    // BackgroundWorker::prewarmResources.
    void prewarm();

    // 2026-07-14 — invalidate the frame mailbox: clear both slots and bump
    // the epoch so an in-flight compose cannot republish pre-boundary pixels.
    // MUST be called at every producer continuity break, because the output
    // slot otherwise survives the gap and its content is re-emitted on the
    // next submit: CallManager keeps ONE engine across calls, and before
    // this existed the final composite of call 1 (sharp face + everything
    // the mask classed as person) went out as the first frames of call 2.
    // Call sites: setMode / setImagePath (internal), PublishPipeline::
    // cleanup() (call teardown), PublishPipeline::enableCamera() (camera
    // (re)start, incl. unmute), BgPreviewSource::stop() (Settings preview
    // handoff). Cheap (two mutexes, no waits) and safe from any thread; the
    // wall-clock staleness bound in processFrame is the backstop for any
    // boundary a future caller forgets.
    void resetMailbox();

    // True when the engine's worker thread is running. The first real
    // composite still cold-starts GL + ORT on the worker, but never on
    // Qt main.
    bool isReady() const;

    // 0.60.5 diagnostics, readable from any thread.
    // framesComposited — real composites the worker has published.
    // framesDropped    — frames overwritten in the input slot because the
    //                    worker was still busy (the drop-oldest backpressure
    //                    that this path never had before; the appsink's
    //                    drop=TRUE/max-buffers=2 could never drop anything,
    //                    because we pull synchronously on the producing
    //                    thread and the queue never reaches its bound).
    quint64 framesComposited() const;
    quint64 framesDropped() const;

    // 0.60.6 P0-d (Petia's 0.60.5 foggy-window RCA) — content-free mosaics the
    // WORKER published into the output slot while the effect was latched off (a
    // structurally-dead compositor this session). Distinct from framesComposited
    // — a mosaic is NOT a composite, so the zero-composite detector still reads
    // structural failure — but it proves the worker, not the camera thread, is
    // now producing the cover (the camera thread's inline mosaicFallback stops,
    // collapsing its per-frame hold to a mutex swap; ground-truth #7). Readable
    // from any thread.
    quint64 framesMosaicked() const;

    // 0.60.6 P0-d — the content-free fallback, promoted to a public static so
    // the worker can publish the SAME cover the engine computes inline. Downscale
    // by 16 and smooth-scale back up: a CPU mosaic blur, ~1 ms at 720p, no GL, no
    // ORT, safe on the streaming thread on the very first frame. Everything that
    // identifies a room — faces, screens, text, the whiteboard behind you — is
    // gone at 1/16 linear resolution. It is the ONE fallback that is always
    // available, so no failure anywhere in the engine ever has an excuse to
    // reveal the real background.
    static QImage mosaicFallback(const QImage &rgba);

    // 0.60.6 (Petia's 0.60.5 foggy-window RCA) — the reason the LAST compose
    // failed (BgFailReason::None if it succeeded, or while passing through an
    // unconfigured Image mode). Readable from any thread. Drives the
    // persistent-failure ladder's rung selection in CallManager::onLoadTick.
    BgFailReason lastFailReason() const;

    // True once the worker has latched compose OFF after a run of consecutive
    // STRUCTURAL failures (a GL compositor that is non-functional this
    // session). In that state the doomed ORT inference + GL work stop, the
    // engine keeps serving the content-free mosaic, and engineDisabled has
    // fired once. Cleared by a producer boundary (mode/image change) or an
    // Off→On release, which give the effect a fresh attempt.
    bool composeLatched() const;

signals:
    // Fires when the engine permanently disables itself for this session
    // (e.g. inference fails repeatedly). The UI should toast the reason
    // and flip the QSetting back to None.
    void engineDisabled(const QString &reason);

    // Fires once when an Image-mode background fails to load. UI surfaces
    // a toast; per-frame qWarnings are then suppressed for that path
    // until setImagePath() changes the source. Per the project rule
    // "never hide errors silently" — surface once, then go quiet.
    void backgroundImageFailed(const QString &path);

    // 0.60.6 (Petia's 0.60.5 foggy-window RCA) — fires when the engine is in
    // Image mode with NO image chosen. That is a misconfiguration, not a
    // hide-my-room request: there is no concealment intent to honour, so the
    // engine sends REAL video (the one non-None state that passes through) and
    // this tells the UI to say so. Distinct from backgroundImageFailed (a
    // chosen plate that won't decode — that keeps obscuring). CallManager
    // notice: "No background image selected — sending normal video."
    void backgroundPassthroughNoImage();

private:
    // Atomic snapshot read by streaming-thread callers without touching
    // the worker. The worker reads the same values from the mailbox.
    std::atomic<Mode> m_modeAtomic{Mode::None};
    std::atomic<int>  m_blurStrengthAtomic{10};

    // Image path needs a small lock because QString isn't atomic-safe.
    // Held only for the duration of a swap or a copy; never under a GL
    // call.
    mutable QMutex m_pathMutex;
    QString        m_imagePath;

    // 0.60.6 (Petia's 0.60.5 foggy-window RCA) — lock-free snapshot of "the
    // Image-mode path is empty", read on the streaming thread by processFrame
    // to route the Image+no-image state to pass-through without touching
    // m_pathMutex. Kept in sync by setImagePath/setMode. Starts true
    // (default path is empty). m_noImageNoticeActive is a one-shot so the
    // "no image selected" notice surfaces once per ENTRY into that state.
    std::atomic<bool> m_imagePathEmpty{true};
    std::atomic<bool> m_noImageNoticeActive{false};

    // Recompute the Image+empty condition after a mode or path change and,
    // on entering it, emit backgroundPassthroughNoImage() exactly once.
    void refreshNoImageNotice();

    // 0.60.5 — the drop-oldest frame mailbox shared with the worker. Held by
    // shared_ptr, NOT by value: if the worker thread fails to exit inside the
    // dtor's 5 s wait we deliberately leak the worker rather than risk a UAF
    // (see ~BackgroundEngine), and a leaked worker that outlives this engine
    // must still find its mailbox alive.
    std::shared_ptr<BgFrameMailbox> m_box;
    quint64 m_submitSeq = 0;                  // guarded by m_box->inMutex

    // Worker thread owns the GL + ORT objects. Both fields are set in
    // the constructor and cleared in the destructor; they are never
    // re-created during the engine's lifetime.
    QThread           *m_thread  = nullptr;   // owned (parent=this)
    BackgroundWorker  *m_worker  = nullptr;   // owned, moved to m_thread
    QOffscreenSurface *m_surface = nullptr;   // owned, moved to m_thread
};
