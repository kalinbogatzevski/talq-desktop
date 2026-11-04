#pragma once

// FolderSyncUiLogic -- the decisions MainWindow makes about folder sync, as pure functions: what the
// Settings row shows, which notice a controller fact offers, when a window event is passed on to
// the controller, and when the shared switch key is written. Qt Core value types only: no widget,
// no QObject, no clock, no QSettings. MainWindow cannot be built in a test, so it keeps only the Qt
// plumbing (reading its members into these inputs, calling the controller, the dialog, QSettings
// and notifySilently) and every rule lives here. See tests/folder_sync_policy_test.cpp.
// Spec: docs/superpowers/specs/2026-11-03-folder-sync-design.md ("Settings row (D24)",
// "Notifications (D5, D23)", "Triggers and cadence (D18)", "First enable and turning off", D17,
// D28, D35, D36, D40).
//
// Texts are source strings, like FolderSyncNotices.h: MainWindow wraps them in tr() where it shows
// them. The few new ones here are marked with QT_TRANSLATE_NOOP("MainWindow", ...) so lupdate finds
// them under the context MainWindow's tr() looks them up in.

#include "core/FolderLogic.h"
#include "core/FolderSyncLogic.h"
#include "core/FolderSyncNotices.h"
#include "core/FolderSyncPolicy.h"

#include <QString>
#include <QtGlobal>

#include <optional>

namespace talq {

// -- the Settings row (D24) ----------------------------------------------------------------------

// Why the row is disabled. None = it is available.
enum class FolderSyncUnavailable {
    None,
    NotSignedIn,          // nobody is signed in (after a logout, the store is detached too)
    LoadFailed,           // the store's attach() failed this login (an unreadable folders file)
    Detached,             // signed in, but no folders store is attached (no account identity yet)
    ReadOnlyNewerFormat,  // the folders file was written by a newer TalQ: the store is read-only
    ReadOnlyCouldNotSetAside,   // a damaged file that could not be renamed away: read-only
};

// Everything the row is built from, read by MainWindow at the moment of the push.
struct FolderSyncRowInputs {
    bool signedIn = false;          // AuthManager::isLoggedIn()
    bool storeAttached = false;     // FolderStore::isAttached()
    bool storeLoadFailed = false;   // MainWindow::m_foldersLoadFailed
    bool storeReadOnly = false;     // FolderStore::isReadOnly()
    bool storeNewerFormat = false;  // FolderStore::readOnlyReason() == NewerFormat
    FolderLoadOutcome loadOutcome = FolderLoadOutcome::Detached;
    // What the controller is doing. `switchOn` is FolderSyncController::isEnabled(): true from the
    // moment enable() is called (the probe and the first cycle included) until disable(), a
    // refusal or a detach, so the switch never flips back under the user's finger while the
    // toggle-ON probe runs.
    bool switchOn = false;
    FolderSyncState state = FolderSyncState::Off;
    FolderSyncPause pause = FolderSyncPause::None;
    bool accountNotReady = false;
    qint64 lastSyncedMs = 0;   // WALL clock, display only (D34); 0 = never
    qint64 nowMs = 0;          // WALL clock at this push: "today" / "yesterday" stay current
    int shrinkDropped = 0;
    int shrinkOf = 0;
    // The last "Not turned on: ..." answer (enableFinished(false, reason)), kept by MainWindow
    // until the next toggle-ON attempt, a successful answer or a detach (refusalTextAfter).
    QString refusalText;
};

// What the Settings row shows (SettingsDialog::FolderSyncUi carries the same five fields).
//   enabled     what the switch SHOWS (checked)
//   available   the switch can be used; false: it is disabled and `whyNot` is shown
//   canSyncNow  "Sync now" is offered (the dialog shows it only when enabled && canSyncNow)
//   whyNot      never empty while available is false (the dialog hides an empty label, which
//               would silently lose "disabled WITH the reason")
//   statusText  the line under the row while available (empty: the label hides)
struct FolderSyncRow {
    bool enabled = false;
    bool available = false;
    bool canSyncNow = false;
    QString whyNot;
    QString statusText;
};

// Which cause, if any, disables the row. The first that holds wins, most specific first: a logout
// also detaches the store, and a failed load leaves it detached.
inline FolderSyncUnavailable folderSyncUnavailableReason(const FolderSyncRowInputs &in)
{
    if (!in.signedIn)
        return FolderSyncUnavailable::NotSignedIn;
    if (in.storeLoadFailed)
        return FolderSyncUnavailable::LoadFailed;
    if (!in.storeAttached || in.loadOutcome == FolderLoadOutcome::Detached)
        return FolderSyncUnavailable::Detached;
    if (in.storeReadOnly || in.loadOutcome == FolderLoadOutcome::ReadOnlyLoad)
        return in.storeNewerFormat ? FolderSyncUnavailable::ReadOnlyNewerFormat
                                   : FolderSyncUnavailable::ReadOnlyCouldNotSetAside;
    return FolderSyncUnavailable::None;
}

// The disabled row's reason. The three store texts are the ones the sidebar's "Group by" notice
// already says for the same causes (MainWindow::syncGroupByMenu), so the two places agree; the
// detached one is the spec's StoreUnavailable status line. Never empty for a real cause.
inline const char *folderSyncUnavailableText(FolderSyncUnavailable why)
{
    switch (why) {
    case FolderSyncUnavailable::None:
        return "";
    case FolderSyncUnavailable::NotSignedIn:
        return QT_TRANSLATE_NOOP("MainWindow", "Sign in to keep your folders in sync.");
    case FolderSyncUnavailable::LoadFailed:
        return QT_TRANSLATE_NOOP("MainWindow", "Folders could not be loaded. Restart TalQ to try again.");
    case FolderSyncUnavailable::Detached:
        return QT_TRANSLATE_NOOP("MainWindow",
                                 "Folders are unavailable on this computer right now, so they cannot be synced.");
    case FolderSyncUnavailable::ReadOnlyNewerFormat:
        return QT_TRANSLATE_NOOP("MainWindow", "Folders are hidden: saved by a newer version of TalQ.");
    case FolderSyncUnavailable::ReadOnlyCouldNotSetAside:
        return QT_TRANSLATE_NOOP("MainWindow", "Folders are read-only: the folders file could not be updated.");
    }
    return QT_TRANSLATE_NOOP("MainWindow",
                             "Folders are unavailable on this computer right now, so they cannot be synced.");
}

// "Sync now" is offered while the controller can start a cycle from the button: not Off, not while
// one is running (SettingUp, Syncing), and not in Paused(AuthRefused), which waits for a sign-in or
// a switch toggle (the controller ignores the button there).
inline bool syncNowOffered(FolderSyncState state, FolderSyncPause pause)
{
    switch (state) {
    case FolderSyncState::Idle:
    case FolderSyncState::Waiting:
        return true;
    case FolderSyncState::Paused:
        return pause != FolderSyncPause::AuthRefused;
    case FolderSyncState::Off:
    case FolderSyncState::SettingUp:
    case FolderSyncState::Syncing:
        return false;
    }
    return false;
}

// The row. Disabled: the reason, nothing else (no status line, no "Sync now"); the switch still
// shows what the controller is doing (a read-only store with the switch on is on, and paused).
// Available with the switch off: the last refusal's text, if any (W4). Available and on: the status
// line for the time of THIS push, and "Sync now" where the controller can act on it.
inline FolderSyncRow folderSyncRow(const FolderSyncRowInputs &in)
{
    FolderSyncRow row;
    row.enabled = in.switchOn;
    const FolderSyncUnavailable why = folderSyncUnavailableReason(in);
    if (why != FolderSyncUnavailable::None) {
        row.whyNot = QString::fromUtf8(folderSyncUnavailableText(why));
        return row;
    }
    row.available = true;
    if (!in.switchOn) {
        row.statusText = in.refusalText;
        return row;
    }
    row.statusText = statusLineFor(in.state, in.pause, in.lastSyncedMs, in.nowMs, in.shrinkDropped,
                                   in.shrinkOf, in.accountNotReady);
    row.canSyncNow = syncNowOffered(in.state, in.pause);
    return row;
}

// -- the notices (D5, D40) ----------------------------------------------------------------------

// What a finished cycle (cycleFinished) offers the notice scheduler, or nothing.
//   - the first successful cycle after the switch was turned on: (b), FirstSync, or FirstSyncHint
//     when folders now exist and the sidebar is not grouped by folders; it replaces (a) for that
//     cycle;
//   - otherwise a change the user can see (StateDiff::visible(): not a collapse alone, Q2 iii):
//     (a), RemoteApplied, in every Group by mode (Q2 ii);
//   - otherwise nothing: an upload of this PC's own change alone is not a notice (kNotifyOnUpload,
//     Q2 i).
inline std::optional<NoticeId> noticeForCycle(const StateDiff &diff, bool firstAfterEnable, bool uploaded,
                                              bool foldersExist, bool groupedByFolders)
{
    // Flipping kNotifyOnUpload (Q2 i) needs a fourth NoticeId with the spec's "Folders synced" text
    // and a branch here; until then an upload is deliberately not a notice.
    static_assert(!kNotifyOnUpload, "an upload notice needs its own NoticeId and a branch here");
    Q_UNUSED(uploaded);
    if (firstAfterEnable)
        return (foldersExist && !groupedByFolders) ? NoticeId::FirstSyncHint : NoticeId::FirstSync;
    if (diff.visible())
        return NoticeId::RemoteApplied;
    return std::nullopt;
}

// What a failed attempt (attemptFailed) offers: the transient notice once the run is long enough
// and the server answers other requests (an offline period never notifies), else nothing.
inline std::optional<NoticeId> noticeForFailedAttempt(int consecutiveFailures, qint64 failureSpanMs,
                                                      bool serverReachable)
{
    if (transientNoticeDue(consecutiveFailures, failureSpanMs, serverReachable))
        return NoticeId::ProblemTransient;
    return std::nullopt;
}

// -- the window triggers (D18, D36) --------------------------------------------------------------

enum class FolderSyncWindowEvent {
    Activated,              // ActivationChange, the main window became active
    Deactivated,            // ActivationChange, judged after a 0 ms hop (D36)
    Hidden,                 // hideEvent (a minimize sends one too)
    ReachabilityRestored,   // the server became reachable again (the true edge)
};

// Whether MainWindow passes the event on to the controller's hook. Nothing at all once the app is
// quitting (D18: no network call at quit). A deactivation only when NO window of this app is active
// (D36: the app's own Settings dialog, a name prompt or a message box taking the focus is not one);
// a hide always. `appWindowActive` is read only for Deactivated. No other gate: the controller
// judges every gap, pause, backoff and the in-flight guard itself, and the poll runs whatever the
// window is doing (Q2 v).
inline bool folderSyncHookDue(FolderSyncWindowEvent event, bool appWindowActive, bool quitting)
{
    if (quitting)
        return false;
    switch (event) {
    case FolderSyncWindowEvent::Deactivated:
        return !appWindowActive;
    case FolderSyncWindowEvent::Activated:
    case FolderSyncWindowEvent::Hidden:
    case FolderSyncWindowEvent::ReachabilityRestored:
        return true;
    }
    return false;
}

// -- the shared switch key (D24, D28, D35) -------------------------------------------------------

// The QSettings key of the switch, per account: FolderSync/<scopeKey>/enabled. Empty for anything
// that is not a scope key (accountScopeKey's 16 lowercase hex), so nothing is ever read from or
// written under a malformed name.
inline QString folderSyncEnabledKey(const QString &scopeKey)
{
    if (!isScopeKey(scopeKey))
        return QString();
    return QStringLiteral("FolderSync/") + scopeKey + QStringLiteral("/enabled");
}

// What happened to the switch, as MainWindow sees it.
enum class FolderSyncSwitchEvent {
    ToggledOn,        // the user turned the switch on (folderSyncToggled(true))
    ToggledOff,       // the user turned it off (folderSyncToggled(false))
    EnableSucceeded,  // enableFinished(true): the probe passed (or the marker was there)
    EnableRefused,    // enableFinished(false, reason): refused, or the first cycle met 401/403
    Attached,         // attachFolders() attached the controller (the switch already on or off)
    Detached,         // detachFolders(): logout or account switch
    StateChanged,     // any controller state change
};

enum class EnabledKeyWrite { Nothing, WriteFalse, WriteTrue };

// When the key is written (D28): false when the user turns the switch off (BEFORE disable()), true
// only when enableFinished(true) says the server is proved, false again when enableFinished(false)
// reverts the switch. NEVER on attach, detach, a toggle-ON (the probe has not passed yet) or any
// state change: the key is shared by the generic and the branded build on one PC, and one build's
// attach-time probe must never flip the other's switch (D17, D28).
inline EnabledKeyWrite enabledKeyWriteFor(FolderSyncSwitchEvent event)
{
    switch (event) {
    case FolderSyncSwitchEvent::ToggledOff:
    case FolderSyncSwitchEvent::EnableRefused:
        return EnabledKeyWrite::WriteFalse;
    case FolderSyncSwitchEvent::EnableSucceeded:
        return EnabledKeyWrite::WriteTrue;
    case FolderSyncSwitchEvent::ToggledOn:
    case FolderSyncSwitchEvent::Attached:
    case FolderSyncSwitchEvent::Detached:
    case FolderSyncSwitchEvent::StateChanged:
        return EnabledKeyWrite::Nothing;
    }
    return EnabledKeyWrite::Nothing;
}

// The refusal text MainWindow keeps for the row (W4): set by enableFinished(false, reason) and kept
// across later state changes (the Off push that follows would otherwise wipe it); cleared by the
// next toggle (either way), a successful answer, an attach and a detach. A refusal with no reason
// still says something.
inline QString refusalTextAfter(FolderSyncSwitchEvent event, const QString &current, const QString &reasonText)
{
    switch (event) {
    case FolderSyncSwitchEvent::EnableRefused:
        return reasonText.isEmpty() ? enableRefusalText(EnableRefusal::CouldNotReach) : reasonText;
    case FolderSyncSwitchEvent::StateChanged:
        return current;
    case FolderSyncSwitchEvent::ToggledOn:
    case FolderSyncSwitchEvent::ToggledOff:
    case FolderSyncSwitchEvent::EnableSucceeded:
    case FolderSyncSwitchEvent::Attached:
    case FolderSyncSwitchEvent::Detached:
        return QString();
    }
    return current;
}

} // namespace talq
