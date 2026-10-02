#include "core/UserStatusManager.h"
#include "core/ApiClient.h"
#include "core/AuthManager.h"
#include "core/TalqLog.h"

#include <QColor>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QGuiApplication>
#include <QPalette>
#include <QScopeGuard>
#include <QSettings>
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
const QString kHeartbeat     = QStringLiteral("apps/user_status/api/v1/heartbeat");
const QString kAutoAwayKey   = QStringLiteral("userStatus/autoAwaySetByTalq");
const QString kPredefined = QStringLiteral("apps/user_status/api/v1/predefined_statuses");
}

UserStatusManager::UserStatusManager(ApiClient *api, AuthManager *auth, QObject *parent)
    : QObject(parent)
    , m_api(api)
    , m_auth(auth)
{
    m_heartbeat.setInterval(60000);
    connect(&m_heartbeat, &QTimer::timeout, this, [this]() {
        // Each cycle: pull the authoritative status (so a change on ANOTHER
        // instance of this account shows up here within ~60 s) and keep our
        // online presence alive — read-before-write so we never re-assert a
        // stale local "online" over an Away/DND/custom another device just set.
        refreshFromServer(/*keepAliveOnline=*/true);
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
    // Was a fixed 5-hex palette, identical across all four themes, so it
    // silently diverged from the SAME contact's presence dot drawn by
    // SidebarPainter (m_theme.online/amber/danger, per-theme) -- e.g. on
    // Paper this returned a green/amber/clay that matched none of Paper's
    // actual tokens. Route through PainterTheme instead: MainWindow's
    // applyDarkPalette() (re-run on every theme switch, not just at start-up)
    // publishes online/amber/danger into three otherwise-unused QPalette
    // roles as a theme-token bag, so this stays a plain static function with
    // no PainterTheme/theme-id plumbing threaded through its two call sites,
    // while still tracking all four themes live from one source of truth.
    // Invisible/Offline keep the original neutral (textMuted -- same warm
    // "faintest" tone the old #8a8680 approximated, but per-theme correct).
    const QPalette p = QGuiApplication::palette();
    switch (s) {
    case Status::Online:    return p.color(QPalette::Link);         // theme.online
    case Status::Away:      return p.color(QPalette::LinkVisited);  // theme.amber
    case Status::Dnd:       return p.color(QPalette::BrightText);   // theme.danger
    case Status::Invisible: return p.color(QPalette::PlaceholderText); // theme.textMuted
    case Status::Offline:   return p.color(QPalette::PlaceholderText);
    }
    return p.color(QPalette::PlaceholderText);
}

void UserStatusManager::onLoggedIn()
{
    fetchPredefined();
    // revertStuckCall() now fetches + applies the current status first, then
    // undoes ONLY a stuck Talk "in a call" status (messageId=="call"). It
    // doubles as the initial status load and can never clobber a user-chosen
    // status, so no separate fetchCurrent() is needed here (it would just be a
    // redundant second GET).
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
                return;
            }
            persistAutoAway(true);
        });
    } else if (m_autoAwayActive && idleMs < kIdleAwayThresholdMs
               && !m_sessionLocked) {
        // OS-wide idle dropped below threshold — the user is back. (bug 13:
        // this is also reached IMMEDIATELY via tryRestoreFromAutoAway() on
        // TalQ window activation / local input, so the restore no longer waits
        // up to 30 s for this poll tick.)
        tryRestoreFromAutoAway();
    }
#endif
}

void UserStatusManager::tryRestoreFromAutoAway()
{
    // bug 13 — restore an AUTO-set Away back to Online the moment the user is
    // demonstrably back (TalQ window activated or local input). No-op unless
    // WE auto-flipped to Away: a user-chosen Away/DND/Invisible never sets
    // m_autoAwayActive (setStatusType clears it), so this never overrides an
    // intentional status. Cross-platform: the caller's activity is the
    // "user is back" signal, so no OS idle query is needed here.
    if (m_sessionLocked) return;
    // Only an Away can be restored. If the status has moved on without us -- most
    // importantly Do Not Disturb set on another device while TalQ held its
    // auto-Away -- the marker is stale, and writing Online here would silently
    // end the user's DND on every device. Drop the marker and leave the status.
    if (m_status != Status::Away) {
        if (m_autoAwayActive) {
            m_autoAwayActive = false;
            persistAutoAway(false);
        }
        return;
    }
    // bug 13 (broadened) — restore on activity for ANY *automatic* Away, not
    // just one WE set: either m_autoAwayActive (TalQ flipped it) OR the status
    // is Away and NOT user-defined (the server's presence-based auto-away, or
    // another device's auto-away). A user-chosen Away/DND/Invisible has
    // statusIsUserDefined=true and is never touched.
    const bool autoAway = m_autoAwayActive
                       || (m_status == Status::Away && !m_userDefined);
    if (!autoAway)
        return;   // no-op (the common case) — log NOTHING: this runs on every
                  // mouse/key/wheel input, so the old unconditional [BUG13]
                  // trace flooded the debug log (2026-06-04).
    if (TalqLog::g_verbose)
        qInfo().nospace() << "[BUG13] tryRestore RESTORING auto-Away -> Online "
                          << "(status=" << static_cast<int>(m_status)
                          << " autoFlag=" << m_autoAwayActive
                          << " userDefined=" << m_userDefined << ")";
    const Status priorStatus = m_status;
    m_status         = Status::Online;
    m_autoAwayActive = false;
    emit statusChanged();
    qInfo() << "UserStatusManager: user active again — restoring to Online";

    QJsonObject body;
    body["statusType"] = "online";
    m_api->put(statusPath(QStringLiteral("/status")), body,
               [this, priorStatus](bool ok, const QJsonObject &, int) {
        if (!ok) {
            m_status         = priorStatus;
            m_autoAwayActive = true;
            emit statusChanged();
            qWarning() << "UserStatusManager: auto-Away restore PUT failed — "
                          "reverting local state, will retry on next activity/tick";
            return;
        }
        persistAutoAway(false);
    });
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
            return;
        }
        persistAutoAway(true);
    });
}

void UserStatusManager::onSessionUnlocked()
{
    m_sessionLocked = false;
    // bug 13 — restore immediately on unlock instead of waiting for the next
    // idle tick (unlocking IS "the user is back").
    tryRestoreFromAutoAway();
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

void UserStatusManager::refreshFromServer(bool keepAliveOnline)
{
    m_api->get(statusPath(), [this, keepAliveOnline](bool ok, const QJsonObject &d, int) {
        if (!ok) return;   // transient; try again next cycle
        // Don't let a stale read revert a status the user JUST changed on this
        // device: closing the status popover re-activates the window (and the
        // 60 s tick can land) and fires a GET whose response can predate our own
        // PUT propagating. Within the grace window, trust the local optimistic
        // state + the in-flight PUT; cross-device sync resumes right after.
        if (QDateTime::currentMSecsSinceEpoch() - m_lastUserChangeMs < 10000) {
            if (TalqLog::g_verbose)
                qDebug() << "UserStatus: refresh skipped (recent local change)";
            return;
        }
        const Status  prevStatus    = m_status;
        const QString prevMessage   = m_message;
        const QString prevIcon      = m_icon;
        const QString prevMessageId = m_messageId;
        applyFromJson(d);
        // Multi-instance sync: NC user_status is per-USER, so this GET returns
        // whatever the newest write (this device or another) left. Repaint only
        // on a real change to avoid needless churn.
        if (m_status != prevStatus || m_message != prevMessage
            || m_icon != prevIcon || m_messageId != prevMessageId) {
            emit statusChanged();
            qInfo() << "UserStatus: refreshed from server (status now"
                    << statusKey(m_status) << "msg=" << m_message << ")";
        }
        // Presence: every cycle, whatever the status we just read. The old
        // keep-alive re-PUT "online" only while the server already said
        // Online — but the server's cleanup job turns every status not
        // refreshed for 15 minutes to Offline (any sleep, closed lid, restart
        // or network gap), and from then on that guard never fired again:
        // the user showed Offline to everyone, all day, while using TalQ.
        // The heartbeat is the server's own presence call and is safe to
        // send unconditionally: it never overrides an Away / Busy / DND /
        // Invisible the user chose (UserLiveStatusListener, PERSISTENT_STATUSES).
        if (keepAliveOnline)
            sendPresenceHeartbeat();
    });
}

void UserStatusManager::sendPresenceHeartbeat()
{
    // A status the user chose themselves (not TalQ's auto-Away) is never
    // changed by a heartbeat; the server answers 204 with no body, which the
    // API layer logs as invalid JSON once a minute. Nothing to say: skip it.
    if (m_userDefined && !m_autoAwayActive && m_status != Status::Online
        && m_status != Status::Offline)
        return;
    QJsonObject body;
    body["status"] = (m_autoAwayActive || m_sessionLocked) ? "away" : "online";
    m_api->put(kHeartbeat, body, [this](bool ok, const QJsonObject &d, int) {
        // 204 = nothing changed; 200 carries the resulting status.
        if (!ok || !d.contains(QStringLiteral("status")))
            return;
        if (QDateTime::currentMSecsSinceEpoch() - m_lastUserChangeMs < 10000)
            return;   // a local change is in flight; same grace as the refresh
        const Status prev = m_status;
        m_status = statusFromKey(d.value(QStringLiteral("status")).toString());
        if (m_status == Status::Online)
            m_autoAwayActive = false;
        if (m_status != prev) {
            emit statusChanged();
            qInfo() << "UserStatus: presence heartbeat ->" << statusKey(m_status);
        }
    });
}

void UserStatusManager::persistAutoAway(bool on)
{
    if (on == m_autoAwayPersisted) return;
    m_autoAwayPersisted = on;
    QSettings s;
    if (on) s.setValue(kAutoAwayKey, true);
    else    s.remove(kAutoAwayKey);
}

void UserStatusManager::revertStuckCall()
{
    // Undo ONLY Talk's stuck "in a call" automation. Talk sets the user_status
    // with messageId "call" (the /revert/call endpoint reverts exactly that id;
    // NO user-selectable predefined — meeting/remote-work/sick-leave/etc — uses
    // it). We MUST gate on that marker: the /message DELETE fallback below
    // clears ANY custom message, so running it unconditionally wiped the user's
    // real status (e.g. "Working remotely") to plain "Online" on every login
    // AND every call-end — the field bug (2026-06-04). So: fetch the current
    // status first; if it isn't the stuck call status, do nothing at all.
    m_api->get(statusPath(), [this](bool ok, const QJsonObject &d, int) {
        if (ok) {
            applyFromJson(d);   // also the initial load
            // An Away that THIS install set automatically before it quit (or
            // restarted to update) is still ours: the server keeps it as a
            // user-defined Away forever. Take it back as auto-Away so the
            // first activity restores Online. Only a plain Away with no
            // message qualifies — anything else was set by a person.
            m_autoAwayPersisted = QSettings().value(kAutoAwayKey, false).toBool();
            if (m_autoAwayPersisted && m_status == Status::Away && m_messageId.isEmpty()
                && m_message.isEmpty()) {
                m_autoAwayActive = true;
                qInfo() << "UserStatus: reclaiming the auto-Away a previous run left";
            } else {
                persistAutoAway(false);
            }
            emit statusChanged();
            // Report presence right away instead of after the first minute.
            sendPresenceHeartbeat();
        }
        if (!ok || m_messageId != QLatin1String("call")) {
            if (TalqLog::g_verbose)
                qDebug().nospace() << "UserStatus: no stuck 'call' status (ok=" << ok
                                   << " messageId=" << m_messageId
                                   << ") — leaving the user's status untouched";
            return;
        }
        qInfo() << "UserStatus: detected stuck 'call' status — reverting";

        // 0.41.4-beta — two-stage clear for the stuck "In a call" status that
        // survives hangup on servers that don't fire the /revert/call hook.
        //   Stage 1: the Talk-specific revert endpoint (restores the pre-call
        //    snapshot on the user_status app side). Works on most NC builds.
        //   Stage 2 (fallback): standard /message DELETE — clears the custom
        //    status Talk set. Field report (NC build in ZA): /revert/call 404s
        //    on that server and the "In a call" message stays sticky. Now safe
        //    to chain because we've confirmed messageId=="call" above.
        m_api->delMustComplete(statusPath(QStringLiteral("/revert/call")), {}, this,
            [this](bool okR, int statusCode) {
                if (okR)
                    qInfo() << "UserStatus: reverted stuck 'call' via /revert/call";
                else
                    qInfo() << "UserStatus: /revert/call did not clear (status"
                            << statusCode << ") — falling back to /message DELETE";
                m_api->delMustComplete(statusPath(QStringLiteral("/message")), {}, this,
                    [this](bool ok2, int statusCode2) {
                        if (ok2)
                            qInfo() << "UserStatus: cleared stuck status via /message DELETE";
                        else
                            qInfo() << "UserStatus: /message DELETE did not clear (status"
                                    << statusCode2 << ") — server state may still be stuck";
                        fetchCurrent();
                    });
            });
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
    // bug 13 — keep the auto-away tracker consistent with the server truth: if
    // the server says we're anything but Away (Online, or Do Not Disturb / Busy /
    // Invisible set on another device), we're no longer in an auto-flipped Away,
    // so clear the flag (otherwise a stale m_autoAwayActive could confuse the
    // next restore/idle decision -- and restore OVER a status the user chose).
    if (m_status != Status::Away) {
        m_autoAwayActive = false;
        persistAutoAway(false);
    }
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
    persistAutoAway(false);
    takeSnapshot();
    m_lastUserChangeMs = QDateTime::currentMSecsSinceEpoch();
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
    m_lastUserChangeMs = QDateTime::currentMSecsSinceEpoch();
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
    m_lastUserChangeMs = QDateTime::currentMSecsSinceEpoch();
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
    m_lastUserChangeMs = QDateTime::currentMSecsSinceEpoch();
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
