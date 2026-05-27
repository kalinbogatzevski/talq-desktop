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
    if (m == BackgroundEngine::Mode::None) return;

    if (!m_compositor) {
        m_compositor = new BackgroundCompositor(this);
        m_compositor->setExternalSurface(m_surface);
        // The compositor's lazy init runs on first compositeBlur/Image
        // call. We don't pre-warm here so a quick toggle ON→OFF in the
        // Settings dialog doesn't pay GL creation cost for nothing.
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
    m_cachedBgRaw     = QImage();
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
        // First frame arrived before applyMode landed — construct now.
        applyMode(modeInt);
    }
    if (!m_compositor || !m_segmenter) return rgba;

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

        if (m_cachedBgRaw.isNull() || m_cachedBgPath != imagePath) {
            QImage loaded(imagePath);
            if (loaded.isNull()) {
                qWarning() << "BackgroundWorker: background image missing:"
                           << imagePath
                           << "(one-shot warning; pick a different image to retry)";
                m_cachedBgFailed = true;
                m_cachedBgPath   = imagePath;
                emit backgroundImageFailed(imagePath);
                return rgba;
            }
            m_cachedBgRaw  = std::move(loaded);
            m_cachedBgPath = imagePath;
            m_cachedBgScaled = QImage();
            m_cachedBgScaledSize = QSize();
            m_cachedBgFailed = false;
        }
        if (m_cachedBgScaled.isNull() || m_cachedBgScaledSize != rgba.size()) {
            m_cachedBgScaled = m_cachedBgRaw.scaled(
                rgba.size(),
                Qt::KeepAspectRatioByExpanding,
                Qt::SmoothTransformation);
            m_cachedBgScaledSize = rgba.size();
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

void BackgroundWorker::shutdown()
{
    // Release GL resources here so the OpenGL context is current on the
    // worker thread when QOpenGLFramebufferObject / QOpenGLTexture
    // destructors run. Deleting a BackgroundCompositor that owns a
    // QOpenGLContext from another thread is undefined.
    delete m_compositor; m_compositor = nullptr;
    delete m_segmenter;  m_segmenter  = nullptr;
}
