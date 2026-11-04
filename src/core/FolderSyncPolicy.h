#pragma once

// FolderSyncPolicy -- the pure numbers and decisions of folder sync's controller: its states, why it
// pauses, every timing it uses (as a struct a test can shorten), the retry ladder, the two brakes
// that stop a sync from losing folders, and the counter for a server that rejects the ETag it just
// handed out. Qt Core value types only: no clock, no QObject, no network. The caller feeds it plain
// millisecond readings from QElapsedTimer (monotonic, D34), never the wall clock, so the whole thing
// is unit-testable without waiting. See tests/folder_sync_policy_test.cpp.
// Spec: docs/superpowers/specs/2026-11-03-folder-sync-design.md
//
// What is NOT here: how an HTTP status is read (DavSyncPolicy.h), the merge and the base-trust rules
// (FolderSyncLogic.h), and the notice gate with its pending slot and popup texts
// (FolderSyncNotices.h, added by a later task).

#include <QByteArray>
#include <QtGlobal>

#include <cmath>

namespace talq {

// What the controller is doing. Off is the zero value on purpose: a default-constructed controller
// is a controller that does nothing.
enum class FolderSyncState {
    Off,        // the switch is off, or no store is attached: no request is ever made
    SettingUp,  // the capability probe, or the first cycle after the switch was turned on
    Idle,       // on, last attempt succeeded (or nothing has run yet), nothing in flight
    Syncing,    // a cycle (or the probe) is in flight
    Waiting,    // a transient failure, backing off; or the account is not ready (status line only)
    Paused,     // needs attention; FolderSyncPause says why
};

// Why the controller is Paused. None is the zero value: it is what "not paused" reads as.
enum class FolderSyncPause {
    None,              // not paused
    AuthRefused,       // 401/403; clears on logout/login or a switch toggle, never retried
    RemoteNewer,       // the shared file was written by a newer sync build
    RemoteUnreadable,  // not JSON, over the cap, needs repair, or a download aborted for size
    TooLarge,          // the MERGE is over the 50-folder / 20000-assignment cap; a local edit can end it
    ServerSizeLimit,   // HTTP 413 on the upload: the server refused the file as too large
    ServerUnsupported, // the probe failed, or two self-inconsistent 412s: the server rejects its own ETag
    QuotaFull,         // HTTP 507; retried on the slowest backoff step
    StoreUnavailable,  // the local store is read-only or detached (status line only, no popup)
    RemoteShrank,      // the shrink brake fired; clears only on "Sync now"
    Fatal,             // a status the table does not name, or a 409; logged
};

// Every gap and interval of the controller. The defaults are the spec's numbers (D18, "Backoff",
// "One cycle", D22); the struct is a test seam: the controller test sets milliseconds here instead
// of waiting 10 s for a debounce. All are plain millisecond counts.
struct FolderSyncTiming {
    qint64 loginDelayMs         = 3000;     // after attach with the switch on, before the first cycle
    qint64 activationGapMs      = 120000;   // window activation pulls at most this often
    qint64 pushGapMs            = 15000;    // a deactivation / hide push waits this long since the last attempt
    qint64 backoffOverrideGapMs = 30000;    // an activation may cut a backoff short after this gap
    qint64 editDebounceMs       = 10000;    // quiet time after the last local edit before a cycle
    qint64 editMaxWaitMs        = 60000;    // ... but never more than this after the FIRST unsent edit
    qint64 pollMs               = 600000;   // the plain-GET poll, focused or not
    qint64 getTimeoutMs         = 20000;    // transfer timeout of the GET
    qint64 putTimeoutMs         = 30000;    // transfer timeout of the conditional PUT
    qint64 applyGatePollMs      = 500;      // how often a held remote apply asks the gate again
    qint64 applyGateGiveUpMs    = 30000;    // ... and when it gives up and retries later
    qint64 conflictRetryMinMs   = 200;      // a 412 re-GET waits a random time in [min, max]
    qint64 conflictRetryMaxMs   = 800;
    // D41, the replace gap: a PUT that REPLACES the file (If-Match) goes out no sooner than this after
    // the answer of the GET it was merged from (a create, If-None-Match: *, never waits). The server
    // keeps a file's version (ETag) for a write in the same whole second as the previous one (spec,
    // "Measured live"); the version being replaced was written no later than that answer, so a write
    // more than a second after it lands in a later second and gets a new version. Over 1000 on purpose.
    qint64 replaceGapMs         = 1200;
    // Every wait the ladder produces (nextBackoffMs: a transient failure's backoff, and the QuotaFull
    // slow step) is multiplied by this and floored at 1 ms. A test seam only: production never
    // changes it, and a test sets e.g. 0.0001 so the 30 s step lasts about 3 ms.
    double backoffScale         = 1.0;
};

// Conditional-write attempts in one cycle (a 412 or a 404 on an If-Match PUT goes back to the GET),
// and consecutive Stale restarts (the user edited while a cycle was in flight) before the cycle
// stops and waits for the next trigger.
constexpr int kMaxPutAttempts = 3;
constexpr int kMaxStaleRestarts = 5;

// Q2 (i): does a PC's OWN upload raise a "Folders synced" notice? Baseline NO (a notice per folder
// click would be noise). Flipping it to true, plus the fourth notice's title and body (spec,
// "Notifications"), is the change the owner would be asking for.
constexpr bool kNotifyOnUpload = false;

// The wait before the next attempt after `failures` consecutive transient failures (1 after the
// first). Ladder: 30 s, 1 min, 2 min, 5 min, 10 min, then 15 min for every later failure; a count
// of 1 or less (0 and negatives included) is the first step. `unit` is a draw in [0,1) made by the
// caller (so the function stays pure and a test can pin it); the wait is the step times
// 0.8 + 0.4 * unit, i.e. plus or minus 20%, rounded to the millisecond. A unit outside [0,1] (and
// NaN) is clamped, so the result is always within 24 s .. 18 min.
inline qint64 nextBackoffMs(int failures, double unit)
{
    static constexpr qint64 kLadderMs[] = {
        30 * 1000, 60 * 1000, 2 * 60 * 1000, 5 * 60 * 1000, 10 * 60 * 1000, 15 * 60 * 1000 };
    constexpr int kSteps = static_cast<int>(sizeof(kLadderMs) / sizeof(kLadderMs[0]));
    const int step = failures <= 1 ? 0 : (failures > kSteps ? kSteps - 1 : failures - 1);
    if (!(unit >= 0.0))      // also true for NaN
        unit = 0.0;
    else if (unit > 1.0)
        unit = 1.0;
    return static_cast<qint64>(std::llround(static_cast<double>(kLadderMs[step]) * (0.8 + 0.4 * unit)));
}

// The remote shrink brake (D29, option (c) of the open question Q1). `foldersDropped` is the merge's
// count of folders that are in the base AND in this PC's local state and absent from the remote: a
// PC's own deletions are never counted, so it cannot brake on its own change, and an untrusted base
// counts 0, so a first enable never brakes. Fires when the remote would remove EVERY folder this PC
// has (at least one), or MORE THAN HALF of them with at least 3. Exactly half does not fire, and
// neither does a drop of fewer than 3 that leaves a folder. Argument order is (local, dropped).
inline bool remoteShrinkBrake(int localFolderCount, int foldersDropped)
{
    if (foldersDropped >= 1 && foldersDropped == localFolderCount)
        return true;
    return foldersDropped >= 3 && 2 * foldersDropped > localFolderCount;
}

// The minimum upload brake (D26): a cycle never uploads an EMPTY state over a remote that is absent
// (404) or zero bytes. An empty first upload would turn "nobody wrote anything yet" into "everyone
// deleted everything" for every PC that trusts its base. Over a readable remote an empty merge is a
// deliberate delete-everything and goes up (the PCs that would lose all their folders meet the
// shrink brake instead).
inline bool uploadAllowed(bool mergedIsEmpty, bool remoteAbsentOrZeroBytes)
{
    return !(mergedIsEmpty && remoteAbsentOrZeroBytes);
}

// "A self-inconsistent 412" (cycle step 5). A race changes the file, so after a 412 the next GET
// returns a DIFFERENT ETag. When the next GET returns the very ETag the failed PUT was conditioned
// on, nobody wrote in between: the server (or a proxy) rejected an ETag it had just handed out.
// Two of those, in one cycle or in two consecutive cycles, mean this server cannot be synced with
// (pause as ServerUnsupported) instead of backing off for ever behind a status line that wrongly
// says the server is unreachable.
//
// Protocol: putFailed412(the If-Match value) when a conditional PUT answers 412; then nextGet(etag)
// with the ETag of the re-GET that follows it.
//   - putFailed412 ARMS the tracker; nextGet CONSUMES the arm. A nextGet with nothing armed (the
//     ordinary GET that starts a cycle or a poll) counts for nothing and changes nothing: an
//     unchanged file is the normal case and must never add up to a pause.
//   - Armed and the ETag equals the failed PUT's, byte for byte (quotes and any W/ prefix kept,
//     never normalised): the count goes up by one.
//   - Armed and the ETag differs: a race. The count goes back to 0.
//   - An EMPTY ETag on either side is never "the same". An If-None-Match: * PUT was conditioned on
//     no ETag, and a GET that carried none says nothing about the server's consistency; it reads
//     as a race (count back to 0).
//   - The count and the arm survive between cycles; a successful PUT calls reset().
class SelfInconsistent412Tracker {
public:
    void putFailed412(const QByteArray &ifMatchEtag)
    {
        m_failedEtag = ifMatchEtag;
        m_armed = true;
    }

    void nextGet(const QByteArray &etag)
    {
        if (!m_armed)
            return;
        m_armed = false;
        if (!m_failedEtag.isEmpty() && etag == m_failedEtag)
            ++m_count;
        else
            m_count = 0;
    }

    bool shouldPause() const { return m_count >= kPauseAt; }
    int count() const { return m_count; }

    void reset()
    {
        m_failedEtag.clear();
        m_armed = false;
        m_count = 0;
    }

private:
    static constexpr int kPauseAt = 2;

    QByteArray m_failedEtag;
    bool m_armed = false;
    int m_count = 0;
};

// The "Folders not syncing" notice for transient failures (D5 (c), "The notices"): at least 3
// consecutive failed attempts spanning at least 10 minutes (the span from the first failure of the
// run to the latest one), and the server reachable (chat works while files do not; an offline
// period never notifies). Both thresholds are inclusive. They are their own constants and are not
// derived from pollMs, so a test that shortens the timing seam cannot change when a person is told.
constexpr int kTransientNoticeMinFailures = 3;
constexpr qint64 kTransientNoticeMinSpanMs = 10 * 60 * 1000;

inline bool transientNoticeDue(int consecutiveFailures, qint64 spanMs, bool serverReachable)
{
    return serverReachable && consecutiveFailures >= kTransientNoticeMinFailures
           && spanMs >= kTransientNoticeMinSpanMs;
}

} // namespace talq
