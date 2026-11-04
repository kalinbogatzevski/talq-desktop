#pragma once

// FolderSyncController -- keeps this account's sidebar folders in step with ONE small file in the
// user's own Nextcloud Files (/.talq-folders.json in the DAV home), over WebDAV. GUI thread only; a
// child of MainWindow, created right after the FolderStore it syncs. It emits facts (state changes,
// a finished cycle, a pause, a failed attempt) and never shows anything itself: no QMessageBox, no
// toast, no notice. Every pixel and every popup is MainWindow's.
// Spec: docs/superpowers/specs/2026-11-03-folder-sync-design.md, "The sync controller".
// Test: tests/folder_sync_controller_test.cpp (against tests/support/FakeDavServer.h).
//
// ONE CYCLE (spec "One cycle"; the order is the safety argument):
//   1. guards: the switch is on, the store is attached to the scope this controller was attached
//      for, writable and not a read-only load, nothing in flight; the canonical uid is known (else
//      Waiting, "account not ready", and nothing is sent); the capability probe has passed (its
//      marker is present; without it the PROBE below runs first, and the cycle only after a pass: no
//      cycle ever touches the real file on a server that has not passed it);
//   2. snapshot: the store's generation, a copy of its state, and the base (chooseBase, every input
//      filled every cycle; rules 1 to 3 judged at attach, rule 4 now);
//   3. GET the file, aborted the moment it passes 4 MiB (D39); decodeRemote (D27);
//   4. merge (untrusted base: the EMPTY state with prefer Remote, R10); over the caps it is
//      refused whole (Paused(TooLarge), D13), never cut down;
//      4b. the remote shrink brake (D29): a merge that would take away every folder of this PC, or
//      more than half of them with at least 3, BECAUSE OF THE REMOTE, applies nothing and sends
//      nothing (Paused(RemoteShrank)); it sits here, before any PUT, so a refused change is never
//      uploaded first;
//   5. upload only when the merge differs from the remote, never an empty state over an absent or
//      zero-byte file (D26), always conditional (If-Match with the GET's ETag byte for byte, or
//      If-None-Match: * for an absent file), and NEVER after a GET this build could not read. A PUT
//      that REPLACES the file (If-Match) goes out no sooner than replaceGapMs after the answer of
//      the GET it was merged from (D41: the server keeps a file's version for two writes in one
//      second; the wait puts ours in a later second); a create never waits. The cycle stays in
//      flight during the wait, and the wait's end checks m_gen and the store like any callback. A 412
//      (or a 404 on the PUT) goes back to step 3 after a short random wait, at most kMaxPutAttempts
//      PUTs, then Waiting; two 412s whose next GET returned the very ETag the PUT was conditioned
//      on mean the server rejects its own ETags: Paused(ServerUnsupported), the probe marker goes;
//   6. apply locally (after the upload, never before), through the apply gate; a Stale apply (the
//      user edited meanwhile) goes back to step 2 with the base where it was, at most
//      kMaxStaleRestarts times, then the cycle waits for the next trigger;
//   7. record: the base moves in memory; the sidecar is written only while the store is clean; a
//      local file that loaded Repaired or SetAside is rewritten once after the first success (D30).
//
// syncNow() IS THE USER'S BUTTON AND NOTHING ELSE. It overrides every pause but AuthRefused. Pressed
// while Paused(RemoteShrank), its cycle skips the shrink brake for the merge of its first GET (the
// user accepted the "N of M" the pause showed); pressed from any other state it brakes like any
// cycle, so a shrink is always shown before it is accepted (R18). Every AUTOMATIC trigger (below)
// starts its cycle through startFromTrigger(), which obeys every pause but the two that are woken by
// their own trigger (a local Edit ends Paused(TooLarge), the SlowStep retries Paused(QuotaFull)), and
// always the brake; never through syncNow(). A timer wired to syncNow() would let a shrunk remote
// file delete folders on every PC without asking anyone. enable() is the third origin, the switch: it
// clears every pause (AuthRefused too) but brakes like an automatic cycle.
//
// THE TRIGGERS (spec "Triggers and cadence", D18; "Backoff"; the pause rules of "States"). One place,
// startFromTrigger(), judges every automatic one; the in-flight guard and the pause rule are
// startCycle's, shared with the button and the switch:
//   - Login: loginDelayMs after attach(true) (not attach(false), not enable(), whose cycle starts at
//     once); a pull before any push, since every cycle GETs first. The first cycle of a session,
//     whatever started it, takes its place.
//   - Poll: every pollMs while the switch is on and attached, whatever the window is doing (Q2 v). The
//     controller has no notion of focus.
//   - Activation (MainWindow's changeEvent): a pull, more than activationGapMs after the last attempt.
//   - Deactivation (MainWindow, once no window of this app is active, D36): a push when one is
//     pending, more than pushGapMs after the last attempt. "Pending" is derived, never stored: the
//     store's state differs from the base, or the base is untrusted (then a cycle is always due).
//   - Edit: editDebounceMs after the last local edit (FolderStore::localEdited, which a remote apply
//     never emits), at most editMaxWaitMs after the first unsent one. An edit made while a cycle is in
//     flight starts the debounce when that cycle ends: it is never lost.
//   - Reachability (MainWindow, the reachability edge): when a retry or a push is pending.
//   - Backoff: after a transient failure, the ladder (nextBackoffMs) decides when to try again. While
//     it waits, only the reachability edge, "Sync now" and an activation more than
//     backoffOverrideGapMs after the last attempt start a cycle; the rest is ignored (the ladder's own
//     timer carries a pending change).
//   - SlowStep: Paused(QuotaFull) retries on the ladder's slowest step.
// While Paused only the pause's own rule starts a cycle: TooLarge an Edit, QuotaFull its SlowStep;
// "Sync now" every pause but AuthRefused; AuthRefused only a logout/login or the switch. A timer that
// fires while a cycle is in flight is skipped. "The last attempt" is one monotonic clock, started when
// a cycle or a probe starts (D34); a trigger that finds the account not ready (D38) starts no attempt
// and arms nothing: the next trigger looks again. Every timer stops on disable(), detach() and a new
// attach(). There is no quit hook: no network call at quit.
//
// THE CAPABILITY PROBE (spec "The capability probe", D17). Before this computer's first cycle on a
// server (the marker <scopeKey>.probed absent), six small requests on a scratch file of its own,
// /.talq-folders.probe-<8 random hex> (a fresh name per attempt), prove that the server honours
// If-None-Match and If-Match and that the ETag a GET hands out is the one a conditional PUT accepts:
//   1. PUT If-None-Match: * on the absent name: 2xx (a 412 here is Refused, R20)
//   2. GET: 2xx WITH an ETag, e1
//   3. PUT If-None-Match: * again: 412 (the file exists)
//   4. PUT If-Match: "talq-probe-bogus": 412
//   5. PUT If-Match: e1: 2xx
//   6. DELETE, fire-and-forget (also after any later failure once step 1 made the file or may have:
//      step 1 answered with a 2xx or a Transient status, a dropped transfer or a timeout included;
//      never after a 401/403 or a 412, nor after a disable, a detach or the controller's end)
// Every answer goes through classifyDav (the cycle's table), and no request follows a redirect (R21).
// Passed: the marker is written (a marker that cannot be written counts as could-not-run: no cycle
// runs on a pass that is not on disk), then the cycle starts. Refused (the server answered, but
// wrongly; also a 404, 409, 413, a stray 3xx or any status the table does not name, a 501 included):
// Paused(ServerUnsupported). Could not run (500, 502, 503, 504, 408, 429, 423, 507, no answer or a
// transfer timeout): Waiting, and the next trigger probes again. 401/403: Paused(AuthRefused). The
// probe shares m_reply and the in-flight guard with the cycle (SettingUp), so one chain at a time.
//
// Lifetimes. The store can be destroyed while this object is alive (MainWindow creates the store
// first and Qt deletes children in creation order), and ~FolderStore flushes and emits saved(): both
// pointers are QPointers and every slot checks that the store is still attached to OUR scope. Every
// network reply goes through ApiClient::bindReplyLifetime, is held in m_reply so detach() and
// disable() can abort it (no ApiClient call made here registers with cancelAll()), and every callback
// first checks m_gen (bumped by attach, detach, disable and enable) and the store's scope. The one
// exception is the probe's fire-and-forget DELETE: never in m_reply, its end changes nothing.

#include "core/FolderSyncLogic.h"
#include "core/FolderSyncPolicy.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <functional>

class ApiClient;
class FolderStore;
class QNetworkReply;

namespace talq {
enum class EnableRefusal;   // FolderSyncNotices.h
}

class FolderSyncController : public QObject
{
    Q_OBJECT
public:
    // What started an automatic cycle (see "THE TRIGGERS" above).
    enum class Trigger {
        Login,          // loginDelayMs after attach(true)
        Activation,     // onWindowActivated()
        Deactivation,   // onWindowDeactivated()
        Edit,           // the local-edit debounce
        Reachability,   // onReachabilityRestored()
        Poll,           // every pollMs; also startAutomaticCycle()
        Backoff,        // the ladder's timer after a transient failure
        SlowStep,       // Paused(QuotaFull)'s retry on the slowest step
    };
    static constexpr int kTriggerKinds = 8;

    FolderSyncController(FolderStore *store, ApiClient *api, QObject *parent = nullptr);
    ~FolderSyncController() override;

    // The test seam: every gap and timeout (FolderSyncPolicy.h). The defaults are the spec's.
    void setTiming(const talq::FolderSyncTiming &t);

    // Asked before a remote change is applied (D22: not while a sidebar drag or a folder dialog is
    // open). Polled every applyGatePollMs; after applyGateGiveUpMs the cycle ends without applying
    // and the next one retries. No gate = always open.
    void setApplyGate(std::function<bool()> gate);

    // The store is already attached; the scope is store->scopeKey(). Reads the sidecar, judges the
    // base-trust rules that hold for the whole attach, and starts listening to the store. A
    // previous attach is dropped first (as detach()). Never writes the shared switch, never sends a
    // request by itself: with the switch on (and the store writable) it arms the login timer and the
    // poll, and the login timer starts the session's first cycle.
    void attach(bool enabledAtAttach);

    // Logout, account switch, quit: bump m_gen, abort the reply in flight, stop every timer, forget
    // the scope. No network call and no sidecar write.
    void detach();

    // The "Sync now" button: start a cycle now, unless one is in flight or the pause is AuthRefused
    // (it waits for a logout/login or a switch toggle). Any other pause is overridden. Pressed while
    // Paused(RemoteShrank), the cycle's first merge skips the shrink brake ONCE (a re-GET after a
    // 412, or a restart after a Stale apply, brakes as usual: the user accepted what they were told,
    // not what came later); from any other state the brake applies (R18). After a ServerUnsupported
    // pause it also starts the self-inconsistent-412 count again (R8).
    void syncNow();

    // A plain automatic trigger, judged exactly as a poll (see the header comment): nothing while a
    // cycle is in flight, while any pause holds or during a backoff, and its merge is always subject
    // to the shrink brake. Nothing in the app calls it; the test uses it as "some trigger".
    void startAutomaticCycle();

    // MainWindow's hooks (D18). Each is one trigger, judged by the rules above; none of them ever
    // overrides a pause, the in-flight guard or (but for the reachability edge and an activation past
    // backoffOverrideGapMs) a backoff.
    // The window was activated: a pull, at most every activationGapMs.
    void onWindowActivated();
    // No window of this app is active any more (the CALLER has decided that: a deactivation towards
    // the app's own modal window is not one, D36), or the window was hidden: a push when one is
    // pending and the last attempt is over pushGapMs old.
    void onWindowDeactivated();
    // The server became reachable again (MainWindow's reachability edge): a retry or a push, when one
    // is pending; it cuts a backoff short.
    void onReachabilityRestored();

    // The user turned the Settings switch ON (the toggle path, D28). State SettingUp; with no probe
    // marker the probe runs first. The answer is enableFinished(): true after a pass (or at once
    // when the marker is present), BEFORE the first cycle starts and from this call's own chain,
    // never queued, so MainWindow writes enabled=true only for an attempt that is still current;
    // false with a reason when the probe was refused or could not run, and then the controller is
    // Off again (not enabled). A first cycle refused with 401/403 also answers false after the
    // earlier true and turns the controller off: MainWindow puts the switch back with a
    // QSignalBlocker (D35), so it never calls disable() for it. A transient failure of that cycle
    // does not (Waiting). The toggle clears every pause, AuthRefused included, brakes like an
    // automatic cycle, and starts the self-inconsistent-412 count again. It never attaches again:
    // the base and the attach-time trust inputs stay, so a re-enable with a kept sidecar is a
    // TRUSTED three-way merge (D21) and a first enable an untrusted union (D20). It arms the poll, not
    // the login timer (its own cycle is the first pull). Already on: nothing (no second probe, no
    // second answer). Not attached, or the store read-only: false with the "could not reach" text,
    // and no request.
    void enable();

    // The user turned the switch OFF (MainWindow has written enabled=false). Bumps m_gen, aborts
    // whatever is in flight (a probe or a cycle; its late answer is ignored), stops every timer and
    // the probe: nothing more is sent, no DELETE either (a disabled computer never contacts the server). The
    // shared file stays on the server and the sidecar stays on disk; while off, the store's saved()
    // hook keeps the sidecar's localSha current. No enableFinished for an attempt this cancels.
    void disable();

    talq::FolderSyncState state() const { return m_state; }
    talq::FolderSyncPause pause() const { return m_pause; }
    qint64 lastSyncedMs() const { return m_lastSyncedMs; }   // wall clock, display only (D34); 0 = never
    bool isEnabled() const { return m_attached && m_enabled; }
    int consecutiveFailures() const { return m_failures; }
    qint64 failureSpanMs() const { return m_failureSpanMs; }   // first to latest failure of the run
    QPair<int, int> shrinkInfo() const { return qMakePair(m_shrinkDropped, m_shrinkOf); }   // (dropped, of)
    bool accountNotReady() const { return m_accountNotReady; }   // Waiting because the uid is unknown (D38)

    // Test seam: a sidecar write is owed (the record step found the store dirty, D25, or its write
    // failed, R17).
    bool sidecarOwed() const { return m_sidecarOwed; }

    // Test seam: the self-inconsistent-412 count (R8; FolderSyncPolicy.h). Two pause the controller.
    int selfInconsistent412Count() const { return m_412.count(); }

    // Test seam: every request this controller has created (a GET, a PUT, a probe step, the probe's
    // DELETE), counted as it is handed to the network, so "nothing was sent" can be judged the moment
    // a call returns instead of after a wait for the server's log.
    int requestsSent() const { return m_requestsSent; }

    // Test seam: how often each trigger came in (fired, or was called), whatever it then decided.
    int triggerCount(Trigger t) const { return m_triggerCounts[static_cast<int>(t)]; }

    // Test seam: the timers armed right now, each with the interval it was armed with (-1: not armed).
    // The poll repeats while the switch is on and attached; the others are single-shot.
    struct ArmedTimers {
        int login = -1;
        int poll = -1;
        int edit = -1;
        int backoff = -1;
        int slowStep = -1;
    };
    ArmedTimers armedTimers() const;

signals:
    // The answer to enable() (the toggle path only; the attach path never sends it). `ok` true: the
    // server is proved, MainWindow writes enabled=true. false: `reasonText` is one of the three "Not
    // turned on: ..." lines (talq::enableRefusalText); MainWindow puts the switch back to off.
    void enableFinished(bool ok, const QString &reasonText);
    void stateChanged();
    // A cycle ended successfully. `diffFromLocalBeforeApply` is the local state the cycle started
    // from against the state it left (nothing visible when only this PC's changes went up).
    void cycleFinished(const talq::StateDiff &diffFromLocalBeforeApply, bool firstAfterEnable, bool uploaded);
    // The controller entered Paused for `reason` (a fact; MainWindow decides about any notice).
    void problemRaised(talq::FolderSyncPause reason);
    // A transient failure ended an attempt (Waiting); consecutiveFailures() and failureSpanMs() say
    // how long the run is.
    void attemptFailed();

private:
    // What the GET found.
    enum class Remote {
        Absent,      // 404: create with If-None-Match: *
        ZeroBytes,   // 200 with an empty or whitespace body: nobody wrote yet; If-Match its ETag
        Present      // a readable file; `remoteState` holds it
    };

    // One cycle's working set. Three lifetimes, reset at three different points:
    //  - the CYCLE (startCycle): the path, the brake skip, the restart count, whether a PUT went up;
    //  - the PASS (the snapshot, step 2, taken again by a Stale restart): gen0, L, the base, the PUTs
    //    sent in this pass;
    //  - the GET (step 3, done again after a 412): what the GET found, when its answer arrived, and
    //    what was merged from it.
    struct Cycle {
        QString path;                  // the DAV path of the shared file, built once
        bool acceptShownShrink = false;// Sync now pressed in Paused(RemoteShrank) (R18); handed to the
                                       // FIRST GET's answer, whatever it is, and gone after that
        int staleRestarts = 0;         // Stale restarts so far (step 6)
        bool uploaded = false;         // a PUT of this cycle succeeded
        bool revertOnAuthRefused = false;   // the first cycle after the switch was turned on: a
                                            // 401/403 turns it back off (D28, D35), not a pause

        quint64 storeGeneration = 0;   // gen0, for applyMerged
        talq::FolderState local;       // L
        talq::BaseChoice choice;
        int putAttempts = 0;           // conditional PUTs sent in this pass (step 5)

        Remote remote = Remote::Absent;
        QElapsedTimer getAnswered;     // started when this GET's answer arrived: the replace gap's clock (D41)
        talq::FolderState remoteState; // R (Present only)
        QByteArray etag;               // from the GET, byte for byte
        bool skipBrake = false;        // this GET's merge may skip the shrink brake (the first only)
        talq::FolderState merged;      // M
        QByteArray ifMatchSent;        // the If-Match of the PUT in flight (empty for If-None-Match: *)
    };

    // Who starts a cycle. Each has its own rules about pauses and the shrink brake.
    enum class CycleOrigin {
        Button,      // syncNow(): clears every pause but AuthRefused; skips the brake once, and only
                     // when pressed from Paused(RemoteShrank) (R18); from Paused(ServerUnsupported) the
                     // self-inconsistent-412 count starts again (R8)
        Automatic,   // every trigger (startFromTrigger; startAutomaticCycle() is one): refused while a
                     // pause holds, except a pause's own trigger: an Edit wakes Paused(TooLarge), the
                     // SlowStep wakes Paused(QuotaFull); always brakes
        SwitchOn,    // enable(): clears EVERY pause, AuthRefused included; brakes like Automatic (the
                     // toggle is not the "Sync now" that accepts a shown shrink); its cycle is the
                     // "first after enable" one, and a 401/403 in it turns the switch back off
    };

    // What one probe answer says (spec "The capability probe", Outcomes).
    enum class ProbeVerdict { Pass, Refused, CouldNotRun, AuthRefused };

    // The capability probe's working set (one probe at a time; it is "in flight" like a cycle).
    struct Probe {
        QString path;                     // the scratch file: a fresh random name per attempt
        CycleOrigin origin = CycleOrigin::Automatic;   // the cycle that follows a pass
        bool acceptShownShrink = false;   // judged at the press (R18), handed on to that cycle
        QByteArray e1;                    // step 2's ETag, step 5's If-Match
        bool created = false;             // step 1 made the file: a DELETE is owed when the probe ends
    };

    bool storeIsOurs() const;
    talq::BaseInputs baseInputsNow() const;
    talq::BaseChoice baseChoiceNow() const;

    // `trigger` matters only for the Automatic origin (the pause rule: a pause's own trigger wakes it).
    void startCycle(CycleOrigin origin, Trigger trigger = Trigger::Poll);
    void startFromTrigger(Trigger trigger);
    bool lastAttemptOlderThan(qint64 ms) const;
    bool pushPending() const;
    void onLocalEdited();
    void armEditDebounce();
    qint64 ladderWaitMs(int failures) const;
    void stopTimers();
    void endFlight();
    void beginCycle(CycleOrigin origin, bool acceptShownShrink);
    void startProbe(CycleOrigin origin, bool acceptShownShrink);
    void sendProbeStep(int step);
    void onProbeReply(QNetworkReply *reply, quint64 gen, int step);
    void probePassed();
    void probeFailed(ProbeVerdict verdict);
    void probeEndedQuietly(const char *why);
    void sendProbeDelete();
    bool reportEnabled();
    void refuseEnable(talq::EnableRefusal why);
    void turnOff();
    void takeSnapshot();
    void restartAfterStale();
    void sendGet();
    void onGetFinished(QNetworkReply *reply, quint64 gen);
    void mergeAndUpload();
    void sendPut(const QByteArray &body);
    void onPutFinished(QNetworkReply *reply, quint64 gen);
    void scheduleReGet();
    void reGet(quint64 gen);
    void applyWhenAllowed();
    void pollApplyGate(quint64 gen);
    void applyNow();
    void recordSuccess();

    void pauseServerUnsupported();
    void endPaused(talq::FolderSyncPause reason);
    void endTransient(const char *why);
    void endQuietly(const char *why);
    void abortReply();
    void resetSession();
    void setState(talq::FolderSyncState state, talq::FolderSyncPause pause = talq::FolderSyncPause::None,
                  bool accountNotReady = false);
    void onStoreSaved(const QString &scopeKey, const QByteArray &sha1Hex);

    QPointer<FolderStore> m_store;
    QPointer<ApiClient> m_api;
    talq::FolderSyncTiming m_timing;
    std::function<bool()> m_applyGate;

    bool m_attached = false;
    bool m_enabled = false;
    QString m_scope;          // the store's scope key at attach
    quint64 m_gen = 0;        // bumped by attach, detach, disable and enable: a callback from before is dropped
    QPointer<QNetworkReply> m_reply;
    bool m_getTooLarge = false;   // the GET in flight was aborted because it passed the cap (D39)
    bool m_getRedirected = false; // the GET in flight was answered with a redirect, and aborted
    bool m_inFlight = false;      // a cycle or the probe is running (request, conflict wait, replace gap,
                                  // apply gate wait)
    Cycle m_cycle;
    Probe m_probe;
    QElapsedTimer m_gateClock;

    // The base. Rules 1 to 3 of chooseBase hold for a whole attach, so their inputs are read once;
    // rule 4 is judged every cycle against the store as it is then.
    talq::BaseInputs m_attachInputs;
    quint64 m_attachGeneration = 0;
    bool m_sessionTrusted = false;   // a cycle succeeded in this attach: m_base is trusted from now on
    talq::FolderState m_base;        // the CURRENT base (spec D25): at attach the base chooseBase hands
                                     // out (the sidecar's if trusted, else empty), then each cycle's M
    QByteArray m_savedSha;           // the sha the store's saved() last delivered (initially loadedSha)
    bool m_sidecarOwed = false;      // the record step could not write the sidecar (the store was dirty,
                                     // or the write failed, R17): the saved() hook or the next record pays it
    bool m_applying = false;         // this controller's own applyMerged is on the stack (R16)
    bool m_repairRewriteDue = false; // D30: the file loaded Repaired or SetAside and the first
                                     // successful cycle of this attach has not rewritten it yet

    talq::FolderSyncState m_state = talq::FolderSyncState::Off;
    talq::FolderSyncPause m_pause = talq::FolderSyncPause::None;
    bool m_accountNotReady = false;
    bool m_togglePending = false;      // enable() is waiting for its answer (enableFinished not sent yet)
    bool m_firstAfterEnable = false;   // set by enable(); the next SUCCESSFUL cycle reports it, once
    qint64 m_lastSyncedMs = 0;
    int m_failures = 0;
    qint64 m_failureSpanMs = 0;
    QElapsedTimer m_failureClock;      // started at the first failure of a run (monotonic, D34)
    int m_shrinkDropped = 0;
    int m_shrinkOf = 0;
    talq::SelfInconsistent412Tracker m_412;
    int m_requestsSent = 0;            // test seam: requestsSent()

    // The triggers (D18). Every timer is stopped by disable(), detach() and attach() (stopTimers).
    QTimer m_loginTimer;               // single-shot, loginDelayMs after attach(true)
    QTimer m_pollTimer;                // repeating, pollMs, while on and attached
    QTimer m_editTimer;                // single-shot, the local-edit debounce
    QTimer m_backoffTimer;             // single-shot, the ladder's wait after a transient failure
    QTimer m_slowStepTimer;            // single-shot, Paused(QuotaFull)'s retry
    bool m_backoff = false;            // a transient failure armed the ladder and no cycle started since
    QElapsedTimer m_lastAttempt;       // started when a cycle or a probe starts (D34); invalid: never
    QElapsedTimer m_editClock;         // started by the first edit no snapshot has taken yet
    bool m_editDuringFlight = false;   // an edit came while a cycle was in flight: debounce at its end
    int m_triggerCounts[kTriggerKinds] = {};   // test seam: triggerCount()
};
