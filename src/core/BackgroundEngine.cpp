#include "BackgroundEngine.h"
#include "BackgroundCompositor.h"

#include <QImage>
#include <QPainter>
#include <QRadialGradient>
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
    // Lazy-construct the compositor when the user actually opts in. Pure
    // Off-mode publishers (the vast majority today) pay nothing for this.
    if (m != Mode::None && !m_compositor)
        m_compositor = new BackgroundCompositor(this);
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

namespace {
// Mock person-mask: a centred radial gradient that's white in the middle
// and fades to black at the edges. Stands in for real per-frame
// segmentation until Phase 2e ports TFLite + selfie_segmenter.tflite.
// Phase 3 onward this gets replaced by a true mask source.
QImage makeMockMask(const QSize &size)
{
    QImage mask(size, QImage::Format_Grayscale8);
    mask.fill(0);
    QPainter p(&mask);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPointF c(size.width() / 2.0, size.height() / 2.0);
    const qreal  r = qMin(size.width(), size.height()) * 0.40;
    QRadialGradient grad(c, r);
    grad.setColorAt(0.0, Qt::white);
    grad.setColorAt(0.7, QColor(150, 150, 150));
    grad.setColorAt(1.0, Qt::black);
    p.setBrush(grad);
    p.setPen(Qt::NoPen);
    p.drawEllipse(c, r * 1.4, r * 1.4);
    return mask;
}
} // namespace

QImage BackgroundEngine::processFrame(const QImage &rgba)
{
    // Off mode: never construct the compositor's GL context. The
    // overwhelming majority of TalQ publishes will be Off.
    if (m_mode == Mode::None || rgba.isNull())
        return rgba;
    if (!m_compositor) return rgba;   // setMode should have built it

    const QImage mask = makeMockMask(rgba.size());

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
