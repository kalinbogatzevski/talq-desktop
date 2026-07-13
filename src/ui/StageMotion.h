#pragma once

#include <algorithm>
#include <vector>

// StageMotion — pure tile promote/demote animation math for the active-
// speaker stage. No Qt, no GStreamer: CallStage is an immediate-mode QPainter
// widget whose tiles are value structs holding a rect, and the surface
// already full-repaints on a 33 ms timer — so "animating a tile" is just
// interpolating its rect between paints. This header holds the math; the
// widget owns the timestamps and converts to/from QRectF at the edges.
// Unit-tested without a Qt runtime in tests/stage_motion_test.cpp.
//
// Deliberately NO overshoot/spring easing: an OutBack wobble on live video
// reads as a glitch (the video content itself moves), not delight. Cubic
// in/out only.

namespace talq::motion {

// POD rect (doubles, like QRectF) so the math stays Qt-free.
struct MRect { double x = 0, y = 0, w = 0, h = 0; };

enum class Ease { OutCubic, InOutCubic };

// ---- Durations -------------------------------------------------------------

// Rail -> stage. Fast: the UI must answer immediately when someone speaks —
// the promotion IS the "who's talking" cue, so any slower and it lags the
// audio.
constexpr int kPromoteMs = 240;

// Stage -> rail. Slower: a demotion should SETTLE, not get yanked — this is
// the "dissolves, minimizes back to the sideview" motion, and it carries no
// urgency (the viewer already stopped attending to it).
constexpr int kDemoteMs = 340;

// Floor for any animated move. Below ~120 ms a rect change reads as a teleport
// with a flicker, which is worse than either a clean jump or a visible glide.
constexpr int kMinMs = 120;

// ---- Easing ----------------------------------------------------------------

// 1 - (1-t)^3 — fast start, gentle landing. Input clamped to [0,1].
constexpr double easeOutCubic(double t)
{
    const double c = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    const double u = 1.0 - c;
    return 1.0 - u * u * u;
}

// Symmetric cubic: slow-in, fast middle, slow-out. Input clamped to [0,1].
constexpr double easeInOutCubic(double t)
{
    const double c = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    if (c < 0.5)
        return 4.0 * c * c * c;
    const double u = -2.0 * c + 2.0;
    return 1.0 - u * u * u / 2.0;
}

constexpr double ease(Ease e, double t)
{
    return e == Ease::OutCubic ? easeOutCubic(t) : easeInOutCubic(t);
}

// Interpolate all four components by an ALREADY-EASED fraction `e`
// (0 = from, 1 = to). Kept separate from the easing so a caller can share
// one eased value across several rects in the same relayout.
constexpr MRect lerpRect(const MRect &from, const MRect &to, double e)
{
    return { from.x + (to.x - from.x) * e,
             from.y + (to.y - from.y) * e,
             from.w + (to.w - from.w) * e,
             from.h + (to.h - from.h) * e };
}

// Duration for a (re)targeted move, scaled by how far there is left to go.
// This is what makes a REVERSAL feel right — and what most implementations
// get wrong: a tile only 8% into a demotion whose speaker starts talking
// again must snap back over that 8% quickly, not crawl for the full 340 ms
// as if it had travelled the whole way. `travelDistance` is the length of
// the move being started, `fullDistance` the length of the corresponding
// full rail<->stage journey; the result is baseMs scaled by their ratio,
// clamped to [kMinMs, baseMs]. (baseMs is expected >= kMinMs; degenerate or
// non-positive distances get the floor — a zero-length "move" needs no
// glide.)
inline int retargetDurationMs(double travelDistance, double fullDistance, int baseMs)
{
    if (travelDistance <= 0.0 || fullDistance <= 0.0)
        return kMinMs;
    const double frac = std::min(travelDistance / fullDistance, 1.0);
    const int scaled = int(double(baseMs) * frac + 0.5);
    return std::max(kMinMs, std::min(baseMs, scaled));
}

// The side-by-side speaker grid: n cells inside `stage`, separated by `gap`.
//
// CRITICAL: this does NOT split the stage rect evenly (and is deliberately
// not the screen-share grid). Cameras render with KeepAspectRatioByExpanding
// (crop-to-fill), so a tall/portrait cell crops a 16:9 camera frame down the
// middle and cuts off both sides of the head. Instead each cell is capped at
// 16:9 (never TALLER, i.e. never narrower-than-16:9), and the resulting
// block of cells is CENTRED in the stage — letterboxed stage space beats
// beheaded speakers.
//
// n == 1 returns the FULL stage rect unchanged: that is the shipped,
// field-hardened single-speaker look and must not change.
inline std::vector<MRect> stageCells(int n, const MRect &stage, double gap)
{
    std::vector<MRect> cells;
    if (n <= 0)
        return cells;
    if (n == 1) {
        cells.push_back(stage);
        return cells;
    }

    // Column count: 2 side-by-side is the design centre. 3 speakers go
    // 3-across only on an ultrawide stage (aspect >= 2.2 — three 16:9 cells
    // in a row need ~5.3:1 of width per unit height before capping bites too
    // hard); otherwise 2+1. 4 -> 2x2.
    const double stageAspect = stage.h > 0.0 ? stage.w / stage.h : 16.0 / 9.0;
    const int cols = (n == 2) ? 2
                   : (n == 3) ? (stageAspect >= 2.2 ? 3 : 2)
                              : 2;
    const int rows = (n + cols - 1) / cols;

    const double cellW = std::max(0.0, (stage.w - (cols - 1) * gap) / cols);
    const double rowH  = std::max(0.0, (stage.h - (rows - 1) * gap) / rows);
    const double cellH = std::min(rowH, cellW * 9.0 / 16.0); // never taller than 16:9

    // Centre the block of cells in the stage.
    const double blockW = cols * cellW + (cols - 1) * gap;
    const double blockH = rows * cellH + (rows - 1) * gap;
    const double x0 = stage.x + (stage.w - blockW) / 2.0;
    const double y0 = stage.y + (stage.h - blockH) / 2.0;

    for (int i = 0; i < n; ++i) {
        const int r = i / cols;
        const int c = i % cols;
        // Centre a partial last row (e.g. the lone 3rd cell of a 2x2) so it
        // doesn't sit orphaned in the bottom-left.
        const int inRow = std::min(cols, n - r * cols);
        const double rowX0 = x0 + (cols - inRow) * (cellW + gap) / 2.0;
        cells.push_back({ rowX0 + c * (cellW + gap),
                          y0 + r * (cellH + gap),
                          cellW, cellH });
    }
    return cells;
}

} // namespace talq::motion
