#include "BackgroundEngine.h"
#include "BackgroundCompositor.h"
#include "TfliteSegmenter.h"

#include <QImage>
#include <QDebug>

// Phase 2d: processFrame now wires through to the compositor. The mask
// source is a centred radial gradient (mock — Phase 2e replaces this
// with TFLite + selfie_segmenter.tflite). The mock is enough to
// validate the Engine → Segmenter → Compositor → output pipeline shape
// without dragging in the TFLite runtime ahead of time.

BackgroundEngine::BackgroundEngine(QObject *parent)
    : QObject(parent)
{}

BackgroundEngine::~BackgroundEngine() = default;
// m_compositor: not deleted here. Qt parent-child ownership (we passed
// `this` as parent in setMode) destroys it automatically. Deleting it
// explicitly would double-free (review finding 4).

void BackgroundEngine::setMode(Mode m)
{
    m_mode = m;
    // Lazy-construct the compositor + segmenter when the user actually
    // opts in. Off-mode publishers (the vast majority today) pay nothing.
    if (m != Mode::None) {
        if (!m_compositor) m_compositor = new BackgroundCompositor(this);
        if (!m_segmenter) {
            m_segmenter = new TfliteSegmenter(this);
            // Re-emit segmenter init failure as the engine-level
            // disabled signal so the UI sees a single funnel for both
            // GL-context and ORT failures. The segmenter still works
            // (returns the centred-gradient stub) so we don't tear
            // down the compositor here.
            connect(m_segmenter, &TfliteSegmenter::unavailable, this,
                    [this](const QString &reason) {
                emit engineDisabled(reason);
            });
        }
    }
}
BackgroundEngine::Mode BackgroundEngine::mode() const { return m_mode; }

void BackgroundEngine::setBlurStrength(int s) { m_blurStrength = qBound(1, s, 20); }
int  BackgroundEngine::blurStrength() const   { return m_blurStrength; }

void BackgroundEngine::setImagePath(const QString &p)
{
    if (p == m_imagePath) return;
    m_imagePath = p;
    // Invalidate the image cache so the new path is loaded on next frame.
    m_cachedBgRaw     = QImage();
    m_cachedBgScaled  = QImage();
    m_cachedBgPath.clear();
    m_cachedBgScaledSize = QSize();
    m_cachedBgFailed = false;       // give the new path a chance
}
QString BackgroundEngine::imagePath() const           { return m_imagePath; }

QImage BackgroundEngine::processFrame(const QImage &rgba)
{
    // Off mode: never construct the compositor's GL context. The
    // overwhelming majority of TalQ publishes will be Off.
    if (m_mode == Mode::None || rgba.isNull())
        return rgba;
    if (!m_compositor || !m_segmenter) return rgba;   // setMode builds both

    // Phase 2e — mask comes from the segmenter. Today's stub returns the
    // same centred radial gradient the inline mock used to. Once TFLite
    // ships in Phase 2e.x the mask becomes person-shaped without any
    // caller change here.
    const QImage mask = m_segmenter->segment(rgba);

    switch (m_mode) {
    case Mode::Blur: {
        // Talk's `blurValue=10` is the strength slider value; the runtime
        // blur radius scales by frame width: radius = strength * width/720.
        const float radius = static_cast<float>(m_blurStrength)
                              * (rgba.width() / 720.0f);
        return m_compositor->compositeBlur(rgba, mask, radius);
    }
    case Mode::Image: {
        if (m_imagePath.isEmpty()) return rgba;
        if (m_cachedBgFailed) return rgba;   // signal already fired once

        // Load from disk only when the path changes (review finding 2).
        if (m_cachedBgRaw.isNull() || m_cachedBgPath != m_imagePath) {
            QImage loaded(m_imagePath);
            if (loaded.isNull()) {
                qWarning() << "BackgroundEngine: background image missing:"
                           << m_imagePath
                           << "(one-shot warning; toggle Background or"
                              " pick a different image to retry)";
                m_cachedBgFailed = true;
                emit backgroundImageFailed(m_imagePath);
                return rgba;
            }
            m_cachedBgRaw  = std::move(loaded);
            m_cachedBgPath = m_imagePath;
            // Force re-scale below.
            m_cachedBgScaled = QImage();
            m_cachedBgScaledSize = QSize();
        }
        // Re-scale only when the camera resolution changes — same disk
        // image at the same scale is reused frame to frame.
        if (m_cachedBgScaled.isNull() || m_cachedBgScaledSize != rgba.size()) {
            m_cachedBgScaled = m_cachedBgRaw.scaled(
                rgba.size(),
                Qt::KeepAspectRatioByExpanding,
                Qt::SmoothTransformation);
            m_cachedBgScaledSize = rgba.size();
        }
        return m_compositor->compositeImage(rgba, mask, m_cachedBgScaled);
    }
    case Mode::None:
    case Mode::Video:
    case Mode::VideoStream:
        return rgba;   // None + reserved-future modes pass through.
    }
    Q_UNREACHABLE_RETURN(rgba);
}

bool BackgroundEngine::isReady() const
{
    // Phase 2a: ready when the compositor exists and reports init success.
    // Phase 2d adds the TFLite interpreter readiness check.
    return m_compositor && m_compositor->isReady();
}
