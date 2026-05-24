#include "TfliteSegmenter.h"

#include <QDebug>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRadialGradient>

#ifdef TALQ_BG_ORT
// ORT headers use MSVC SAL annotations that mingw's <sal.h> doesn't
// fully stub. Define the missing ones as no-ops BEFORE including the
// ORT headers. Each #ifndef so an upgraded mingw or alt toolchain that
// provides them keeps working unchanged.
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#ifndef _In_reads_bytes_
#define _In_reads_bytes_(s)
#endif
#ifndef _Out_writes_bytes_
#define _Out_writes_bytes_(s)
#endif
#include <onnxruntime_cxx_api.h>
#include <array>
#include <vector>
#endif

// Phase 2e — real selfie segmentation via ONNX Runtime.
//
// Bundle: `:/bg/models/selfie_segmenter.onnx` is MediaPipe's Selfie
// Segmenter (256×256 input, 256×256×1 sigmoid mask) converted .tflite →
// .onnx via tf2onnx (opset 18). One TFLite-only fused op
// (`Convolution2DTransposeBias`) was hand-replaced post-conversion by
// the mathematically equivalent ConvTranspose+Add pair so the model
// loads with stock ORT.
//
// When TALQ_BG_ORT is not defined we degrade to the centred-gradient
// stub (0.39.x betas shipped this); BackgroundEngine still produces a
// visually plausible blur/replace, just centred on the frame instead of
// on the actual person.

namespace {

#ifdef TALQ_BG_ORT
constexpr int kInputW = 256;
constexpr int kInputH = 256;
constexpr int kInputC = 3;
#endif

// Stub fallback — same centred radial gradient the 0.39.x betas drew.
// Lives at this layer (not BackgroundEngine) so callers always get a
// "best effort" mask back even when ORT is disabled or fails to load.
QImage fallbackGradientMask(const QSize &size)
{
    QImage mask(size, QImage::Format_Grayscale8);
    mask.fill(0);
    QPainter p(&mask);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPointF c(size.width() / 2.0, size.height() / 2.0);
    const qreal r = qMin(size.width(), size.height()) * 0.40;
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

TfliteSegmenter::TfliteSegmenter(QObject *parent)
    : QObject(parent)
{
#ifdef TALQ_BG_ORT
    // Load the bundled .onnx from qrc into a QByteArray we own for the
    // session lifetime. ORT's session can also load from a file path,
    // but qrc:// isn't a normal filesystem path on Windows, and the
    // model is small (~450 KB) so the in-memory buffer is fine.
    QFile f(QStringLiteral(":/bg/models/selfie_segmenter.onnx"));
    if (!f.open(QIODevice::ReadOnly)) {
        const QString reason = QStringLiteral(
            "bundled selfie_segmenter.onnx missing from qrc - "
            "falling back to centred-gradient");
        qWarning() << "TfliteSegmenter:" << reason;
        // Deferred emit: the connect in BackgroundEngine happens after
        // the segmenter is constructed, so a synchronous emit here
        // would be dropped. Queue it for the next event-loop spin.
        QMetaObject::invokeMethod(this, [this, reason]() {
            emit unavailable(reason);
        }, Qt::QueuedConnection);
        return;
    }
    m_modelBytes = f.readAll();
    f.close();

    try {
        // ORT_LOGGING_LEVEL_WARNING — quiet on the happy path, but if
        // the GPU/CPU EP needs to fall back, we surface the warning.
        m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "talq-bg");

        m_sessOpts = std::make_unique<Ort::SessionOptions>();
        m_sessOpts->SetIntraOpNumThreads(2);   // small model, 2 threads is enough
        m_sessOpts->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        m_session = std::make_unique<Ort::Session>(
            *m_env,
            m_modelBytes.constData(),
            static_cast<size_t>(m_modelBytes.size()),
            *m_sessOpts);

        m_memInfo = std::make_unique<Ort::MemoryInfo>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

        // Warm-up Run with zeros. JITs internal kernels so the first
        // real frame doesn't pay the compilation cost. ~25 ms one-shot
        // on a modern CPU.
        std::vector<float> dummy(kInputH * kInputW * kInputC, 0.0f);
        const std::array<int64_t, 4> shape = {1, kInputH, kInputW, kInputC};
        Ort::Value in = Ort::Value::CreateTensor<float>(
            *m_memInfo, dummy.data(), dummy.size(),
            shape.data(), shape.size());

        const char *inputNames[]  = {"input_1"};
        const char *outputNames[] = {"activation_10"};
        auto outs = m_session->Run(Ort::RunOptions{nullptr},
                                   inputNames,  &in, 1,
                                   outputNames, 1);
        if (outs.empty()) {
            qWarning() << "TfliteSegmenter: warm-up Run produced no output";
            m_session.reset();
            return;
        }
        m_ready = true;
        qInfo() << "TfliteSegmenter: ONNX Runtime session ready"
                << "(model" << (m_modelBytes.size() / 1024) << "KB,"
                << "input 256x256 NHWC float32)";
    } catch (const Ort::Exception &e) {
        const QString reason = QStringLiteral(
            "ORT initialisation failed: %1 - falling back to centred-gradient")
            .arg(QString::fromLatin1(e.what()));
        qWarning() << "TfliteSegmenter:" << reason;
        m_session.reset();
        m_sessOpts.reset();
        m_env.reset();
        m_memInfo.reset();
        QMetaObject::invokeMethod(this, [this, reason]() {
            emit unavailable(reason);
        }, Qt::QueuedConnection);
    }
#else
    qInfo() << "TfliteSegmenter: built without TALQ_BG_ORT — "
               "using centred-gradient stub";
#endif
}

TfliteSegmenter::~TfliteSegmenter() = default;

bool TfliteSegmenter::isReady() const
{
    return m_ready;
}

QImage TfliteSegmenter::segment(const QImage &rgba)
{
#ifdef TALQ_BG_ORT
    if (!m_ready || !m_session) {
        return fallbackGradientMask(rgba.size());
    }

    // 1. Downscale the camera frame to 256×256 in RGB float32 NHWC.
    //    SmoothTransformation is good enough at this size; the model's
    //    input is already lossy to scale changes.
    QImage small = rgba.convertToFormat(QImage::Format_RGBA8888)
                       .scaled(kInputW, kInputH,
                               Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation);
    if (small.width() != kInputW || small.height() != kInputH) {
        return fallbackGradientMask(rgba.size());
    }

    std::vector<float> nhwc(kInputH * kInputW * kInputC);
    for (int y = 0; y < kInputH; ++y) {
        const uchar *row = small.constScanLine(y);
        float *dst = nhwc.data() + y * kInputW * kInputC;
        for (int x = 0; x < kInputW; ++x) {
            // Format_RGBA8888 byte order is R,G,B,A. The model takes
            // RGB; drop alpha. Normalise to [0,1].
            dst[x * 3 + 0] = row[x * 4 + 0] / 255.0f;
            dst[x * 3 + 1] = row[x * 4 + 1] / 255.0f;
            dst[x * 3 + 2] = row[x * 4 + 2] / 255.0f;
        }
    }

    // 2. Run inference.
    try {
        const std::array<int64_t, 4> shape = {1, kInputH, kInputW, kInputC};
        Ort::Value in = Ort::Value::CreateTensor<float>(
            *m_memInfo, nhwc.data(), nhwc.size(),
            shape.data(), shape.size());
        const char *inputNames[]  = {"input_1"};
        const char *outputNames[] = {"activation_10"};
        auto outs = m_session->Run(Ort::RunOptions{nullptr},
                                   inputNames,  &in, 1,
                                   outputNames, 1);
        if (outs.empty() || !outs[0].IsTensor()) {
            return fallbackGradientMask(rgba.size());
        }
        const float *mask = outs[0].GetTensorData<float>();

        // 3. Convert 256×256×1 sigmoid output → Grayscale8 256×256, then
        //    upscale to the source resolution. Talk's WebGLCompositor.js
        //    does the upsample on the GPU after a bilateral refine; for
        //    parity we let our shader handle the smoothstep + light wrap
        //    and just produce a clean upsampled grayscale here.
        QImage maskSmall(kInputW, kInputH, QImage::Format_Grayscale8);
        for (int y = 0; y < kInputH; ++y) {
            uchar *row = maskSmall.scanLine(y);
            const float *src = mask + y * kInputW;
            for (int x = 0; x < kInputW; ++x) {
                float v = src[x];
                if (v < 0.0f) v = 0.0f;
                else if (v > 1.0f) v = 1.0f;
                row[x] = static_cast<uchar>(v * 255.0f + 0.5f);
            }
        }
        return maskSmall.scaled(rgba.size(),
                                Qt::IgnoreAspectRatio,
                                Qt::SmoothTransformation);
    } catch (const Ort::Exception &e) {
        // Surface once, then quiet. At 30 fps a persistent fault would
        // otherwise log thousands of identical lines per minute. We do
        // NOT tear down the engine: most Run errors are transient (e.g.
        // momentary OOM) and a future frame will succeed on its own.
        if (!m_runFailedOnce.exchange(true, std::memory_order_relaxed)) {
            qWarning() << "TfliteSegmenter: ORT Run failed (first occurrence) -"
                       << e.what()
                       << "- further per-frame failures suppressed for this session";
        }
        return fallbackGradientMask(rgba.size());
    }
#else
    return fallbackGradientMask(rgba.size());
#endif
}
