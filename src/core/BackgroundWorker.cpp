#include "BackgroundWorker.h"

#include "BackgroundCompositor.h"
#include "TfliteSegmenter.h"
#include "BackgroundEngine.h"   // for Mode enum

#include <QDebug>
#include <QImage>
#include <QOffscreenSurface>

BackgroundWorker::BackgroundWorker(QOffscreenSurface *surface, QObject *parent)
    : QObject(parent)
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
}

void BackgroundWorker::invalidateImageCache()
{
    m_cachedBgScaled  = QImage();
    m_cachedBgPath.clear();
    m_cachedBgScaledSize = QSize();
    m_cachedBgFailed = false;
}

QImage BackgroundWorker::process(const QImage &rgba, int modeInt,
                                  int blurStrength, const QString &imagePath)
{
    const auto m = static_cast<BackgroundEngine::Mode>(modeInt);
    if (m == BackgroundEngine::Mode::None || rgba.isNull()) return rgba;

    if (!m_compositor || !m_segmenter) {
        // 0.60.2 — pass through, do NOT reconstruct. The pre-0.60.2 code
        // auto-constructed here ("first frame before applyMode landed"),
        // but that path has been dead since the 0.40.9 isReady() gate:
        // processFrame() only dispatches to us while m_ready is true, and
        // m_ready is only true while compositor + segmenter exist. The one
        // way to land here now is a frame that passed the caller's
        // isReady() check in the same instant applyMode(None) released
        // everything (nanosecond window at toggle-off). Reconstructing for
        // that frame would silently resurrect the ~145 MB engine right
        // after the user turned the background OFF — the exact retention
        // bug the 2026-07-13 RCA measured.
        return rgba;
    }

    const QImage mask = m_segmenter->segment(rgba);

    switch (m) {
    case BackgroundEngine::Mode::Blur: {
        const float radius = static_cast<float>(blurStrength)
                              * (rgba.width() / 720.0f);
        return m_compositor->compositeBlur(rgba, mask, radius);
    }
    case BackgroundEngine::Mode::Image: {
        if (imagePath.isEmpty()) return rgba;
        if (m_cachedBgFailed && m_cachedBgPath == imagePath) return rgba;

        // 0.60.2 (2026-07-13 field RCA) — cache ONLY the camera-resolution
        // scaled plate, never the full decode. The bundled JPEGs are
        // 3200×1800 → ~23 MB as 32-bit pixels, and the old m_cachedBgRaw
        // held that for the whole session even though it was only ever
        // read to produce this scaled copy. Re-decoding from disk on the
        // two rare invalidation events (user picks a different image /
        // camera resolution change) costs one decode+scale — tens of ms,
        // on this worker thread, never the UI — instead of 23 MB forever.
        // The decode-failure latch (m_cachedBgFailed) is unchanged: a
        // missing file warns once and passes through until the path changes.
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
                return rgba;
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
        return m_compositor->compositeImage(rgba, mask, m_cachedBgScaled);
    }
    case BackgroundEngine::Mode::None:
    case BackgroundEngine::Mode::Video:
    case BackgroundEngine::Mode::VideoStream:
        return rgba;
    }
    return rgba;
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
