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

BackgroundEngine::~BackgroundEngine()
{
    delete m_compositor;
}

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

void BackgroundEngine::setImagePath(const QString &p) { m_imagePath = p; }
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
        // Load + scale the chosen background once per frame (Phase 2f
        // caches this on the engine's QThread to avoid the disk hit).
        if (m_imagePath.isEmpty()) return rgba;
        QImage bg(m_imagePath);
        if (bg.isNull()) {
            qWarning() << "BackgroundEngine: background image missing:"
                       << m_imagePath;
            return rgba;
        }
        QImage scaled = bg.scaled(rgba.size(), Qt::KeepAspectRatioByExpanding,
                                  Qt::SmoothTransformation);
        return m_compositor->compositeImage(rgba, mask, scaled);
    }
    case Mode::None:
    case Mode::Video:
    case Mode::VideoStream:
    default:
        return rgba;
    }
}

bool BackgroundEngine::isReady() const
{
    // Phase 2a: ready when the compositor exists and reports init success.
    // Phase 2d adds the TFLite interpreter readiness check.
    return m_compositor && m_compositor->isReady();
}
