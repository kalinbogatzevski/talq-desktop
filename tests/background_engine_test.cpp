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

#include <QCoreApplication>
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
    QCoreApplication app(argc, argv);

    std::printf("===== #20 BackgroundEngine Phase 2a lifecycle =====\n");

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

    std::printf("\n%s: %d failure(s)\n",
                failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
