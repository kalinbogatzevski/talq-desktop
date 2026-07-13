#pragma once

// Pure decision logic for the host-protection memory watchdog (CallManager's
// onLoadTick): hard-shed every peer to the 180p layer when memory genuinely
// overloads the host (original field case: Ivan ~2 GB and climbing on a
// stalled hybrid-GPU decode, chopped audio), restore when it eases.
// Hysteresis on every mark so it cannot oscillate. No Qt, no GStreamer — the
// hysteresis is unit-tested as a sequence in tests/mem_shed_policy_test.cpp.
// The side effects (applyReceiveLoadCaps + the quality notice) and the
// logging stay in CallManager; only the DECISION lives here.
//
// 0.60.2 REWORK (2026-07-13 field RCA): the original trip wire was an
// ABSOLUTE 1500 MB working set. A peer on an RTX 2060 desktop with plenty
// of free RAM sat at 1542 MB — largely the virtual-background engine
// (measured +145 MB) plus the GPU driver/ICD that the first
// QOpenGLContext::create() maps into the process (not our allocation, not
// reclaimable) — tripped it, and EVERYONE's receive quality was shed to
// 180p. 1500 MB is not "host overload" on a modern machine; it was an
// arbitrary number. New policy:
//   shed    = runaway backstop OR (system pressure AND we contribute)
//   restore = runaway eased AND (system eased OR we no longer contribute)

namespace talq {

// Absolute RUNAWAY backstop — a genuine safety bound, not a routine
// trigger. Nothing legitimate in TalQ needs >3.5 GB; the runaway this
// watchdog was written for (a stalled decoder ballooning the working set)
// grows unboundedly, so it still trips this on its way up. Restore below
// 3 GB (512 MB hysteresis gap).
inline constexpr long long kMemShedRunawayHighMb = 3584;
inline constexpr long long kMemShedRunawayLowMb  = 3072;
// SYSTEM-pressure trip: physical memory ≥90% committed or <1 GB available
// (GlobalMemoryStatusEx) — the region where Windows starts hard-paging and
// audio/video chop regardless of who owns the RAM. Eased = ≤80% AND
// ≥1.5 GB available (the gap is the hysteresis).
inline constexpr int       kMemShedSysLoadHighPct = 90;
inline constexpr int       kMemShedSysLoadLowPct  = 80;
inline constexpr long long kMemShedSysAvailLowMb  = 1024;
inline constexpr long long kMemShedSysAvailOkMb   = 1536;
// "We are a significant contributor" floor: below ~1 GB of working set,
// shedding OUR receive quality cannot meaningfully relieve system
// pressure (some other process is eating the RAM) — degrading the call
// would punish the user for someone else's leak. The low mark (768 MB)
// is the restore side of this axis, so a working set flapping around
// the floor under sustained system pressure can't oscillate the shed.
inline constexpr long long kMemShedContributorMb    = 1024;
inline constexpr long long kMemShedContributorLowMb = 768;

// One ~1 s sample of the host, as read by DebugMonitor.
struct HostLoad {
    long long procMb     = 0;     // OUR process working set (MB)
    long long sysTotalMb = 0;     // physical memory total (MB); valid only when haveSys
    long long sysAvailMb = 0;     // physical memory available (MB); valid only when haveSys
    int       sysLoadPct = -1;    // physical memory load % (GlobalMemoryStatusEx dwMemoryLoad)
    bool      haveSys    = false; // GlobalMemoryStatusEx succeeded
};

// Latching shed/restore decision. Feed one HostLoad per tick; act on Shed /
// Restore edges (NoChange while the latch holds either way).
class MemShedPolicy {
public:
    enum Decision { NoChange, Shed, Restore };

    Decision update(const HostLoad &l)
    {
        // If GlobalMemoryStatusEx fails (or non-Windows), we can't judge system
        // pressure — fall back to the runaway backstop only. Treat "unknown" as
        // "not pressured" for the trip, and as "eased" for the restore, so a
        // probe failure can neither strand us shed nor shed us spuriously.
        const bool sysPressureHigh = l.haveSys
            && (l.sysLoadPct >= kMemShedSysLoadHighPct
                || l.sysAvailMb < kMemShedSysAvailLowMb);
        const bool sysPressureEased = !l.haveSys
            || (l.sysLoadPct <= kMemShedSysLoadLowPct
                && l.sysAvailMb >= kMemShedSysAvailOkMb);

        const bool shedTrip = (l.procMb > kMemShedRunawayHighMb)
            || (sysPressureHigh && l.procMb >= kMemShedContributorMb);
        const bool shedRestore = (l.procMb < kMemShedRunawayLowMb)
            && (sysPressureEased || l.procMb < kMemShedContributorLowMb);

        if (!m_active && shedTrip)    { m_active = true;  return Shed; }
        if (m_active  && shedRestore) { m_active = false; return Restore; }
        return NoChange;
    }

    bool active() const { return m_active; }

private:
    bool m_active = false;
};

} // namespace talq
