#include "BackgroundEngine.h"
#include "BackgroundWorker.h"

#include <QDebug>
#include <QImage>
#include <QMutexLocker>
#include <QOffscreenSurface>
#include <QSurfaceFormat>
#include <QThread>

// 0.40.1 — the engine is a thin facade; the heavy lifting (BackgroundCompositor
// + TfliteSegmenter, both with thread-affine GL / ORT state) lives on a
// dedicated QThread inside BackgroundWorker. See BackgroundEngine.h for the
// threading rationale.

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

    m_worker = new BackgroundWorker(m_surface);   // constructed on caller's thread
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
    m_modeAtomic.store(m, std::memory_order_relaxed);
    if (!m_worker) return;
    // Kick the worker so the compositor + segmenter exist before the
    // first frame arrives. Queued so the caller (Qt main) never blocks
    // on the worker for a settings change.
    QMetaObject::invokeMethod(m_worker, "applyMode",
                              Qt::QueuedConnection,
                              Q_ARG(int, int(m)));
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
    if (!m_worker) return;
    QMetaObject::invokeMethod(m_worker, "invalidateImageCache",
                              Qt::QueuedConnection);
}

QString BackgroundEngine::imagePath() const
{
    QMutexLocker locker(&m_pathMutex);
    return m_imagePath;
}

QImage BackgroundEngine::processFrame(const QImage &rgba)
{
    // Hot path: Off-mode returns immediately without touching the worker.
    // PublishPipeline already short-circuits this case before calling us,
    // but the BgPreviewSource (Settings dialog) still reaches here and
    // benefits from the same fast-path.
    const Mode m = m_modeAtomic.load(std::memory_order_relaxed);
    if (m == Mode::None || rgba.isNull() || !m_worker) return rgba;

    QString path;
    {
        QMutexLocker locker(&m_pathMutex);
        path = m_imagePath;
    }

    QImage out;
    const bool dispatched = QMetaObject::invokeMethod(
        m_worker, "process", Qt::BlockingQueuedConnection,
        Q_RETURN_ARG(QImage, out),
        Q_ARG(QImage, rgba),
        Q_ARG(int, int(m)),
        Q_ARG(int, m_blurStrengthAtomic.load(std::memory_order_relaxed)),
        Q_ARG(QString, path));

    if (!dispatched) {
        // Worker thread is shutting down or invokeMethod metadata
        // mismatched; either way push the unprocessed frame through so
        // the call doesn't black-frame.
        return rgba;
    }
    return out.isNull() ? rgba : out;
}
