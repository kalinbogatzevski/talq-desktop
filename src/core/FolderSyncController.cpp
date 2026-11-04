#include "FolderSyncController.h"

#include "core/ApiClient.h"
#include "core/DavSyncPolicy.h"
#include "core/FolderStore.h"
#include "core/FolderSyncMeta.h"
#include "core/FolderSyncNotices.h"
#include "core/TalqLog.h"

#include <QDateTime>
#include <QMap>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTimer>
#include <QVariant>

#include <cmath>
#include <limits>
#include <utility>

using talq::BaseChoice;
using talq::BaseInputs;
using talq::DavOutcome;
using talq::FolderState;
using talq::FolderSyncPause;
using talq::FolderSyncState;

namespace {

// The shared file, at the root of the user's DAV home (D6).
const QString kRemoteName = QStringLiteral(".talq-folders.json");

// The capability probe's scratch file: the shared file's dot-name pattern (so a name the server
// forbids fails here too) and 8 random lowercase hex digits, fresh for every attempt, so two
// computers proving the server at the same moment never meet on one file.
const QString kProbePrefix = QStringLiteral(".talq-folders.probe-");
// Its body: small, but not empty, so every PUT really sends one.
const QByteArray kProbeBody = QByteArrayLiteral("{}");
// Step 4's If-Match: an ETag no server has handed out.
const QByteArray kProbeBogusEtag = QByteArrayLiteral("\"talq-probe-bogus\"");

// What a base that a cycle of THIS session produced hands chooseBase for rules 2 and 3 (see
// baseInputsNow). Only ever compared with itself; never written anywhere.
const QByteArray kSessionFingerprint = QByteArrayLiteral("merged-in-this-session");

// Any failure count past the end of the ladder is its slowest step (15 min): what Paused(QuotaFull)
// waits before it tries again ("States"). Not a failure count of its own: a QuotaFull pause is no
// transient failure and never touches m_failures.
constexpr int kPastTheLadder = 1000;

// A QTimer takes an int: every interval is clamped into [0, INT_MAX] ms first.
int timerMs(qint64 ms)
{
    constexpr qint64 kZero = 0;
    constexpr qint64 kMax = std::numeric_limits<int>::max();
    return static_cast<int>(qBound(kZero, ms, kMax));
}

// Spec "States": while Paused, the one automatic trigger that starts a cycle is the pause's own: a
// local edit for TooLarge (the merge may be back under the caps), the slow backoff step for
// QuotaFull. No other trigger wakes any pause (Sync now, the switch and a logout are not triggers).
bool wakesPause(FolderSyncController::Trigger trigger, FolderSyncPause pause)
{
    return (trigger == FolderSyncController::Trigger::Edit && pause == FolderSyncPause::TooLarge)
           || (trigger == FolderSyncController::Trigger::SlowStep && pause == FolderSyncPause::QuotaFull);
}

const char *pauseName(FolderSyncPause p)
{
    switch (p) {
    case FolderSyncPause::None: return "none";
    case FolderSyncPause::AuthRefused: return "AuthRefused";
    case FolderSyncPause::RemoteNewer: return "RemoteNewer";
    case FolderSyncPause::RemoteUnreadable: return "RemoteUnreadable";
    case FolderSyncPause::TooLarge: return "TooLarge";
    case FolderSyncPause::ServerSizeLimit: return "ServerSizeLimit";
    case FolderSyncPause::ServerUnsupported: return "ServerUnsupported";
    case FolderSyncPause::QuotaFull: return "QuotaFull";
    case FolderSyncPause::StoreUnavailable: return "StoreUnavailable";
    case FolderSyncPause::RemoteShrank: return "RemoteShrank";
    case FolderSyncPause::Fatal: return "Fatal";
    }
    return "?";
}

// The status of a finished reply as classifyDav reads it. A 2xx whose transfer did not complete
// (the connection dropped mid-body, a timeout after the headers) is NOT a success: the body is cut
// short, and an empty or truncated body would read as "nobody wrote yet" and be overwritten. It is
// transient. `aborted` is always false here: this controller's own cancels (detach, disable, a new
// attach) bump m_gen first, so their callbacks never get this far, and the size abort (D39) is
// decided before this is consulted.
DavOutcome outcomeOf(const QNetworkReply *reply, int *statusOut)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusOut)
        *statusOut = status;
    DavOutcome o = talq::classifyDav(status, false);
    if ((o == DavOutcome::Ok || o == DavOutcome::Created) && reply->error() != QNetworkReply::NoError)
        o = DavOutcome::Transient;
    return o;
}

} // namespace

FolderSyncController::FolderSyncController(FolderStore *store, ApiClient *api, QObject *parent)
    : QObject(parent), m_store(store), m_api(api)
{
    // One connection each for the controller's life; the slots themselves check that the store is
    // attached to OUR scope (saved() can fire for a scope being left, and from ~FolderStore). The
    // debounce listens to localEdited(), never changed(): a remote apply emits only the latter, so it
    // can never schedule an upload of what it just took in.
    if (store) {
        connect(store, &FolderStore::saved, this, &FolderSyncController::onStoreSaved);
        connect(store, &FolderStore::localEdited, this, &FolderSyncController::onLocalEdited);
    }
    // The triggers' timers. A timer is stopped (stopTimers) by everything that ends the switch or the
    // session, so none fires for a session that is gone; startFromTrigger checks the store's scope
    // all the same.
    m_loginTimer.setSingleShot(true);
    m_editTimer.setSingleShot(true);
    m_backoffTimer.setSingleShot(true);
    m_slowStepTimer.setSingleShot(true);
    connect(&m_loginTimer, &QTimer::timeout, this, [this] { startFromTrigger(Trigger::Login); });
    connect(&m_pollTimer, &QTimer::timeout, this, [this] { startFromTrigger(Trigger::Poll); });
    connect(&m_editTimer, &QTimer::timeout, this, [this] { startFromTrigger(Trigger::Edit); });
    connect(&m_backoffTimer, &QTimer::timeout, this, [this] { startFromTrigger(Trigger::Backoff); });
    connect(&m_slowStepTimer, &QTimer::timeout, this, [this] { startFromTrigger(Trigger::SlowStep); });
}

FolderSyncController::~FolderSyncController()
{
    ++m_gen;   // a callback still in flight answers to nobody
    abortReply();
}

void FolderSyncController::setTiming(const talq::FolderSyncTiming &t)
{
    m_timing = t;
}

void FolderSyncController::setApplyGate(std::function<bool()> gate)
{
    m_applyGate = std::move(gate);
}

FolderSyncController::ArmedTimers FolderSyncController::armedTimers() const
{
    const auto armed = [](const QTimer &t) { return t.isActive() ? t.interval() : -1; };
    ArmedTimers a;
    a.login = armed(m_loginTimer);
    a.poll = armed(m_pollTimer);
    a.edit = armed(m_editTimer);
    a.backoff = armed(m_backoffTimer);
    a.slowStep = armed(m_slowStepTimer);
    return a;
}

// Everything that could start a cycle later. The edit clock and the in-flight edit go too: an edit
// made while off is not "unsent" (the switch's own cycle takes it), nor one from another session.
void FolderSyncController::stopTimers()
{
    m_loginTimer.stop();
    m_pollTimer.stop();
    m_editTimer.stop();
    m_backoffTimer.stop();
    m_slowStepTimer.stop();
    m_backoff = false;
    m_editClock.invalidate();
    m_editDuringFlight = false;
}

// ---------------------------------------------------------------------------------------------
// attach / detach

void FolderSyncController::abortReply()
{
    if (QNetworkReply *r = m_reply) {
        m_reply = nullptr;   // FIRST: abort() can emit finished() synchronously
        r->abort();
    }
}

// Everything an attach knows is forgotten, and whatever is in flight is dropped: m_gen moves first,
// so the abort's own finished() (and a gate poll still queued) is ignored.
void FolderSyncController::resetSession()
{
    ++m_gen;
    abortReply();
    stopTimers();
    m_lastAttempt.invalidate();   // a new session has made no attempt yet
    m_getTooLarge = false;
    m_getRedirected = false;
    m_inFlight = false;
    m_cycle = Cycle();
    m_probe = Probe();
    m_attached = false;
    m_enabled = false;
    m_scope.clear();
    m_attachInputs = BaseInputs();
    m_attachGeneration = 0;
    m_sessionTrusted = false;
    m_base = FolderState();
    m_savedSha.clear();
    m_sidecarOwed = false;
    m_togglePending = false;
    m_firstAfterEnable = false;
    m_lastSyncedMs = 0;
    m_failures = 0;
    m_failureSpanMs = 0;
    m_failureClock.invalidate();
    m_shrinkDropped = 0;
    m_shrinkOf = 0;
    m_repairRewriteDue = false;
    m_412.reset();   // R8: a detach or an account switch starts the count again
}

void FolderSyncController::attach(bool enabledAtAttach)
{
    resetSession();
    if (!m_store || !m_store->isAttached() || !talq::isScopeKey(m_store->scopeKey())) {
        TWARN("FolderSync: attach() without an attached folder store; sync stays off");
        setState(FolderSyncState::Off);
        return;
    }
    m_attached = true;
    m_enabled = enabledAtAttach;
    m_scope = m_store->scopeKey();

    // Rules 1 to 3 of chooseBase hold for the whole attach: the sidecar, how the file loaded and the
    // hash of what was loaded. Rule 4 is judged at every cycle (baseInputsNow).
    const talq::foldersyncmeta::SidecarRead rd = talq::foldersyncmeta::readSidecar(m_store->rootDir(), m_scope);
    m_attachInputs.sidecarStatus = rd.status;
    m_attachInputs.sidecar = rd.sidecar;   // EMPTY unless Ok
    m_attachInputs.outcome = m_store->loadOutcome();
    m_attachInputs.loadedSha = m_store->loadedSha();
    m_attachGeneration = m_store->generation();
    // The base chooseBase hands out now: the sidecar's when it is trusted, else the EMPTY state, so
    // nothing that reads m_base (the hook, and later the "push pending" test) can act on a base the
    // rules distrust.
    m_base = baseChoiceNow().base;
    m_savedSha = m_store->loadedSha();   // until the store's saved() says otherwise (empty: no file, D31)
    m_repairRewriteDue = m_attachInputs.outcome == talq::FolderLoadOutcome::Repaired
                         || m_attachInputs.outcome == talq::FolderLoadOutcome::SetAside;   // D30

    if (!m_enabled) {
        setState(FolderSyncState::Off);
    } else if (m_store->isReadOnly() || m_store->loadOutcome() == talq::FolderLoadOutcome::ReadOnlyLoad) {
        setState(FolderSyncState::Paused, FolderSyncPause::StoreUnavailable);   // no timers: nothing can run
        emit problemRaised(FolderSyncPause::StoreUnavailable);   // status line only (D40)
    } else {
        // D18: the session's first pull loginDelayMs from now (never a push first: every cycle GETs
        // before it PUTs), and the poll from here on. Armed before the state is announced, so a slot
        // that detaches right there stops them.
        m_loginTimer.start(timerMs(m_timing.loginDelayMs));
        m_pollTimer.start(timerMs(m_timing.pollMs));
        setState(FolderSyncState::Idle);
    }
}

void FolderSyncController::detach()
{
    resetSession();   // no network call, no sidecar write
    setState(FolderSyncState::Off);
}

void FolderSyncController::syncNow()
{
    startCycle(CycleOrigin::Button);
}

void FolderSyncController::startAutomaticCycle()
{
    startFromTrigger(Trigger::Poll);   // a plain automatic trigger: no exception of any kind
}

void FolderSyncController::onWindowActivated()
{
    startFromTrigger(Trigger::Activation);
}

void FolderSyncController::onWindowDeactivated()
{
    startFromTrigger(Trigger::Deactivation);
}

void FolderSyncController::onReachabilityRestored()
{
    startFromTrigger(Trigger::Reachability);
}

// ---------------------------------------------------------------------------------------------
// the triggers (D18, "Backoff", "States")

// "The last attempt" (D34): one monotonic clock, started whenever a cycle or a probe starts; never
// started counts as infinitely old.
bool FolderSyncController::lastAttemptOlderThan(qint64 ms) const
{
    return !m_lastAttempt.isValid() || m_lastAttempt.elapsed() > ms;
}

// "Pending is derived, not stored" (D18): the store's state is not the base, or the base is not
// trusted now (then a cycle is always due). m_base is the attach-time choice until a cycle of this
// session moves it, so a restart with a local file that differs from a kept, trusted base reads as
// pending at once. The caller has checked storeIsOurs().
bool FolderSyncController::pushPending() const
{
    return !baseChoiceNow().trusted || !talq::sameFolderState(m_store->state(), m_base);
}

// The ONE place an automatic trigger is judged: the backoff and the gaps here; the in-flight guard
// and the pause rule in startCycle, which the button and the switch share. A trigger wakes no pause
// but its own (an Edit ends TooLarge, the SlowStep retries QuotaFull), never starts a second chain,
// and never goes through syncNow().
void FolderSyncController::startFromTrigger(Trigger trigger)
{
    ++m_triggerCounts[static_cast<int>(trigger)];
    // Load-bearing: pushPending() below reads the store, and the store can be gone (destroyed before
    // this controller at shutdown) or attached elsewhere while MainWindow still calls the window hooks.
    if (!storeIsOurs())
        return;   // detached, the store gone, or the store left this account: there is nothing to judge
    if (m_state != FolderSyncState::Paused) {   // a pause is judged by startCycle's rule alone
        if (m_backoff) {
            // "Backoff": ignored, except the reachability edge, an activation more than
            // backoffOverrideGapMs after the last attempt, and the ladder's own timer. The rest is
            // carried by that timer (a pending change is a base diff, it waits with it).
            switch (trigger) {
            case Trigger::Backoff:
            case Trigger::Reachability:
                break;
            case Trigger::Activation:
                if (!lastAttemptOlderThan(m_timing.backoffOverrideGapMs))
                    return;
                break;
            case Trigger::Login:
            case Trigger::Deactivation:
            case Trigger::Edit:
            case Trigger::Poll:
            case Trigger::SlowStep:
                return;
            }
        } else {
            switch (trigger) {
            case Trigger::Activation:   // a pull, the user is looking: at most every activationGapMs
                if (!lastAttemptOlderThan(m_timing.activationGapMs))
                    return;
                break;
            case Trigger::Deactivation:   // a push, only when one is pending (the cheap gap first)
                if (!lastAttemptOlderThan(m_timing.pushGapMs) || !pushPending())
                    return;
                break;
            case Trigger::Reachability:   // a retry (Waiting: the account not ready) or a push pending
                if (m_state != FolderSyncState::Waiting && !pushPending())
                    return;
                break;
            case Trigger::Backoff:    // their state is gone (a cycle started since): nothing to retry
            case Trigger::SlowStep:
                return;
            case Trigger::Login:
            case Trigger::Edit:
            case Trigger::Poll:
                break;
            }
        }
    }
    startCycle(CycleOrigin::Automatic, trigger);
}

// The local-edit debounce (D18): restarted by every edit, never later than editMaxWaitMs after the
// first edit no cycle has taken yet. While off or not attached an edit arms nothing (the switch's own
// cycle takes it). While a cycle is in flight it is noted, and the debounce starts when that cycle
// ends: its snapshot may predate the edit, and an edit is never lost.
void FolderSyncController::onLocalEdited()
{
    if (!m_attached || !m_enabled || !storeIsOurs())
        return;
    if (!m_editClock.isValid())
        m_editClock.start();
    if (m_inFlight) {
        m_editDuringFlight = true;
        return;
    }
    armEditDebounce();
}

void FolderSyncController::armEditDebounce()
{
    const qint64 sinceFirst = m_editClock.isValid() ? m_editClock.elapsed() : 0;
    m_editTimer.start(timerMs(qMin(m_timing.editDebounceMs, m_timing.editMaxWaitMs - sinceFirst)));
}

// A wait on the ladder (nextBackoffMs, plus or minus 20%), times the timing's backoffScale (a test
// seam; 1.0 in production), floored at 1 ms.
qint64 FolderSyncController::ladderWaitMs(int failures) const
{
    const double ms = static_cast<double>(talq::nextBackoffMs(failures, QRandomGenerator::global()->generateDouble()))
                      * m_timing.backoffScale;
    if (!(ms >= 1.0))   // also NaN
        return 1;
    if (ms >= static_cast<double>(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    return static_cast<qint64>(std::llround(ms));
}

// The cycle (or the probe) is no longer in flight. An edit made meanwhile starts its debounce now.
void FolderSyncController::endFlight()
{
    m_inFlight = false;
    if (m_editDuringFlight) {
        m_editDuringFlight = false;
        armEditDebounce();
    }
}

// The toggle path (D28). Never attach() again from here: a second attach would read back the sidecar
// the saved() hook kept current while the switch was off and compare it with the hash the store
// loaded at ITS attach, so every re-enable within a session would distrust its base and bring back a
// folder deleted while off (D21 wants a trusted three-way merge there). The attach-time inputs and
// m_base stay as they are; chooseBase decides trust exactly as at attach.
void FolderSyncController::enable()
{
    if (m_attached && m_enabled)
        return;   // already on: no second probe, no second answer
    if (!storeIsOurs() || !m_api || m_store->isReadOnly()
        || m_store->loadOutcome() == talq::FolderLoadOutcome::ReadOnlyLoad) {
        // The Settings row is disabled in this case; this is the defensive answer.
        TWARN("FolderSync: the switch cannot be turned on: this account's folders are not available here");
        emit enableFinished(false, talq::enableRefusalText(talq::EnableRefusal::CouldNotReach));
        return;
    }
    // A new attempt: an answer still owed to an earlier one (a refusal whose stateChanged slot turned
    // the switch on again, right here) sees m_gen move and is never sent after this attempt's own.
    ++m_gen;
    m_enabled = true;
    m_togglePending = true;
    m_firstAfterEnable = true;
    m_412.reset();   // R8, carry 2: the toggle starts the self-inconsistent-412 count again
    // D18: the poll runs whenever the switch is on. No login timer: the toggle's own cycle is the
    // first pull. A refused toggle (turnOff) stops it again.
    m_pollTimer.start(timerMs(m_timing.pollMs));
    startCycle(CycleOrigin::SwitchOn);
}

void FolderSyncController::disable()
{
    turnOff();
    setState(FolderSyncState::Off);
}

// Everything that is the switch's: the session (attach, scope, base, sidecar bookkeeping) stays, so
// the saved() hook keeps working while off and a re-enable is a trusted merge.
void FolderSyncController::turnOff()
{
    ++m_gen;   // a late answer, a conflict wait, the replace gap or a held apply answers to nobody
    abortReply();
    stopTimers();   // a disabled computer never contacts the server
    m_getTooLarge = false;
    m_getRedirected = false;
    m_inFlight = false;
    m_cycle = Cycle();
    m_probe = Probe();   // no DELETE: a disabled computer never contacts the server
    m_enabled = false;
    m_togglePending = false;
    m_firstAfterEnable = false;
    m_412.reset();   // R8, carry 2: an exit from Paused(ServerUnsupported) that is not Sync now
    m_failures = 0;
    m_failureSpanMs = 0;
    m_failureClock.invalidate();
    m_shrinkDropped = 0;
    m_shrinkOf = 0;
}

// D28: the switch is reported on BEFORE the first cycle, synchronously, from this chain (a queued
// answer could arrive after a disable() and make MainWindow write enabled=true again). A slot may
// turn the switch off (or log out) right here: then nothing more happens for this attempt.
bool FolderSyncController::reportEnabled()
{
    m_togglePending = false;
    const quint64 gen = m_gen;
    emit enableFinished(true, QString());
    if (gen != m_gen)
        return false;   // disabled, detached or attached again from a slot: that attempt is over
    if (!storeIsOurs() || !m_api) {
        endQuietly("the folder store left this account as the switch was turned on");
        return false;
    }
    return true;
}

// The toggle path's failure (D28, D35): the switch goes back off with a reason. The reason beside the
// switch is the whole answer: no problemRaised, no attemptFailed.
void FolderSyncController::refuseEnable(talq::EnableRefusal why)
{
    TWARN("FolderSync: the switch was not turned on:"
          << (why == talq::EnableRefusal::ServerUnsupported ? "the server does not honour conditional saves"
              : why == talq::EnableRefusal::SignInRefused   ? "the saved sign-in was refused"
                                                            : "the server could not be reached"));
    turnOff();
    // A stateChanged slot may turn the switch on again (or log out) right here. That new attempt
    // answers for itself; a false sent after it would make MainWindow put the switch back off while
    // the controller is on.
    const quint64 gen = m_gen;
    setState(FolderSyncState::Off);
    if (gen != m_gen)
        return;
    emit enableFinished(false, talq::enableRefusalText(why));
}

// ---------------------------------------------------------------------------------------------
// the base

bool FolderSyncController::storeIsOurs() const
{
    return m_attached && m_store && m_store->isAttached() && m_store->scopeKey() == m_scope;
}

// Every BaseInputs field, filled now (a default left in place would skip rule 4).
//  - Until a cycle of this attach has succeeded: the sidecar, load outcome and loaded hash read at
//    attach (rules 1 to 3).
//  - After one has: the base is the merged state that cycle left, and both memory and (once
//    written) the file descend from it, so rules 1 to 3 hold by construction. The inputs say so
//    with a fingerprint that matches itself, and the base handed over is m_base.
//  - Either way rule 4 judges the store as it is NOW: empty, the edit count, and whether its
//    generation is still the one seen at attach.
BaseInputs FolderSyncController::baseInputsNow() const
{
    BaseInputs in;
    if (m_sessionTrusted) {
        in.sidecarStatus = talq::SidecarStatus::Ok;
        in.sidecar.base = m_base;
        in.sidecar.localSha = kSessionFingerprint;
        in.outcome = talq::FolderLoadOutcome::Ok;
        in.loadedSha = kSessionFingerprint;
    } else {
        in = m_attachInputs;
    }
    in.localEmpty = talq::isEmptyFolderState(m_store->state());
    in.localEditCount = m_store->localEditCount();
    in.stateUnchangedSinceAttach = m_store->generation() == m_attachGeneration;
    return in;
}

BaseChoice FolderSyncController::baseChoiceNow() const
{
    return talq::chooseBase(baseInputsNow());
}

// ---------------------------------------------------------------------------------------------
// one cycle

// Step 1, the guards; then the probe when the marker is absent, else the cycle itself.
// The in-flight guard here is the one thing that keeps a second chain from starting, for the button,
// the switch and every trigger alike (a timer that fires mid-cycle is skipped right here).
void FolderSyncController::startCycle(CycleOrigin origin, Trigger trigger)
{
    if (!m_attached || !m_enabled || m_inFlight)
        return;
    if (!storeIsOurs() || !m_api)
        return;
    if (m_store->isReadOnly() || m_store->loadOutcome() == talq::FolderLoadOutcome::ReadOnlyLoad)
        return;
    if (m_state == FolderSyncState::Paused) {
        switch (origin) {
        case CycleOrigin::Automatic:
            // Spec "States": only the pause's own rule (a local edit ends TooLarge, the slow step
            // retries QuotaFull). Poll, activation, deactivation, the reachability edge, the login
            // timer and the backoff never wake a pause.
            if (!wakesPause(trigger, m_pause))
                return;
            break;
        case CycleOrigin::Button:
            // AuthRefused clears only on a logout/login or a switch toggle.
            if (m_pause == FolderSyncPause::AuthRefused)
                return;
            // R8: "Sync now" after a ServerUnsupported pause counts self-inconsistent 412s from zero
            // again (the pause deleted the probe marker, so the server is probed again first).
            if (m_pause == FolderSyncPause::ServerUnsupported)
                m_412.reset();
            break;
        case CycleOrigin::SwitchOn:
            break;   // the toggle clears every pause, AuthRefused included (enable() reset the count)
        }
    }

    // R18: "Sync now" accepts a remote shrink only when the RemoteShrank pause has already told the
    // user "N of M". Pressed from any other state (Idle, Waiting, another pause: say after an older
    // version of the file was restored in Files) the cycle brakes like any other and raises the
    // pause first; the next press accepts. The switch toggle never accepts: it brakes again. m_state
    // and m_pause still hold their values from before the press here: nothing above changes them on
    // a path that gets this far. Judged NOW, at the press, and carried through a probe (which changes
    // the state) to the cycle after it.
    const bool acceptShownShrink = origin == CycleOrigin::Button && m_state == FolderSyncState::Paused
                                   && m_pause == FolderSyncPause::RemoteShrank;
    const bool markerPresent = talq::foldersyncmeta::probeMarkerPresent(m_store->rootDir(), m_scope);
    // This attempt answers whatever was waiting to start one: a backoff (the ladder resumes from
    // m_failures if it fails again), Paused(QuotaFull)'s slow step (any exit from that pause cancels
    // it; the cycle re-arms it if it pauses QuotaFull again), and the login pull (this is the
    // session's first cycle, or a later one).
    m_backoff = false;
    m_backoffTimer.stop();
    m_slowStepTimer.stop();
    m_loginTimer.stop();
    m_cycle = Cycle();   // nothing of an earlier cycle reaches this one, nor the probe before it
    m_inFlight = true;   // one chain from here: a slot that presses Sync now meanwhile starts nothing

    // D28: on the toggle path with the marker present, enabled=true is written at once (before the
    // first cycle, before the account check).
    if (m_togglePending && markerPresent) {
        const quint64 gen = m_gen;
        setState(FolderSyncState::SettingUp);
        // A stateChanged slot may turn the switch off (or log out) right here: then that attempt is
        // over before it was answered, and true is never sent for it.
        if (gen != m_gen || !reportEnabled())
            return;
    }
    // D38: davUser() would fall back to the login name and address somebody else's home. No
    // request, no retry loop: the next trigger looks again. Status line only. On the toggle path
    // with no marker the probe cannot run without the uid: the switch is refused (could not reach).
    // Not an attempt: the last-attempt clock does not move and no timer is armed (D38: the next
    // trigger, of any ordinary kind, looks again).
    if (!m_api->davUserKnown()) {
        endFlight();
        if (m_togglePending) {
            refuseEnable(talq::EnableRefusal::CouldNotReach);
            return;
        }
        setState(FolderSyncState::Waiting, FolderSyncPause::None, /*accountNotReady*/ true);
        return;
    }
    m_lastAttempt.start();   // D34: a probe-only attempt counts, and so does one that fails
    // D17: no cycle ever touches the real file on a server whose capability probe has not passed.
    if (!markerPresent) {
        startProbe(origin, acceptShownShrink);
        return;
    }
    beginCycle(origin, acceptShownShrink);
}

// Step 2, the snapshot, and step 3: the cycle proper, once the guards held and the server has passed
// the probe (now or on an earlier day).
void FolderSyncController::beginCycle(CycleOrigin origin, bool acceptShownShrink)
{
    const QString path = m_api->davFilePath(kRemoteName);
    if (path.isEmpty()) {
        // Cannot happen with the uid known; never build a path from the login name.
        endQuietly("the account's file path is not known; nothing is sent");
        return;
    }
    m_cycle = Cycle();
    m_cycle.path = path;
    m_cycle.acceptShownShrink = acceptShownShrink;   // for the first GET's merge only (4b)
    // The toggle's own cycle: "SettingUp" until it ends, and a 401/403 in it is the user's action
    // failing, so the switch goes back off (D28, D35) instead of a pause.
    const bool firstAfterToggle = origin == CycleOrigin::SwitchOn;
    m_cycle.revertOnAuthRefused = firstAfterToggle;
    m_inFlight = true;
    const quint64 gen = m_gen;
    setState(firstAfterToggle ? FolderSyncState::SettingUp : FolderSyncState::Syncing);
    if (gen != m_gen)
        return;   // a stateChanged slot turned the switch off or logged out: the cycle sends nothing
    takeSnapshot();
    sendGet();
}

// ---------------------------------------------------------------------------------------------
// the capability probe (spec "The capability probe", D17; the steps are in the header comment)

void FolderSyncController::startProbe(CycleOrigin origin, bool acceptShownShrink)
{
    const quint32 suffix = QRandomGenerator::global()->generate();
    const QString path = m_api->davFilePath(kProbePrefix + QStringLiteral("%1").arg(suffix, 8, 16, QLatin1Char('0')));
    if (path.isEmpty()) {
        // Cannot happen with the uid known (checked just before); never build a path from the login.
        probeEndedQuietly("the account's file path is not known; nothing is sent");
        return;
    }
    m_probe = Probe();
    m_probe.path = path;
    m_probe.origin = origin;
    m_probe.acceptShownShrink = acceptShownShrink;
    m_inFlight = true;   // one chain at a time: Sync now or a trigger meanwhile starts nothing
    const quint64 gen = m_gen;
    setState(FolderSyncState::SettingUp);
    if (gen != m_gen)
        return;   // a stateChanged slot turned the switch off or logged out: the probe sends nothing
    TLOG("FolderSync: this computer has not proved the server yet; the capability probe runs before any sync");
    sendProbeStep(1);
}

void FolderSyncController::sendProbeStep(int step)
{
    const int putMs = static_cast<int>(m_timing.putTimeoutMs);
    QMap<QByteArray, QByteArray> headers;
    headers.insert("Content-Type", "application/json");
    QNetworkReply *reply = nullptr;
    // Every request opts out of following a redirect (R21): a followed one would send this request,
    // the Basic credentials included, somewhere else, and its answer could "pass" a server that is not
    // the one asked.
    switch (step) {
    case 1:   // create the absent scratch file: the header the cycle creates the shared file with
    case 3:   // the same again, now that it exists: must be refused
        headers.insert("If-None-Match", "*");
        reply = m_api->davRequest("PUT", m_probe.path, kProbeBody, headers, putMs, /*followRedirects*/ false);
        break;
    case 2:   // the ETag comes from a GET, as the cycle's If-Match always does
        reply = m_api->getAbsoluteUrl(m_probe.path, static_cast<int>(m_timing.getTimeoutMs), /*followRedirects*/ false);
        break;
    case 4:   // an ETag nobody handed out: must be refused
        headers.insert("If-Match", kProbeBogusEtag);
        reply = m_api->davRequest("PUT", m_probe.path, kProbeBody, headers, putMs, /*followRedirects*/ false);
        break;
    default:  // 5: the ETag the GET handed out, byte for byte: must be accepted
        headers.insert("If-Match", m_probe.e1);
        reply = m_api->davRequest("PUT", m_probe.path, kProbeBody, headers, putMs, /*followRedirects*/ false);
        break;
    }
    ++m_requestsSent;
    ApiClient::bindReplyLifetime(reply, this);
    m_reply = reply;
    const quint64 gen = m_gen;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen, step] { onProbeReply(reply, gen, step); });
}

void FolderSyncController::onProbeReply(QNetworkReply *reply, quint64 gen, int step)
{
    // From before a disable, a detach or another attach, or not the request this step sent: nothing.
    // (Every cancel here nulls m_reply before it aborts, so the abort's own finished() stops here.)
    if (gen != m_gen || reply != m_reply)
        return;
    m_reply = nullptr;
    if (!storeIsOurs() || !m_api) {
        probeEndedQuietly("the folder store left this account during the capability probe; nothing more is sent");
        return;
    }
    // The cycle's own reading of an answer: a 2xx whose transfer broke off is Transient, never a 2xx.
    int status = 0;
    const DavOutcome outcome = outcomeOf(reply, &status);
    // Step 1 may have made the scratch file even when its answer is not a clean 2xx: a transfer that
    // broke off after the server stored it, a timeout, or a 5xx from a proxy after the server wrote.
    // Every such answer (a 2xx, or the table's Transient) owes the best-effort DELETE. A 412 (the
    // name is not ours), a 401/403, a 423, a 507 or a refusal made nothing.
    if (step == 1 && (outcome == DavOutcome::Ok || outcome == DavOutcome::Created || outcome == DavOutcome::Transient))
        m_probe.created = true;
    ProbeVerdict v = ProbeVerdict::Refused;
    const bool expect412 = step == 3 || step == 4;
    switch (outcome) {
    case DavOutcome::Ok:
    case DavOutcome::Created:
        v = expect412 ? ProbeVerdict::Refused : ProbeVerdict::Pass;   // a precondition the server ignored
        break;
    case DavOutcome::PreconditionFailed:
        v = expect412 ? ProbeVerdict::Pass : ProbeVerdict::Refused;   // step 1: R20; step 5: a rewritten ETag
        break;
    case DavOutcome::AuthRefused:
        v = ProbeVerdict::AuthRefused;
        break;
    case DavOutcome::Transient:
    case DavOutcome::Locked:
    case DavOutcome::QuotaFull:
        v = ProbeVerdict::CouldNotRun;
        break;
    case DavOutcome::NotFound:          // the file step 1 just made is not there: the server answers wrongly
    case DavOutcome::Conflict:
    case DavOutcome::PayloadTooLarge:
    case DavOutcome::Fatal:             // a forbidden name, a stray 3xx (no redirect is ever followed)
        v = ProbeVerdict::Refused;
        break;
    case DavOutcome::Cancelled:
        // Unreachable: outcomeOf passes aborted=false, and this controller's own cancels move m_gen
        // first. An abort is silent in the status table: no verdict, no pause, no DELETE.
        probeEndedQuietly("the capability probe was cancelled; nothing more is sent");
        return;
    }
    if (v == ProbeVerdict::Pass && step == 2) {
        m_probe.e1 = ApiClient::davEtag(reply);   // byte for byte, as the cycle takes it
        if (m_probe.e1.isEmpty()) {
            TWARN("FolderSync: the capability probe failed: the server sends no file version (ETag)");
            v = ProbeVerdict::Refused;
        }
    }
    if (v != ProbeVerdict::Pass) {
        TWARN("FolderSync: the capability probe stopped at step" << step << "of 5, HTTP status" << status);
        probeFailed(v);
        return;
    }
    if (step < 5) {
        sendProbeStep(step + 1);
        return;
    }
    probePassed();
}

void FolderSyncController::probePassed()
{
    sendProbeDelete();   // step 6
    m_probe.created = false;
    // A pass that is not on disk is not a pass: the next start would have no marker either way, and a
    // cycle must never run on a server whose pass is not recorded. The next trigger probes again.
    if (!talq::foldersyncmeta::writeProbeMarker(m_store->rootDir(), m_scope)) {
        TWARN("FolderSync: the server passed the capability probe, but its marker could not be written; nothing is synced");
        probeFailed(ProbeVerdict::CouldNotRun);
        return;
    }
    TLOG("FolderSync: the server passed the capability probe");
    const CycleOrigin origin = m_probe.origin;
    const bool acceptShownShrink = m_probe.acceptShownShrink;
    m_probe = Probe();
    // D28: on the toggle path enabled=true is written only now, after the pass and the marker, and
    // before the first cycle. Still in flight meanwhile, so a slot's Sync now starts nothing.
    if (m_togglePending && !reportEnabled())
        return;
    beginCycle(origin, acceptShownShrink);
}

void FolderSyncController::probeFailed(ProbeVerdict verdict)
{
    // Step 6 after any exit once step 1 made the file, except a refused sign-in: the server may throttle
    // bad credentials, and a DELETE would be refused anyway.
    if (m_probe.created && verdict != ProbeVerdict::AuthRefused)
        sendProbeDelete();
    m_probe = Probe();
    endFlight();
    if (m_togglePending) {
        // The toggle path (D28): the switch goes back off with the reason. Never a pause, never a
        // failed attempt: the user is looking at the switch and is told there.
        refuseEnable(verdict == ProbeVerdict::Refused       ? talq::EnableRefusal::ServerUnsupported
                     : verdict == ProbeVerdict::AuthRefused ? talq::EnableRefusal::SignInRefused
                                                            : talq::EnableRefusal::CouldNotReach);
        return;
    }
    // The attach path (D28): the shared switch is never written from here.
    switch (verdict) {
    case ProbeVerdict::Refused:
        endPaused(FolderSyncPause::ServerUnsupported);
        return;
    case ProbeVerdict::AuthRefused:
        endPaused(FolderSyncPause::AuthRefused);
        return;
    case ProbeVerdict::CouldNotRun:
    case ProbeVerdict::Pass:
        endTransient("the capability probe could not run; the next trigger runs it again");
        return;
    }
}

// The probe ends with no verdict (the store left this account, or an abort): nothing more is sent, no
// DELETE, no pause and no failure. The switch, if it is waiting, is still answered: never left
// waiting with m_enabled set and no answer.
void FolderSyncController::probeEndedQuietly(const char *why)
{
    m_probe = Probe();
    if (m_togglePending) {
        refuseEnable(talq::EnableRefusal::CouldNotReach);   // turnOff() ends the flight
        return;
    }
    endQuietly(why);
}

// Step 6: fire-and-forget. Never m_reply (nothing waits for it, and a disable must not abort it into
// a probe answer), never anything but a log line when it ends; bound to this controller, so it is
// aborted, not left running, once the controller is gone.
void FolderSyncController::sendProbeDelete()
{
    if (!m_api || m_probe.path.isEmpty())
        return;
    QNetworkReply *reply = m_api->davRequest("DELETE", m_probe.path, QByteArray(), {},
                                             static_cast<int>(m_timing.putTimeoutMs), /*followRedirects*/ false);
    ++m_requestsSent;
    ApiClient::bindReplyLifetime(reply, this);
    connect(reply, &QNetworkReply::finished, this, [reply] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status < 200 || status >= 300)
            TLOG("FolderSync: the capability probe's test file was not removed, HTTP status" << status
                 << "(harmless: it is a few bytes, and maybe a trash entry)");
    });
}

// Step 2: gen0, a copy of the local state and the base, judged now. Also what a Stale restart does
// again: the base is whatever baseInputsNow() hands out, which a Stale apply never moved.
void FolderSyncController::takeSnapshot()
{
    // The snapshot takes every edit made so far: the debounce and its max-wait clock start again
    // with the next edit (one made during this cycle starts them when the cycle ends).
    m_editTimer.stop();
    m_editClock.invalidate();
    m_editDuringFlight = false;
    m_cycle.storeGeneration = m_store->generation();
    m_cycle.local = m_store->state();
    m_cycle.choice = baseChoiceNow();
    m_cycle.putAttempts = 0;
}

// Step 3. getAbsoluteUrl buffers the whole body with no limit, so the cap is enforced here: the reply
// is aborted the moment its declared length or the bytes received pass 4 MiB (D39), and that abort
// is RemoteUnreadable, never a silent cancel. Whatever an earlier GET of this cycle found (before a
// 412 sent the cycle back here) is forgotten first: an old ETag must never reach an If-Match. The
// replace gap's clock (m_cycle.getAnswered) is left alone on purpose: onGetFinished restarts it for
// every GET before mergeAndUpload, its only reader, looks at it.
void FolderSyncController::sendGet()
{
    m_cycle.remote = Remote::Absent;
    m_cycle.remoteState = FolderState();
    m_cycle.etag.clear();
    m_cycle.skipBrake = false;
    m_cycle.merged = FolderState();
    m_cycle.ifMatchSent.clear();
    // Never follow a redirect: Qt would copy this request, Basic credentials included, to wherever
    // it points, and a folders file there is not this account's. The 3xx comes back as the answer
    // (the table's stray 3xx: Fatal).
    QNetworkReply *reply = m_api->getAbsoluteUrl(m_cycle.path, static_cast<int>(m_timing.getTimeoutMs),
                                                 /*followRedirects*/ false);
    ++m_requestsSent;
    ApiClient::bindReplyLifetime(reply, this);
    m_reply = reply;
    m_getTooLarge = false;
    m_getRedirected = false;
    const quint64 gen = m_gen;

    const auto declaredLength = [reply]() -> qint64 {
        const QVariant v = reply->header(QNetworkRequest::ContentLengthHeader);
        return v.isValid() ? v.toLongLong() : -1;
    };
    const auto stopIfOverCap = [this, reply, gen](qint64 received, qint64 declared) {
        if (gen != m_gen || m_reply != reply)
            return;
        if (received <= talq::kMaxFolderFileBytes && declared <= talq::kMaxFolderFileBytes)
            return;
        m_getTooLarge = true;   // read by onGetFinished, which abort() may run right now
        m_reply = nullptr;
        reply->abort();
    };
    connect(reply, &QNetworkReply::metaDataChanged, this,
            [reply, declaredLength, stopIfOverCap] { stopIfOverCap(reply->bytesAvailable(), declaredLength()); });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [reply, declaredLength, stopIfOverCap](qint64 received, qint64 total) {
                stopIfOverCap(qMax(received, reply->bytesAvailable()), qMax(total, declaredLength()));
            });
    connect(reply, &QNetworkReply::readyRead, this,
            [reply, declaredLength, stopIfOverCap] { stopIfOverCap(reply->bytesAvailable(), declaredLength()); });
    // Defence in depth. With the manual policy above Qt never follows a redirect and never emits
    // redirected(). Should a redirect ever be followed anyway (the policy lost in some refactor),
    // nothing from it is used: the reply is stopped the moment Qt announces it, and onGetFinished
    // reads that as a stray 3xx (Fatal). The abort may not stop a follow-up request; the policy
    // above is what keeps it from being sent, and with it the credentials at home.
    connect(reply, &QNetworkReply::redirected, this, [this, reply, gen](const QUrl &) {
        if (gen != m_gen || m_reply != reply)
            return;
        m_getRedirected = true;   // read by onGetFinished, which abort() may run right now
        m_reply = nullptr;
        reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen] { onGetFinished(reply, gen); });
}

void FolderSyncController::onGetFinished(QNetworkReply *reply, quint64 gen)
{
    if (gen != m_gen)
        return;   // from before a detach or another attach
    // D41: the replace gap counts from the moment THIS answer arrived. Every GET starts it again (the
    // one after a 412, the one after a Stale restart), so a retry waits the whole gap too.
    m_cycle.getAnswered.start();
    if (m_reply == reply)
        m_reply = nullptr;
    if (!storeIsOurs()) {
        endQuietly("the folder store left this account while the download was out; nothing is used");
        return;
    }
    // The skip "Sync now" was pressed for belongs to the answer of the cycle's FIRST GET, whatever
    // that answer is (a file, a 404, zero bytes, something unreadable), and to nothing after it: a
    // GET after a 412 or a Stale restart may find a different file, which nobody was shown.
    m_cycle.skipBrake = m_cycle.acceptShownShrink;
    m_cycle.acceptShownShrink = false;
    if (m_getRedirected) {
        m_getRedirected = false;
        TWARN("FolderSync: the server answered the download of the shared file with a redirect; nothing is used");
        endPaused(FolderSyncPause::Fatal);
        return;
    }
    if (m_getTooLarge) {
        m_getTooLarge = false;
        TWARN("FolderSync: the shared folders file is larger than" << talq::kMaxFolderFileBytes
              << "bytes; the download was stopped and nothing is written");
        endPaused(FolderSyncPause::RemoteUnreadable);
        return;
    }

    int status = 0;
    switch (outcomeOf(reply, &status)) {
    case DavOutcome::Ok:
    case DavOutcome::Created: {
        // The self-inconsistent-412 count (step 5): after a 412 this GET says whether the file
        // changed (a race) or not (the server refused the ETag it had just handed out). A GET with
        // no 412 before it counts for nothing.
        const QByteArray etag = ApiClient::davEtag(reply);   // byte for byte, never normalised
        m_412.nextGet(etag);
        if (m_412.shouldPause()) {
            pauseServerUnsupported();
            return;
        }
        const talq::RemoteDecode d = talq::decodeRemote(reply->readAll());
        switch (d.kind) {
        case talq::RemoteKind::Present:
            m_cycle.remote = Remote::Present;
            m_cycle.remoteState = d.state;
            break;
        case talq::RemoteKind::Absent:
            m_cycle.remote = Remote::ZeroBytes;   // nobody wrote yet: never "everyone deleted everything"
            break;
        case talq::RemoteKind::Newer:
            TWARN("FolderSync: the shared folders file was written by a newer TalQ; it is left alone");
            endPaused(FolderSyncPause::RemoteNewer);
            return;
        case talq::RemoteKind::Unreadable:
            TWARN("FolderSync: the shared folders file cannot be read without changing it; it is left alone");
            endPaused(FolderSyncPause::RemoteUnreadable);
            return;
        }
        m_cycle.etag = etag;
        break;
    }
    case DavOutcome::NotFound:
        m_412.nextGet(QByteArray());       // the file went: a race, never a broken server
        m_cycle.remote = Remote::Absent;   // not a deletion: it is recreated from local (E18)
        break;
    case DavOutcome::AuthRefused:
        endPaused(FolderSyncPause::AuthRefused);   // never retried, never a logout, never "missing"
        return;
    case DavOutcome::Transient:
    case DavOutcome::Locked:
        endTransient("the download did not complete");
        return;
    case DavOutcome::QuotaFull:
        endPaused(FolderSyncPause::QuotaFull);
        return;
    case DavOutcome::PreconditionFailed:
    case DavOutcome::Conflict:
    case DavOutcome::PayloadTooLarge:
    case DavOutcome::Fatal:
    case DavOutcome::Cancelled:
        TWARN("FolderSync: unexpected answer to the download, HTTP status" << status);
        endPaused(FolderSyncPause::Fatal);
        return;
    }
    mergeAndUpload();
}

// Steps 4 and 5.
void FolderSyncController::mergeAndUpload()
{
    const FolderState &L = m_cycle.local;
    if (m_cycle.remote == Remote::Present) {
        // R10: an untrusted choice carries the EMPTY base (chooseBase hands nothing else), merged
        // with prefer Remote; a trusted one its base with prefer Local (D10).
        const talq::MergePrefer prefer = m_cycle.choice.trusted ? talq::MergePrefer::Local : talq::MergePrefer::Remote;
        talq::MergeResult mr = talq::mergeFolderStates(m_cycle.choice.base, L, m_cycle.remoteState, prefer);
        switch (mr.status) {
        case talq::MergeStatus::Ok:
            break;
        case talq::MergeStatus::TooManyFolders:
        case talq::MergeStatus::TooManyAssignments:
            TWARN("FolderSync: together the folders would pass the caps; nothing is applied or sent");
            endPaused(FolderSyncPause::TooLarge);
            return;
        case talq::MergeStatus::Invalid:
            TWARN("FolderSync: the merge failed its own check; nothing is applied or sent");
            endPaused(FolderSyncPause::Fatal);
            return;
        }
        // Step 4b, the remote shrink brake (D29), BEFORE any PUT or apply: a refused change is never
        // uploaded first. foldersDropped counts only folders the REMOTE takes away (in base and
        // local, absent there), so this PC's own deletions never brake, and an untrusted base
        // counts none. "Sync now", pressed while this pause showed "N of M", accepts the change the
        // merge of its FIRST GET shows (m_cycle.skipBrake is set for that GET's answer only, R18).
        const int localFolders = static_cast<int>(L.folders.size());
        if (talq::remoteShrinkBrake(localFolders, mr.foldersDropped)) {
            if (!m_cycle.skipBrake) {
                m_shrinkDropped = mr.foldersDropped;   // set before problemRaised: its slot reads it
                m_shrinkOf = localFolders;
                TWARN("FolderSync: the shared file would remove" << mr.foldersDropped << "of" << localFolders
                      << "folders here; nothing is applied or sent until Sync now");
                endPaused(FolderSyncPause::RemoteShrank);
                return;
            }
            TLOG("FolderSync: Sync now accepts a shared file that removes" << mr.foldersDropped << "of"
                 << localFolders << "folders here");
        }
        m_cycle.merged = std::move(mr.merged);
    } else {
        m_cycle.merged = L;   // absent or zero bytes: the local state is the whole truth
    }

    const bool remoteAbsentOrZeroBytes = m_cycle.remote != Remote::Present;
    bool upload = remoteAbsentOrZeroBytes || !talq::sameFolderState(m_cycle.merged, m_cycle.remoteState);
    if (upload && !talq::uploadAllowed(talq::isEmptyFolderState(m_cycle.merged), remoteAbsentOrZeroBytes))
        upload = false;   // D26: success with nothing sent
    if (!upload) {
        applyWhenAllowed();
        return;
    }

    if (!m_api) {
        endQuietly("the API client is gone");
        return;
    }
    const QByteArray body = talq::encodeFolders(m_cycle.merged, talq::kFolderSyncRemoteVersion);
    if (body.size() > talq::kMaxFolderFileBytes) {
        TWARN("FolderSync: internal error, the encoded folders pass the size cap; nothing is sent");
        endPaused(FolderSyncPause::Fatal);
        return;
    }
    if (m_cycle.remote != Remote::Absent && m_cycle.etag.isEmpty()) {
        // Never an unconditional write over a file someone else may have changed.
        TWARN("FolderSync: the server sent no file version (ETag); nothing is sent");
        endPaused(FolderSyncPause::Fatal);
        return;
    }

    // D41, the replace gap. The server keeps a file's version (ETag) when a write lands in the same
    // whole second as the previous one, and then an If-Match of the replaced version would still be
    // accepted (spec, "Measured live"). The version this PUT replaces was written no later than the
    // server's answer to our GET, so a PUT sent more than a second after that answer lands in a later
    // second and gets a new version. Only a REPLACE waits: a create (If-None-Match: *, the file was
    // absent) replaces nothing. The cycle stays in flight meanwhile (a second chain cannot start), and
    // an edit made during the wait ends as Stale at the apply, as one made during the PUT does.
    // The clock is always running here (mergeAndUpload follows onGetFinished); were it not, the whole
    // gap is waited.
    const qint64 sinceAnswer = m_cycle.getAnswered.isValid() ? m_cycle.getAnswered.elapsed() : 0;
    const qint64 waitMs = m_cycle.remote == Remote::Absent ? 0 : m_timing.replaceGapMs - sinceAnswer;
    if (waitMs <= 0) {
        sendPut(body);
        return;
    }
    const quint64 gen = m_gen;
    QTimer::singleShot(timerMs(waitMs), this, [this, gen, body] {
        if (gen != m_gen)
            return;   // detached, turned off or attached again during the wait: nothing is sent
        if (!storeIsOurs() || !m_api) {
            endQuietly("the folder store left this account during the wait before the upload; nothing is sent");
            return;
        }
        sendPut(body);
    });
}

// Step 5's request: the conditional PUT of `body` (the encoded merge), now. mergeAndUpload has made
// every check (the size cap, a version to condition a replace on) and waited the replace gap; nothing
// changes m_cycle in between without moving m_gen.
void FolderSyncController::sendPut(const QByteArray &body)
{
    QMap<QByteArray, QByteArray> headers;
    headers.insert("Content-Type", "application/json");
    if (m_cycle.remote == Remote::Absent) {
        headers.insert("If-None-Match", "*");
        m_cycle.ifMatchSent.clear();   // conditioned on no ETag at all
    } else {
        headers.insert("If-Match", m_cycle.etag);
        m_cycle.ifMatchSent = m_cycle.etag;
    }
    ++m_cycle.putAttempts;
    // Never follow a redirect of the PUT: a followed 301/302/303 comes back as the 200 of a GET of
    // the target, which would read as this upload's success (measured; the table's stray 3xx: Fatal).
    QNetworkReply *reply = m_api->davRequest("PUT", m_cycle.path, body, headers,
                                             static_cast<int>(m_timing.putTimeoutMs), /*followRedirects*/ false);
    ++m_requestsSent;
    ApiClient::bindReplyLifetime(reply, this);
    m_reply = reply;
    const quint64 gen = m_gen;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen] { onPutFinished(reply, gen); });
}

void FolderSyncController::onPutFinished(QNetworkReply *reply, quint64 gen)
{
    if (gen != m_gen)
        return;
    if (m_reply == reply)
        m_reply = nullptr;
    if (!storeIsOurs()) {
        endQuietly("the folder store left this account while the upload was out; nothing is applied");
        return;
    }
    int status = 0;
    const DavOutcome outcome = outcomeOf(reply, &status);
    switch (outcome) {
    case DavOutcome::Ok:
    case DavOutcome::Created:
        m_cycle.uploaded = true;
        m_412.reset();   // R8
        applyWhenAllowed();
        return;
    case DavOutcome::PreconditionFailed:
    case DavOutcome::NotFound:
        // The file changed (or went) between this cycle's GET and its PUT: nothing is applied, and
        // the cycle goes back to step 3 (GET, merge again from what is there NOW, PUT again). The
        // re-merge is idempotent (E20), so a PUT whose answer was lost and now reads as a 412 is
        // never sent twice in the same form. A 404 is no 412: it never feeds the 412 count.
        if (outcome == DavOutcome::PreconditionFailed)
            m_412.putFailed412(m_cycle.ifMatchSent);
        if (m_cycle.putAttempts >= talq::kMaxPutAttempts) {
            endTransient("the shared file kept changing on the server; the next cycle reads it again");
            return;
        }
        scheduleReGet();
        return;
    case DavOutcome::PayloadTooLarge:
        endPaused(FolderSyncPause::ServerSizeLimit);
        return;
    case DavOutcome::AuthRefused:
        endPaused(FolderSyncPause::AuthRefused);
        return;
    case DavOutcome::Transient:
    case DavOutcome::Locked:
        endTransient("the upload did not complete");
        return;
    case DavOutcome::QuotaFull:
        endPaused(FolderSyncPause::QuotaFull);
        return;
    case DavOutcome::Conflict:
    case DavOutcome::Fatal:
    case DavOutcome::Cancelled:
        TWARN("FolderSync: unexpected answer to the upload, HTTP status" << status);
        endPaused(FolderSyncPause::Fatal);
        return;
    }
}

// Back to step 3 after a refused PUT, after a random wait in [conflictRetryMinMs, conflictRetryMaxMs]
// (jitter, so two computers that refused each other do not collide again in lockstep). Only the
// GET is repeated: the snapshot (gen0, L, the base) stays, a user edit meanwhile ends as Stale at
// the apply. (The brake skip of "Sync now" belonged to the first GET's answer and is gone.)
void FolderSyncController::scheduleReGet()
{
    constexpr qint64 kZero = 0;
    constexpr qint64 kCeiling = 60000;   // keeps the int conversion below safe for any timing value
    const qint64 lo = qBound(kZero, m_timing.conflictRetryMinMs, kCeiling);
    const qint64 hi = qBound(lo, m_timing.conflictRetryMaxMs, kCeiling);
    const int waitMs = QRandomGenerator::global()->bounded(static_cast<int>(lo), static_cast<int>(hi) + 1);
    const quint64 gen = m_gen;
    QTimer::singleShot(waitMs, this, [this, gen] { reGet(gen); });
}

void FolderSyncController::reGet(quint64 gen)
{
    if (gen != m_gen)
        return;   // detached (or attached again) during the wait
    if (!storeIsOurs() || !m_api) {
        endQuietly("the folder store left this account during a conflict wait; nothing more is sent");
        return;
    }
    sendGet();
}

// Step 6: only after the upload (or when none was needed), and only through the gate (D22).
void FolderSyncController::applyWhenAllowed()
{
    if (talq::sameFolderState(m_cycle.merged, m_cycle.local)) {
        recordSuccess();
        return;
    }
    if (!m_applyGate || m_applyGate()) {
        applyNow();
        return;
    }
    m_gateClock.start();
    const quint64 gen = m_gen;
    QTimer::singleShot(m_timing.applyGatePollMs, this, [this, gen] { pollApplyGate(gen); });
}

void FolderSyncController::pollApplyGate(quint64 gen)
{
    if (gen != m_gen)
        return;   // detached (or attached again) while the apply was held
    if (!storeIsOurs()) {
        endQuietly("the folder store left this account while an apply was held");
        return;
    }
    if (!m_applyGate || m_applyGate()) {
        applyNow();
        return;
    }
    if (m_gateClock.elapsed() >= m_timing.applyGateGiveUpMs) {
        // Nothing is lost: whatever went up is on the server, and the next cycle merges it again.
        endQuietly("a remote change waited too long for the sidebar to be free; the next cycle applies it");
        return;
    }
    QTimer::singleShot(m_timing.applyGatePollMs, this, [this, gen] { pollApplyGate(gen); });
}

void FolderSyncController::applyNow()
{
    using AR = FolderStore::ApplyResult;
    // R16: applyMerged writes the file and emits saved() before it returns, while m_base is still the
    // PREVIOUS base. The hook must not write { previous base, sha(file now holding M) } then: if the
    // record step's write failed right after, the next start would trust a base the file does not
    // descend from. While this flag is up the hook only caches the sha, and recordSuccess is the one
    // writer for this save.
    const quint64 gen = m_gen;
    m_applying = true;
    const AR result = m_store->applyMerged(m_scope, m_cycle.storeGeneration, m_cycle.merged);
    m_applying = false;
    // applyMerged emits saved() and changed() before it returns, and a slot on either may have
    // detached this controller or attached it again: the session this cycle belonged to is gone, and
    // nothing below may run against the one that replaced it (its m_cycle is not this cycle's).
    if (gen != m_gen)
        return;
    if (!storeIsOurs()) {
        endQuietly("the folder store left this account during the apply; nothing is recorded");
        return;
    }
    switch (result) {
    case AR::Applied:
    case AR::AppliedNotSaved:   // in memory, the write failed: recordSuccess sees isDirty()
    case AR::Unchanged:
        recordSuccess();
        return;
    case AR::Stale:
        // The user changed the folders while the cycle was out (or the store was attached again).
        // The base does NOT move: it moves only together with a local state that matches it, and M
        // was merged from a state the store no longer holds (moved here, the next merge would read
        // the remote's additions as deletions made on this PC). Back to step 2 instead: a fresh
        // snapshot, GET and merge. Not a failure; after kMaxStaleRestarts the next trigger retries.
        if (m_cycle.staleRestarts >= talq::kMaxStaleRestarts) {
            endQuietly("the folders kept changing here during the cycle; the next trigger merges again");
            return;
        }
        ++m_cycle.staleRestarts;
        restartAfterStale();
        return;
    case AR::NotAttached:
    case AR::ReadOnly:
        endQuietly("the folder store can no longer take the change");
        return;
    case AR::Invalid:
        TWARN("FolderSync: internal error, the folder store refused the merged state");
        endPaused(FolderSyncPause::Fatal);
        return;
    }
}

// Step 6's Stale: back to step 2 within the same cycle (m_inFlight stays up, so it cannot go through
// startCycle). The account is checked again first: nothing is sent on a guess about whose home the
// path names. (The brake skip of "Sync now" belonged to the first GET's answer and is gone.)
void FolderSyncController::restartAfterStale()
{
    if (!m_api || !m_api->davUserKnown() || m_api->davFilePath(kRemoteName) != m_cycle.path) {
        endQuietly("the account changed during the cycle; nothing more is sent");
        return;
    }
    TLOG("FolderSync: the folders changed here during the cycle; merging again, restart"
         << m_cycle.staleRestarts << "of" << talq::kMaxStaleRestarts);
    takeSnapshot();
    sendGet();
}

// Step 7. The base moves in memory; the sidecar describes only what is on disk, so it is written only
// while the store is clean (D25, E23) and never with a sha this step computed: m_savedSha is the one
// the store's saved() last delivered.
// R17: a sidecar write that fails does not undo the cycle. The upload and the apply already happened
// and the UI must show them, and nothing on the network is worth retrying, so the cycle is still a
// success (Idle, lastSyncedMs, cycleFinished) and is not counted as a failure. The write is owed: the
// next record step or the saved() hook pays it. Until then a restart reads the older sidecar, which
// is safe (the merge is idempotent, E20).
// The cycle stays IN FLIGHT until just before cycleFinished: forceSave() below emits saved(), and a
// slot that pressed "Sync now" there would otherwise start a second cycle that overwrites m_cycle,
// after which this one would announce the new cycle's (empty) diff and report Idle mid-flight. The
// facts this cycle announces are taken first, for the same reason.
void FolderSyncController::recordSuccess()
{
    const talq::StateDiff diff = talq::diffStates(m_cycle.local, m_cycle.merged);
    const bool uploaded = m_cycle.uploaded;
    m_base = m_cycle.merged;
    m_sessionTrusted = true;
    m_lastSyncedMs = QDateTime::currentMSecsSinceEpoch();   // display only (D34)
    m_failures = 0;
    m_failureSpanMs = 0;
    m_failureClock.invalidate();

    if (m_store->isDirty()) {
        m_sidecarOwed = true;   // the saved() hook pays it when a later write succeeds
    } else if (talq::foldersyncmeta::writeSidecar(m_store->rootDir(), m_scope, m_base, m_savedSha)) {
        m_sidecarOwed = false;
    } else {
        TWARN("FolderSync: the sync record could not be written; it is written at the next save or cycle");
        m_sidecarOwed = true;
    }

    if (m_repairRewriteDue) {
        // D30: a file that loaded Repaired or SetAside is not rewritten by an Unchanged apply, so it
        // would stay damaged on disk and every start would distrust the base (rule 2) and merge as a
        // union. Once per attach, after the first success; its saved() reaches the hook, which
        // writes { m_base, the new file's hash } (the base is trusted now).
        m_repairRewriteDue = false;
        const quint64 gen = m_gen;
        if (!m_store->forceSave())
            TWARN("FolderSync: the repaired folders file could not be rewritten; the next start tries again");
        if (gen != m_gen)
            return;   // a slot on saved() detached or re-attached this controller: nothing more here
        if (!storeIsOurs()) {
            endQuietly("the folder store left this account during the rewrite");
            return;
        }
    }

    const bool first = m_firstAfterEnable;
    m_firstAfterEnable = false;
    endFlight();
    setState(FolderSyncState::Idle);
    emit cycleFinished(diff, first, uploaded);
}

// ---------------------------------------------------------------------------------------------
// ends of a cycle

// Two 412s whose next GET returned the very ETag the PUT was conditioned on: nobody wrote in between,
// the server (or a proxy) refuses the ETag it hands out, and every retry would fail the same way.
// The probe marker goes, so "Sync now" proves the server again before anything touches the file.
void FolderSyncController::pauseServerUnsupported()
{
    TWARN("FolderSync: the server refused the file version it had just handed out, twice; sync stops until Sync now");
    if (!talq::foldersyncmeta::removeProbeMarker(m_store->rootDir(), m_scope))
        TWARN("FolderSync: the capability probe marker could not be deleted");
    endPaused(FolderSyncPause::ServerUnsupported);
}

void FolderSyncController::endPaused(FolderSyncPause reason)
{
    endFlight();
    if (reason == FolderSyncPause::AuthRefused && m_cycle.revertOnAuthRefused) {
        // D28, D35: the first cycle after the user turned the switch on met 401/403. MainWindow puts
        // the switch back with a QSignalBlocker and never calls disable() for it, so the controller
        // turns itself off: otherwise it would stay on and paused, and the next toggle-on would be the
        // "already on" no-op.
        refuseEnable(talq::EnableRefusal::SignInRefused);
        return;
    }
    // "States": QuotaFull retries on the ladder's slowest step (re-armed each time a cycle pauses
    // QuotaFull again; any other exit from the pause cancels it in startCycle). Armed before the
    // state is announced, so a slot that turns the switch off or logs out stops it.
    if (reason == FolderSyncPause::QuotaFull)
        m_slowStepTimer.start(timerMs(ladderWaitMs(kPastTheLadder)));
    TWARN("FolderSync: paused:" << pauseName(reason));
    setState(FolderSyncState::Paused, reason);
    emit problemRaised(reason);
}

// "Backoff": the next try waits the ladder's step for this many failures in a row (with jitter),
// unless the reachability edge, "Sync now" or a late enough activation comes first. Armed before the
// state is announced, so a slot that turns the switch off or logs out stops it.
void FolderSyncController::endTransient(const char *why)
{
    endFlight();
    ++m_failures;
    if (!m_failureClock.isValid())
        m_failureClock.start();
    m_failureSpanMs = m_failureClock.elapsed();
    TWARN("FolderSync: attempt failed:" << why << "- failures in a row:" << m_failures);
    m_backoff = true;
    m_backoffTimer.start(timerMs(ladderWaitMs(m_failures)));
    setState(FolderSyncState::Waiting);
    emit attemptFailed();
}

// Not a success and not a failure: nothing was applied or recorded, and the next cycle starts over.
void FolderSyncController::endQuietly(const char *why)
{
    endFlight();
    TLOG("FolderSync:" << why);
    setState(FolderSyncState::Idle);
}

void FolderSyncController::setState(FolderSyncState state, FolderSyncPause pause, bool accountNotReady)
{
    if (state != FolderSyncState::Paused)
        pause = FolderSyncPause::None;
    if (state != FolderSyncState::Waiting)
        accountNotReady = false;
    if (state == m_state && pause == m_pause && accountNotReady == m_accountNotReady)
        return;
    m_state = state;
    m_pause = pause;
    m_accountNotReady = accountNotReady;
    emit stateChanged();
}

// ---------------------------------------------------------------------------------------------
// the store's saved() hook (spec "Keeping localSha current", D25)

// Fires after every successful write of the local file: a user edit, a remote apply, forceSave, a
// flush retry, and the flush of a scope being left (or of a store being destroyed). It only RECORDS.
//  - The sha is cached for the record step, whatever happens next.
//  - The sidecar is rewritten as { m_base, sha } only while that base is TRUSTED right now (rules 1
//    to 3 held at attach, or a cycle of this attach succeeded, and rule 4 does not fire). An
//    untrusted base must stay untrusted on disk: written next to the new file's hash it would be
//    believed at the next start, and every folder it holds that the file lacks would read as a
//    deletion made here and be uploaded.
//  - A write owed by the record step is paid here.
//  - Except for the save inside this controller's own applyMerged (R16): then only the sha is
//    cached, and the record step that follows writes { merged base, that sha } itself.
void FolderSyncController::onStoreSaved(const QString &scopeKey, const QByteArray &sha1Hex)
{
    if (scopeKey != m_scope || !storeIsOurs())
        return;   // another scope, or not attached: never written under the wrong name
    m_savedSha = sha1Hex;
    if (m_applying)
        return;   // R16: recordSuccess is the only writer for the cycle's own save
    if (m_store->isDirty() || !baseChoiceNow().trusted)
        return;
    if (talq::foldersyncmeta::writeSidecar(m_store->rootDir(), m_scope, m_base, sha1Hex)) {
        m_sidecarOwed = false;
    } else {
        TWARN("FolderSync: the sync record could not be updated after a save; the next start merges as a union");
        m_sidecarOwed = true;
    }
}
