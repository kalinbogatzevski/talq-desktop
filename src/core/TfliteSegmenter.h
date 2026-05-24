#pragma once

// #20 Phase 2e — person-segmentation backend.
//
// Encapsulates the model load + per-frame inference that produces a
// single-channel mask (white = person, black = background) from an RGBA
// camera frame.
//
// The class name is historical: the bundled model started as TFLite, but
// runtime inference uses ONNX Runtime (TFLite has no usable prebuilt
// Windows mingw64 binary; the model is converted .tflite → .onnx in dev,
// and the patched .onnx is bundled at :/bg/models/selfie_segmenter.onnx).
// The header keeps the same shape so callers don't move.
//
// At construction (when TALQ_BG_ORT is defined):
//   1. Reads :/bg/models/selfie_segmenter.onnx from qrc into RAM.
//   2. Builds an ORT session (CPU EP) on a hidden Ort::Env.
//   3. One warm-up Run with zeros to JIT-compile internals.
// On any failure the segmenter stays !isReady() and segment() falls back
// to a centred radial gradient — same shape the 0.39.x betas shipped.
//
// Per-frame: downscale RGBA to 256×256 (model expects NHWC float32 [0,1]),
// run the session, upscale the 256×256×1 confidence mask back to source
// resolution as Grayscale8. ~10 ms per frame on a modern CPU; well under
// the 33 ms budget at 30 fps.

#include <QByteArray>
#include <QImage>
#include <QObject>

#include <memory>

#ifdef TALQ_BG_ORT
// Forward decls of the ORT C++ types we keep as members. The full header
// is heavy; the impl includes it.
namespace Ort {
class Env;
class Session;
class SessionOptions;
class MemoryInfo;
}
#endif

class TfliteSegmenter : public QObject
{
    Q_OBJECT

public:
    explicit TfliteSegmenter(QObject *parent = nullptr);
    ~TfliteSegmenter() override;

    // True when the ONNX session loaded and the warm-up Run succeeded.
    // False after any construction-time failure (qrc missing, ORT
    // failed to compile, etc.); segment() then falls back to the
    // centred-gradient stub.
    bool isReady() const;

    // Compute a per-pixel person mask the same size as `rgba`. Returns
    // an 8-bit Grayscale8 QImage: 255 = full person, 0 = full background.
    QImage segment(const QImage &rgba);

private:
    bool m_ready = false;

#ifdef TALQ_BG_ORT
    // Owned. Constructed in the ctor when the model and ORT both succeed,
    // destroyed in the dtor. unique_ptr so the heavy ORT header stays
    // out of consumers' translation units.
    std::unique_ptr<Ort::Env>            m_env;
    std::unique_ptr<Ort::SessionOptions> m_sessOpts;
    std::unique_ptr<Ort::Session>        m_session;
    std::unique_ptr<Ort::MemoryInfo>     m_memInfo;
    // Keep the model bytes alive for the lifetime of the session — the
    // session references them through Ort::Session's internal copy, but
    // holding the source buffer makes debugging easier and is cheap
    // (~450 KB).
    QByteArray m_modelBytes;
#endif
};
