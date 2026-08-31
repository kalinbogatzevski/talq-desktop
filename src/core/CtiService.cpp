#include "CtiService.h"

#include "core/CtiClient.h"
#include "core/CtiDefaults.h"
#include "core/TalqLog.h"
#include "ui/CallerCardPopup.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScreen>
#include <QSettings>
#include <QSysInfo>
#include <QTimer>

namespace {

// Only ever hand http/https to the desktop. `url` and `approve_url` arrive
// from a server, and the caller card is an UNPROMPTED popup, so without this
// a compromised or merely careless server could turn one click into
// file://host/share/payload.exe or any other registered URL handler. Mirrors
// the guard already applied to message links in ChatPainter.
bool isLoopback(const QUrl &url)
{
    const QString h = url.host().toLower();
    return h == QLatin1String("localhost") || h == QLatin1String("127.0.0.1")
           || h == QLatin1String("::1");
}

bool isSafeToOpen(const QUrl &url)
{
    if (!url.isValid() || url.host().isEmpty())
        return false;
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("https") || scheme == QLatin1String("http");
}

} // namespace

namespace {

constexpr auto kKeyEnabled   = "cti/enabled";
constexpr auto kKeyServer    = "cti/serverUrl";
constexpr auto kKeyErpBase   = "cti/erpBaseUrl";
constexpr auto kKeyToken     = "cti/token";

constexpr int kCardGap    = 8;
constexpr int kEdgeMargin = 16;

// Pairing polls every 2s for up to 3 minutes, which is generous for "switch to
// the browser, read the prompt, click Approve".
constexpr int kPairPollMs    = 2000;
constexpr int kPairMaxPolls  = 90;

} // namespace

CtiService::CtiService(QObject *parent)
    : QObject(parent)
    , m_client(new CtiClient(this))
    , m_net(new QNetworkAccessManager(this))
{
    connect(m_client, &CtiClient::dialResult, this, &CtiService::dialResult);
    connect(m_client, &CtiClient::ringing, this, &CtiService::onRinging);
    connect(m_client, &CtiClient::ended,   this, &CtiService::onEnded);
    connect(m_client, &CtiClient::disconnected,
            this, &CtiService::onDisconnectedFromDaemon);
    connect(m_client, &CtiClient::authenticationFailed,
            this, &CtiService::onAuthenticationFailed);
    connect(m_client, &CtiClient::ready, this, [this](const QString &ext, const QString &) {
        TLOG_NET("CTI ready on extension" << ext);
        emit statusChanged();
    });
}

CtiService::~CtiService() = default;

// ── Settings ────────────────────────────────────────────────────────────────

bool CtiService::enabledSetting()
{
    // Branded builds default ON. An explicit off is remembered, because
    // QSettings tells "never set" apart from "set to false" -- otherwise
    // turning it off would silently revert on the next launch.
    return QSettings().value(kKeyEnabled, TalQCti::kEnabledByDefault).toBool();
}
void CtiService::setEnabledSetting(bool on) { QSettings().setValue(kKeyEnabled, on); }
QUrl CtiService::serverUrl()
{
    const QUrl set = QSettings().value(kKeyServer).toUrl();
    return set.isEmpty() ? QUrl(QString::fromUtf8(TalQCti::kServerUrl)) : set;
}
void CtiService::setServerUrl(const QUrl &u) { QSettings().setValue(kKeyServer, u); }
QUrl CtiService::erpBaseUrl()
{
    const QUrl set = QSettings().value(kKeyErpBase).toUrl();
    return set.isEmpty() ? QUrl(QString::fromUtf8(TalQCti::kErpBaseUrl)) : set;
}
void CtiService::setErpBaseUrl(const QUrl &u) { QSettings().setValue(kKeyErpBase, u); }
QString CtiService::token()             { return QSettings().value(kKeyToken).toString(); }
void CtiService::setToken(const QString &t)  { QSettings().setValue(kKeyToken, t); }
void CtiService::clearToken()           { QSettings().remove(kKeyToken); }

bool CtiService::isEnabled() const   { return enabledSetting(); }
bool CtiService::isConnected() const { return m_client && m_client->isConnected(); }

bool CtiService::canDial() const
{
    return m_client && m_client->canDial();
}

void CtiService::dial(const QString &number)
{
    if (!m_client) {
        emit dialResult(false, QStringLiteral("not-enabled"));
        return;
    }
    m_client->dial(number);
}
QString CtiService::extension() const { return m_client ? m_client->extension() : QString(); }

// ── Lifecycle ───────────────────────────────────────────────────────────────

void CtiService::start()
{
    if (!enabledSetting() || serverUrl().isEmpty() || token().isEmpty()) {
        stop();
        return;
    }

    // The token sent in the hello frame is the SAME credential used as the ERP
    // bearer, so anyone who can read the frame can read customer records as
    // that agent. Plain ws:// is therefore refused except on loopback, which is
    // only reachable from the machine itself and is how a developer runs a
    // daemon locally.
    const QUrl url = serverUrl();
    if (url.scheme().toLower() != QLatin1String("wss") && !isLoopback(url)) {
        stop();
        TWARN("CTI refused: server address must use wss://");
        emit pairingMessage(tr("Call pop-ups are switched off: the call service "
                               "address must start with wss:// so your details "
                               "are encrypted."), true);
        return;
    }

    m_client->start(url, token());
}

void CtiService::stop()
{
    if (m_client)
        m_client->stop();
    onDisconnectedFromDaemon();
}

void CtiService::setTheme(PainterTheme::Theme theme)
{
    m_theme = theme;
    for (const auto &e : m_cards)
        if (e.card)
            e.card->setTheme(theme);
}


// Turn the server's response into a card.
//
// Two shapes are understood. The CURRENT one is self-describing -- title,
// subtitle, badges, ordered label/value rows, actions -- so the server can add
// a field, or show different fields to different roles, without anyone
// rebuilding this client. The OLDER flat shape (display_name, ucn, is_outage,
// contract_count...) is still read as a fallback, because a server may be
// ahead of or behind any given desktop and neither should break the other.
static CallerCardPopup::CardData parseCard(const QJsonObject &data)
{
    CallerCardPopup::CardData card;
    card.known = data.value(QStringLiteral("known")).toBool();
    if (!card.known)
        return card;

    // The server decides how many rows its card is worth; the client only
    // enforces a ceiling. Absent means "use the client default".
    card.maxFields = data.value(QStringLiteral("max_fields")).toInt();

    card.title    = data.value(QStringLiteral("title")).toString();
    card.subtitle = data.value(QStringLiteral("subtitle")).toString();

    for (const QJsonValue &v : data.value(QStringLiteral("badges")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString text = o.value(QStringLiteral("text")).toString();
        if (!text.isEmpty())
            card.badges.append({ text, o.value(QStringLiteral("style")).toString() });
    }

    for (const QJsonValue &v : data.value(QStringLiteral("fields")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString label = o.value(QStringLiteral("label")).toString();
        const QString value = o.value(QStringLiteral("value")).toString();
        // A row with neither half is noise, not information.
        if (label.isEmpty() && value.isEmpty())
            continue;
        card.fields.append({ label, value, o.value(QStringLiteral("style")).toString() });
    }

    for (const QJsonValue &v : data.value(QStringLiteral("actions")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString label = o.value(QStringLiteral("label")).toString();
        const QString href  = o.value(QStringLiteral("url")).toString();
        if (label.isEmpty() || href.isEmpty())
            continue;
        card.actions.append({ label, QUrl(href) });
    }

    // ── Fallback: the pre-self-describing flat shape ────────────────────────
    if (card.title.isEmpty())
        card.title = data.value(QStringLiteral("display_name")).toString();

    if (card.subtitle.isEmpty()) {
        QStringList parts;
        const QString company = data.value(QStringLiteral("company")).toString();
        const QString ucn     = data.value(QStringLiteral("ucn")).toString();
        if (!company.isEmpty()) parts << company;
        if (!ucn.isEmpty())     parts << ucn;
        card.subtitle = parts.join(QStringLiteral("  ·  "));
    }

    if (card.badges.isEmpty() && data.value(QStringLiteral("is_outage")).toBool())
        card.badges.append({ QObject::tr("OUTAGE"), QStringLiteral("danger") });

    if (card.fields.isEmpty()) {
        const int contracts = data.value(QStringLiteral("contract_count")).toInt();
        const int tickets   = data.value(QStringLiteral("open_ticket_count")).toInt();
        if (contracts > 0)
            card.fields.append({ QObject::tr("Contracts"),
                                 QString::number(contracts), QString() });
        if (tickets > 0)
            card.fields.append({ QObject::tr("Open tickets"),
                                 QString::number(tickets), QString() });
    }

    if (card.actions.isEmpty()) {
        const QString href = data.value(QStringLiteral("url")).toString();
        if (!href.isEmpty())
            card.actions.append({ QObject::tr("Open customer"), QUrl(href) });
    }

    return card;
}

// ── Card bookkeeping ────────────────────────────────────────────────────────

CallerCardPopup *CtiService::cardFor(const QString &callId) const
{
    for (const auto &e : m_cards)
        if (e.callId == callId && e.card)
            return e.card;
    return nullptr;
}

void CtiService::removeCard(const QString &callId)
{
    for (int i = 0; i < m_cards.size(); ++i) {
        if (m_cards[i].callId == callId) {
            m_cards.removeAt(i);
            return;
        }
    }
}

// ── Events ──────────────────────────────────────────────────────────────────

void CtiService::onRinging(const QString &callId, const QString &caller,
                           const QString &extension)
{
    if (!talq::ringEventIsUsable(callId.toStdString(), extension.toStdString())) {
        TLOG_NET("CTI: discarding unusable ring event");
        return;
    }

    const talq::CardAction action =
        m_store.onRing(callId.toStdString(), caller.toStdString(), extension.toStdString());
    if (action != talq::CardAction::Show)
        return;   // duplicate — a reconnect can replay a ring

    auto *card = new CallerCardPopup();
    card->setTheme(m_theme);
    card->setCanDial(canDial());
    connect(card, &CallerCardPopup::dialRequested, this, [this](const QString &n) {
        dial(n);
    });
    connect(card, &CallerCardPopup::dismissed, this, [this](const QString &id) {
        m_store.onEnd(id.toStdString(), "cancelled");
        dropCard(id, 0);
    });
    connect(card, &CallerCardPopup::openRequested, this,
            [this](const QString &id, const QUrl &url) {
        Q_UNUSED(id);
        // The card hands us a URL and never opens anything itself, so scheme
        // validation lives in exactly one place. `url` came from a server, and
        // the card is an UNPROMPTED popup -- without this, one click could
        // launch file:// or any registered handler.
        const QUrl resolved = url.isRelative() ? erpBaseUrl().resolved(url) : url;
        if (isSafeToOpen(resolved))
            QDesktopServices::openUrl(resolved);
        else
            TWARN("CTI: refusing to open" << resolved.scheme() << "URL from the server");
    });

    m_cards.append(CardEntry{ callId, card, false });
    card->showForCall(callId, caller, QPoint(0, 0));
    repositionCards();

    // The card is already on screen; the lookup only fills it in.
    lookupCustomer(callId, caller);
}

void CtiService::onEnded(const QString &callId, const QString &reason)
{
    const talq::CardAction action = m_store.onEnd(callId.toStdString(), reason.toStdString());
    CallerCardPopup *card = cardFor(callId);

    switch (action) {
    case talq::CardAction::MarkActive:
        // This agent answered. Keep the card — this is the moment its content
        // matters most, and when "open customer" actually gets clicked.
        if (card) card->setState(CallerCardPopup::State::Active);
        break;
    case talq::CardAction::MarkMissed:
        if (card) card->setState(CallerCardPopup::State::Missed);
        dropCard(callId, talq::lingerMsForAction(action));
        break;
    case talq::CardAction::Dismiss:
        dropCard(callId, 0);
        break;
    case talq::CardAction::Show:
    case talq::CardAction::Ignore:
        break;
    }
}

void CtiService::onDisconnectedFromDaemon()
{
    // Anything still ringing is now unknowable — it may have been answered by
    // a colleague minutes ago. A stale card that lies is worse than none.
    m_store.clear();
    for (const auto &e : m_cards)
        if (e.card)
            e.card->deleteLater();
    m_cards.clear();
    emit statusChanged();
}

void CtiService::onAuthenticationFailed(const QString &reason)
{
    TWARN("CTI disabled: authentication failed —" << reason);
    emit pairingMessage(tr("This device is no longer authorised for call pop-ups. "
                           "Pair it again in Settings."), true);
    emit statusChanged();
}

void CtiService::dropCard(const QString &callId, int lingerMs)
{
    CallerCardPopup *card = cardFor(callId);
    if (!card) {
        removeCard(callId);
        return;
    }

    if (lingerMs <= 0) {
        removeCard(callId);
        card->deleteLater();
        repositionCards();
        return;
    }

    // Keep the entry while the card lingers. Removing it here (as this used to)
    // left a visible card that Dismiss could not close, Open could not resolve,
    // repositionCards() would stack the next call directly on top of, and the
    // disconnect wipe would skip -- all while its own comment promised the
    // agent 15 seconds to "read and act on" a missed call.
    for (auto &e : m_cards)
        if (e.callId == callId)
            e.lingering = true;

    QTimer::singleShot(lingerMs, this, [this, callId]() {
        if (CallerCardPopup *c = cardFor(callId))
            c->deleteLater();
        removeCard(callId);
        repositionCards();
    });
}

void CtiService::repositionCards()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const QRect avail = screen->availableGeometry();

    int y = avail.bottom() - kEdgeMargin;
    for (const auto &e : m_cards) {
        if (!e.card)
            continue;
        y -= e.card->height();
        e.card->move(avail.right() - e.card->width() - kEdgeMargin, y);
        y -= kCardGap;
    }
}

// ── Customer lookup ─────────────────────────────────────────────────────────

void CtiService::lookupCustomer(const QString &callId, const QString &caller)
{
    CallerCardPopup *card = cardFor(callId);
    if (!card)
        return;

    const QUrl base = erpBaseUrl();
    if (base.isEmpty() || caller.isEmpty()) {
        card->applyUnknownCaller();
        return;
    }
    // The lookup carries the agent's token as a bearer header, so plain http
    // off-loopback would leak a working customer-lookup credential to anyone
    // on the path. Show the card with the bare number rather than do that.
    if (base.scheme().toLower() != QLatin1String("https") && !isLoopback(base)) {
        TWARN("CTI lookup skipped: ERP address must use https://");
        card->applyUnknownCaller();
        return;
    }

    // `caller` arrives over a socket and goes into an AUTHENTICATED request,
    // so it is percent-encoded and digit-checked first: unencoded, a crafted
    // value could walk the path and make the desktop issue a GET to an
    // endpoint it never chose, carrying the agent's own bearer token. A '#'
    // or '?' in a legitimate SIP caller id would also silently truncate it.
    const QString digits = QString::fromLatin1(
        QUrl::toPercentEncoding(caller, QByteArray(), QByteArray("/?#")));
    QUrl url(base.toString() + QStringLiteral("/api/v1/pbx/screen-pop/") + digits);
    QNetworkRequest req(url);
    // The AGENT's own token, not a system key — so the ERP applies that
    // agent's permissions rather than the daemon's.
    //
    // X-API-Key, NOT "Authorization: Bearer". The server accepts both in code,
    // but Apache strips Authorization before PHP sees it, so a Bearer request
    // is rejected by the allowlist gate with a 403 — which would surface here
    // as "Not a known customer" for every single caller. Verified against
    // production before shipping.
    req.setRawHeader("X-API-Key", token().toUtf8());
    req.setRawHeader("Accept", "application/json");
    // The phone rings for ~20s. A lookup still outstanding after 15 is no
    // longer useful, and without a timeout the card sits on "Looking up..."
    // indefinitely. ApiClient sets timeouts the same way.
    req.setTransferTimeout(15'000);

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, callId]() {
        reply->deleteLater();
        CallerCardPopup *card = cardFor(callId);
        if (!card)
            return;   // the card is gone entirely; nothing to fill in

        if (reply->error() != QNetworkReply::NoError) {
            // The card stays up with the bare number. A failed lookup is
            // exactly when the agent has least information, so removing the
            // card would be the wrong response.
            TLOG_NET("CTI lookup failed:" << reply->errorString());
            card->applyUnknownCaller();
            return;
        }

        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonObject data = body.contains(QStringLiteral("data"))
                                     ? body.value(QStringLiteral("data")).toObject()
                                     : body;

        if (!data.value(QStringLiteral("known")).toBool()) {
            card->applyUnknownCaller();
            return;
        }

        card->applyCard(parseCard(data));
    });
}

// ── Colleague card ──────────────────────────────────────────────────────────

// The same contract as the screen-pop lookup, keyed by person instead of phone
// number. That is the whole design: one dumb-renderer card format, two
// sources, so a site adds a field to either by changing a server response and
// nobody ships a build.
//
// Every failure here is silent on purpose. A site that never implements this
// endpoint lands in the error branch on every click, and that is not an error
// state -- the card keeps its identity and shift layers, which is exactly what
// an OSS deployment with no ERP at all sees.
void CtiService::fetchPersonCard(const QString &ncUsername)
{
    const QUrl base = erpBaseUrl();
    if (base.isEmpty() || token().isEmpty())
        return;   // unconfigured or unpaired: dormant, not broken
    // Same rule as the caller lookup: the request carries a working credential,
    // so plaintext off-loopback would leak it to anyone on the path. Loopback is
    // exempt because that is where someone develops an integration against the
    // documented demo, which serves http://127.0.0.1.
    if (base.scheme().toLower() != QLatin1String("https") && !isLoopback(base)) {
        TWARN("Person card skipped: ERP address must use https://");
        return;
    }

    // The id goes into an AUTHENTICATED request path, so it is percent-encoded
    // for the same reason the caller number is: unencoded, a crafted value
    // could walk the path and make the desktop GET an endpoint it never chose,
    // carrying the user's own credential.
    const QString safe = QString::fromLatin1(
        QUrl::toPercentEncoding(ncUsername, QByteArray(), QByteArray("/?#")));
    QNetworkRequest req(QUrl(base.toString()
                             + QStringLiteral("/api/v1/people/card/") + safe));
    // The VIEWER's own token, not a system key -- so the ERP applies that
    // person's permissions and can return a different card to different
    // people. X-API-Key, not Bearer: Apache strips Authorization before PHP
    // sees it.
    req.setRawHeader("X-API-Key", token().toUtf8());
    req.setRawHeader("Accept", "application/json");
    // Shorter than the caller lookup's 15s: nobody is holding a ringing phone,
    // and a card the user has already dismissed should stop waiting.
    req.setTransferTimeout(10'000);

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, ncUsername]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            TLOG_NET("Person card lookup failed:" << reply->errorString());
            return;
        }

        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonObject data = body.contains(QStringLiteral("data"))
                                     ? body.value(QStringLiteral("data")).toObject()
                                     : body;

        // "known": false must be indistinguishable from "you may not see this
        // person", which is why a site returns it rather than a 403.
        if (!data.value(QStringLiteral("known")).toBool())
            return;

        emit personCardReady(ncUsername, parseCard(data));
    });
}

// ── Pairing ─────────────────────────────────────────────────────────────────

void CtiService::beginPairing()
{
    const QUrl base = erpBaseUrl();
    if (base.isEmpty()) {
        emit pairingMessage(tr("Set the ERP address first."), true);
        return;
    }

    QNetworkRequest req(QUrl(base.toString() + QStringLiteral("/api/v1/cti/pair/start")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(15'000);

    QJsonObject payload{
        { QStringLiteral("device"), QSysInfo::machineHostName() },
    };

    QNetworkReply *reply = m_net->post(req, QJsonDocument(payload).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit pairingMessage(tr("Could not reach the server: %1")
                                    .arg(reply->errorString()), true);
            return;
        }
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonObject data = body.contains(QStringLiteral("data"))
                                     ? body.value(QStringLiteral("data")).toObject()
                                     : body;

        m_pairToken = data.value(QStringLiteral("pair_token")).toString();
        const QUrl approve(data.value(QStringLiteral("approve_url")).toString());
        if (m_pairToken.isEmpty() || !approve.isValid()) {
            emit pairingMessage(tr("The server did not offer a pairing."), true);
            return;
        }

        m_pairPollsLeft = kPairMaxPolls;
        // The server tells us how often to ask; hardcoding a rate here would
        // drift the moment it changes its mind.
        const int interval = data.value(QStringLiteral("poll_interval")).toInt();
        m_pairPollMs = interval > 0 ? interval * 1000 : kPairPollMs;
        // They are already signed in there; approval is one click and no
        // password is typed into TalQ.
        QDesktopServices::openUrl(approve);
        emit pairingMessage(tr("Approve this device in your browser…"), false);
        QTimer::singleShot(m_pairPollMs, this, &CtiService::pollPairing);
    });
}

void CtiService::cancelPairing()
{
    m_pairToken.clear();
    m_pairPollsLeft = 0;
}

void CtiService::pollPairing()
{
    if (m_pairToken.isEmpty())
        return;
    if (--m_pairPollsLeft < 0) {
        m_pairToken.clear();
        emit pairingMessage(tr("Pairing timed out. Try again."), true);
        return;
    }

    QNetworkRequest req(QUrl(erpBaseUrl().toString()
                             + QStringLiteral("/api/v1/cti/pair/poll")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(15'000);
    const QJsonObject payload{ { QStringLiteral("pair_token"), m_pairToken } };

    QNetworkReply *reply = m_net->post(req, QJsonDocument(payload).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (status == 202) {                       // still waiting on the human
            QTimer::singleShot(m_pairPollMs, this, &CtiService::pollPairing);
            return;
        }
        if (status == 410 || reply->error() != QNetworkReply::NoError) {
            m_pairToken.clear();
            emit pairingMessage(tr("Pairing was declined or expired."), true);
            return;
        }

        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonObject data = body.contains(QStringLiteral("data"))
                                     ? body.value(QStringLiteral("data")).toObject()
                                     : body;

        const QString key = data.value(QStringLiteral("api_key")).toString();
        if (key.isEmpty()) {
            m_pairToken.clear();
            emit pairingMessage(tr("The server did not return a key."), true);
            return;
        }

        m_pairToken.clear();
        const QString ext = data.value(QStringLiteral("extension")).toString();
        setToken(key);
        setEnabledSetting(true);
        emit pairingSucceeded(data.value(QStringLiteral("display_name")).toString(), ext);

        // `extension` comes back null when the account has no pbx_extension
        // attribute. Pairing has genuinely succeeded, but the daemon will
        // refuse every connection with "no-extension" and the user would be
        // left staring at a paired device that never pops anything. Say so
        // now, while there is something they can act on.
        if (ext.isEmpty()) {
            emit pairingMessage(tr("Paired, but no phone extension is linked to your account. "
                                   "Ask an administrator to set one, or calls will not appear."),
                                true);
            return;
        }

        if (serverUrl().isEmpty()) {
            emit pairingMessage(tr("Paired. Now enter the call service address "
                                   "to start receiving pop-ups."), true);
            return;
        }

        emit pairingMessage(tr("This device is paired."), false);
        start();
    });
}
