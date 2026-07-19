#pragma once
#include <vector>
// 0.61.0 "Blue Fiesta Week 2" — vertical stacking layout for top-center in-call
// notice pills, so two simultaneous notices never overlap. Given the heights of
// the currently-active notices in priority (top-to-bottom) order, a start-y, and
// a gap, return each notice's y. Pure, unit-tested.
namespace talq {
inline std::vector<double> noticeStackYs(double startY, double gap,
                                         const std::vector<double> &heights) {
    std::vector<double> ys;
    ys.reserve(heights.size());
    double y = startY;
    for (double h : heights) { ys.push_back(y); y += h + gap; }
    return ys;
}
} // namespace talq
