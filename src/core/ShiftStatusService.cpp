#include "ShiftStatusService.h"

#include "core/CtiService.h"
#include "core/TalqLog.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <string>
#include <vector>

namespace {

// Shift state moves on the order of minutes -- a roster is a day, a break is
// tens of minutes. Polling faster would cost the ERP a query per client per
// tick and tell nobody anything new.
//
// Deliberately NOT piggybacked on ConversationListModel::fetchUserStatuses():
// that path already runs more often than its own 60 s timer implies (every
// refresh() ends by calling it, and the 30 s fallback timer fires refresh()),
// so riding it would roughly triple this feature's request rate for no gain.
constexpr int kPollMs = 120000;

bool isLoopback(const QUrl &url)
{
    const QString h = url.host().toLower();
    return h == QLatin1String("localhost") || h == QLatin1String("127.0.0.1")
           || h == QLatin1String("::1");
}

} // namespace

ShiftStatusService::ShiftStatusService(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
    m_timer.setInterval(kPollMs);
    connect(&m_timer, &QTimer::timeout, this, &ShiftStatusService::fetchDue);
}

void ShiftStatusService::start()
{
    // A fresh pairing deserves a fresh chance: clear the latch so a previously
    // revoked key that has since been re-paired starts working again without
    // an app restart.
    m_authRejected = false;
    if (!m_timer.isActive())
        m_timer.start();
    fetchDue();
}

void ShiftStatusService::stop()
{
    m_timer.stop();
}

talq::ShiftState ShiftStatusService::stateFor(const QString &userId) const
{
    const auto it = m_cache.constFind(userId);
    return it == m_cache.constEnd() ? talq::ShiftState::Unknown : it->state;
}

QString ShiftStatusService::labelFor(const QString &userId) const
{
    const auto it = m_cache.constFind(userId);
    return it == m_cache.constEnd() ? QString() : it->label;
}

void ShiftStatusService::observe(const QStringList &userIds)
{
    bool addedSomethingUnknown = false;
    for (const QString &id : userIds) {
        if (id.isEmpty())
            continue;
        m_observed.insert(id);
        if (!m_cache.contains(id))
            addedSomethingUnknown = true;
    }
    // Opening a conversation should not wait up to two minutes for the first
    // answer, but a scroll that reveals only already-known colleagues must not
    // trigger a request either.
    if (addedSomethingUnknown)
        fetchDue();
}

void ShiftStatusService::fetchDue()
{
    if (m_inFlight || m_authRejected)
        return;

    const QUrl base = CtiService::erpBaseUrl();
    if (base.isEmpty() || CtiService::token().isEmpty())
        return;   // unconfigured or unpaired: dormant, not broken

    // This request carries the agent's own credential. The https-or-loopback
    // gate exists on exactly ONE of CtiService's three ERP calls, so there is
    // nothing here to inherit it from -- plain http off-loopback would leak a
    // working ERP credential to anyone on the path.
    if (base.scheme().toLower() != QLatin1String("https") && !isLoopback(base)) {
        TWARN("shift-status skipped: ERP address must use https://");
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    std::vector<std::string> want;
    want.reserve(static_cast<std::size_t>(m_observed.size()));
    for (const QString &id : m_observed) {
        const auto it = m_cache.constFind(id);
        if (it == m_cache.constEnd() || talq::shiftEntryIsStale(it->fetchedAtMs, now))
            want.push_back(id.toStdString());
    }

    const std::vector<std::string> batch = talq::buildShiftBatch(want);
    if (batch.empty())
        return;

    QStringList asked;
    QJsonArray names;
    asked.reserve(static_cast<int>(batch.size()));
    for (const std::string &s : batch) {
        const QString id = QString::fromStdString(s);
        asked << id;
        names.append(id);
    }

    // String concatenation, NOT QUrl::resolved(). A site's ERP base may carry
    // a path segment (a locale prefix, say), and resolved() with an
    // absolute-path child silently discards it -- which shows up much later as
    // a redirect that downgrades the POST to a GET.
    QString endpoint = base.toString();
    while (endpoint.endsWith(QLatin1Char('/')))
        endpoint.chop(1);
    endpoint += QStringLiteral("/api/v1/hr/shift-status");

    QNetworkRequest req(QUrl{endpoint});
    // X-API-Key, NOT "Authorization: Bearer". Some deployments strip the
    // Authorization header before the application sees it, and the request
    // then fails as a permission error rather than an auth one -- which looks
    // exactly like "this colleague has no data". Same rule as the CTI lookup.
    req.setRawHeader("X-API-Key", CtiService::token().toUtf8());
    req.setRawHeader("Accept", "application/json");
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    req.setTransferTimeout(15'000);

    const QJsonObject payload{{QStringLiteral("nc_usernames"), names}};

    m_inFlight = true;
    QNetworkReply *reply = m_net->post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, asked]() {
        reply->deleteLater();
        m_inFlight = false;

        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401 || status == 403) {
            // Go quiet for the session. Everything stays Unknown, which draws
            // nothing -- indistinguishable from never having asked, which is
            // exactly the intent.
            TWARN("shift-status: credential rejected, no further polling this session");
            m_authRejected = true;
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            TLOG_NET("shift-status lookup failed:" << reply->errorString());
            return;   // fail closed and silent; the cache keeps what it had
        }
        applyReply(reply->readAll(), asked);
    });
}

void ShiftStatusService::applyReply(const QByteArray &body, const QStringList &asked)
{
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    const QJsonObject data = root.contains(QStringLiteral("data"))
                                 ? root.value(QStringLiteral("data")).toObject()
                                 : root;
    const QJsonObject statuses = data.value(QStringLiteral("statuses")).toObject();

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;

    // Stamp EVERY name we asked about, including any the server omitted.
    // Without this an unmapped colleague would never acquire a fetchedAtMs,
    // stay permanently stale, and be re-requested on every single tick.
    for (const QString &id : asked) {
        Entry entry;
        entry.fetchedAtMs = now;

        const QJsonObject o = statuses.value(id).toObject();
        entry.state = talq::shiftStateFromWire(
            o.value(QStringLiteral("state")).toString().toStdString());
        entry.label = QString::fromStdString(talq::sanitizeShiftLabel(
            o.value(QStringLiteral("label")).toString().toStdString()));

        const auto it = m_cache.constFind(id);
        if (it == m_cache.constEnd() || it->state != entry.state || it->label != entry.label)
            changed = true;
        m_cache.insert(id, entry);
    }

    if (changed)
        emit statusesChanged();
}
