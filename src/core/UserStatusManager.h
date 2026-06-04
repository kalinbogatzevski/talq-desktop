#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QTimer>

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
    const QVector<Predefined> &predefinedStatuses() const { return m_predefined; }

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
    // Driven by the 60 s heartbeat (keepAlive=true) + window activation (false).
    void refreshFromServer(bool keepAliveOnline);

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
    void applyFromJson(const QJsonObject &d);
    void takeSnapshot();
    void rollback();

    ApiClient   *m_api;
    AuthManager *m_auth;

    Status  m_status = Status::Offline;
    QString m_message;
    QString m_icon;
    QString m_messageId;
    qint64  m_clearAt = 0;
    bool    m_userDefined = false;
    // Wall-clock ms of the last LOCAL (user-initiated) status write.
    // refreshFromServer skips applying the server snapshot within ~10 s of this
    // so a stale read — fired by the popover-close window-activation (or the 60 s
    // tick) BEFORE our own PUT has propagated — can't revert the change the user
    // just made on THIS device. Bug: "status changed from TalQ doesn't apply /
    // display", 2026-06-04.
    qint64  m_lastUserChangeMs = 0;
    QVector<Predefined> m_predefined;

    // Rollback snapshot for optimistic writes.
    Status  m_snapStatus = Status::Offline;
    QString m_snapMessage, m_snapIcon, m_snapMessageId;
    qint64  m_snapClearAt = 0;
    bool    m_snapUserDefined = false;

    // Keeps automatic presence alive; never overwrites a user-set state.
    QTimer m_heartbeat;

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
