#pragma once

// #20 Phase 2e — person-segmentation backend.
//
// Encapsulates the model load + per-frame inference that produces a
// single-channel mask (white = person, black = background) from an RGBA
// camera frame. Today's implementation is a stub that returns the same
// centred radial gradient BackgroundEngine used to compute inline; the
// header is finalised so the future real-TFLite swap is a single
// implementation change, no callers re-wired.
//
// When the real inference lands (vendored TFLite C++ runtime + GPU
// delegate + selfie_segmenter.tflite from spreed v23.0.4), this class
// will:
//   1. Load `:/bg/models/selfie_segmenter.tflite` from the bundled qrc
//      at construction.
//   2. Per-frame: downscale input to 256×256, run interpreter, post-
//      process the confidence mask back to source resolution.
//   3. Live on its own QThread alongside the GL compositor.
//
// The stub matches the contract so the engine works end-to-end without
// the runtime.

#include <QImage>
#include <QObject>

class TfliteSegmenter : public QObject
{
    Q_OBJECT

public:
    explicit TfliteSegmenter(QObject *parent = nullptr);
    ~TfliteSegmenter() override;

    // True when a working TFLite interpreter is loaded. The stub returns
    // false; the real implementation flips this true once the model file
    // is mapped and the interpreter is invocable.
    bool isReady() const;

    // Compute a per-pixel person mask the same size as `rgba`. Returns
    // an 8-bit Grayscale8 QImage: 255 = full person, 0 = full background.
    // The stub returns a centred radial gradient regardless of input
    // content; the real implementation does TFLite inference on a
    // downscaled view, then upscales the result back.
    QImage segment(const QImage &rgba);

private:
    bool m_ready = false;   // stub: stays false. Real impl flips to true.
};
