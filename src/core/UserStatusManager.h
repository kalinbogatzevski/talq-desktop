#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVector>
#include <QTimer>

#include <functional>

class ApiClient;
class AuthManager;
class QColor;
class QJsonObject;

/**
 * Owns the current user's Nextcloud user-status (the user_status app), the
 * full protocol behind it, and the crash-stuck "in a call" recovery.
 *
 * The "📞 In a call / Busy" status is set and reverted server-side by Talk
 * as a side effect of call participation; the client never sets it. What
 * the client CAN do — and does here on every login — is call
 * `user_status/revert/call`, which restores the pre-call backup if a crash
 * left it stuck. Everything else (online/away/dnd/invisible, custom
 * message, predefined presets, clear-after) is a normal user_status write.
 */
class UserStatusManager : public QObject
{
    Q_OBJECT
public:
    enum class Status { Online, Away, Dnd, Invisible, Offline };

    struct Predefined {
        QString id;
        QString icon;
        QString message;
    };

    UserStatusManager(ApiClient *api, AuthManager *auth, QObject *parent = nullptr);

    Status  status() const { return m_status; }
    QString message() const { return m_message; }
    QString icon() const { return m_icon; }
    QString messageId() const { return m_messageId; }
    qint64  clearAt() const { return m_clearAt; }
    bool    isUserDefined() const { return m_userDefined; }
    // Whether the user is in Do Not Disturb, as best we know. Until the first
    // answer from the server arrives after startup the status is unknown (it
    // reads Offline), so this returns what the LAST run knew: relaunching TalQ
    // must not start out ringing and popping up for a user who is in DND. The
    // first server answer corrects it. Always use this, not status() == Dnd.
    bool    isDoNotDisturb() const { return m_statusLoaded ? m_status == Status::Dnd : m_lastKnownDnd; }
    const QVector<Predefined> &predefinedStatuses() const { return m_predefined; }

    // Read the status from the server right now, then call `done` - exactly once,
    // whatever happens: it fires when the answer has been applied, when the read
    // fails, or after a short cap (a few seconds), and at once if the status was
    // read a few seconds ago. Used before an incoming call is REFUSED for Do Not
    // Disturb, so a DND that was switched off elsewhere (or is only remembered
    // from the last run) does not drop a real call. `done` is therefore always
    // safe to treat as "go ahead and decide".
    void refreshThen(std::function<void()> done);

    // Shared with the contacts' presence dots so own/other dots match.
    static QColor colorFor(Status s);
    static QString label(Status s);
    static QString statusKey(Status s);          // "online"/"away"/...
    static Status  statusFromKey(const QString &k);

public slots:
    // Login/restore: load predefined list + current status, then revert a
    // crash-stuck 'call' status. Logout: reset + stop heartbeat.
    void onLoggedIn();
    void onLoggedOut();

    // bug 13 — restore an auto-set Away to Online immediately when the user is
    // back (call from window activation / local input). No-op unless WE
    // auto-flipped to Away; never touches a user-chosen status.
    void tryRestoreFromAutoAway();

    // Poll the authoritative (per-user, server-side) status so a change made on
    // ANOTHER TalQ/Talk instance of the same account propagates here, then —
    // only when keepAliveOnline and the SERVER truth is Online — re-assert
    // online to keep presence alive. Read-before-write so a stale local
    // "online" can't stomp an Away/DND/custom status another device just set.
    // Driven by the 60 s heartbeat (keepAlive=true), the 20 s read-only status
    // poll and window activation (both false).
    void refreshFromServer(bool keepAliveOnline);
    // Tell the server we are here (status "online", or "away" while TalQ holds
    // an auto-Away / the session is locked) via the user_status heartbeat.
    void sendPresenceHeartbeat();
    // Write the auto-Away marker to disk when it changes.
    void persistAutoAway(bool on);

    void setStatusType(Status s);
    void setPredefined(const QString &messageId, qint64 clearAt);
    void setCustom(const QString &icon, const QString &text, qint64 clearAt);
    void clearStatusMessage();
    // Undo Talk's "call" status automation. Idempotent (404/200) — safe
    // to call after every call-end path, so a glitched server-side
    // clear can't leave the user reading as "in call" indefinitely.
    void revertStuckCall();

signals:
    void statusChanged();
    void predefinedLoaded();
    void error(const QString &msg);

private:
    void fetchPredefined();
    void fetchCurrent();
    // refreshFromServer's body; `done` (may be empty) runs after the answer has
    // been handled on EVERY path, including a failed read.
    void doRefresh(bool keepAliveOnline, std::function<void()> done);
    // Remember (on disk) whether we are in Do Not Disturb, once the status is
    // known, so the next launch can start quiet. See isDoNotDisturb().
    void persistKnownDnd();
    void applyFromJson(const QJsonObject &d);
    void takeSnapshot();
    void rollback();

    ApiClient   *m_api;
    AuthManager *m_auth;

    Status  m_status = Status::Offline;
    // False from startup/logout until the status has really been learned (server
    // answer, or the user choosing one): m_status is then only a placeholder.
    bool    m_statusLoaded = false;
    bool    m_lastKnownDnd = false;     // DND as of the last run; see isDoNotDisturb()
    QElapsedTimer m_sinceLastRead;      // time since the last successful status read (invalid = never)
    QString m_message;
    QString m_icon;
    QString m_messageId;
    qint64  m_clearAt = 0;
    bool    m_userDefined = false;
    // Wall-clock ms of the last LOCAL status write - the user's own, or TalQ's
    // automatic Away flip / restore. refreshFromServer skips applying the server
    // snapshot within ~10 s of this so a stale read - fired by the popover-close
    // window-activation, the 60 s tick or the 20 s status poll BEFORE our own PUT
    // has propagated - can't revert the change just made on THIS device. Bug:
    // "status changed from TalQ doesn't apply / display", 2026-06-04.
    qint64  m_lastUserChangeMs = 0;
    QVector<Predefined> m_predefined;

    // Rollback snapshot for optimistic writes.
    Status  m_snapStatus = Status::Offline;
    QString m_snapMessage, m_snapIcon, m_snapMessageId;
    qint64  m_snapClearAt = 0;
    bool    m_snapUserDefined = false;
    bool    m_snapStatusLoaded = false;   // so a failed write can't leave "loaded" set over a placeholder
    bool    m_snapLastKnownDnd = false;   // ...nor move the remembered DND on a write that never landed

    // Keeps automatic presence alive; never overwrites a user-set state.
    QTimer m_heartbeat;
    // Read-only, much faster poll of the server's status, so a change made on
    // ANOTHER device (Do Not Disturb above all) arrives in seconds. See
    // kStatusPollMs in the .cpp for why this is a poll and not a push.
    QTimer m_statusPoll;
    bool   m_statusPollInFlight = false;

    // ---- Auto-away on Windows idle / lock / sleep ----
    //
    // Polls GetLastInputInfo every 30 s. When idle > threshold AND the
    // user is currently Online (not user-set Away/Dnd/Invisible), flip
    // to Away and remember the auto-flip. On the next input, restore
    // to Online if the away was auto-set.
    //
    // NOTE: lock / unlock / sleep / resume are currently handled best-effort by
    // the 30 s GetLastInputInfo poll above — WTSRegisterSessionNotification is
    // NOT yet wired (onSessionLocked/onSessionUnlocked are defined but never
    // connected), so m_sessionLocked stays false at runtime. Follow-up.
    QTimer m_idlePoll;
    bool   m_autoAwayActive = false;   // true while we hold an auto-flipped Away
    // m_autoAwayActive, as last written to disk. TalQ's auto-Away is written to
    // the server as a USER-DEFINED Away, which the server never clears or
    // lets a heartbeat override; without remembering it across a restart, a
    // TalQ that quit while idle left the user showing Away for good.
    bool   m_autoAwayPersisted = false;
    bool   m_sessionLocked  = false;
    bool   m_inIdleTick     = false;   // reentrancy guard (nested event loops)
public:
    // Idle threshold in milliseconds. 5 min matches upstream NC Talk web.
    // Public so tests/integrations can tighten it.
    static constexpr qint64 kIdleAwayThresholdMs = 5 * 60 * 1000;
private:
    // Per-tick: read GetLastInputInfo, decide flip / restore.
    void onIdleTick();
    // Intended to be driven by WTSRegisterSessionNotification (Windows) to flip
    // Away immediately on lock and restore on unlock. NOT yet connected — see
    // the m_idlePoll note above; today lock/unlock is covered by the idle poll.
    void onSessionLocked();
    void onSessionUnlocked();
};
