#include "TfliteSegmenter.h"

#include <QPainter>
#include <QRadialGradient>

// Phase 2e stub. The real TFLite-driven inference will replace
// segment() once the C++ runtime + GPU delegate are vendored in. The
// bundled `:/bg/models/selfie_segmenter.tflite` (from spreed v23.0.4)
// is already in the qrc and ready to be loaded.

TfliteSegmenter::TfliteSegmenter(QObject *parent)
    : QObject(parent)
{
    // Real implementation will:
    //   1. QFile :/bg/models/selfie_segmenter.tflite → readAll().
    //   2. tflite::FlatBufferModel::BuildFromBuffer(...)
    //   3. tflite::InterpreterBuilder + GPU delegate
    //   4. AllocateTensors, warm-up one dry-run inference
    //   5. m_ready = true on success
}

TfliteSegmenter::~TfliteSegmenter() = default;

bool TfliteSegmenter::isReady() const
{
    return m_ready;
}

QImage TfliteSegmenter::segment(const QImage &rgba)
{
    // Stub: return the same centred radial gradient BackgroundEngine
    // used to compute inline. Caller (BackgroundEngine) treats this as
    // "the mask" and feeds it to the compositor. Once real inference
    // lands the mask becomes person-shaped without any caller changes.
    QImage mask(rgba.size(), QImage::Format_Grayscale8);
    mask.fill(0);
    QPainter p(&mask);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPointF c(rgba.width() / 2.0, rgba.height() / 2.0);
    const qreal  r = qMin(rgba.width(), rgba.height()) * 0.40;
    QRadialGradient grad(c, r);
    grad.setColorAt(0.0, Qt::white);
    grad.setColorAt(0.7, QColor(150, 150, 150));
    grad.setColorAt(1.0, Qt::black);
    p.setBrush(grad);
    p.setPen(Qt::NoPen);
    p.drawEllipse(c, r * 1.4, r * 1.4);
    return mask;
}
