#pragma once

// When a downloaded update may restart TalQ.
//
// Every decision about a pending installer lives here so it can be tested
// without a window, a call or a network. MainWindow only gathers the inputs
// (InstallGateInputs) and carries out what these functions return.
//
// Two ways this used to restart TalQ at a bad moment, both found in 0.72.2:
//
//  1. The auto-install gate looked only at the OPEN conversation's composer.
//     Since 0.65.3 every conversation keeps its own unsent draft in memory
//     (MainWindow::m_composerDrafts), and those drafts do not survive a
//     restart, so an update installed "because nothing was unsent" silently
//     threw away text typed into any other room. A file staged in the
//     composer and a voice message still being recorded are lost the same
//     way, so they block too.
//
//  2. CallManager::stateChanged was wired straight to the installer launch.
//     With an update waiting, ANY call ending started the install after the
//     post-call grace -- skipping the idle wait and the unsent-work check the
//     auto-install countdown exists for, and even after the user had clicked
//     "Cancel auto-install".
//
// The fix for (2) is to record WHAT KIND of install is pending once, when the
// download lands, instead of re-deriving it from settings and flags at every
// decision point. An automatic install only ever launches from the idle
// countdown, which treats a recent call as one more reason to wait. A
// cancelled one launches only when the user explicitly asks again. What the
// user asked for also survives the installer being downloaded again: after a
// failed launch, or when a newer version replaces the one waiting.

#include <QHash>
#include <QSet>
#include <QString>
#include <QtGlobal>

namespace talq {

// ---------------------------------------------------------------------------
// Unsent drafts
// ---------------------------------------------------------------------------

// Unsent text anywhere in the app. The open conversation is judged by its live
// composer only. Entering a room moves its draft out of the map and into the
// composer (takeDraftOnEnter), so the open room should never have an entry;
// the exclusion is kept so that a stray one could not block an update over a
// message the user has since sent.
inline bool hasUnsentComposerText(const QString &openComposerText,
                                  const QHash<QString, QString> &drafts,
                                  const QString &openConversationToken)
{
    if (!openComposerText.isEmpty())
        return true;
    for (auto it = drafts.cbegin(); it != drafts.cend(); ++it) {
        if (it.key() != openConversationToken && !it.value().isEmpty())
            return true;
    }
    return false;
}

// What the composer text was bound to when the room was left.
enum class ComposerBinding {
    None,      // plain text
    Reply,     // the reply bar was up: the text is still the user's own
    Edit,      // the editing bar was up: the text is an existing message
};

// Leaving a conversation, either for another one or for Home. Whatever the
// composer holds becomes that room's draft, or the room's entry is removed --
// never left as it was, because a leftover entry keeps blocking the automatic
// install long after the text it held was sent.
//
// - Empty composer: the entry is removed.
// - Editing a message: the entry is removed. An edit buffer holds an existing
//   message's text (showing the editing bar replaces the composer text with
//   it), so keeping it would hand that message back as unsent text.
// - Replying to a message: the text is kept as a plain draft. The user wrote
//   it, and the reply bar keeps whatever was typed before it opened -- often a
//   draft restored a moment earlier. Only the binding to the message is
//   dropped (the caller clears it; a draft is plain text and cannot hold one).
//
// Whether the room is still in the conversation list is deliberately NOT
// checked here: the list lags the server, and dropping real text because a
// refresh had not landed yet is the very loss drafts exist to prevent. Rooms
// that disappear lose their drafts through dropDraftsForUnlistedRooms instead.
inline void stashDraftOnLeave(QHash<QString, QString> &drafts,
                              const QString &leavingToken,
                              const QString &composerText,
                              ComposerBinding binding)
{
    if (leavingToken.isEmpty())
        return;
    if (composerText.isEmpty() || binding == ComposerBinding::Edit)
        drafts.remove(leavingToken);
    else
        drafts.insert(leavingToken, composerText);
}

// Entering a conversation: its draft moves into the composer and leaves the
// map. Reading it with value() kept the entry, so a message typed from the
// restored draft and then sent still counted as unsent in that room the
// moment the user switched away without typing anything new.
inline QString takeDraftOnEnter(QHash<QString, QString> &drafts,
                                const QString &token)
{
    return drafts.take(token);
}

// After a successful conversation-list refresh: drop drafts for rooms that
// are no longer listed. The user cannot open those rooms any more, so the
// drafts can never be sent -- but they would block the automatic install
// until logout.
inline void dropDraftsForUnlistedRooms(QHash<QString, QString> &drafts,
                                       const QSet<QString> &listedTokens)
{
    for (auto it = drafts.begin(); it != drafts.end();) {
        if (listedTokens.contains(it.key()))
            ++it;
        else
            it = drafts.erase(it);
    }
}

// ---------------------------------------------------------------------------
// What kind of install is pending
// ---------------------------------------------------------------------------

enum class InstallKind {
    None,           // no installer is waiting
    Automatic,      // launches only from the idle countdown
    UserAccepted,   // the user asked for it or accepted it: only a call defers it
    AwaitingUser,   // an automatic install the user cancelled: waits for them
};

struct InstallRequest {
    InstallKind kind = InstallKind::None;
    bool explicitRequest = false;   // Install now / Update now: skips the post-call grace
};

// A new version on offer makes the installer already waiting obsolete. It is
// dropped at once: the banner now offers the new version, so its Install now
// must not start the old one, and any download of the new version deletes
// the old installer's file anyway (UpdateChecker sweeps earlier installers
// when a download starts). What that install was -- above all one the user
// accepted -- is carried to the download that replaces it.
inline bool newVersionReplacesWaitingInstaller(bool isNewVersion,
                                               bool installerWaiting)
{
    return isNewVersion && installerWaiting;
}

// Whether a newly offered version is downloaded without a click.
//
// Only a genuinely new version: the periodic check re-offers the same one and
// must not pull the installer again. Automatic when auto-install is on and was
// not cancelled this session; otherwise only when it replaces an install the
// user had already accepted, so their request is not silently lost to the
// newer version.
inline bool downloadOfferedVersion(bool isNewVersion,
                                   bool autoInstallEnabled,
                                   bool autoInstallCancelledForSession,
                                   InstallKind replacedKind)
{
    if (!isNewVersion)
        return false;
    if (autoInstallEnabled && !autoInstallCancelledForSession)
        return true;
    return replacedKind == InstallKind::UserAccepted;
}

// Decided once, when the download lands. Later decisions never re-read the
// auto-install setting: switching it while an installer waits must not turn
// an install the user cancelled, or one still counting down, into something
// that launches on its own.
//
// updateNowRequested:  Settings "Update now" started this download.
// replaced:            the install this download replaces, while that
//                      download is in flight: the one whose installer failed
//                      to start (the silent re-download), or the one a newer
//                      version made obsolete. None otherwise.
//
// A replacement keeps what the user asked for. An accepted install stays
// accepted -- explicit included -- whatever the settings say now; an automatic
// one stays automatic, so switching auto-install off during the re-download
// cannot launch it past the idle and unsent-work gates. A cancelled install is
// never carried: its successor is only ever downloaded because the user
// clicked, and lands as accepted by the rule below.
//
// Without a replacement: automatic when auto-install is on and was not
// cancelled this session. Otherwise the download did not come from the
// automatic download (it does not run in either case) but from the user --
// Install now or Retry on the banner -- so it is accepted.
inline InstallRequest installWhenDownloadLands(bool updateNowRequested,
                                               bool autoInstallEnabled,
                                               bool autoInstallCancelledForSession,
                                               InstallRequest replaced)
{
    if (updateNowRequested)
        return {InstallKind::UserAccepted, true};
    switch (replaced.kind) {
    case InstallKind::UserAccepted:
        return {InstallKind::UserAccepted, replaced.explicitRequest};
    case InstallKind::Automatic:
        if (!autoInstallCancelledForSession)
            return {InstallKind::Automatic, false};
        break;
    case InstallKind::AwaitingUser:
    case InstallKind::None:
        break;
    }
    if (autoInstallEnabled && !autoInstallCancelledForSession)
        return {InstallKind::Automatic, false};
    return {InstallKind::UserAccepted, false};
}

// "Cancel auto-install". Only an automatic install can be cancelled; the
// installer stays on disk, but nothing launches it until the user asks.
inline InstallKind kindAfterCancelAutoInstall(InstallKind kind)
{
    return kind == InstallKind::Automatic ? InstallKind::AwaitingUser : kind;
}

// Banner "Install now" on a waiting installer, or Settings "Update now".
inline InstallKind kindAfterExplicitInstall(InstallKind kind)
{
    return kind == InstallKind::None ? InstallKind::None
                                     : InstallKind::UserAccepted;
}

// ---------------------------------------------------------------------------
// Gates
// ---------------------------------------------------------------------------

struct InstallGateInputs {
    InstallKind kind = InstallKind::None;
    bool   explicitRequest = false;      // Install now / Update now: skips the post-call grace
    bool   waitingForCallToEnd = false;  // a user-accepted launch was deferred by a call
    bool   callActive = false;           // a call or a screen share is running
    qint64 msSinceLastCall = -1;         // -1: no call this session
    bool   unsentText = false;           // the composer, or a draft in any conversation
    bool   attachmentStaged = false;     // a file waiting in the composer to be sent
    bool   voiceRecording = false;       // a voice message is being recorded
    bool   uploadInProgress = false;
    bool   mouseButtonHeld = false;      // a drag or a slow selection
};

inline bool withinPostCallGrace(const InstallGateInputs &in, qint64 graceMs)
{
    return in.msSinceLastCall >= 0 && in.msSinceLastCall < graceMs;
}

// How long is left of the post-call grace; 0 once it has run out.
inline qint64 postCallGraceRemainingMs(const InstallGateInputs &in, qint64 graceMs)
{
    return withinPostCallGrace(in, graceMs) ? graceMs - in.msSinceLastCall : 0;
}

// Whether the automatic countdown must wait. A call ending less than the
// grace ago counts: back-to-back calls, or a call dropped and redialled, must
// not find TalQ restarting in between. Everything else here is work that a
// restart would throw away.
inline bool autoInstallBlocked(const InstallGateInputs &in, qint64 graceMs)
{
    return in.callActive
        || withinPostCallGrace(in, graceMs)
        || in.unsentText
        || in.attachmentStaged
        || in.voiceRecording
        || in.uploadInProgress
        || in.mouseButtonHeld;
}

// The countdown is waiting for nothing but the post-call grace to run out.
inline bool blockedOnlyByPostCallGrace(const InstallGateInputs &in, qint64 graceMs)
{
    return withinPostCallGrace(in, graceMs)
        && !in.callActive
        && !in.unsentText
        && !in.attachmentStaged
        && !in.voiceRecording
        && !in.uploadInProgress
        && !in.mouseButtonHeld;
}

// ---------------------------------------------------------------------------
// Countdown timing
// ---------------------------------------------------------------------------

// The last stretch of the idle wait, shown as a visible "Installing in M:SS"
// countdown (and announced by a notification) so the user can still cancel.
inline constexpr qint64 kAutoInstallFinalCountdownMs = 60 * 1000;

// The idle time the countdown counts: since the last input that reached
// TalQ, but never more than since the countdown's anchor. The anchor is where
// the countdown (re)started, so a user who was already idle then still sees
// it run instead of the install firing on the first tick.
inline qint64 countdownIdleMs(qint64 nowMs, qint64 lastInputMs, qint64 readyAtMs)
{
    qint64 idleMs = qMax<qint64>(0, nowMs - lastInputMs);
    if (readyAtMs > 0)
        idleMs = qMin(idleMs, qMax<qint64>(0, nowMs - readyAtMs));
    return idleMs;
}

// Every other block resets the idle time: the user is busy, or in a call,
// which produces no input while they talk, so the full idle window must pass
// after it. The post-call grace is different -- the user may well be idle
// through it -- and resetting there too made an automatic install wait the
// grace PLUS the whole idle window after every call.
//
// So while only the grace blocks, the idle time is left alone and the anchor
// is moved instead, to no earlier than
//     graceEnd - (idleWait - finalCountdown)
// When the grace runs out, at most idleWait - finalCountdown of idle time is
// counted, so the final countdown is always seen in full before the install
// starts. Dropping the reset alone would not do that: with an idle window of
// three minutes or less, the idle time would already be past it when the
// grace ended, and the install would start in that same tick. After a call
// the install waits max(idle window, grace + final countdown). The anchor
// only ever moves later, so repeating this every tick changes nothing.
inline qint64 countdownReadyAtAfterGrace(qint64 readyAtMs, qint64 graceEndMs,
                                         qint64 idleWaitMs, qint64 finalCountdownMs)
{
    return qMax(readyAtMs, graceEndMs - (idleWaitMs - finalCountdownMs));
}

// ---------------------------------------------------------------------------
// Decisions
// ---------------------------------------------------------------------------

enum class CallStateInstallAction {
    None,                  // leave the pending installer alone
    RetryLaunch,           // a user-accepted launch was waiting for this call to end
    ResumeAutoCountdown,   // automatic install: make sure the countdown is running
};

// What a call starting, changing or ending may do with a pending installer.
// It never launches an automatic or a cancelled install.
inline CallStateInstallAction onCallStateChanged(const InstallGateInputs &in)
{
    switch (in.kind) {
    case InstallKind::None:
    case InstallKind::AwaitingUser:
        return CallStateInstallAction::None;
    case InstallKind::Automatic:
        // The countdown re-checks the call and the grace itself every tick;
        // this only restarts it if something had stopped it.
        return CallStateInstallAction::ResumeAutoCountdown;
    case InstallKind::UserAccepted:
        return (in.waitingForCallToEnd && !in.callActive)
                   ? CallStateInstallAction::RetryLaunch
                   : CallStateInstallAction::None;
    }
    return CallStateInstallAction::None;
}

enum class LaunchDecision {
    Launch,
    Hold,                   // nothing may start this installer now
    WaitForCall,            // user-accepted: retried when the call ends
    WaitForPostCallGrace,   // user-accepted: retried when the grace runs out
    ResumeAutoCountdown,    // automatic: a gate is closed, back to the countdown
};

// Last check right before the installer starts.
//
// An automatic install re-checks every countdown gate, grace included, and
// goes back to the countdown if any is closed -- it never waits on a timer of
// its own, which is how it used to launch past the idle wait. A user-accepted
// install waits only for a call and, unless the user explicitly clicked, the
// post-call grace; unsent work does not stop an install the user asked for.
// A cancelled install is held until the user asks again.
inline LaunchDecision beforeLaunch(const InstallGateInputs &in, qint64 graceMs)
{
    switch (in.kind) {
    case InstallKind::None:
    case InstallKind::AwaitingUser:
        return LaunchDecision::Hold;
    case InstallKind::Automatic:
        return autoInstallBlocked(in, graceMs) ? LaunchDecision::ResumeAutoCountdown
                                               : LaunchDecision::Launch;
    case InstallKind::UserAccepted:
        if (in.callActive)
            return LaunchDecision::WaitForCall;
        if (!in.explicitRequest && withinPostCallGrace(in, graceMs))
            return LaunchDecision::WaitForPostCallGrace;
        return LaunchDecision::Launch;
    }
    return LaunchDecision::Hold;
}

} // namespace talq
