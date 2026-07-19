#pragma once
// 0.61.0 "Blue Fiesta Week 2" — convert a frame count over an elapsed wall-time
// window (ns) to an integer fps, for the on-screen send-fps badge. Pure, tested.
// Returns 0 for an empty or non-positive window (no divide-by-zero, no garbage).
namespace talq {
inline int fpsFromCount(long long frames, long long elapsedNs) {
    if (elapsedNs <= 0 || frames <= 0) return 0;
    const double fps = (double)frames * 1'000'000'000.0 / (double)elapsedNs;
    return (int)(fps + 0.5);
}
} // namespace talq
