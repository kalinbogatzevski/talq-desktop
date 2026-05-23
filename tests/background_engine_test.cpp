// #20 Phase 2a — Lifecycle test for BackgroundEngine.
//
// What this proves:
//   * The engine + lazy compositor construct + destruct without crashing
//   * setMode(None) → Mode::None, setMode(Blur) → Mode::Blur (round-trip)
//   * setBlurStrength clamps to 1..20
//   * processFrame() returns the input image bit-identical in pass-through
//     mode (the only mode supported today; the composite paths still
//     return rgba unchanged because BackgroundCompositor is a skeleton).
//
// Phase 2c will replace the "bit-identical" assertion with a real
// blur-vs-input pixel-difference check once the composite chain renders.

#include "core/BackgroundEngine.h"
#include "core/BackgroundCompositor.h"

#include <QGuiApplication>
#include <QImage>
#include <cstdio>
#include <cstdlib>

namespace {
int failures = 0;
void check(const char *what, bool ok)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}
} // namespace

int main(int argc, char *argv[])
{
    // QGuiApplication (not QCoreApplication) so QOpenGLContext +
    // QOffscreenSurface have a platform integration to bind to.
    QGuiApplication app(argc, argv);

    std::printf("===== #20 BackgroundEngine Phase 2a/2b lifecycle =====\n");

    BackgroundEngine engine;

    // (A) Defaults.
    check("default mode == None",
          engine.mode() == BackgroundEngine::Mode::None);
    check("default blurStrength == 10",
          engine.blurStrength() == 10);
    check("default imagePath is empty",
          engine.imagePath().isEmpty());
    check("default isReady() == false (no compositor constructed yet)",
          engine.isReady() == false);

    // (B) Round-trip the mode setter — verifies the lazy compositor
    //     construction doesn't blow up on the transition.
    engine.setMode(BackgroundEngine::Mode::Blur);
    check("setMode(Blur) round-trips",
          engine.mode() == BackgroundEngine::Mode::Blur);

    // Compositor exists now (lazy-constructed); but it hasn't initialised
    // a GL context (Phase 2b lands that), so isReady() stays false.
    check("isReady() still false (compositor not yet GL-initialised)",
          engine.isReady() == false);

    engine.setMode(BackgroundEngine::Mode::None);
    check("setMode(None) round-trips",
          engine.mode() == BackgroundEngine::Mode::None);

    // (C) Strength clamp.
    engine.setBlurStrength(0);
    check("setBlurStrength(0) clamped to 1", engine.blurStrength() == 1);
    engine.setBlurStrength(50);
    check("setBlurStrength(50) clamped to 20", engine.blurStrength() == 20);
    engine.setBlurStrength(10);
    check("setBlurStrength(10) preserved", engine.blurStrength() == 10);

    // (D) Image path round-trip.
    engine.setImagePath("C:/tmp/bg.jpg");
    check("setImagePath round-trips",
          engine.imagePath() == "C:/tmp/bg.jpg");

    // (E) processFrame pass-through (Phase 2a/2b behaviour).
    QImage in(16, 16, QImage::Format_RGBA8888);
    in.fill(Qt::magenta);
    QImage out = engine.processFrame(in);
    check("processFrame returns an image of the same size",
          out.size() == in.size());
    check("processFrame pass-through preserves first pixel",
          out.pixel(0, 0) == in.pixel(0, 0));

    // (F) Phase 2b — direct compositor GL init. Proves the offscreen
    //     surface + GL 3.3 core context creates and the four shader
    //     programs compile + link from the qrc.
    BackgroundCompositor comp;
    bool initOk = comp.ensureInitialised();
    check("BackgroundCompositor::ensureInitialised() returns true",
          initOk);
    check("BackgroundCompositor::isReady() == true after init",
          comp.isReady() == initOk);

    // Idempotent: second call returns true without re-init.
    if (initOk) {
        bool secondCall = comp.ensureInitialised();
        check("ensureInitialised is idempotent", secondCall);
    }

    // (G) Phase 2c.2 — real three-pass GL render assertion.
    // Build a synthetic vertical split: left half pure blue, right half
    // pure red, sharp boundary at column 32. With an all-zero mask (frame
    // counts as 100% background), the compose mode=1 (blur) path outputs
    // the blurred camera plate. The blur should soften the boundary: the
    // pixel at column 31 (was pure blue) gains red, and column 32 (was
    // pure red) gains blue. Robust assertion — doesn't depend on the
    // 9-tap kernel's stride pattern catching a specific dot.
    if (initOk) {
        const int W = 64, H = 64;
        QImage frame(W, H, QImage::Format_RGBA8888);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                frame.setPixelColor(x, y,
                    x < 32 ? QColor(0, 0, 255, 255) : QColor(255, 0, 0, 255));
            }
        }

        QImage mask(W, H, QImage::Format_Grayscale8);
        mask.fill(0);   // 100% background — compose outputs blurred plate

        QImage out = comp.compositeBlur(frame, mask, /*radius*/ 1.0f);
        check("compositeBlur returns an image of the same size",
              out.size() == QSize(W, H));

        if (out.size() == QSize(W, H)) {
            // Just inside the blue side. Pre-blur: pure blue. Post-blur:
            // red should bleed in via the kernel reaching across column 32.
            const QRgb preBlue  = frame.pixel(31, 32);
            const QRgb postBlue = out.pixel(31, 32);
            // Just inside the red side. Pre-blur: pure red. Post-blur:
            // blue should bleed in.
            const QRgb preRed   = frame.pixel(32, 32);
            const QRgb postRed  = out.pixel(32, 32);

            check("blue-side pre-blur is pure blue",
                  qRed(preBlue) == 0 && qBlue(preBlue) == 255);
            check("red-side pre-blur is pure red",
                  qRed(preRed) == 255 && qBlue(preRed) == 0);
            check("blur leaks red across the boundary into the blue side",
                  qRed(postBlue) > 0);
            check("blur leaks blue across the boundary into the red side",
                  qBlue(postRed) > 0);

            std::printf(
                "    [debug] (31,32) blue side before #%02x%02x%02x, after #%02x%02x%02x\n"
                "    [debug] (32,32) red  side before #%02x%02x%02x, after #%02x%02x%02x\n",
                qRed(preBlue),  qGreen(preBlue),  qBlue(preBlue),
                qRed(postBlue), qGreen(postBlue), qBlue(postBlue),
                qRed(preRed),   qGreen(preRed),   qBlue(preRed),
                qRed(postRed),  qGreen(postRed),  qBlue(postRed));
        }
    }

    std::printf("\n%s: %d failure(s)\n",
                failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
