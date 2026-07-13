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
//   * `process(...)` is the invokable entry point reached via
//     QMetaObject::invokeMethod(... Qt::BlockingQueuedConnection ...).
//     Engine drains atomic settings on the caller's thread and passes
//     them in by value so the worker never needs to look back at the
//     engine.
//   * `shutdown()` releases GL resources on the worker thread; the
//     engine calls it via BlockingQueuedConnection before quitting the
//     thread.

#include <QObject>
#include <QImage>
#include <QString>
#include <atomic>

class BackgroundCompositor;
class TfliteSegmenter;
class QOffscreenSurface;

class BackgroundWorker : public QObject
{
    Q_OBJECT

public:
    // Constructed on the Qt main thread. The surface is created on Qt
    // main by BackgroundEngine (QOffscreenSurface::create() requires the
    // GUI thread on most platforms) and then moved to the worker
    // thread together with this worker.
    explicit BackgroundWorker(QOffscreenSurface *surface, QObject *parent = nullptr);
    ~BackgroundWorker() override;

public slots:
    // Synchronous entry point called via BlockingQueuedConnection. Runs
    // on the worker thread. `modeInt` is BackgroundEngine::Mode cast to
    // int so we don't drag the enum across header boundaries.
    QImage process(const QImage &rgba, int modeInt,
                   int blurStrength, const QString &imagePath);

    // Called on EVERY engine setMode(). OFF→ON lazy-constructs compositor +
    // segmenter on the worker thread so their GL/ORT resources are born
    // with the worker affinity. ON→OFF (Mode::None) RELEASES them again
    // (0.60.2, 2026-07-13 field RCA): the old code early-returned on None,
    // so the ORT session + GL context + FBOs/textures lived until process
    // exit — measured ~112 MB retained after the user turned the
    // background off (256 MB steady → 401 MB with the engine up, only
    // 29 MB returned on OFF).
    void applyMode(int modeInt);

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
    // particular is the multi-second cold-init). BackgroundEngine's
    // streaming-thread processFrame() reads this and passes the frame
    // through unmodified while we're still initialising, instead of
    // BlockingQueuedConnection-ing onto a busy worker event loop and
    // pinning the camera pad's stream lock. The lock-pin used to wedge
    // every subsequent gst_element_set_state() — Qt main's enableCamera
    // / disableCamera / cleanup all parked on the same critical section.
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

    std::atomic<bool> m_ready{false};   // see isReady()
};
