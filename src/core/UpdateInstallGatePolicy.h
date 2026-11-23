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
#include <QStringList>
#include <QVector>
#include <QtGlobal>
#include <string>

namespace talq {

// ---------------------------------------------------------------------------
// Unsent drafts
// ---------------------------------------------------------------------------

// Whether a composer's text is something the user would lose. Spaces and line
// breaks alone are not: Send ignores them, and a stray newline left in a room
// held every automatic install for the rest of the session while the log line
// (which trimmed) named no draft at all.
inline bool isUnsentText(const QString &text)
{
    return !text.trimmed().isEmpty();
}

// Unsent text anywhere in the app. The open conversation is judged by its live
// composer only. Entering a room moves its draft out of the map and into the
// composer (takeDraftOnEnter), so the open room should never have an entry;
// the exclusion is kept so that a stray one could not block an update over a
// message the user has since sent.
inline bool hasUnsentComposerText(const QString &openComposerText,
                                  const QHash<QString, QString> &drafts,
                                  const QString &openConversationToken)
{
    if (isUnsentText(openComposerText))
        return true;
    for (auto it = drafts.cbegin(); it != drafts.cend(); ++it) {
        if (it.key() != openConversationToken && isUnsentText(it.value()))
            return true;
    }
    return false;
}

// The rooms whose draft holds the install, sorted, for the log: by the same rule
// as hasUnsentComposerText, so the line names every room that holds it. Tokens
// only -- never the text.
inline QStringList roomsWithUnsentDrafts(const QHash<QString, QString> &drafts,
                                         const QString &openConversationToken)
{
    QStringList rooms;
    for (auto it = drafts.cbegin(); it != drafts.cend(); ++it) {
        if (it.key() != openConversationToken && isUnsentText(it.value()))
            rooms.append(it.key());
    }
    rooms.sort();
    return rooms;
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

// Text that was handed to a request which then failed (a scheduled message the server refused)
// goes back where it came from WITHOUT overwriting what the user has typed since: it is added
// after it, separated by a blank line, so the caret and the new text stay where they were. An
// empty box simply takes it back. Used both for the open composer and for another room's draft.
inline QString mergeRestoredText(const QString &current, const QString &restored)
{
    if (restored.trimmed().isEmpty())
        return current;
    if (current.trimmed().isEmpty())
        return restored;
    QString head = current;
    while (!head.isEmpty() && head.back().isSpace())
        head.chop(1);
    return head + QStringLiteral("\n\n") + restored;
}

// Where text that was handed to a failed request goes back to.
enum class RestoreTarget {
    Composer,    // the room is open and its box takes text: it is typed back in
    RoomDraft,   // another room is open now: it becomes that room's draft
    Clipboard,   // the room is open but its box is taken (boxIsTaken): nothing in it is touched
};

// What the open room's box holds when refused text comes back to it. A refusal can arrive long
// after the request (a slow or dead connection), and the user goes on writing meanwhile.
struct RestoreBox {
    bool editing = false;      // an edit buffer: an existing message's text
    bool stagedFile = false;   // a file is staged, or a pasted image is being prepared: the text is its caption
    bool hasText = false;      // something is typed
    int  armedReplyToId = 0;   // the message the box replies to now; 0 = none
};

// Whether typing the refused text back into the open box would change what the user is writing
// there. Then it goes to the clipboard instead: the one place that never touches the box. The open
// room's draft is not such a place -- the box IS that room's draft, and leaving the room replaces
// or removes the entry (stashDraftOnLeave), so text parked there would be lost without a word.
//   - an edit buffer: added text would be sent as an edit of an existing message;
//   - a staged file: added text would become the file's caption;
//   - a reply armed to ANOTHER message (or to one while the refused text was a plain message): the
//     refused text would be sent as part of that reply; a reply to the same message is fine;
//   - text typed since, no reply armed, and the refused text would come back AS a reply
//     (`comesBackAsReply`, rearmReplyAfterRefusal): arming the reply again would turn what the user
//     has typed since into a reply as well.
// Plain text typed since, with a plain refused message, is not taken: mergeRestoredText adds the
// restored text after it and never overwrites it.
inline bool restoreBoxIsTaken(const RestoreBox &box, int failedReplyToId, bool comesBackAsReply)
{
    if (box.editing || box.stagedFile)
        return true;
    if (box.armedReplyToId != 0)
        return box.armedReplyToId != failedReplyToId;
    return comesBackAsReply && box.hasText;
}

inline RestoreTarget restoreTargetFor(const QString &roomToken, const QString &openRoomToken,
                                      bool boxIsTaken)
{
    if (roomToken != openRoomToken)
        return RestoreTarget::RoomDraft;
    return boxIsTaken ? RestoreTarget::Clipboard : RestoreTarget::Composer;
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
// The send outbox
// ---------------------------------------------------------------------------

// One message in the send outbox (MessageListModel, SendRetryPolicy.h): composer
// text that was sent and that the server has not confirmed. It lives in memory
// only, so a restart throws it away.
struct OutboxMessage {
    QString roomToken;
    qint64  sentAtMs = 0;       // client clock
    bool    givenUp = false;    // marked permanent: a refusal that is never retried on its own
                                // (401/403/404/412/413), the attempt budget spent, or a history
                                // window that could not prove the send. Only Retry continues it.
    bool    inFlight = false;   // an attempt is waiting for its answer
    int     lastStatus = 0;     // HTTP status of the last failed attempt; 0 = none, or no answer
    bool    roomListed = true;  // its room is still in the conversation list. The model cannot
                                // tell; the caller fills it in, and true is the side that holds.
};

// Whether it holds the automatic install.
//
// One still being retried does: it may yet be delivered, and if its room is
// really gone it gives up on its own. One given up does too. Its failed bubble
// still offers Retry and Discard and its text is nowhere else, yet it used not
// to be counted, so an idle install dropped it without a word.
//
// Except a given-up message whose room has left the conversation list (deleted,
// left, removed on another device): the user cannot open the room to see the
// bubble any more, so it could only hold every install until logout -- the
// reason dropDraftsForUnlistedRooms drops such a room's draft. The list is
// replaced only by a successful refresh, so a room missing from it is really
// gone. The entry itself is left alone, and holds again if the room comes back.
inline bool outboxMessageHoldsInstall(const OutboxMessage &m)
{
    return !m.givenUp || m.roomListed;
}

inline bool outboxHoldsInstall(const QVector<OutboxMessage> &outbox)
{
    for (const OutboxMessage &m : outbox)
        if (outboxMessageHoldsInstall(m))
            return true;
    return false;
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
    bool   unsentText = false;           // the composer, a draft in any conversation, or a sent
                                         // message the server has not confirmed yet (in memory
                                         // only), a given-up one included (outboxHoldsInstall)
    bool   attachmentStaged = false;     // a file waiting in the composer to be sent
    bool   voiceRecording = false;       // a voice message is being recorded
    bool   uploadInProgress = false;     // a file uploading or queued behind it. Uploads run one at
                                         // a time (MessageListModel's file queue), and one that
                                         // makes no progress is given up after its stall watchdog
    bool   mouseButtonHeld = false;      // a drag or a slow selection (see mouseButtonHeldNow)
};

// Whether a mouse button is really down, for the "a drag or a slow selection" gate.
// Qt's own view (QApplication::mouseButtons) is updated only from events delivered to
// TalQ's windows, so a press inside TalQ whose release lands elsewhere (a drag that left
// the window, a native menu or dialog) leaves it set until the user next touches TalQ:
// 2026-10-15 an install sat held for 35 minutes with "qt buttons=1, windows buttons=0".
//
// So the OS may CLEAR Qt's hold -- a button that is not physically down is no drag -- but it
// must never ADD one. Windows' own state is desktop-wide: a click in a browser or an IDE would
// hold the install and restart the idle countdown, and only input that reaches TalQ may do
// that (the 0.40.16 rule). osButtons is the mask of buttons Windows reports down, or -1 where
// it cannot be asked (not Windows); -1 is not zero, so Qt's view then stands as it is.
inline bool mouseButtonHeldNow(bool qtSaysHeld, int osButtons)
{
    return qtSaysHeld && osButtons != 0;
}

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

// Which gates are closed right now, for the log only: "" when none is, otherwise
// a comma-separated list in a fixed order. It must stay in step with
// autoInstallBlocked() -- the test checks that exhaustively -- because this line is
// the only way to tell, after the fact, why an automatic install did not happen
// (2026-10-08: an install sat held ~2 h 45 min and nothing recorded by what).
inline std::string closedGateNames(const InstallGateInputs &in, qint64 graceMs)
{
    std::string s;
    auto add = [&s](const char *name) { if (!s.empty()) s += ','; s += name; };
    if (in.callActive)                   add("call");
    if (withinPostCallGrace(in, graceMs)) add("post-call-grace");
    if (in.unsentText)                   add("unsent-text");
    if (in.attachmentStaged)             add("attachment-staged");
    if (in.voiceRecording)               add("voice-recording");
    if (in.uploadInProgress)             add("upload");
    if (in.mouseButtonHeld)              add("mouse-button-held");
    return s;
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
// What the hold line names
// ---------------------------------------------------------------------------
// "Update: auto-install held by ..." also says, inside each gate, WHAT holds it:
// the room, the file, how long. Never a message's text and never a caption.

// "45 s", "12 min", "3 h 5 min". A clock that stepped back reads as 0 s.
inline QString holdAgeText(qint64 ms)
{
    const qint64 s = qMax<qint64>(0, ms) / 1000;
    if (s < 60)
        return QStringLiteral("%1 s").arg(s);
    const qint64 min = s / 60;
    if (min < 60)
        return QStringLiteral("%1 min").arg(min);
    return QStringLiteral("%1 h %2 min").arg(min / 60).arg(min % 60);
}

// At most `shown` items, then how many more: a long list must not swamp the line.
inline QString namedList(const QStringList &items, const QString &separator, int shown = 5)
{
    if (items.size() <= shown)
        return items.join(separator);
    return items.mid(0, shown).join(separator)
         + QStringLiteral(" and %1 more").arg(items.size() - shown);
}

// The outbox messages that hold the install (outboxMessageHoldsInstall), each with
// its room, where it stands, the last refusal and how long ago it was sent.
inline QString describeOutboxHold(const QVector<OutboxMessage> &outbox, qint64 nowMs)
{
    QStringList parts;
    for (const OutboxMessage &m : outbox) {
        if (!outboxMessageHoldsInstall(m))
            continue;
        QString state = m.givenUp ? QStringLiteral("given up, waiting for Retry or Discard")
                      : m.inFlight ? QStringLiteral("sending")
                                   : QStringLiteral("waiting to be sent again");
        if (m.lastStatus > 0)
            state += QStringLiteral(", last HTTP %1").arg(m.lastStatus);
        parts.append(QStringLiteral("room %1 (%2), sent %3 ago")
                         .arg(m.roomToken, state, holdAgeText(nowMs - m.sentAtMs)));
    }
    if (parts.isEmpty())
        return QString();
    return QStringLiteral("%1 unsent message(s) in the send outbox: %2")
        .arg(QString::number(parts.size()), namedList(parts, QStringLiteral("; "), 3));
}

// What is staged in the composer. A pasted image shows its bar while it is still
// being encoded, before it has a file, so it must not read as a file.
inline QString describeStagedAttachment(const QString &firstPath, int moreFiles, bool barUp)
{
    if (firstPath.isEmpty())
        return barUp ? QStringLiteral("a pasted image still being prepared") : QString();
    const int cut = qMax(firstPath.lastIndexOf(QLatin1Char('/')),
                         firstPath.lastIndexOf(QLatin1Char('\\')));
    QString s = QStringLiteral("file %1").arg(firstPath.mid(cut + 1));
    if (moreFiles > 0)
        s += QStringLiteral(" and %1 more").arg(moreFiles);
    return s;
}

// The file uploading now, how far it is, and what waits behind it.
inline QString describeUploadHold(const QString &fileName, double progress,
                                  int batchIndex, int batchTotal, int queued)
{
    QStringList parts;
    if (!fileName.isEmpty()) {
        // One arg() call: a chained one would also replace a "%2" inside the file's name.
        QString s = QStringLiteral("uploading %1 (%2%)")
                        .arg(fileName, QString::number(int(qBound(0.0, progress, 1.0) * 100)));
        if (batchTotal > 1)
            s += QStringLiteral(", file %1 of %2").arg(batchIndex).arg(batchTotal);
        parts.append(s);
    }
    if (queued > 0)
        parts.append(QStringLiteral("%1 more file(s) queued").arg(queued));
    return parts.join(QStringLiteral("; "));
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
