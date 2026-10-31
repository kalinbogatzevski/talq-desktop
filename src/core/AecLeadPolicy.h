#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

// AecLeadPolicy -- pure decisions for the "AEC align" controller: given the measured
// echo delay, how far to move the loopback reference's timestamps so the reference
// reaches the echo canceller BEFORE its echo, and when to give a shift back. No
// toolkit, no media framework, no clock, no threads: the caller feeds plain numbers
// (milliseconds from one monotonic clock), so the whole policy is unit-testable
// without a call -- see tests/aec_lead_policy_test.cpp. The delay estimator that
// produces D_raw lives in AecDelayEstimator.h; this header never sees it.
//
// Why it exists (field bench 2026-10-30): the canceller needs the reference ahead of
// its echo, and the reference's timestamps depend on machine state. On one laptop the
// reference ran about 285 ms late (D_total -245 ms): no cancellation, residual -23 dB.
// A forced 280 ms shift of the reference timestamps restored full cancellation (-68 dB,
// D_total +39 ms), while shifts of 330 ms and more failed: the data is only so early
// (the "headroom"), no label can fix a reference that physically arrives late.
//
// THE QUANTITIES (all milliseconds)
//   D_raw : mic echo running time minus the reference sample's running time, measured
//           on the RAW reference labels, i.e. BEFORE any shift this controller applied.
//           That is what makes the policy OPEN LOOP: D_raw does not move when S moves,
//           so feeding the same D_raw after a change must give "hold" (a closed loop on
//           the shifted labels oscillated in simulation).
//   S     : the shift, reference labels moved EARLIER by S (S > 0 helps a late reference).
//   L     : latency of the probe path; delay: the plugin's own buffering. Both measured.
//   lead  = D_raw + S - L + delay     (bench window, Phase D 2026-10-30: the canceller works for
//                                      lead +4 .. +44 and fails at <= -10 and at >= +58; the
//                                      target is the middle of it, 25)
//   S*    = target + L - delay - D_raw (the S that puts the lead on the target)
//   m     : headroom, the largest EXTRA shift that is physically still available.
//           Raising S by dS lowers m by dS (HeadroomMin keeps its 1 s and 10 s minima).
//
// THE RULES
//   - Act only on a LOCKED estimate, in mode On (Observe reports the same decision and
//     the caller logs "would shift"), with L known and no forced shift in effect.
//   - Trigger: lead < lowEdge (reference too late: raise S) or lead > leadHigh (reference too
//     EARLY, a slow render path such as Bluetooth or HDMI: delay the labels, S < 0). Between
//     the two the lead is fine: hold. Both edges sit inside the measured window with room to
//     spare, and the target is its middle, so one shift lands well away from either edge.
//     Negative shifts are on by default; TALQ_AEC_ALIGN_ALLOW_NEG=0 makes the upper edge a
//     log line (`lead-high`) again. A delay needs no headroom: the data is already there.
//   - A raise never exceeds the headroom: S <= S_cur + min(m10s, m1s) - margin. A capped
//     result is accepted only if it still leaves lead >= 5 ms; otherwise the machine is
//     "unfixable by labels" and we hold (`no-headroom`) rather than shift for nothing.
//   - Minimum step and dwell stop hunting. A back-off (the guard, below) bypasses dwell.
//   - THE GUARD: if the last second's minimum headroom stays negative while S > 0, the
//     shift is eating data that is not there. "Stays" = backoffTicks consecutive ticks
//     (a one-off reference stall dips m1s for at most 1.2 s, i.e. 3 ticks, and is not a
//     shortage). Back off by (margin - the least negative m1s of the streak) at once and
//     latch the result as a ceiling, so the policy cannot re-raise into the same wall
//     (raise / back-off ping-pong). The ceiling belongs to ONE timeline and ONE lock: it is
//     released on a reset, on a re-lock to a different delay, and after capLatchTtlS.

namespace talq {

enum class AlignMode { Off, Observe, On };

// The ONE place the shipped default lives. On since 0.79.2 (Zenbook bench, Phase E: the canceller
// works with it and fails without it; the Yoga, which has no echo path, never shifts). Known limit:
// a far end that echoes a headset user back looks like a late reference (see the plan doc);
// TALQ_AEC_ALIGN=off|observe switches it back.
constexpr AlignMode kAlignDefaultMode = AlignMode::On;

// "No change yet this build" for AlignInput::sinceChangeMs / BackoffInput::sinceBackoffMs.
constexpr double kAlignNeverMs = 1e12;

inline const char *alignModeName(AlignMode m)
{
    switch (m) {
    case AlignMode::Off:     return "off";
    case AlignMode::Observe: return "observe";
    case AlignMode::On:      return "on";
    }
    return "off";
}

// Every switch and constant of the controller. Defaults are the shipped values.
struct AlignParams {
    // TALQ_AEC_ALIGN*  (environment switches, see fromGetter)
    AlignMode mode        = kAlignDefaultMode;   // TALQ_AEC_ALIGN = off | observe | on
    double targetMs       = 25.0;    // _TARGET_MS    lead the shift aims for (middle of the +4 .. +44 window)
    double lowEdgeMs      = 8.0;     // _LOW_MS       a lead below this triggers action
    double leadHighMs     = 45.0;    // _HIGH_MS      a lead above this triggers a delay (needs allowNeg)
    double marginMs       = 20.0;    // _MARGIN_MS    headroom always left unused
    double minStepMs      = 15.0;    // _MIN_STEP_MS  smaller changes are not worth the glitch
    double dwellS         = 20.0;    // _DWELL_S      minimum time between changes
    double maxShiftMs     = 500.0;   // _MAX_SHIFT_MS upper clamp of S
    bool   allowNeg       = true;    // _ALLOW_NEG    negative shifts (=0 turns the upper edge into a log line)
    // TALQ_TEST_REF_LABEL_SKEW_MS=<n>[,AT_MS=<t>]  test only: skew added to the SOURCE label
    double labelSkewMs    = 0.0;
    double labelSkewAtMs  = 0.0;     // when the skew starts, from the first buffer

    // Constants without a switch.
    double minNegShiftMs  = -150.0;  // lower clamp of S when allowNeg
    double cappedLeadMinMs = 5.0;    // a capped raise must still leave at least this lead
    double backoffTriggerMs = -5.0;  // guard: m1s below this means the shift outruns the data
    int    backoffTicks   = 4;       // ... on this many consecutive 500 ms ticks (a stall shows in <= 3)
    double backoffGapMs   = 2000.0;  // guard acts at most once per this (> the 1 s window)
    double capLatchTtlS   = 120.0;   // a back-off ceiling lapses after this
    double delayFallbackMs = 0.0;    // plugin delay when it could not be measured: it keeps 0 then too
    double delayMaxMs     = 100.0;   // plugin delay clamp

    // Reads the switches through `get(name) -> const char *` (nullptr when unset), so the
    // parsing is testable; fromEnvironment() hands it getenv. Garbage keeps the default,
    // numbers are clamped to a sane range.
    template <class Get>
    static AlignParams fromGetter(Get get)
    {
        AlignParams p;
        p.mode = parseMode(get("TALQ_AEC_ALIGN"), p.mode);
        p.targetMs   = parseMs(get("TALQ_AEC_ALIGN_TARGET_MS"),   p.targetMs,   10.0, 200.0);
        p.lowEdgeMs  = parseMs(get("TALQ_AEC_ALIGN_LOW_MS"),      p.lowEdgeMs,   0.0,  100.0);
        p.leadHighMs = parseMs(get("TALQ_AEC_ALIGN_HIGH_MS"),     p.leadHighMs,  10.0, 400.0);
        p.marginMs   = parseMs(get("TALQ_AEC_ALIGN_MARGIN_MS"),   p.marginMs,    0.0,  200.0);
        p.minStepMs  = parseMs(get("TALQ_AEC_ALIGN_MIN_STEP_MS"), p.minStepMs,   1.0,  200.0);
        p.dwellS     = parseMs(get("TALQ_AEC_ALIGN_DWELL_S"),     p.dwellS,      0.0,  600.0);
        p.maxShiftMs = parseMs(get("TALQ_AEC_ALIGN_MAX_SHIFT_MS"), p.maxShiftMs, 0.0,  500.0);
        p.allowNeg = parseBool(get("TALQ_AEC_ALIGN_ALLOW_NEG"), p.allowNeg);
        if (p.lowEdgeMs >= p.targetMs) p.lowEdgeMs = p.targetMs - 1.0;   // the trigger must sit below the target
        if (p.leadHighMs <= p.targetMs) p.leadHighMs = p.targetMs + 1.0; // ... and the upper one above it
        if (const char *skew = get("TALQ_TEST_REF_LABEL_SKEW_MS")) {
            char *end = nullptr;
            const double v = std::strtod(skew, &end);
            if (end != skew && std::isfinite(v)) p.labelSkewMs = std::min(1000.0, std::max(-1000.0, v));
            for (const char *at = skew; *at; ++at) {
                if (at[0] == 'A' && at[1] == 'T' && at[2] == '_' && at[3] == 'M' && at[4] == 'S' && at[5] == '=') {
                    const double t = std::strtod(at + 6, &end);
                    if (end != at + 6 && std::isfinite(t)) p.labelSkewAtMs = std::min(3.6e6, std::max(0.0, t));
                    break;
                }
            }
        }
        return p;
    }
    static AlignParams fromEnvironment()
    {
        return fromGetter([](const char *name) { return std::getenv(name); });
    }

    static AlignMode parseMode(const char *s, AlignMode def)
    {
        if (!s) return def;
        char b[16] = {};
        size_t n = 0;
        for (; s[n] && n + 1 < sizeof b; ++n) b[n] = static_cast<char>(s[n] >= 'A' && s[n] <= 'Z' ? s[n] + 32 : s[n]);
        if (s[n]) return def;                                   // too long to be a mode
        auto is = [&b](const char *w) { return sameWord(b, w); };
        if (is("off") || is("0") || is("false") || is("no"))     return AlignMode::Off;
        if (is("observe"))                                       return AlignMode::Observe;
        if (is("on") || is("1") || is("true") || is("yes"))      return AlignMode::On;
        return def;
    }

private:
    // 1/t/y(es)/on = true, 0/f/n(o)/off = false (first letter, any case); anything else keeps the default.
    static bool parseBool(const char *s, bool def)
    {
        if (!s) return def;
        switch (s[0]) {
        case '1': case 't': case 'T': case 'y': case 'Y': return true;
        case '0': case 'f': case 'F': case 'n': case 'N': return false;
        case 'o': case 'O': return s[1] == 'n' || s[1] == 'N' ? true : (s[1] == 'f' || s[1] == 'F' ? false : def);
        default: return def;
        }
    }
    static bool sameWord(const char *a, const char *b)
    {
        for (; *a && *a == *b; ++a, ++b) {}
        return *a == *b;
    }
    static double parseMs(const char *s, double def, double lo, double hi)
    {
        if (!s) return def;
        char *end = nullptr;
        const double v = std::strtod(s, &end);
        if (end == s || !std::isfinite(v)) return def;
        return std::min(hi, std::max(lo, v));
    }
};

// ---------------------------------------------------------------------------------
// decideShift

enum class AlignHold {
    None,          // not a hold: the decision is a Set
    Off,           // mode off
    NotLocked,     // no locked estimate (or a non-finite reading)
    LUnknown,      // probe latency not known yet: the lead cannot be computed
    Forced,        // a forced shift is in effect: the controller observes only
    InBand,        // lead between lowEdge and leadHigh: nothing to fix
    LeadHigh,      // lead above leadHigh and negative shifts are switched off: log only
    NegDisabled,   // the fix would need a negative shift and they are off
    NoHeadroom,    // the data is not there (or the cap leaves too little lead): unfixable by labels
    MinStep,       // the change is smaller than the minimum step
    Dwell,         // too soon after the previous change
};

inline const char *alignHoldName(AlignHold h)
{
    switch (h) {
    case AlignHold::None:        return "none";
    case AlignHold::Off:         return "off";
    case AlignHold::NotLocked:   return "not-locked";
    case AlignHold::LUnknown:    return "L-unknown";
    case AlignHold::Forced:      return "forced";
    case AlignHold::InBand:      return "in-band";
    case AlignHold::LeadHigh:    return "lead-high";
    case AlignHold::NegDisabled: return "neg-disabled";
    case AlignHold::NoHeadroom:  return "no-headroom";
    case AlignHold::MinStep:     return "min-step";
    case AlignHold::Dwell:       return "dwell";
    }
    return "none";
}

inline double alignLeadMs(double rawDelayMs, double shiftMs, double latencyMs, double delayMs)
{
    return rawDelayMs + shiftMs - latencyMs + delayMs;
}

// Plugin delay for the lead: the measurement when it succeeded, clamped; else the fallback.
// The plugin starts from 0 and keeps it when its own latency query fails or returns an
// invalid minimum, so the fallback is 0, not a typical figure: a guess here would put the
// lead off by that guess exactly when the plugin has nothing.
inline double alignClampDelayMs(const AlignParams &p, bool measured, double measuredMs)
{
    if (!measured || !std::isfinite(measuredMs)) return p.delayFallbackMs;
    return std::min(p.delayMaxMs, std::max(0.0, measuredMs));
}

// Headroom m of one microphone frame, from running times in ns:
//   m = refEndShiftedRt - (micFrameStartRt + delay - L + slack)
// Raising S moves refEndShiftedRt earlier by the same amount, so m falls by dS.
inline double alignHeadroomMs(int64_t refEndShiftedRtNs, int64_t micFrameStartRtNs,
                              double delayMs, double latencyMs, double slackMs = 10.0)
{
    return static_cast<double>(refEndShiftedRtNs - micFrameStartRtNs) / 1.0e6
           - (delayMs - latencyMs + slackMs);
}

struct AlignInput {
    bool   locked        = false;           // the estimator holds a LOCK
    double rawDelayMs    = 0.0;             // D_raw of that lock
    bool   latencyKnown  = false;           // L measured
    double latencyMs     = 0.0;
    double delayMs       = 10.0;            // plugin delay (alignClampDelayMs)
    double shiftMs       = 0.0;             // S in effect now
    bool   headroomKnown = false;           // m10s / m1s hold samples
    double m10sMs        = 0.0;
    double m1sMs         = 0.0;
    double sinceChangeMs = kAlignNeverMs;   // since the last applied change
    bool   forced        = false;           // a forced shift is in effect
    double capLatchMs    = -1.0;            // ceiling latched by a back-off (AlignGuard::latchMs), < 0 = none
};

struct AlignDecision {
    bool      set = false;                  // true: move the shift to shiftMs (ABSOLUTE, not a delta)
    double    shiftMs = 0.0;
    AlignHold hold = AlignHold::None;
    bool      capped = false;               // set, but below S* (headroom / latch / max shift)
    double    leadBeforeMs = 0.0;           // lead at the current shift
    double    leadAfterMs = 0.0;            // lead at the shift considered (valid once past the band check)
    double    wantMs = 0.0;                 // S* after rounding and clamping, before the headroom cap
    bool      latchLimited = false;         // a NoHeadroom hold whose binding limit was the back-off ceiling, not the data
};

inline AlignDecision decideShift(const AlignParams &p, const AlignInput &in)
{
    AlignDecision d;
    auto hold = [&d](AlignHold h) { d.hold = h; return d; };

    if (p.mode == AlignMode::Off) return hold(AlignHold::Off);
    if (in.forced) return hold(AlignHold::Forced);
    if (!in.locked || !std::isfinite(in.rawDelayMs) || !std::isfinite(in.shiftMs)) return hold(AlignHold::NotLocked);
    if (!in.latencyKnown || !std::isfinite(in.latencyMs) || !std::isfinite(in.delayMs)) return hold(AlignHold::LUnknown);

    d.leadBeforeMs = alignLeadMs(in.rawDelayMs, in.shiftMs, in.latencyMs, in.delayMs);
    const bool high = d.leadBeforeMs > p.leadHighMs;
    if (high && !p.allowNeg) return hold(AlignHold::LeadHigh);
    if (!high && d.leadBeforeMs >= p.lowEdgeMs) return hold(AlignHold::InBand);

    // S* is a function of D_raw alone, never of the current S (open loop). Whole ms.
    const double raw = std::floor(p.targetMs + in.latencyMs - in.delayMs - in.rawDelayMs + 0.5);
    if (raw < 0.0 && !p.allowNeg) return hold(AlignHold::NegDisabled);
    const double lo = p.allowNeg ? p.minNegShiftMs : 0.0;
    double want = std::min(std::max(raw, lo), p.maxShiftMs);
    d.wantMs = want;
    bool limited = raw > p.maxShiftMs;

    bool byLatch = false;      // the ceiling, not the data, is what held the raise back
    if (want > in.shiftMs) {   // a RAISE must fit in the data that physically exists
        if (!in.headroomKnown) return hold(AlignHold::NoHeadroom);
        const double headCap = in.shiftMs + std::min(in.m10sMs, in.m1sMs) - p.marginMs;
        double cap = headCap;
        if (in.capLatchMs >= 0.0) cap = std::min(cap, in.capLatchMs);
        if (want > cap) { want = cap; limited = true; byLatch = cap < headCap; }
        if (want <= in.shiftMs) {   // the cap leaves nothing to raise
            d.latchLimited = byLatch;
            return hold(AlignHold::NoHeadroom);
        }
    }
    d.leadAfterMs = alignLeadMs(in.rawDelayMs, want, in.latencyMs, in.delayMs);
    if (limited && d.leadAfterMs < p.cappedLeadMinMs) {
        d.latchLimited = byLatch;
        return hold(AlignHold::NoHeadroom);
    }

    if (std::fabs(want - in.shiftMs) < p.minStepMs) return hold(AlignHold::MinStep);
    if (in.sinceChangeMs < p.dwellS * 1000.0) return hold(AlignHold::Dwell);

    d.set = true;
    d.shiftMs = want;
    d.capped = limited;
    return d;
}

// ---------------------------------------------------------------------------------
// decideBackoff -- the guard

struct BackoffInput {
    bool   forced         = false;          // a forced shift is in effect: no controller action
    double shiftMs        = 0.0;            // S in effect now (or pending)
    bool   refFresh       = false;          // the reference delivered recently
    bool   latencyKnown   = false;
    bool   headroomKnown  = false;          // m1s holds samples
    double m1sMs          = 0.0;
    double sinceBackoffMs = kAlignNeverMs;  // since the last back-off
    double capLatchMs     = -1.0;           // latch so far, < 0 = none
};

struct BackoffDecision {
    bool   act = false;
    double newShiftMs = 0.0;                // lower S to this, urgently (bypasses dwell)
    double capLatchMs = -1.0;               // ceiling for the rest of the build
    double reduceMs = 0.0;
};

// True when this tick's reading says the shift outruns the data: the guard may act, the
// headroom is known and below the trigger. One tick of it is not yet a reason (AlignGuard).
inline bool alignBackoffLow(const AlignParams &p, const BackoffInput &in)
{
    if (p.mode != AlignMode::On || in.forced) return false;
    if (!(in.shiftMs > 0.0) || !in.refFresh || !in.latencyKnown || !in.headroomKnown) return false;
    return std::isfinite(in.m1sMs) && in.m1sMs < p.backoffTriggerMs;
}

// The arithmetic of ONE back-off, given a reading that is already believed (AlignGuard
// debounces; this does not).
inline BackoffDecision decideBackoff(const AlignParams &p, const BackoffInput &in)
{
    BackoffDecision b;
    b.capLatchMs = in.capLatchMs;
    if (!alignBackoffLow(p, in)) return b;
    if (in.sinceBackoffMs < p.backoffGapMs) return b;   // the 1 s window still holds the pre-change samples

    b.reduceMs = p.marginMs - in.m1sMs;                 // brings m1s back to the margin
    b.newShiftMs = std::max(0.0, in.shiftMs - b.reduceMs);
    b.capLatchMs = in.capLatchMs >= 0.0 ? std::min(in.capLatchMs, b.newShiftMs) : b.newShiftMs;
    b.act = true;
    return b;
}

// ---------------------------------------------------------------------------------
// AlignGuard -- the guard's memory: the debounce streak, the rate limit and the ceiling

// Why a streak and not the latest reading: m1s is a 1 s minimum of per-buffer samples, so
// ONE late reference buffer (a delivery stall, a scheduler hiccup) holds it low for 1 s plus
// the 200 ms the headroom meter keeps sampling a silent reference, i.e. for at most 3 ticks
// 500 ms apart. Treating that as a shortage cut S by (margin - the dip) and latched it:
// a 100 ms stall took a working 275 ms shift to 180 and the echo came back. A real
// shortage stays: the data is not there on every tick. The cut is sized from the LEAST
// negative reading of the streak, the deficit that persisted, not the worst sample.
//
// The ceiling a back-off latches is only as good as the timeline it was measured on: the
// owner releases it on a reset and on a re-lock to a different delay, and it lapses on its
// own after capLatchTtlS, so a transient cannot disable the controller for the whole call.
// Single thread (the worker); times in ms from one monotonic clock.
class AlignGuard {
public:
    // One call per worker tick. `in.capLatchMs` and `in.sinceBackoffMs` are filled in here.
    BackoffDecision tick(const AlignParams &p, BackoffInput in, int64_t nowMs)
    {
        in.capLatchMs = latchMs(p, nowMs);
        in.sinceBackoffMs = m_lastBackoffMs >= 0 ? static_cast<double>(nowMs - m_lastBackoffMs) : kAlignNeverMs;
        BackoffDecision none;
        none.capLatchMs = in.capLatchMs;
        if (!alignBackoffLow(p, in)) { m_run = 0; return none; }   // any tick that does not qualify ends the streak
        m_worst = m_run == 0 ? in.m1sMs : std::max(m_worst, in.m1sMs);
        ++m_run;
        if (m_run < p.backoffTicks) return none;
        in.m1sMs = m_worst;
        const BackoffDecision b = decideBackoff(p, in);
        if (b.act) {
            m_run = 0;
            m_lastBackoffMs = nowMs;
            m_latch = b.capLatchMs;
            m_latchAtMs = nowMs;
        }
        return b;
    }

    // The ceiling in force at `nowMs`, or -1 (none, released, or lapsed).
    double latchMs(const AlignParams &p, int64_t nowMs) const
    {
        if (m_latch < 0.0) return -1.0;
        if (static_cast<double>(nowMs - m_latchAtMs) >= p.capLatchTtlS * 1000.0) return -1.0;
        return m_latch;
    }
    // True when a ceiling was in force and is now gone.
    bool releaseLatch(const AlignParams &p, int64_t nowMs)
    {
        const bool had = latchMs(p, nowMs) >= 0.0;
        m_latch = -1.0;
        return had;
    }
    // A new timeline: nothing measured on the old one applies.
    bool reset(const AlignParams &p, int64_t nowMs)
    {
        const bool had = releaseLatch(p, nowMs);
        m_run = 0;
        m_lastBackoffMs = -1;
        return had;
    }
    int streak() const { return m_run; }

private:
    int     m_run = 0;                 // consecutive qualifying ticks
    double  m_worst = 0.0;             // the least negative m1s of the streak
    int64_t m_lastBackoffMs = -1;
    double  m_latch = -1.0;
    int64_t m_latchAtMs = 0;
};

// ---------------------------------------------------------------------------------
// A shift request in ONE 64-bit word

// The worker and the reference re-open (a pool thread) both ask the reference probe for a
// shift; the probe reads on a streaming thread. As three separate atomics a reader could
// combine the shift of one request with the urgency of another. One word cannot be torn:
//   bits  0..24  shift in microseconds, biased by 2^24 (so a pending word is never 0)
//   bit   25     urgent: apply at the next buffer
//   bits 26..63  the monotonic ms at which it was asked (the 4 s timeout counts from here)
// A word of 0 means "nothing pending".
constexpr int64_t kAlignReqBias = 1LL << 24;

struct AlignRequest {
    int64_t shiftNs = 0;
    bool    urgent = false;
    int64_t sinceMs = 0;
};

inline uint64_t alignPackRequest(int64_t shiftNs, bool urgent, int64_t sinceMs)
{
    int64_t us = shiftNs >= 0 ? (shiftNs + 500) / 1000 : -((-shiftNs + 500) / 1000);
    us = std::min(kAlignReqBias - 1, std::max(-(kAlignReqBias - 1), us));   // +-16.7 s, far beyond any shift
    const uint64_t since = static_cast<uint64_t>(std::max<int64_t>(0, sinceMs)) & ((1ULL << 38) - 1);
    return (since << 26) | (urgent ? (1ULL << 25) : 0ULL) | static_cast<uint64_t>(us + kAlignReqBias);
}

inline AlignRequest alignUnpackRequest(uint64_t word)
{
    AlignRequest r;
    r.shiftNs = (static_cast<int64_t>(word & 0x1FFFFFFULL) - kAlignReqBias) * 1000;
    r.urgent = ((word >> 25) & 1ULL) != 0;
    r.sinceMs = static_cast<int64_t>(word >> 26);
    return r;
}

// ---------------------------------------------------------------------------------
// Observe mode's "would shift" line

// The actuator never moves in observe mode, so the same answer comes back every estimate.
// Say it when it moved by a real step (a whole-ms wobble of a locked delay is not one), or
// once a minute so a long log still carries it. lastShiftMs NaN = never said.
inline bool alignWouldLogDue(double lastShiftMs, int64_t lastLogMs, double shiftMs, int64_t nowMs, double minStepMs)
{
    if (std::isnan(lastShiftMs)) return true;
    if (std::fabs(shiftMs - lastShiftMs) >= minStepMs) return true;
    return nowMs - lastLogMs >= 60000;
}

// ---------------------------------------------------------------------------------
// HeadroomMin -- sliding minimum of the per-frame headroom

// Monotonic-queue sliding minimum over the last `windowMs`: a sample is in the window
// while now - t < windowMs. Fixed storage allocated once (it is fed from a streaming
// thread); when it fills up, the newest sample is MERGED into the tail (lower value, later
// time), which can only make the minimum lower and longer-lived, never higher. Single
// thread: the caller publishes the results through atomics.
class SlidingMin {
public:
    explicit SlidingMin(int64_t windowMs, size_t capacity = 4096)
        : m_window(windowMs), m_buf(capacity < 2 ? 2 : capacity) {}

    void push(int64_t tMs, double v)
    {
        if (!std::isfinite(v)) return;
        if (m_count > 0 && tMs < at(m_count - 1).t) tMs = at(m_count - 1).t;   // a clock stepping back
        expire(tMs);
        while (m_count > 0 && at(m_count - 1).v >= v) --m_count;
        if (m_count == m_buf.size()) {
            Entry &tail = at(m_count - 1);
            tail.v = std::min(tail.v, v);
            tail.t = tMs;
            return;
        }
        at(m_count) = Entry{ tMs, v };
        ++m_count;
    }
    // False when the window holds no sample.
    bool value(int64_t nowMs, double &out)
    {
        expire(nowMs);
        if (m_count == 0) return false;
        out = at(0).v;
        return true;
    }
    void clear() { m_head = 0; m_count = 0; }

private:
    struct Entry { int64_t t; double v; };
    Entry &at(size_t i) { return m_buf[(m_head + i) % m_buf.size()]; }
    void expire(int64_t nowMs)
    {
        while (m_count > 0 && nowMs - at(0).t >= m_window) {
            m_head = (m_head + 1) % m_buf.size();
            --m_count;
        }
    }

    int64_t m_window;
    std::vector<Entry> m_buf;
    size_t m_head = 0;
    size_t m_count = 0;
};

class HeadroomMin {
public:
    HeadroomMin() : m_1s(1000), m_10s(10000) {}
    void push(int64_t nowMs, double mMs) { m_1s.push(nowMs, mMs); m_10s.push(nowMs, mMs); }
    bool min1s(int64_t nowMs, double &out) { return m_1s.value(nowMs, out); }
    bool min10s(int64_t nowMs, double &out) { return m_10s.value(nowMs, out); }
    void clear() { m_1s.clear(); m_10s.clear(); }

private:
    SlidingMin m_1s;
    SlidingMin m_10s;
};

}  // namespace talq
