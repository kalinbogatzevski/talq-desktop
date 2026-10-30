#pragma once

#include <algorithm>
#include <cstdint>

// FarRefStallPolicy -- pure decision for "the AEC far-end REFERENCE stopped
// delivering": when to re-open the loopback source, how often, and when to stop
// trying. No Qt, no GStreamer, no clock: the caller feeds it millisecond readings
// from ONE monotonic clock, so the whole thing is unit-testable without a call --
// see tests/far_ref_stall_policy_test.cpp.
//
// Why it exists (field call 2026-10-29, Kalin <-> Petia): a USB headset was plugged
// in mid-call and Windows made it the default output. The reference is a WASAPI
// loopback of the DEFAULT render endpoint (pub-aec-ref, a wasapi2src with
// loopback=TRUE). At that instant its level meter froze at one value and never
// moved again for the remaining 254 s of the call, while the far end kept
// arriving at normal speech levels: webrtcdsp had no reference, so nothing
// cancelled the far end leaking into the microphone, and the other party heard
// their own voice back about 2 s late. Nothing said so. GStreamer 1.28's wasapi2
// has no default-device handling of its own and no timeout (the ring buffer waits
// on its event handles with INFINITE and an empty read is a quiet no-op), the
// log shows no error from the source, and TalQ's own telemetry kept reading
// healthy: the erle-proxy line reprints a CACHED number and `[MEDIA] aec=` only
// reports that the probe element exists. Upstream gstreamer#5347 describes the same
// silence for a default device; it is open and not fixed through 1.28.8, so a newer
// GStreamer is not the answer. WHETHER Windows' automatic stream routing re-routes a
// loopback client is not established by anything we read (Microsoft's pages are
// silent), and what exactly stopped on the incident machine is not known: this is a
// detector of the symptom and a re-open, not a fix of a proven mechanism.
//
// THE SIGNAL IS "A LEVEL MESSAGE ARRIVED", NEVER THE dB VALUE. A loopback source
// delivers buffers continuously even when nothing is playing (wasapi2 feeds a
// silent render client for exactly that), so the -96 dB floor of an idle call
// still produces a message every 200 ms. A frozen VALUE would also be wrong the
// other way: a perfectly quiet far end repeats itself legitimately.
//
// Staleness is measured against the time the bus was last DRAINED, not against
// wall time. The main thread that drains it can itself stall across a device
// change (the incident log has a 4.3 s hole in the 2/s erle line at the freeze);
// messages posted meanwhile are then popped in one burst, and wall-clock
// staleness would call a healthy reference dead the instant that burst is queued.
//
// The threshold sits well above the pauses that heal on their own: in the July
// field logs four stalls of 2.9 to 5.0 s all recovered by themselves (three of the
// four with a pub-aec-ref DISCONT at the resume; they were gaps in the 2/s erle
// line, so that only the reference paused is an inference), and re-opening a source
// that was about to recover costs an AEC re-convergence for nothing. Only the
// incident never healed.
//
// What this does NOT cover: a reference that keeps producing messages but of the
// wrong audio (e.g. a loopback bound to an endpoint nothing plays to), and any
// failure whose cause is not the reference. It is detection and recovery of one
// proven failure, not a fix of the underlying wasapi2 behaviour.

namespace talq {

enum class FarRefAction { None, Reopen, GiveUp };

struct FarRefStallConfig {
    int warmupMs     = 10000;  // armed but NO message yet: device activation can be slow
    int staleMs      = 8000;   // quiet this long after having delivered => stalled
    int settleMs     = 6000;   // after a re-open, wait this long for a message before calling it failed
    int backoffMs    = 10000;  // before the 2nd re-open; doubled for each later one
    int maxBackoffMs = 40000;
    int maxAttempts  = 5;      // re-opens NOT followed by a sustained run (healthyMs) before giving up
    int healthyMs    = 10000;  // continuous messages for this long restore the full attempt budget
    int streakGapMs  = 2000;   // a gap longer than this breaks a "continuous" run (cadence is 200 ms)
    int deadAfterMs  = 2000;   // for DISPLAY: no message for this long = report "no reference", not the cache
};

class FarRefStallPolicy {
public:
    FarRefStallPolicy() = default;
    explicit FarRefStallPolicy(FarRefStallConfig cfg) : m_c(cfg) {}

    // Call when the reference tail has been built and the pipeline is PLAYING.
    // Forgets everything: a rebuilt publisher starts with a clean budget.
    void arm(int64_t nowMs)
    {
        m_armed = true;
        m_armedMs = nowMs;
        m_lastMsgMs = -1;
        m_lastReopenMs = -1;
        m_streakStartMs = -1;
        m_attempts = 0;
        m_reopenPending = false;
        m_episodeActedOn = false;
        m_gaveUp = false;
    }
    void disarm() { m_armed = false; }

    // A pub-far-ref-level message was popped off the bus at `nowMs`.
    // Returns true exactly once per stall that was ACTED ON (a re-open was issued, or
    // the policy gave up) when the reference is delivering again: the caller logs the
    // recovery. That holds however late the first message comes -- a re-activated
    // loopback can take longer than settleMs, and the log must not read as a failed
    // recovery when it was not. A stall that healed before any re-open returns false.
    //
    // After a give-up a stray message proves nothing (a flapping device would log a
    // give-up/recovery pair every few seconds), so the give-up ends, and is
    // acknowledged, only once the reference has delivered continuously for healthyMs.
    bool onMessage(int64_t nowMs)
    {
        if (!m_armed) return false;
        if (m_lastMsgMs < 0 || nowMs - m_lastMsgMs > m_c.streakGapMs) m_streakStartMs = nowMs;
        m_lastMsgMs = nowMs;
        m_reopenPending = false;                           // something arrived: stop waiting on the re-open
        const bool sustained = nowMs - m_streakStartMs >= m_c.healthyMs;
        if (sustained) m_attempts = 0;                     // sustained -> full budget again
        if (m_gaveUp) {
            if (!sustained) return false;
            m_gaveUp = false;
        }
        const bool actedOn = m_episodeActedOn;
        m_episodeActedOn = false;
        return actedOn;
    }

    // Once per watchdog tick (1 s). `drainedMs` = the reading of the caller's
    // monotonic clock when the bus was last emptied -- NOT "now".
    FarRefAction onTick(int64_t drainedMs)
    {
        if (!m_armed || m_gaveUp) return FarRefAction::None;
        const int64_t base = std::max({ m_lastMsgMs, m_lastReopenMs, m_armedMs });
        const int64_t quiet = drainedMs > base ? drainedMs - base : 0;   // a clock stepping back never fires
        if (m_reopenPending) {
            if (quiet < m_c.settleMs) return FarRefAction::None;         // give the re-open time to take
            m_reopenPending = false;                                     // it did not
        } else {
            const int threshold = (m_lastMsgMs >= 0) ? m_c.staleMs : m_c.warmupMs;
            if (quiet < threshold) return FarRefAction::None;
        }
        // Stalled: either fresh, or a previous re-open produced nothing.
        if (m_attempts >= m_c.maxAttempts) {
            m_gaveUp = true;
            m_episodeActedOn = true;
            return FarRefAction::GiveUp;
        }
        if (m_lastReopenMs >= 0 && drainedMs - m_lastReopenMs < backoffFor(m_attempts))
            return FarRefAction::None;                                   // not yet: spacing between re-opens
        ++m_attempts;
        m_lastReopenMs = drainedMs;
        m_reopenPending = true;
        m_episodeActedOn = true;
        return FarRefAction::Reopen;
    }

    // Age of the newest message in ms, or -1 when armed with none yet / not armed.
    int64_t messageAgeMs(int64_t drainedMs) const
    {
        if (!m_armed || m_lastMsgMs < 0) return -1;
        return drainedMs > m_lastMsgMs ? drainedMs - m_lastMsgMs : 0;
    }
    // True when the cached level is worth showing: a message arrived recently.
    bool referenceAlive(int64_t drainedMs) const
    {
        const int64_t age = messageAgeMs(drainedMs);
        return age >= 0 && age <= m_c.deadAfterMs;
    }

    bool armed() const { return m_armed; }
    int attempts() const { return m_attempts; }
    bool gaveUp() const { return m_gaveUp; }
    bool reopenPending() const { return m_reopenPending; }
    const FarRefStallConfig &config() const { return m_c; }

private:
    int backoffFor(int attempts) const   // spacing after the `attempts`-th re-open
    {
        int64_t b = m_c.backoffMs;
        for (int i = 1; i < attempts && b < m_c.maxBackoffMs; ++i) b *= 2;
        return static_cast<int>(std::min<int64_t>(b, m_c.maxBackoffMs));
    }

    FarRefStallConfig m_c{};
    bool    m_armed = false;
    int64_t m_armedMs = 0;
    int64_t m_lastMsgMs = -1;       // -1 = no message since arm()
    int64_t m_lastReopenMs = -1;    // -1 = never re-opened
    int64_t m_streakStartMs = -1;   // start of the current continuous run of messages
    int     m_attempts = 0;
    bool    m_reopenPending = false;   // a re-open was issued and its settle window is still open
    bool    m_episodeActedOn = false;  // this stall has been acted on (re-open issued / gave up): acknowledge its end
    bool    m_gaveUp = false;
};

}  // namespace talq
