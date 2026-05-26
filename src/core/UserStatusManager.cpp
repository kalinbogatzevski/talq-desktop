#include "core/UserStatusManager.h"
#include "core/ApiClient.h"
#include "core/AuthManager.h"

#include <QColor>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QGuiApplication>
#include <QScopeGuard>
#include <QSessionManager>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
inline QString statusPath(const QString &suffix = QString())
{
    return QStringLiteral("apps/user_status/api/v1/user_status") + suffix;
}
const QString kPredefined = QStringLiteral("apps/user_status/api/v1/predefined_statuses");
}

UserStatusManager::UserStatusManager(ApiClient *api, AuthManager *auth, QObject *parent)
    : QObject(parent)
    , m_api(api)
    , m_auth(auth)
{
    m_heartbeat.setInterval(120000);
    connect(&m_heartbeat, &QTimer::timeout, this, [this]() {
        // Keep automatic presence alive without ever stomping a user-set
        // Away/DND/Invisible. PUT .../status only changes the type, never
        // the message, so an online re-assert can't wipe a custom message.
        if (m_status == Status::Online) {
            QJsonObject body;
            body["statusType"] = "online";
            m_api->put(statusPath(QStringLiteral("/status")), body,
                       [](bool, const QJsonObject &, int) {});
        }
    });

    // Idle-poll wakeup. 30 s cadence is the upstream NC Talk web idle
    // tick; on a 5 min threshold that means worst-case 5:00..5:30 before
    // the auto-away flips. Started in onLoggedIn, stopped in onLoggedOut.
    m_idlePoll.setInterval(30000);
    connect(&m_idlePoll, &QTimer::timeout, this, &UserStatusManager::onIdleTick);

#ifdef Q_OS_WIN
    // Listen for session lock / sleep transitions so we react immediately
    // (no 30 s tick wait). Qt's QSessionManager doesn't fire for Windows
    // workstation lock - that's a WTS notification only delivered via
    // window messages. The QGuiApplication-level signals we DO have:
    // applicationStateChanged (focus + visibility) is too noisy for this,
    // and commitDataRequest fires only on shutdown. So on Windows we
    // additionally poll Qt's PowerNotification via QGuiApplication; for
    // a richer signal a separate native event filter would be needed,
    // tracked as a follow-up. For now: rely on the 30 s idle poll and
    // catch sleep/resume via the same poll's "GetLastInputInfo returned
    // a value older than 30 s" path.
#endif
}

QString UserStatusManager::statusKey(Status s)
{
    switch (s) {
    case Status::Online:    return QStringLiteral("online");
    case Status::Away:      return QStringLiteral("away");
    case Status::Dnd:       return QStringLiteral("dnd");
    case Status::Invisible: return QStringLiteral("invisible");
    case Status::Offline:   return QStringLiteral("offline");
    }
    return QStringLiteral("offline");
}

UserStatusManager::Status UserStatusManager::statusFromKey(const QString &k)
{
    if (k == QLatin1String("online"))    return Status::Online;
    if (k == QLatin1String("away"))      return Status::Away;
    if (k == QLatin1String("dnd"))       return Status::Dnd;
    if (k == QLatin1String("invisible")) return Status::Invisible;
    return Status::Offline;
}

QString UserStatusManager::label(Status s)
{
    switch (s) {
    case Status::Online:    return tr("Online");
    case Status::Away:      return tr("Away");
    case Status::Dnd:       return tr("Do not disturb");
    case Status::Invisible: return tr("Invisible");
    case Status::Offline:   return tr("Offline");
    }
    return tr("Offline");
}

QColor UserStatusManager::colorFor(Status s)
{
    // Warm palette, consistent with the contacts' presence dots
    // (SidebarPainter: online green, away amber, dnd warm clay).
    switch (s) {
    case Status::Online:    return QColor(0x4c, 0xaf, 0x7d);
    case Status::Away:      return QColor(0xf0, 0xa0, 0x50);
    case Status::Dnd:       return QColor(0xd9, 0x69, 0x4c);
    case Status::Invisible: return QColor(0x8a, 0x86, 0x80);
    case Status::Offline:   return QColor(0x8a, 0x86, 0x80);
    }
    return QColor(0x8a, 0x86, 0x80);
}

void UserStatusManager::onLoggedIn()
{
    fetchPredefined();
    fetchCurrent();
    // Order matters only for correctness, not race: revert is idempotent
    // and re-fetches on success, so a stuck 'call' status is corrected
    // even if it loaded into m_status a moment ago.
    revertStuckCall();
    m_heartbeat.start();
    m_idlePoll.start();
}

void UserStatusManager::onLoggedOut()
{
    m_heartbeat.stop();
    m_idlePoll.stop();
    m_autoAwayActive = false;
    m_sessionLocked  = false;
    m_inIdleTick     = false;
    m_status = Status::Offline;
    m_message.clear();
    m_icon.clear();
    m_messageId.clear();
    m_clearAt = 0;
    m_userDefined = false;
    emit statusChanged();
}

void UserStatusManager::onIdleTick()
{
    // Reentrancy guard: a synchronous statusChanged listener could pump
    // a nested event loop (modal dialog), and the next QTimer firing on
    // that nested loop would re-enter onIdleTick with mid-transition
    // state. The guard returns early in that case; the outer call's
    // remaining logic finishes the transition.
    if (m_inIdleTick) return;
    m_inIdleTick = true;
    auto resetGuard = qScopeGuard([this]{ m_inIdleTick = false; });

    // Only auto-flip if the user is currently Online (we never touch a
    // user-set Away/Dnd/Invisible/Offline - that would override an
    // intentional choice). We also need to check m_autoAwayActive for
    // the restore-on-return branch.
    if (m_status != Status::Online && !m_autoAwayActive) return;

#ifdef Q_OS_WIN
    LASTINPUTINFO lii{};
    lii.cbSize = sizeof(lii);
    if (!GetLastInputInfo(&lii)) return;
    const DWORD nowTicks  = GetTickCount();
    const DWORD idleTicks = nowTicks - lii.dwTime;   // modular unsigned, handles 49.7-d wrap
    const qint64 idleMs   = qint64(idleTicks);
#else
    // Non-Windows: no idle source plumbed today. Explicit return so the
    // dead comparisons below never run.
    return;
#endif

#ifdef Q_OS_WIN
    if (!m_autoAwayActive && idleMs >= kIdleAwayThresholdMs
        && m_status == Status::Online) {
        // Flip up to Away. We hardcode the restore-target to Online
        // (the only currently-writeable pre-state); a forward-defensive
        // m_preAutoStatus member was over-engineered and removed per
        // PR-review feedback.
        // Optimistic UI update + server-driven rollback. setStatusType
        // already established the pattern; mirror it here so a 401/5xx
        // doesn't leave local state showing Away while the server is
        // still Online.
        const Status priorStatus = m_status;
        const bool   priorAuto   = m_autoAwayActive;
        m_status         = Status::Away;
        m_autoAwayActive = true;
        emit statusChanged();
        qInfo() << "UserStatusManager: idle" << idleMs / 1000 << "s -> auto-Away";

        QJsonObject body;
        body["statusType"] = "away";
        m_api->put(statusPath(QStringLiteral("/status")), body,
                   [this, priorStatus, priorAuto](bool ok, const QJsonObject &, int) {
            if (!ok) {
                m_status         = priorStatus;
                m_autoAwayActive = priorAuto;
                emit statusChanged();
                qWarning() << "UserStatusManager: auto-Away PUT failed, "
                              "reverting local state";
            }
        });
    } else if (m_autoAwayActive && idleMs < kIdleAwayThresholdMs
               && !m_sessionLocked) {
        // User came back. Restore to Online. setStatusType clears
        // m_autoAwayActive when the user makes an explicit choice, so
        // reaching here means the Away really was our auto-flip.
        const Status priorStatus = m_status;
        const bool   priorAuto   = m_autoAwayActive;
        m_status         = Status::Online;
        m_autoAwayActive = false;
        emit statusChanged();
        qInfo() << "UserStatusManager: user returned, restoring to Online";

        QJsonObject body;
        body["statusType"] = "online";
        m_api->put(statusPath(QStringLiteral("/status")), body,
                   [this, priorStatus, priorAuto](bool ok, const QJsonObject &, int) {
            if (!ok) {
                m_status         = priorStatus;
                m_autoAwayActive = priorAuto;
                emit statusChanged();
                qWarning() << "UserStatusManager: auto-Away restore PUT failed, "
                              "reverting local state - will retry next tick";
            }
        });
    }
#endif
}

void UserStatusManager::onSessionLocked()
{
    // NOT wired to a Qt signal in 0.39.7 - declared for the WTS
    // notification follow-up. The 30 s idle poll covers lock/sleep
    // best-effort via GetLastInputInfo's natural climb (no input ->
    // idle rises -> auto-Away). Listed in the changelog as a known
    // gap to be filled by a native event filter in a later beta.
    m_sessionLocked = true;
    if (m_status != Status::Online) return;
    const Status priorStatus = m_status;
    const bool   priorAuto   = m_autoAwayActive;
    m_status         = Status::Away;
    m_autoAwayActive = true;
    emit statusChanged();
    QJsonObject body;
    body["statusType"] = "away";
    m_api->put(statusPath(QStringLiteral("/status")), body,
               [this, priorStatus, priorAuto](bool ok, const QJsonObject &, int) {
        if (!ok) {
            m_status         = priorStatus;
            m_autoAwayActive = priorAuto;
            emit statusChanged();
        }
    });
}

void UserStatusManager::onSessionUnlocked()
{
    m_sessionLocked = false;
    // Don't restore eagerly here - the idle poll's next tick will see
    // that we're below threshold and put us back to Online. That keeps
    // the "we're back" decision in one place.
}

void UserStatusManager::fetchPredefined()
{
    m_api->getArray(kPredefined, [this](bool ok, const QJsonArray &arr, int) {
        if (!ok) return;  // non-fatal: popover just won't show presets
        m_predefined.clear();
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            Predefined p;
            p.id      = o.value(QStringLiteral("id")).toString();
            p.icon    = o.value(QStringLiteral("icon")).toString();
            p.message = o.value(QStringLiteral("message")).toString();
            if (!p.id.isEmpty()) m_predefined.push_back(p);
        }
        emit predefinedLoaded();
    });
}

void UserStatusManager::fetchCurrent()
{
    m_api->get(statusPath(), [this](bool ok, const QJsonObject &d, int) {
        if (!ok) return;  // keep whatever we had; non-fatal
        applyFromJson(d);
        emit statusChanged();
    });
}

void UserStatusManager::revertStuckCall()
{
    // Undo a crash-stuck Talk 'call' automation: restores the pre-call
    // backup server-side. Harmless (404/200) if nothing was stuck.
    // Caller-side timing race (the prior bug) is fixed at the source —
    // CallManager emits callEnded only AFTER the leaveCall DELETE /call
    // has been acknowledged by the server, so by the time we get here
    // the server has already processed the call transition and the
    // pre-call snapshot is the next thing to restore.
    // Must-complete: the revert can be lost on a high-latency/flaky link
    // (NC server in ZA, user in BG), leaving the status stuck "In a call".
    // delMustComplete retries on confirmed non-delivery (status 0 / 5xx),
    // bounded, non-blocking. 4xx (incl. 404) = nothing stuck → done.
    m_api->delMustComplete(statusPath(QStringLiteral("/revert/call")), {}, this,
        [this](bool ok, int statusCode) {
            if (ok) {
                qInfo() << "UserStatus: reverted stuck 'call' status";
                fetchCurrent();
            } else {
                qInfo() << "UserStatus: no stuck 'call' status to revert "
                           "(status" << statusCode << ")";
            }
        });
}

void UserStatusManager::applyFromJson(const QJsonObject &d)
{
    m_status      = statusFromKey(d.value(QStringLiteral("status")).toString());
    m_message     = d.value(QStringLiteral("message")).toString();
    m_icon        = d.value(QStringLiteral("icon")).toString();
    m_messageId   = d.value(QStringLiteral("messageId")).toString();
    m_clearAt     = d.value(QStringLiteral("clearAt")).toVariant().toLongLong();
    m_userDefined = d.value(QStringLiteral("statusIsUserDefined")).toBool();
}

void UserStatusManager::takeSnapshot()
{
    m_snapStatus      = m_status;
    m_snapMessage     = m_message;
    m_snapIcon        = m_icon;
    m_snapMessageId   = m_messageId;
    m_snapClearAt     = m_clearAt;
    m_snapUserDefined = m_userDefined;
}

void UserStatusManager::rollback()
{
    m_status      = m_snapStatus;
    m_message     = m_snapMessage;
    m_icon        = m_snapIcon;
    m_messageId   = m_snapMessageId;
    m_clearAt     = m_snapClearAt;
    m_userDefined = m_snapUserDefined;
    emit statusChanged();
}

void UserStatusManager::setStatusType(Status s)
{
    // A user-driven status change always wins over the auto-away
    // tracker; clear the auto flag so the next idle tick doesn't try
    // to "restore" their intentional choice to Online.
    m_autoAwayActive = false;
    takeSnapshot();
    m_status = s;
    m_userDefined = true;
    emit statusChanged();  // optimistic

    QJsonObject body;
    body["statusType"] = statusKey(s);
    m_api->put(statusPath(QStringLiteral("/status")), body,
        [this](bool ok, const QJsonObject &, int) {
            if (!ok) { rollback(); emit error(tr("Couldn't update status — try again")); }
        });
}

void UserStatusManager::setPredefined(const QString &messageId, qint64 clearAt)
{
    takeSnapshot();
    m_messageId = messageId;
    for (const auto &p : m_predefined) {
        if (p.id == messageId) { m_message = p.message; m_icon = p.icon; break; }
    }
    m_clearAt = clearAt;
    m_userDefined = true;
    emit statusChanged();

    QJsonObject body;
    body["messageId"] = messageId;
    body["clearAt"]   = clearAt > 0 ? QJsonValue(static_cast<double>(clearAt))
                                     : QJsonValue();
    m_api->put(statusPath(QStringLiteral("/message/predefined")), body,
        [this](bool ok, const QJsonObject &, int) {
            if (!ok) { rollback(); emit error(tr("Couldn't set status message — try again")); }
        });
}

void UserStatusManager::setCustom(const QString &icon, const QString &text, qint64 clearAt)
{
    takeSnapshot();
    m_icon = icon;
    m_message = text;
    m_messageId.clear();
    m_clearAt = clearAt;
    m_userDefined = true;
    emit statusChanged();

    QJsonObject body;
    if (!icon.isEmpty()) body["statusIcon"] = icon;
    body["message"] = text;
    body["clearAt"] = clearAt > 0 ? QJsonValue(static_cast<double>(clearAt))
                                   : QJsonValue();
    m_api->put(statusPath(QStringLiteral("/message/custom")), body,
        [this](bool ok, const QJsonObject &, int) {
            if (!ok) { rollback(); emit error(tr("Couldn't set status message — try again")); }
        });
}

void UserStatusManager::clearStatusMessage()
{
    takeSnapshot();
    m_message.clear();
    m_icon.clear();
    m_messageId.clear();
    m_clearAt = 0;
    emit statusChanged();

    m_api->del(statusPath(QStringLiteral("/message")),
        [this](bool ok, const QJsonObject &, int) {
            if (!ok) { rollback(); emit error(tr("Couldn't clear status — try again")); }
        });
}
