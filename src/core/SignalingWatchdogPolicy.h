#pragma once

#include <algorithm>
#include <cstdint>

// How long TalQ waits on its signaling (HPB) WebSocket before giving up on it:
// the connect bound, and the pong watchdog that notices a connection that is up
// in name only.
//
// This exists because of a POP egress outage on 2026-09-16: the POP kept
// receiving packets but nothing it sent got out. One client's log:
//
//   11:30:12.218 Signaling: keepalive pong, RTT 17 ms
//   (ping due ~11:30:37 was never answered -- pongs were only logged)
//   11:30:58.826 Signaling: WebSocket error: RemoteHostClosedError
//   11:30:59.078 Signaling: connecting to "wss://<same POP>/spreed"
//   11:31:41.180 Signaling: WebSocket error: NetworkError "Connection timed out"
//
// About 21 s passed before Windows declared the socket dead, and the fast
// resume then spent 42.1 s connecting to the same dead POP. TalQ had no bound
// of its own, so the connect took longer than the server's 30 s resume grace
// and failover took ~46 s in all.
//
// Three decisions live here:
//  - signalingConnectTimeoutMs(): every QWebSocket::open() is bounded. On
//    timeout the caller aborts the socket, which runs the normal disconnected
//    path: a failed fast resume falls back to a settings fetch plus an HPB
//    probe, which is the actual failover.
//  - PongWatchdogPolicy: a ping with no answer inside the bound means the path
//    is dead, and the caller aborts. A system suspend is reported as a separate
//    verdict (TalQ has seen 29-minute suspends). It still ends in a reconnect,
//    but is never counted as a pong miss.
//  - resumeAfterFailedAttempt(): that fallback keeps the resume id for one
//    attempt, so a bounded connect does not throw away a resumable session.
//
// Both clocks are read at every event because it is not known which one Windows
// keeps running through sleep. Qt's QElapsedTimer is QueryPerformanceCounter,
// the wall clock is UTC ms, and the larger delta wins. A wall clock stepped
// backwards (NTP) yields a negative delta and loses the max.
//
// Qt-free on purpose, like the other policy headers: SignalingClient reads the
// clocks, runs the timers and applies the verdict; the decision is testable
// without a socket (tests/signaling_watchdog_policy_test.cpp).

namespace talq {

using SigMs = std::int64_t;

// Two clocks read at the same instant.
struct SigClock {
    SigMs monoMs = 0;   // monotonic (QElapsedTimer)
    SigMs wallMs = 0;   // UTC wall clock (QDateTime::currentMSecsSinceEpoch)
};

// Time between two readings, taken as the LARGER of the two clocks' deltas, so
// a sleep shows up on whichever clock kept counting through it.
inline SigMs sigElapsedMs(const SigClock &from, const SigClock &to)
{
    return std::max(to.monoMs - from.monoMs, to.wallMs - from.wallMs);
}

inline SigClock sigShift(const SigClock &c, SigMs ms)
{
    return SigClock{c.monoMs + ms, c.wallMs + ms};
}

// --- Connect bound -----------------------------------------------------------
// 10 s for the first attempt. A healthy open() is DNS + TCP + TLS + the HTTP
// upgrade, about five round trips: 318 ms to a ~70 ms-RTT POP in the field,
// ~1 s to a 200 ms POP, plus one CONNECT on a proxied desk. It is about half
// of the ~21 s the OS itself waits on an unanswered SYN (Initial RTO 1 s, up to
// 4 SYN retransmissions on a test machine), so a connect that needs a resend
// or two still completes, and 250 ms fast-resume backoff + 10 s keeps a resume
// attempt well inside the server's 30 s sessionExpireDuration. Qt's own
// per-address DefaultConnectTimeout is 30 s, which is no help here.
//
// After a timeout the bound steps to 30 s, then 45 s (the cap). Qt tries a
// host's resolved addresses one after another and moves on only when the OS
// gives up on one (~21 s for an unanswered SYN). A dual-stack host whose IPv6
// path is black-holed therefore answers on IPv4 only after ~22 s: a 20 s second
// step aborted exactly that attempt, and the resume id was dropped with it.
// 30 s outlasts one dead address plus a handshake, 45 s two.
//
// A success resets the count, so every reconnect starts at the short bound
// again. For a host whose last successful connect took longer than the first
// bound (so it can only have succeeded on a later, longer step), that would
// repeat the same doomed first attempt on every reconnect, so the first bound
// starts at the step that covers that connect time (lastConnectMs, measured per
// host by the caller) plus kSigConnectSlackMs. A connect that was merely slow
// but still inside the first bound (Wi-Fi coming up after a wake) changes
// nothing: the short bound is what leaves a dead POP inside the resume grace.
// A fast connect later brings it back down.
inline constexpr SigMs kSigConnectTimeoutMs    = 10000;
inline constexpr SigMs kSigConnectTimeoutMaxMs = 45000;
inline constexpr SigMs kSigConnectSlackMs      = 5000;
inline constexpr SigMs kSigConnectTimeoutStepsMs[] = {kSigConnectTimeoutMs, 30000, kSigConnectTimeoutMaxMs};

// lastConnectMs: how long the last successful connect to this host took, or -1
// if unknown.
inline SigMs signalingConnectTimeoutMs(int consecutiveTimeouts, SigMs lastConnectMs = -1)
{
    constexpr int kSteps = int(sizeof(kSigConnectTimeoutStepsMs) / sizeof(kSigConnectTimeoutStepsMs[0]));
    int step = std::clamp(consecutiveTimeouts, 0, kSteps - 1);
    if (lastConnectMs > kSigConnectTimeoutMs) {
        while (step < kSteps - 1 && kSigConnectTimeoutStepsMs[step] < lastConnectMs + kSigConnectSlackMs)
            ++step;
    }
    return kSigConnectTimeoutStepsMs[step];
}

// --- Pong watchdog -----------------------------------------------------------
// The keepalive pings every 25 s (see SignalingClient::m_keepAliveTimer).
//
// Pong latency is NOT bounded by the network RTT. nextcloud-spreed-signaling
// answers a ping only from the client's read pump (no ping handler of its own,
// so gorilla's default replies when NextReader reaches the frame). That pump
// stops reading while its 16-slot message queue is full (client/client.go:149,
// :382), and the queue drains through handlers that run one at a time; an
// `offer` is handled synchronously under the [mcu] timeout (server/hub.go
// :2213-2222, :2915), which is 25 s on all three POPs. During a call-setup
// burst (one client sent 18 messages in one second) a healthy server can therefore
// leave a ping unanswered for ~25 s.
//
// 30 s covers one such stalled handler with a few seconds for the queue to
// drain and a far POP's RTT. Residual false positive, accepted: two blocked
// handlers back to back (a video and a screen publisher both stalling) can
// exceed it; the abort then costs a resume. On a direct path Windows already
// resets a connection ~20 s after an unacknowledged ping, so the bound matters
// most behind an HTTP CONNECT proxy, which acknowledges locally and where
// nothing else would ever notice a dead POP. The server's own read deadline
// is 60 s.
//
// The bound exceeds the ping interval. That is safe: onKeepAliveTick() never
// pings over an outstanding ping; it judges (or rechecks) the one in flight.
inline constexpr SigMs kSigKeepAliveIntervalMs = 25000;
inline constexpr SigMs kSigPongTimeoutMs       = 30000;
// If a deadline check runs later than this past its due time, the event loop
// was stalled (a main-thread pipeline build, say), and the pong may be sitting
// unread behind the timer event. Recheck after this long instead of judging.
inline constexpr SigMs kSigStallSlackMs        = 2000;
// A gap between two watchdog events longer than this is a suspend, or a stall
// long enough that the server's 60 s read deadline has run out anyway. Either
// way the only sensible move is a reconnect.
inline constexpr SigMs kSigSuspendGapMs        = 60000;

static_assert(kSigPongTimeoutMs > 25000,
              "the pong bound must outlast the server's 25 s synchronous [mcu] handler");
static_assert(kSigSuspendGapMs > kSigKeepAliveIntervalMs + kSigPongTimeoutMs,
              "a normal keepalive cycle must never read as a suspend gap");

class PongWatchdogPolicy {
public:
    enum class Verdict {
        None,        // nothing to do
        SendPing,    // keepalive tick: send a ping now (recorded as sent)
        Recheck,     // too early, or ran late after a stall: check again in recheckInMs()
        PongMissed,  // no answer inside the bound: the path is dead, abort
        SuspendGap,  // a clock gap (suspend/long stall): reconnect, not a miss
    };

    explicit PongWatchdogPolicy(SigMs pongTimeoutMs = kSigPongTimeoutMs,
                                SigMs suspendGapMs = kSigSuspendGapMs,
                                SigMs stallSlackMs = kSigStallSlackMs)
        : m_timeout(pongTimeoutMs), m_suspendGap(suspendGapMs), m_stallSlack(stallSlackMs) {}

    // The socket (re)connected: nothing is outstanding, and the gap baseline
    // starts now.
    void reset(const SigClock &now)
    {
        m_outstanding = false;
        m_lastEval = now;
        m_haveBaseline = true;
        m_lastGap = 0;
    }

    // Keepalive timer tick. Returns SendPing (the ping is recorded as sent at
    // `now`, caller pings and arms the deadline), SuspendGap, or -- when an
    // earlier ping is still unanswered -- the deadline verdict for it, so a
    // lost deadline timer can never leave a dead socket unnoticed.
    Verdict onKeepAliveTick(const SigClock &now)
    {
        if (gapped(now)) return Verdict::SuspendGap;
        if (m_outstanding) return judgeDeadline(now);
        m_outstanding = true;
        m_due = sigShift(now, m_timeout);
        return Verdict::SendPing;
    }

    // A pong, or any other inbound frame: the path to the server works.
    void onInbound(const SigClock &now)
    {
        m_outstanding = false;
        m_lastEval = now;
        m_haveBaseline = true;
    }

    // The pong deadline timer fired.
    Verdict onDeadline(const SigClock &now)
    {
        if (!m_outstanding) return Verdict::None;
        if (gapped(now)) return Verdict::SuspendGap;
        return judgeDeadline(now);
    }

    SigMs recheckInMs() const { return m_recheckIn; }
    SigMs lastGapMs() const { return m_lastGap; }
    bool pingOutstanding() const { return m_outstanding; }

private:
    // Records `now` as the latest evaluation. Returns true (and consumes any
    // outstanding ping) if the time since the previous one is a suspend gap.
    bool gapped(const SigClock &now)
    {
        const SigMs gap = m_haveBaseline ? sigElapsedMs(m_lastEval, now) : 0;
        m_lastEval = now;
        m_haveBaseline = true;
        if (gap > m_suspendGap) {
            m_lastGap = gap;
            m_outstanding = false;
            return true;
        }
        return false;
    }

    Verdict judgeDeadline(const SigClock &now)
    {
        const SigMs late = sigElapsedMs(m_due, now);
        if (late < 0) {                        // coarse timer fired early
            m_recheckIn = -late;
            return Verdict::Recheck;
        }
        if (late > m_stallSlack) {             // loop was stalled: let queued reads run first
            m_due = sigShift(now, m_stallSlack);
            m_recheckIn = m_stallSlack;
            return Verdict::Recheck;
        }
        m_outstanding = false;
        return Verdict::PongMissed;
    }

    SigMs    m_timeout;
    SigMs    m_suspendGap;
    SigMs    m_stallSlack;
    bool     m_outstanding = false;
    bool     m_haveBaseline = false;
    SigClock m_due;
    SigClock m_lastEval;
    SigMs    m_recheckIn = 0;
    SigMs    m_lastGap = 0;
};

// --- Resume id after a failed attempt ----------------------------------------
// A connect attempt ended before the server authenticated us (socket failure,
// the connect bound, or the settings fetch failing). What happens to a held
// resume id?
//
// A failed fast resume used to drop it at once. With the 10 s connect bound
// that turned every loss longer than 10 s into a fresh hello -- SESSION RESET
// and a full call rebuild -- where the OS would have kept retrying the SYN for
// 21-42 s. None of those failures says the id is invalid: a hello carrying it
// is resumed by the issuing server, and any other cluster member looks it up
// and proxies the session (server/hub.go:1266-1370). So the id survives for
// ONE attempt through the settings fetch and HPB probe (fresh URL and ticket).
// A rejection there falls back to a fresh hello on the same socket. If that
// attempt fails as well, the id is dropped, so an unreachable network cannot
// keep a stale id cycling. Cost when the issuing POP is dead: the lookup waits
// its 5 s gRPC bound before the rejection.
enum class ResumeAfterFailure {
    Unchanged,          // no resume attempt was involved
    RetryViaSettings,   // keep the id; next attempt = settings fetch + probe
    Drop,               // the settings attempt failed too: fresh session next
};

inline ResumeAfterFailure resumeAfterFailedAttempt(bool haveResumeId, bool fastResumeAttempt,
                                                   bool viaSettingsAttempt)
{
    if (!haveResumeId) return ResumeAfterFailure::Unchanged;
    if (fastResumeAttempt) return ResumeAfterFailure::RetryViaSettings;
    if (viaSettingsAttempt) return ResumeAfterFailure::Drop;
    return ResumeAfterFailure::Unchanged;
}

} // namespace talq
