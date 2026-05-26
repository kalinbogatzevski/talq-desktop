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
// that thread. processFrame() routes work to the worker via
// Qt::BlockingQueuedConnection so any caller (Qt main from the Settings
// preview, the GStreamer streaming thread from PublishPipeline) blocks
// briefly on the worker thread and NEVER on Qt main. Settings are
// snapshotted atomically on the caller side so the worker doesn't have
// to look back at the engine.
//
// The 0.40.0 stable shipped with this work on Qt main; under Blur/Image
// mode the streaming thread parked on Qt main while the GL compositor
// cold-started, and any Qt-main code path that touched the GST stream
// lock deadlocked the UI. See task #32.

#include <QImage>
#include <QObject>
#include <QString>
#include <QMutex>

#include <atomic>

class BackgroundWorker;
class QOffscreenSurface;
class QThread;

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

    // Per-frame entry point. Thread-safe: callable from Qt main (Settings
    // preview path) OR from a GStreamer streaming thread (PublishPipeline
    // BG bridge) without any QMetaObject hop on the caller side. Internally
    // blocks until the worker thread finishes the composite.
    QImage processFrame(const QImage &rgba);

    // True when the engine's worker thread is running. The first real
    // composite still cold-starts GL + ORT on the worker, but never on
    // Qt main.
    bool isReady() const;

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

private:
    // Atomic snapshot read by streaming-thread callers without touching
    // the worker. The worker reads the same values via process() args.
    std::atomic<Mode> m_modeAtomic{Mode::None};
    std::atomic<int>  m_blurStrengthAtomic{10};

    // Image path needs a small lock because QString isn't atomic-safe.
    // Held only for the duration of a swap or a copy; never under a GL
    // call.
    mutable QMutex m_pathMutex;
    QString        m_imagePath;

    // Worker thread owns the GL + ORT objects. Both fields are set in
    // the constructor and cleared in the destructor; they are never
    // re-created during the engine's lifetime.
    QThread           *m_thread  = nullptr;   // owned (parent=this)
    BackgroundWorker  *m_worker  = nullptr;   // owned, moved to m_thread
    QOffscreenSurface *m_surface = nullptr;   // owned, moved to m_thread
};
