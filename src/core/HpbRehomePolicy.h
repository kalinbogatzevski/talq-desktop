#pragma once

#include "core/SignalingWatchdogPolicy.h"   // SigClock, sigElapsedMs, kSigSuspendGapMs

#include <string>
#include <vector>

// Which HPB (signaling server) to use, and when an idle client should look
// again and move.
//
// This exists because of a POP egress outage on 2026-09-16. One client failed
// over to a far POP overnight and stayed there for hours after its nearer POP
// came back:
//
//   00:58:04.466 Signaling: HPB select -- incumbent silent chosen="<far POP>"
//   (no further HPB select in the log)
//
// Selection ran only on a (re)connect, and a live incumbent was sticky. A
// connection that never dropped therefore never measured again. Every signaling
// message took 60 ms more than it needed to, and the next call was placed from
// a POP a continent away.
//
// Three decisions live here:
//
//  - pickHpb(): the ONE selection rule, used both when connecting and by the
//    idle background probe. It was inline in selectNearestHpbAndConnect(). A
//    live incumbent is displaced only by a challenger with >= 2 samples that
//    beats it by kHpbSwitchMarginMs and is not held down. One TCP:443 sample is
//    too noisy to trust, and jitter only ever adds latency, so the min of
//    several samples is the floor. A silent incumbent takes any answering
//    challenger, a healthy one before a held-down one, and a held-down one
//    before nothing (never serverless). The reason strings are the exact text
//    of the "HPB select" log line, which RCAs grep.
//
//  - Hold-down (observeProbe / observeConnectFailure): a host that was silent
//    in a probe somebody else answered, or whose signaling failed a connect or
//    a verification, may not displace a live incumbent until it has answered
//    every probe for kHpbHoldDownMs. One POP failed a second time ~95 min after
//    its repair, and a client that had already moved back to it was cut off
//    again. The clock runs from the first good sighting, not from the
//    last bad one: measured from the bad one, a long outage would already be
//    "old" on the first probe that sees the POP back. A probe nobody answered
//    says nothing about any one POP (our own network is down) and holds nobody.
//
//  - HpbRehomePolicy scheduling: when the background probe runs, and whether
//    its result may move us.
//      * It only runs when signaling is settled (authenticated, room join
//        acked), no call of ours is ringing, connecting, live or reconnecting,
//        a minute has passed since the last one ended, and no peer in the
//        viewed room is in a call. A re-join clears the per-session call
//        flags, so the next participants update re-emits "joined the call"
//        for every peer, and an idle CallManager rings on that.
//      * After a selection in which something was not right (a silent POP, a
//        held-down POP, a wanted switch that could not be made), it re-checks
//        after 2 min and backs off 5, 10, 20 min to the 30 min settled pace.
//        Any change in what the probe sees starts again at 2 min. A POP that is
//        silent for good from this network (a site firewall, a retired pool
//        entry) therefore costs a probe every 30 min, not every 5.
//      * A probe in which everyone answered and we are on the pick settles us
//        on the 30 min check, so a start-up pick that jitter got wrong is
//        still corrected eventually.
//      * A winning probe does not move us by itself (Verdict::Verify). TCP:443
//        is the web front; the signaling container behind it can be down while
//        it answers. The caller first proves the target's signaling answers a
//        WebSocket handshake, then reports it (onVerifyResult). Only then do we
//        give up the working connection.
//      * A switch starts a 10 min cooldown.
//      * A background probe that finds the CURRENT server silent never moves
//        us: its WebSocket is answering pings, and that is the stronger
//        evidence. A server that really died is handled by the pong watchdog
//        and the reconnect path.
//      * The first tick after a clock gap (suspend) never probes, because the
//        network may not be up yet. A result that straddles a gap is stale.
//      * probeDue() needs no gates, so the caller only pays for them (QSettings,
//        the candidate list) on the tick a probe is actually due. A due probe
//        the gates refuse is asked again after a minute, not on every tick.
//
// Qt-free; SignalingClient runs the probe, the verification and the timers
// (tests/hpb_rehome_policy_test.cpp).

namespace talq {

// A challenger must be at least this much faster than the incumbent.
inline constexpr int kHpbSwitchMarginMs = 30;
// ...and have at least this many landed samples to displace a LIVE incumbent.
inline constexpr int kHpbDisplaceMinSamples = 2;
// Bound on the WebSocket handshake that verifies a switch target. A healthy
// open() is about five round trips: ~1 s to a 200 ms POP, plus one CONNECT on
// a proxied desk. A target slower than this is not one worth moving to.
inline constexpr SigMs kHpbVerifyTimeoutMs = 5000;

// One candidate's probe outcome.
struct HpbProbeSample {
    int  minRttMs = -1;        // lowest TCP:443 connect time; -1 = nothing landed
    int  samples = 0;          // landed samples
    bool isIncumbent = false;  // the server we are on / last connected to
    bool heldDown = false;     // failed us recently (HpbRehomePolicy::observeProbe)
};

enum class HpbPickReason {
    NoneAnswered,       // nobody answered: keep the URL we have
    Sticky,             // incumbent answered, nobody beat it by the margin
    SwitchedByMargin,   // a >= 2-sample challenger beat the incumbent by the margin
    IncumbentSilent,    // incumbent silent: best answering challenger
    HeldDown,           // incumbent answered; only a held-down challenger beat it
};

struct HpbPick {
    int index = -1;                   // chosen candidate, -1 = none
    HpbPickReason reason = HpbPickReason::NoneAnswered;
    int incumbentRttMs = -1;
    int bestChallengerRttMs = -1;     // best eligible challenger with >= 2 samples
    int anyChallengerRttMs = -1;      // best answering challenger (the one taken if the incumbent is silent)
    int heldDownRttMs = -1;           // best held-down challenger with >= 2 samples
    bool allAnswered = false;         // every candidate landed at least one sample
};

// Exact text of the "Signaling: HPB select — <why>" log line.
inline const char *hpbPickReasonText(HpbPickReason r)
{
    switch (r) {
    case HpbPickReason::NoneAnswered:     return "no candidate answered";
    case HpbPickReason::Sticky:           return "sticky (within margin)";
    case HpbPickReason::SwitchedByMargin: return "switched (beat incumbent by margin, >=2 samples)";
    case HpbPickReason::IncumbentSilent:  return "incumbent silent";
    case HpbPickReason::HeldDown:         return "sticky (nearer server in hold-down)";
    }
    return "?";
}

// Candidate order breaks ties (first wins), exactly as the inline code did.
inline HpbPick pickHpb(const HpbProbeSample *c, int n, int marginMs = kHpbSwitchMarginMs)
{
    HpbPick out;
    int incumbentIdx = -1, bestIdx = -1, anyIdx = -1, heldAnyIdx = -1, heldAnyRtt = -1;
    bool all = (n > 0);
    for (int i = 0; i < n; ++i) {
        const HpbProbeSample &s = c[i];
        if (s.minRttMs < 0) all = false;
        if (s.isIncumbent) {
            if (incumbentIdx < 0) {
                incumbentIdx = i;
                if (s.minRttMs >= 0) out.incumbentRttMs = s.minRttMs;
            }
        } else if (s.minRttMs >= 0 && s.heldDown) {
            if (heldAnyRtt < 0 || s.minRttMs < heldAnyRtt) { heldAnyRtt = s.minRttMs; heldAnyIdx = i; }
            if (s.samples >= kHpbDisplaceMinSamples
                && (out.heldDownRttMs < 0 || s.minRttMs < out.heldDownRttMs))
                out.heldDownRttMs = s.minRttMs;
        } else if (s.minRttMs >= 0) {
            if (out.anyChallengerRttMs < 0 || s.minRttMs < out.anyChallengerRttMs) {
                out.anyChallengerRttMs = s.minRttMs; anyIdx = i;
            }
            if (s.samples >= kHpbDisplaceMinSamples
                && (out.bestChallengerRttMs < 0 || s.minRttMs < out.bestChallengerRttMs)) {
                out.bestChallengerRttMs = s.minRttMs; bestIdx = i;
            }
        }
    }
    out.allAnswered = all;

    if (out.incumbentRttMs >= 0 && out.bestChallengerRttMs >= 0
        && out.bestChallengerRttMs <= out.incumbentRttMs - marginMs) {
        out.index = bestIdx; out.reason = HpbPickReason::SwitchedByMargin;
    } else if (out.incumbentRttMs >= 0) {
        out.index = incumbentIdx;
        out.reason = (out.heldDownRttMs >= 0 && out.heldDownRttMs <= out.incumbentRttMs - marginMs)
                         ? HpbPickReason::HeldDown : HpbPickReason::Sticky;
    } else if (out.anyChallengerRttMs >= 0) {
        out.index = anyIdx; out.reason = HpbPickReason::IncumbentSilent;
    } else if (heldAnyRtt >= 0) {
        // Only held-down POPs answer. Something beats nothing.
        out.index = heldAnyIdx; out.reason = HpbPickReason::IncumbentSilent;
        out.anyChallengerRttMs = heldAnyRtt;
    }
    return out;
}

struct HpbRehomeTimings {
    // Re-check intervals while not settled: first, then backing off. Past the
    // last step the settled interval applies.
    SigMs recoveringStepsMs[4] = {2 * 60 * 1000, 5 * 60 * 1000, 10 * 60 * 1000, 20 * 60 * 1000};
    SigMs settledEveryMs    = 30 * 60 * 1000;   // everyone answered, we are on the pick
    SigMs switchCooldownMs  = 10 * 60 * 1000;   // no second switch inside this
    SigMs postCallSettleMs  = 60 * 1000;        // leave REST calls / room restore finish first
    SigMs probeLostMs       = 60 * 1000;        // a probe or verification that never reported is dropped
    SigMs holdDownMs        = 15 * 60 * 1000;   // a POP that failed us must answer this long first
    SigMs gateRecheckMs     = 60 * 1000;        // a due probe the gates refused asks again after this
    SigMs suspendGapMs      = kSigSuspendGapMs;
};

// Facts the caller reads from SignalingClient at the moment of asking.
struct HpbRehomeGates {
    bool connected = false;       // authenticated, socket up, room join (if any) acked, no resume pending
    bool roomCallActive = false;  // a peer in the current room is in a call
    bool canChoose = false;       // >= 2 candidate HPBs and no manual pin / re-home not disabled
};

class HpbRehomePolicy {
public:
    enum class Mode { Settled, Recovering };
    enum class Verdict {
        Stay,
        Verify,   // onProbeResult: a switch is wanted; verify the target's signaling first
        Switch,   // onVerifyResult: verified and still allowed; move now
    };

    explicit HpbRehomePolicy(HpbRehomeTimings t = {}) : m_t(t) {}

    // A probe-based selection just chose the server we are connecting to.
    void onSelection(const SigClock &now, const HpbPick &pick)
    {
        const bool onBest = pick.allAnswered
            && (pick.reason == HpbPickReason::Sticky || pick.reason == HpbPickReason::SwitchedByMargin);
        reschedule(now, onBest ? Mode::Settled : Mode::Recovering, /*restart*/ true);
        m_probeInFlight = false;
        m_verifyPending = false;
        m_haveLastReason = false;   // a new connection: nothing to compare with
        m_haveGateRefusal = false;
    }

    // Our call became busy (ringing, connecting, live, reconnecting) or idle.
    void onCallBusyChanged(const SigClock &now, bool busy)
    {
        if (m_callBusy && !busy) { m_busyEndedAt = now; m_haveBusyEnd = true; }
        m_callBusy = busy;
    }

    // Every keepalive tick while connected. Needs no gates. True = a probe is
    // due; ask startProbe() with the gates.
    bool probeDue(const SigClock &now)
    {
        const bool gap = m_haveTick && sigElapsedMs(m_lastTick, now) > m_t.suspendGapMs;
        m_lastTick = now;
        m_haveTick = true;
        if (gap) return false;

        if (m_probeInFlight || m_verifyPending) {
            if (sigElapsedMs(m_probeStartedAt, now) <= m_t.probeLostMs) return false;
            m_probeInFlight = false;             // it never reported; do not wedge
            m_verifyPending = false;
        }
        if (m_callBusy || inPostCallSettle(now) || inCooldown(now)) return false;
        if (!m_haveAnchor) {
            // Connected without a selection (e.g. resumed): start the slow clock.
            reschedule(now, Mode::Settled, /*restart*/ true);
            return false;
        }
        if (m_haveGateRefusal && sigElapsedMs(m_gateRefusedAt, now) < m_t.gateRecheckMs)
            return false;
        return sigElapsedMs(m_anchor, now) >= m_intervalMs;
    }

    // A probe is due: start it if the gates allow (recorded as in flight). A
    // refusal leaves the schedule as it is (the probe stays due, and a peer's
    // call ending must not push it out by minutes) but asks again only after
    // gateRecheckMs, so a gate that stays closed (a single HPB, a manual pin, a
    // long call in the viewed room) is not rebuilt on every keepalive tick.
    bool startProbe(const SigClock &now, const HpbRehomeGates &g)
    {
        if (!g.connected || !g.canChoose || g.roomCallActive) {
            m_gateRefusedAt = now;
            m_haveGateRefusal = true;
            return false;
        }
        m_haveGateRefusal = false;
        m_probeInFlight = true;
        m_probeStartedAt = now;
        return true;
    }

    bool shouldProbe(const SigClock &now, const HpbRehomeGates &g)
    {
        return probeDue(now) && startProbe(now, g);
    }

    // The background probe finished. Stay, or Verify: check the target's
    // signaling, then call onVerifyResult().
    Verdict onProbeResult(const SigClock &now, const HpbPick &pick, const HpbRehomeGates &g)
    {
        const bool wasInFlight = m_probeInFlight;
        m_probeInFlight = false;
        // Nothing outstanding: a reconnect's selection (or a reset) already
        // superseded this probe and rescheduled. Its result must not move us.
        if (!wasInFlight)
            return Verdict::Stay;
        if (sigElapsedMs(m_probeStartedAt, now) > m_t.suspendGapMs) {
            // Measured before a sleep: the network it measured may be gone.
            reschedule(now, m_mode, /*restart*/ false);
            return Verdict::Stay;
        }

        const bool news = m_haveLastReason && pick.reason != m_lastReason;
        m_lastReason = pick.reason;
        m_haveLastReason = true;

        const bool wantSwitch = pick.reason == HpbPickReason::SwitchedByMargin && pick.index >= 0;
        if (wantSwitch && mayMove(now, g)) {
            m_verifyPending = true;              // probeLostMs still runs from m_probeStartedAt
            m_verifyAllAnswered = pick.allAnswered;
            return Verdict::Verify;
        }
        // Stay. Settled only if everyone answered and nobody should replace us;
        // a switch we wanted but could not make keeps the short cadence.
        const bool onBest = pick.allAnswered && pick.reason == HpbPickReason::Sticky;
        reschedule(now, onBest ? Mode::Settled : Mode::Recovering, news);
        return Verdict::Stay;
    }

    // The target's signaling answered (ok) or not. The caller records a failure
    // with observeConnectFailure(), which holds the target down.
    Verdict onVerifyResult(const SigClock &now, bool ok, const HpbRehomeGates &g)
    {
        const bool wasPending = m_verifyPending;
        m_verifyPending = false;
        if (!wasPending)
            return Verdict::Stay;
        if (sigElapsedMs(m_probeStartedAt, now) > m_t.suspendGapMs) {
            reschedule(now, m_mode, /*restart*/ false);
            return Verdict::Stay;
        }
        if (ok && mayMove(now, g)) {
            m_lastSwitchAt = now;
            m_haveSwitch = true;
            m_haveLastReason = false;            // a new incumbent
            reschedule(now, m_verifyAllAnswered ? Mode::Settled : Mode::Recovering, /*restart*/ true);
            return Verdict::Switch;
        }
        reschedule(now, Mode::Recovering, /*restart*/ false);
        return Verdict::Stay;
    }

    // Record one probe (either call site) and mark each sample whose host is
    // held down. hosts[i] names samples[i]. Call before pickHpb().
    void observeProbe(const SigClock &now, const std::string *hosts, HpbProbeSample *samples, int n)
    {
        bool anyAnswered = false;
        for (int i = 0; i < n; ++i) anyAnswered = anyAnswered || samples[i].minRttMs >= 0;
        for (int i = 0; i < n; ++i) {
            HostHealth *h = find(hosts[i]);
            if (samples[i].minRttMs >= 0) {
                if (h && !h->haveGoodSince) { h->goodSince = now; h->haveGoodSince = true; }
            } else if (anyAnswered) {
                markBad(now, hosts[i]);
            }
        }
        forgetRecovered(now);
        for (int i = 0; i < n; ++i) samples[i].heldDown = heldDown(now, hosts[i]);
    }

    // A connect to `host`, or the verification of it, failed before its
    // signaling authenticated us.
    void observeConnectFailure(const SigClock &now, const std::string &host) { markBad(now, host); }

    bool heldDown(const SigClock &now, const std::string &host) const
    {
        for (const HostHealth &h : m_hosts) {
            if (h.host != host) continue;
            return !h.haveGoodSince || sigElapsedMs(h.goodSince, now) < m_t.holdDownMs;
        }
        return false;
    }

    void reset() { *this = HpbRehomePolicy(m_t); }

    Mode mode() const { return m_mode; }
    bool probeInFlight() const { return m_probeInFlight; }
    bool verifyPending() const { return m_verifyPending; }
    bool callBusy() const { return m_callBusy; }

private:
    struct HostHealth {
        std::string host;
        SigClock goodSince;          // first answer since it last failed us
        bool haveGoodSince = false;
    };

    HostHealth *find(const std::string &host)
    {
        for (HostHealth &h : m_hosts)
            if (h.host == host) return &h;
        return nullptr;
    }
    void markBad(const SigClock &, const std::string &host)
    {
        HostHealth *h = find(host);
        if (!h) { m_hosts.push_back(HostHealth{host, SigClock{}, false}); return; }
        h->haveGoodSince = false;
    }
    void forgetRecovered(const SigClock &now)
    {
        for (size_t i = 0; i < m_hosts.size();) {
            const HostHealth &h = m_hosts[i];
            if (h.haveGoodSince && sigElapsedMs(h.goodSince, now) >= m_t.holdDownMs)
                m_hosts.erase(m_hosts.begin() + static_cast<std::ptrdiff_t>(i));
            else
                ++i;
        }
    }

    bool mayMove(const SigClock &now, const HpbRehomeGates &g) const
    {
        return g.connected && g.canChoose && !g.roomCallActive
               && !m_callBusy && !inPostCallSettle(now) && !inCooldown(now);
    }

    // `restart` = something changed (a new connection, a switch, a different
    // probe outcome): re-check at the first step. Otherwise staying in
    // recovery takes the next, longer step.
    void reschedule(const SigClock &now, Mode next, bool restart)
    {
        if (next == Mode::Settled) {
            m_step = 0;
            m_intervalMs = m_t.settledEveryMs;
        } else {
            if (restart || !m_haveAnchor || m_mode == Mode::Settled) m_step = 0;
            else ++m_step;
            const int steps = static_cast<int>(sizeof(m_t.recoveringStepsMs) / sizeof(m_t.recoveringStepsMs[0]));
            m_intervalMs = m_step < steps ? m_t.recoveringStepsMs[m_step] : m_t.settledEveryMs;
            if (m_step > steps) m_step = steps;
        }
        m_mode = next;
        m_anchor = now;
        m_haveAnchor = true;
    }

    bool inPostCallSettle(const SigClock &now) const
    {
        return m_haveBusyEnd && sigElapsedMs(m_busyEndedAt, now) < m_t.postCallSettleMs;
    }
    bool inCooldown(const SigClock &now) const
    {
        return m_haveSwitch && sigElapsedMs(m_lastSwitchAt, now) < m_t.switchCooldownMs;
    }

    HpbRehomeTimings m_t;
    Mode     m_mode = Mode::Settled;
    int      m_step = 0;
    SigClock m_anchor;
    bool     m_haveAnchor = false;
    SigMs    m_intervalMs = 0;
    SigClock m_lastTick;
    bool     m_haveTick = false;
    bool     m_probeInFlight = false;
    bool     m_verifyPending = false;
    bool     m_verifyAllAnswered = false;
    SigClock m_probeStartedAt;
    HpbPickReason m_lastReason = HpbPickReason::NoneAnswered;
    bool     m_haveLastReason = false;
    bool     m_callBusy = false;
    SigClock m_busyEndedAt;
    bool     m_haveBusyEnd = false;
    SigClock m_lastSwitchAt;
    bool     m_haveSwitch = false;
    SigClock m_gateRefusedAt;
    bool     m_haveGateRefusal = false;
    std::vector<HostHealth> m_hosts;   // hosts that failed us, until they recover
};

} // namespace talq
