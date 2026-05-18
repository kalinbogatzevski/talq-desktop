#include "core/UserStatusManager.h"
#include "core/ApiClient.h"
#include "core/AuthManager.h"

#include <QColor>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

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
}

void UserStatusManager::onLoggedOut()
{
    m_heartbeat.stop();
    m_status = Status::Offline;
    m_message.clear();
    m_icon.clear();
    m_messageId.clear();
    m_clearAt = 0;
    m_userDefined = false;
    emit statusChanged();
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
    m_api->del(statusPath(QStringLiteral("/revert/call")),
        [this](bool ok, const QJsonObject &, int statusCode) {
            if (ok) {
                qInfo() << "UserStatus: reverted stuck 'call' status";
                fetchCurrent();
            } else {
                qInfo() << "UserStatus: no stuck 'call' status to revert (status"
                        << statusCode << ")";
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
