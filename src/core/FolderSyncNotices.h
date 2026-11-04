#pragma once

// FolderSyncNotices -- what folder sync tells the person, and when. Pure: Qt Core value types only,
// no clock of its own, no QObject, no notification call. MainWindow owns the wiring (it feeds the
// controller's facts in, asks what to do and calls NotificationManager::notifySilently); every rule
// that decides WHETHER and WHEN lives here so a unit test can pin it. See
// tests/folder_sync_policy_test.cpp.
// Spec: docs/superpowers/specs/2026-11-03-folder-sync-design.md ("Notifications (D5, D23)", D40,
// "Settings row" -> "Status line texts", D34).
//
// Five things:
//   1. the popup texts (`noticeText`) -- the spec's "The notices" table, character for character;
//   2. which notice a pause reason raises (`noticeForPause`);
//   3. `NoticeScheduler`, the spacing gate with ONE pending slot (D40);
//   4. the Settings status line (`statusLineFor`) and its "Last synced" time (`formatLastSynced`);
//   5. why turning the switch on did not work (`enableRefusalText`, the "Not turned on: ..." lines).
//
// TWO CLOCKS, never mixed (D34). `NoticeScheduler` takes `nowMs` from the caller's MONOTONIC clock
// (a QElapsedTimer); every gap in this file is a difference of two such readings, so a change of the
// system clock cannot stall or flood the notices. `statusLineFor` / `formatLastSynced` are display
// only and read the WALL clock (Unix epoch milliseconds, or a QDateTime): they never time anything.
// Do not pass one to the other.
//
// Texts are source strings (`const char *` / literals). The UI wraps them in tr() when it uses them.

#include "core/FolderSyncPolicy.h"

#include <QDateTime>
#include <QLocale>
#include <QString>
#include <QTimeZone>
#include <QtGlobal>

#include <optional>

namespace talq {

// -- the notices ---------------------------------------------------------------------------------

// The twelve popups. PausedServerUnsupported also serves FolderSyncPause::Fatal (same text, D40).
// There is no notice for StoreUnavailable and none for the account-not-ready wait: the Settings
// status line is their only voice.
enum class NoticeId {
    RemoteApplied,          // (a) another computer's change was applied
    FirstSync,              // (b) the first successful sync after the switch was turned on
    FirstSyncHint,          // (b), variant: folders exist but the sidebar is not in Folders mode
    ProblemTransient,       // (c) at least 3 failures over 10 minutes with the server reachable
    PausedAuthRefused,      // (c) terminal pauses, one per reason
    PausedRemoteNewer,
    PausedRemoteUnreadable,
    PausedTooLarge,
    PausedServerSizeLimit,
    PausedQuotaFull,
    PausedServerUnsupported,
    PausedRemoteShrank,
};

// The spec's (a), (b), (c). The ORDER IS THE PRECEDENCE, lowest first: a pending notice is replaced
// only by one of the same or a higher class.
enum class NoticeClass { Applied, First, Problem };

inline NoticeClass classOf(NoticeId id)
{
    switch (id) {
    case NoticeId::RemoteApplied:
        return NoticeClass::Applied;
    case NoticeId::FirstSync:
    case NoticeId::FirstSyncHint:
        return NoticeClass::First;
    case NoticeId::ProblemTransient:
    case NoticeId::PausedAuthRefused:
    case NoticeId::PausedRemoteNewer:
    case NoticeId::PausedRemoteUnreadable:
    case NoticeId::PausedTooLarge:
    case NoticeId::PausedServerSizeLimit:
    case NoticeId::PausedQuotaFull:
    case NoticeId::PausedServerUnsupported:
    case NoticeId::PausedRemoteShrank:
        return NoticeClass::Problem;
    }
    return NoticeClass::Problem;
}

struct NoticeText {
    const char *title;   // 28 characters or fewer
    const char *body;    // under 80 characters: the popup elides what does not fit in two lines
};

// The exact titles and bodies of the spec's "The notices" table (a plain sentence ending in a
// period, says "TalQ" and "your folders" and "your other computers", no protocol words). The test
// holds the limits and re-types every text from the table.
inline NoticeText noticeText(NoticeId id)
{
    switch (id) {
    case NoticeId::RemoteApplied:
        return { "Folders updated", "Your folders were updated from another computer." };
    case NoticeId::FirstSync:
        return { "Folder sync is on", "Your folders now stay the same on your other computers." };
    case NoticeId::FirstSyncHint:
        return { "Folder sync is on",
                 "Your folders are now in sync. To see them, choose Group by, then Folders." };
    case NoticeId::ProblemTransient:
        return { "Folders not syncing",
                 "TalQ could not save your folders to Nextcloud. It will keep trying." };
    case NoticeId::PausedAuthRefused:
        return { "Folder sync paused",
                 "Nextcloud no longer accepts the saved sign-in. Sign in again to resume." };
    case NoticeId::PausedRemoteNewer:
        return { "Folder sync paused",
                 "The synced folders were saved by a newer TalQ. Update TalQ to resume." };
    case NoticeId::PausedRemoteUnreadable:
        return { "Folder sync paused", "The shared folders file looks damaged, so TalQ left it alone." };
    case NoticeId::PausedTooLarge:
        return { "Folder sync paused",
                 "Your computers together have too many folders. Delete some to resume." };
    case NoticeId::PausedServerSizeLimit:
        return { "Folder sync paused",
                 "Nextcloud refused the folders file as too large, so syncing is paused." };
    case NoticeId::PausedQuotaFull:
        return { "Folders not syncing", "Your Nextcloud storage is full, so folders could not be saved." };
    case NoticeId::PausedServerUnsupported:
        return { "Folder sync paused", "Nextcloud did not accept TalQ's saves, so folder sync is paused." };
    case NoticeId::PausedRemoteShrank:
        return { "Folder sync paused",
                 "Folders were removed on another computer. Press Sync now in Settings." };
    }
    return { "", "" };
}

// The terminal notice a pause reason raises, or nothing: StoreUnavailable has no popup (D40), and
// None is "not paused". Fatal reuses the ServerUnsupported notice.
inline std::optional<NoticeId> noticeForPause(FolderSyncPause pause)
{
    switch (pause) {
    case FolderSyncPause::AuthRefused: return NoticeId::PausedAuthRefused;
    case FolderSyncPause::RemoteNewer: return NoticeId::PausedRemoteNewer;
    case FolderSyncPause::RemoteUnreadable: return NoticeId::PausedRemoteUnreadable;
    case FolderSyncPause::TooLarge: return NoticeId::PausedTooLarge;
    case FolderSyncPause::ServerSizeLimit: return NoticeId::PausedServerSizeLimit;
    case FolderSyncPause::QuotaFull: return NoticeId::PausedQuotaFull;
    case FolderSyncPause::RemoteShrank: return NoticeId::PausedRemoteShrank;
    case FolderSyncPause::ServerUnsupported:
    case FolderSyncPause::Fatal:
        return NoticeId::PausedServerUnsupported;
    case FolderSyncPause::StoreUnavailable:
    case FolderSyncPause::None:
        return std::nullopt;
    }
    return std::nullopt;
}

// -- the scheduler -------------------------------------------------------------------------------

// Any two sync popups are at least this far apart, measured from the popup that went out last, so
// they never reach the popup stack's 3 s coalescing window (D40).
constexpr qint64 kNoticeSpacingMs = 30000;

// An (a) is not shown more than once per this, measured from the last (a) that went out.
constexpr qint64 kAppliedGapMs = 120000;

// The gate and the one-slot pending queue of D40. The caller feeds it facts and asks it what to do;
// it never calls out and never reads a clock: every method that needs the time takes `nowMs` from
// the caller's monotonic clock.
//
// The loop MainWindow runs:
//     offer(id, now)                      // a fact: "tell the person this"
//     a = next(now)
//     a.kind == Show  -> ok = notifySilently(...); delivered(a.id, now, ok)
//     a.kind == Wait  -> arm a single-shot timer for a.waitMs and ask again when it fires
//     a.kind == None  -> nothing to do (empty slot, or held)
//     kick()                              // at every cycle outcome and every poll
//     episodeEnded()                      // at the next success
//
// The rules (spec "Notifications", "Rules"):
//   - SPACING WINS. A notice the spacing suppresses is NOT spent: `next()` answers Wait with the
//     time left, the notice stays in the slot, and the caller asks again when the spacing ends.
//     This beats "a terminal (c) notifies immediately".
//   - ONE SLOT, precedence Problem over First over Applied; within one class the newest wins; a
//     lower class never replaces a higher one.
//   - SPENT ONLY WHEN DELIVERED. `delivered(id, now, true)` means notifySilently returned true: the
//     slot is cleared and the spacing starts. `false` (notifications off, Do Not Disturb) spends
//     nothing and starts no spacing: the notice stays pending but HELD, so `next()` answers None
//     until `kick()`; there is no timer loop, and a person in Do Not Disturb is told later.
//   - (a) at most once per kAppliedGapMs, measured from the last (a) that WENT OUT. One offered
//     inside the gap is ignored, not queued: the change is already applied and shown by the sidebar
//     and the Settings line.
//   - (c) once per episode and reason: a Problem that went out is ignored when offered again until
//     `episodeEnded()`. Whether a transient failure is worth a notice at all
//     (`transientNoticeDue`), whether a change is visible, and the one-shot "first sync" flag are
//     the caller's and the controller's decisions; they arrive here as an `offer`.
//   - `episodeEnded()` (the next success) drops a pending Problem, whose premise has gone, and
//     forgets which Problems were shown. It leaves a pending First or Applied alone and does not
//     touch the spacing or the Applied gap.
class NoticeScheduler {
public:
    struct Action {
        enum Kind {
            None,   // nothing to do: the slot is empty, or its notice is held until kick()
            Show,   // show `id` now, then report delivered(id, now, ok)
            Wait,   // the spacing is not over: ask again in `waitMs`
        } kind = None;
        NoticeId id = NoticeId::RemoteApplied;   // the pending notice, for Show and Wait
        qint64 waitMs = 0;                       // Wait only: the spacing time left
    };

    // Offers a notice for the one pending slot.
    //
    // There is deliberately NO "Group by mode" input: (a) is shown in every Group by mode (Q2 ii),
    // so the sidebar's mode matters only to WHICH (b) text the caller offers, FirstSync or
    // FirstSyncHint, and that choice is made before this call. A second gate keyed on the mode
    // would be a rule that decides nothing.
    void offer(NoticeId id, qint64 nowMs)
    {
        const NoticeClass cls = classOf(id);
        if (cls == NoticeClass::Problem && (m_shownProblems & bitOf(id)) != 0)
            return;   // already shown in this episode
        if (cls == NoticeClass::Applied && m_haveShownApplied && nowMs - m_lastAppliedMs < kAppliedGapMs)
            return;   // inside the 2-minute gap: ignored, not queued
        if (m_pending && classOf(*m_pending) > cls)
            return;   // a lower class never replaces a higher one
        m_pending = id;   // the same class or higher: the newest wins
    }

    // The next success: the episode is over. Drops a pending Problem (its premise has gone: for
    // example a TooLarge fixed within the 30 s) and forgets which Problems were shown.
    void episodeEnded()
    {
        m_shownProblems = 0;
        if (m_pending && classOf(*m_pending) == NoticeClass::Problem)
            dropPending();
    }

    // Re-arms a notice that was held because notifications were off: the caller's next cycle
    // outcome or poll asks again.
    void kick() { m_held = false; }

    // What to do now. Read-only: asking twice gives the same answer, and asking spends nothing.
    Action next(qint64 nowMs) const
    {
        Action a;
        if (!m_pending || m_held)
            return a;
        a.id = *m_pending;
        if (m_haveShown) {
            const qint64 sinceLastMs = nowMs - m_lastShownMs;
            if (sinceLastMs < kNoticeSpacingMs) {
                a.kind = Action::Wait;
                a.waitMs = kNoticeSpacingMs - sinceLastMs;
                return a;
            }
        }
        a.kind = Action::Show;
        return a;
    }

    // The outcome of showing `id`. `ok` is notifySilently's return value.
    //   true:  the popup went out. The spacing starts at `nowMs`; an Applied also starts its gap; a
    //          Problem is spent for the episode; the slot is cleared if it still holds `id` (a newer
    //          notice that took the slot meanwhile stays pending).
    //   false: nothing is spent and no spacing starts. The pending notice is held until kick().
    void delivered(NoticeId id, qint64 nowMs, bool ok)
    {
        if (!ok) {
            if (m_pending)
                m_held = true;
            return;
        }
        m_haveShown = true;
        m_lastShownMs = nowMs;
        switch (classOf(id)) {
        case NoticeClass::Applied:
            m_haveShownApplied = true;
            m_lastAppliedMs = nowMs;
            break;
        case NoticeClass::Problem:
            m_shownProblems |= bitOf(id);
            break;
        case NoticeClass::First:
            break;
        }
        if (m_pending && *m_pending == id)
            dropPending();
    }

private:
    static quint32 bitOf(NoticeId id) { return quint32(1) << static_cast<unsigned>(id); }

    // Empties the slot; the hold belonged to the notice that was in it.
    void dropPending()
    {
        m_pending.reset();
        m_held = false;
    }

    std::optional<NoticeId> m_pending;
    bool m_held = false;               // delivered(..., false) and no kick() since
    bool m_haveShown = false;          // a popup has gone out (so a reading of 0 is not "never")
    qint64 m_lastShownMs = 0;
    bool m_haveShownApplied = false;
    qint64 m_lastAppliedMs = 0;
    quint32 m_shownProblems = 0;       // bit per NoticeId of a Problem shown in this episode
};

// -- the status line -----------------------------------------------------------------------------

// "today 14:32", "yesterday 09:10" or "3 Nov 14:32". Display only (wall clock, D34).
//
// `last` is read in the time zone of `now`, so the day boundary is the reader's local midnight (both
// arguments are local-time readings in normal use; a test passes fixed-offset zones). The day is
// compared as a calendar date (`daysTo`), not as a count of 24 hours, so midnight, a month end, a
// year end and a DST change behave. A `last` on a LATER day than `now` (the wall clock was set back)
// prints the date, never "today". Month names are English whatever the machine's locale (C locale):
// the words go through tr() in the UI. An invalid argument gives an empty string.
inline QString formatLastSynced(const QDateTime &last, const QDateTime &now)
{
    if (!last.isValid() || !now.isValid())
        return QString();
    const QDateTime lastThere = last.toTimeZone(now.timeZone());
    const QLocale c = QLocale::c();
    const QString hm = c.toString(lastThere.time(), QStringLiteral("HH:mm"));
    const qint64 daysAgo = lastThere.date().daysTo(now.date());
    if (daysAgo == 0)
        return QStringLiteral("today ") + hm;
    if (daysAgo == 1)
        return QStringLiteral("yesterday ") + hm;
    return c.toString(lastThere.date(), QStringLiteral("d MMM")) + QLatin1Char(' ') + hm;
}

// The text under the Settings row: the exact "Status line texts" table of the spec.
//   - `lastSyncedMs` and `nowMs` are WALL-CLOCK Unix epoch milliseconds (display only, D34), NOT
//     the scheduler's monotonic readings. `lastSyncedMs <= 0` means "never synced".
//   - `shrinkDropped` / `shrinkOf` fill N and M of the RemoteShrank text.
//   - `accountNotReady` picks the other Waiting row (D38). The state `Waiting` covers both a
//     transient failure and an account that has not finished loading, and nothing else in the
//     signature tells them apart, so the caller (which knows whether `davUserKnown()` failed) says
//     so. It is ignored in every other state.
//   - Only the state decides, except that `Paused` reads its reason; the reason is ignored in every
//     other state. A Paused state with reason None (the caller's inconsistency) shows the generic
//     "did not expect" text rather than nothing.
// Off is the empty string. The texts are source strings; the UI wraps them in tr().
inline QString statusLineFor(FolderSyncState state, FolderSyncPause pause, qint64 lastSyncedMs,
                             qint64 nowMs, int shrinkDropped, int shrinkOf, bool accountNotReady = false)
{
    switch (state) {
    case FolderSyncState::Off:
        return QString();
    case FolderSyncState::SettingUp:
        return QStringLiteral("Setting up...");
    case FolderSyncState::Syncing:
        return QStringLiteral("Syncing...");
    case FolderSyncState::Idle:
        if (lastSyncedMs <= 0)
            return QStringLiteral("Not synced yet.");
        return QStringLiteral("Last synced: ")
               + formatLastSynced(QDateTime::fromMSecsSinceEpoch(lastSyncedMs),
                                  QDateTime::fromMSecsSinceEpoch(nowMs));
    case FolderSyncState::Waiting:
        if (accountNotReady)
            return QStringLiteral("Waiting for your Nextcloud account to finish loading.");
        return QStringLiteral("TalQ can't reach your Nextcloud files right now. It will try again.");
    case FolderSyncState::Paused:
        switch (pause) {
        case FolderSyncPause::AuthRefused:
            return QStringLiteral("Nextcloud no longer accepts the saved sign-in. Sign in again to resume.");
        case FolderSyncPause::RemoteNewer:
            return QStringLiteral("The synced folders were saved by a newer TalQ. Update TalQ to resume.");
        case FolderSyncPause::RemoteUnreadable:
            return QStringLiteral(
                "The shared folders file in your Nextcloud looks damaged, so TalQ left it alone. To "
                "start again, turn sync off on every computer, then delete .talq-folders.json in your "
                "Files (or fix it there), then turn sync on again.");
        case FolderSyncPause::TooLarge:
            return QStringLiteral(
                "Together your computers have more than 50 folders (or more filed chats than TalQ can "
                "hold). Delete some folders; TalQ checks again after each change you make, or press "
                "Sync now.");
        case FolderSyncPause::ServerSizeLimit:
            return QStringLiteral(
                "The Nextcloud server refused the shared file as too large, so TalQ stopped saving "
                "it. Folders stay on this computer. Press Sync now to try again.");
        case FolderSyncPause::QuotaFull:
            return QStringLiteral("Your Nextcloud storage is full, so folders could not be saved.");
        case FolderSyncPause::StoreUnavailable:
            return QStringLiteral(
                "Folders are unavailable on this computer right now, so they cannot be synced.");
        case FolderSyncPause::RemoteShrank:
            return QStringLiteral(
                       "Another computer removed %1 of your %2 folders. Press Sync now to accept this "
                       "change.")
                .arg(shrinkDropped)
                .arg(shrinkOf);
        case FolderSyncPause::ServerUnsupported:
        case FolderSyncPause::Fatal:
        case FolderSyncPause::None:
            break;
        }
        return QStringLiteral(
            "Your Nextcloud server answered in a way TalQ did not expect. Folders stay on this "
            "computer.");
    }
    return QString();
}

// -- the switch's own answer ---------------------------------------------------------------------

// Why the switch did not turn on (the TOGGLE path only, spec "Settings row" and D28/D35): the
// capability probe was refused or could not run, or the first cycle after the toggle met 401/403.
// The controller sends the text with enableFinished(false, ...); the switch goes back to off and the
// text is shown beside it. Status text, not a popup: there is no 80-character limit. At attach
// nothing ever reverts the switch, and the status line says what is wrong instead.
enum class EnableRefusal {
    ServerUnsupported,   // the probe was refused: the server ignores a precondition (or answered wrongly)
    SignInRefused,       // 401/403, from the probe or from the first cycle after the toggle
    CouldNotReach,       // the probe could not run (offline, 5xx, a timeout, the folders unavailable)
};

inline QString enableRefusalText(EnableRefusal why)
{
    switch (why) {
    case EnableRefusal::ServerUnsupported:
        return QStringLiteral(
            "Not turned on: your Nextcloud server does not support safe saving of this file. Your "
            "folders stay on this computer.");
    case EnableRefusal::SignInRefused:
        return QStringLiteral("Not turned on: Nextcloud no longer accepts the saved sign-in.");
    case EnableRefusal::CouldNotReach:
        return QStringLiteral("Not turned on: TalQ could not reach your Nextcloud files.");
    }
    return QString();
}

} // namespace talq
