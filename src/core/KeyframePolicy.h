#pragma once

// Periodic-IDR policy for the screen share. Pure C++ — unit-tested in
// tests/keyframe_policy_test.cpp.
//
// WHY THIS EXISTS. A screen share that lost a packet stayed visibly corrupted
// on a completely STATIC screen (field 2026-07-28). A still image costs almost
// nothing to encode and should be pixel-perfect after one keyframe, so
// persistent damage means a reference frame was hit and nothing repaired it.
//
// The receiving side already asks: SubscribePipeline sends an RTCP PLI at
// 0.5/1.5/3 s and then every 5 s for the whole share. Since the corruption
// still outlived that, the request is not producing an IDR — either it does not
// survive the trip through the SFU, or the publisher's encoder ignores it. The
// repair therefore has to be guaranteed by the SENDER, independently of PLI.
//
// The economics favour a periodic IDR here: a keyframe is cheapest exactly when
// it matters most (static screen, nothing else in flight) and dearest exactly
// when it matters least (heavy motion, where damage is overwritten within a
// frame or two anyway).
namespace talq {

// GOP length in FRAMES for a repair interval of `seconds`.
// Returns 0 when the inputs cannot produce a sane interval — callers MUST treat
// 0 as "leave the encoder's own default alone" rather than setting it, because
// 0 means all-intra on some encoders and infinite on others.
inline int keyframeIntervalFrames(int fps, int seconds)
{
    if (fps <= 0 || seconds <= 0) return 0;
    long long n = (long long)fps * (long long)seconds;
    // Ceiling: a GOP longer than this defeats the point of a backstop.
    if (n > 600) n = 600;
    return (int)n;
}

} // namespace talq
