#include "core/SignalingClient.h"
#include "core/TalqLog.h"
#include "core/ChatSyncLogic.h"
#include "core/HpbPool.h"
#include <QJsonDocument>
#include <QSettings>
#include <QDateTime>
#include <QHostInfo>
#include <QTcpSocket>
#include <QElapsedTimer>
#include <QSet>
#include <QUrl>
#include <memory>
#include <vector>
#include <limits>

// bug 3 — a cached peer TalQ version is only trusted as "current" if observed
// within this window; older entries render as no chip rather than a wrong
// number. Generous because TalQ identity is sticky and re-announces are
// infrequent (room overlap / calls), so we would rather under-blank than show
// a confidently-wrong stale version.
static constexpr qint64 kPeerVersionFreshMs = 30LL * 24 * 60 * 60 * 1000; // 30 days

SignalingClient::SignalingClient(ApiClient *api, QObject *parent)
    : QObject(parent)
    , m_api(api)
{
    connect(&m_ws, &QWebSocket::connected, this, &SignalingClient::onConnected);
    connect(&m_ws, &QWebSocket::disconnected, this, &SignalingClient::onDisconnected);
    connect(&m_ws, &QWebSocket::textMessageReceived, this, &SignalingClient::onTextMessage);
    connect(&m_ws, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError err) {
        qWarning() << "Signaling: WebSocket error:" << err << m_ws.errorString();
        // A1 fix — a pure CONNECT failure (host unreachable / refused — the normal
        // case during an outage, and exactly when the 250ms fast-resume retry
        // fires) does NOT emit `disconnected` (Qt only emits that when an already
        // ESTABLISHED socket closes), so onDisconnected()'s reconnect driver never
        // runs. Before this wave fetchSettings()'s REST failure always re-armed
        // reconnect; the fast path skips that, so without this the client would be
        // left with no socket and no pending retry -> signaling dead until restart.
        // Drive the retry here when the socket isn't connected, and unwind the
        // fast-resume bookkeeping (so the next attempt does a full settings fetch).
        if (m_ws.state() != QAbstractSocket::ConnectedState) {
            if (m_fastResumePending) { m_fastResumePending = false; m_resumeId.clear(); }
            if (!m_reconnectTimer.isActive()) reconnect();   // guard vs onDisconnected double-arm
        }
    });

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &SignalingClient::start);

    // Clear typing indicator after 15s (safety net)
    m_typingClearTimer.setSingleShot(true);
    m_typingClearTimer.setInterval(15000);
    connect(&m_typingClearTimer, &QTimer::timeout, this, [this]() {
        if (!m_typingUser.isEmpty()) {
            m_typingUser.clear();
            m_typingRoom.clear();
            emit typingUserChanged();
        }
    });

    loadPersistedPeerClients();

    // Periodic talq.client re-announce while in a room. Defense-in-depth
    // for the upgrade case: a peer that joined the room before us
    // upgraded silently keeps a stale-version cache for the whole session
    // unless something forces a re-announce. The one-shot room-join hello
    // covers a fresh peer-join, but co-resident sessions need this tick.
    // 5 minutes is small relative to a long call/chat session and well
    // under the spam threshold (the receive path de-duplicates by info
    // string equality, so an unchanged version triggers no state churn).
    m_talqClientReannounce.setInterval(5 * 60 * 1000);
    connect(&m_talqClientReannounce, &QTimer::timeout, this, [this]() {
        if (!m_currentRoom.isEmpty() && m_ws.state() == QAbstractSocket::ConnectedState)
            sendTalqClientHello();
    });

    // HPB signaling keepalive (see m_keepAliveTimer). Ping every 25 s while
    // the socket is up so the server's 60 s read-deadline -- and any idle
    // proxy/NAT -- never culls the connection mid-call. Started on connect,
    // stopped on disconnect.
    m_keepAliveTimer.setInterval(25 * 1000);
    connect(&m_keepAliveTimer, &QTimer::timeout, this, [this]() {
        if (m_ws.state() == QAbstractSocket::ConnectedState)
            m_ws.ping();
    });
    connect(&m_ws, &QWebSocket::pong, this,
            [this](quint64 elapsed, const QByteArray &) {
        qDebug() << "Signaling: keepalive pong, RTT" << elapsed << "ms";
        // Keep the telemetry RTT fresh for the life of the connection: the
        // one-shot nearest-HPB probe only measures at connect time, so
        // without this selectedSignalingRttMs() would freeze at whatever it
        // was when the session started.
        m_signalingRttMs = static_cast<int>(elapsed);
        emit signalingRttChanged(m_signalingRttMs);
    });
}

// A1 — resume reconnect backoff. A held resume id means the server is keeping
// our session alive for only a short grace (~30s on strukturag), so race to the
// socket fast rather than the cold-start exponential backoff.
static constexpr int kResumeReconnectMs = 250;

void SignalingClient::start()
{
    // A1 fast resume: if we hold a resume id AND a cached signaling URL, skip the
    // REST settings fetch (the resume hello only needs m_resumeId — the freshly
    // fetched ticket/auth is pure waste on this path) and go straight to the
    // WebSocket. This is what keeps the resume inside the server's grace window.
    // Cold start, or a definitive resume rejection (which clears m_resumeId),
    // falls through to the full settings fetch.
    if (!m_resumeId.isEmpty() && !m_signalingUrl.isEmpty()) {
        m_fastResumePending = true;
        connectWebSocket();
        return;
    }
    fetchSettings();
}

void SignalingClient::stop()
{
    m_reconnectTimer.stop();
    m_talqClientReannounce.stop();
    m_keepAliveTimer.stop();
    sendBye();                 // graceful HPB disconnect (matches upstream)
    m_ws.close();
    m_sessionId.clear();
    m_resumeId.clear();        // bye released the session -- no resume after a deliberate stop
    m_resuming = false;
    m_fastResumePending = false;
    m_sessionEstablished = false;   // A2 — deliberate stop is a clean slate
    m_sessionToUserId.clear(); // 1.0 audit — release the session->userId map on a
                               // deliberate disconnect/logout (no grace after stop)
    m_reconnectDelay = 2000;
    if (m_authenticated) {
        m_authenticated = false;
        emit connectedChanged();
    }
}

void SignalingClient::fetchSettings()
{
    m_api->get("apps/spreed/api/v3/signaling/settings",
        [this](bool ok, const QJsonObject &data, int) {
            if (!ok) {
                qWarning() << "Signaling: failed to get settings";
                reconnect();
                return;
            }

            m_signalingUrl = data["server"].toString().trimmed();
            // Server-provided HPB discovery (optional "servers" field -- see
            // m_discoveredHpbPool in the header). Absent on stock Nextcloud;
            // present when apps/spreed carries the additive server-side patch.
            m_discoveredHpbPool.clear();
            for (const auto &v : data["servers"].toArray()) {
                const QString u = v.toObject()["server"].toString().trimmed();
                if (!u.isEmpty()) m_discoveredHpbPool << u;
            }
            // Manual signaling-server override (hidden, for A/B stability testing —
            // QSettings key Signaling/overrideServer). Pins a specific regional HPB
            // (e.g. storm/BG) instead of the per-conversation server the backend
            // assigns; the Nextcloud-issued ticket still validates because a clustered
            // HPB shares the same backend + secret. Empty string = normal behaviour.
            {
                const QString ov = QSettings(QStringLiteral("TalQ"), QStringLiteral("TalQ"))
                                       .value(QStringLiteral("Signaling/overrideServer")).toString().trimmed();
                if (!ov.isEmpty()) {
                    qInfo() << "Signaling: MANUAL override ->" << ov << "(backend gave" << m_signalingUrl << ")";
                    m_signalingUrl = ov;
                }
            }
            // Per-instance override (highest precedence). talq-call-test drives each
            // bot to a SPECIFIC HPB to exercise a cross-server clustered call — the
            // process-global QSettings override can't do that (one key, both bots).
            if (!m_serverOverride.isEmpty()) {
                qInfo() << "Signaling: per-instance override ->" << m_serverOverride;
                m_signalingUrl = m_serverOverride;
            }
            if (m_signalingUrl.isEmpty()) {
                qDebug() << "Signaling: no standalone server configured";
                return;
            }

            // Auth params: prefer hello v2.0 (signed JWT) when the server
            // provides it, exactly like the official client; fall back to
            // v1.0 (userid/ticket) otherwise.
            QJsonObject authParams = data["helloAuthParams"].toObject();
            QJsonObject v1 = authParams["1.0"].toObject();
            m_userId = v1["userid"].toString();
            m_ticket = v1["ticket"].toString();
            // (v2.0 token is also offered by the server but our v2 hello
            // envelope was rejected at runtime — see sendHello(); we use
            // v1.0, which requires the ticket.)
            m_helloV2Token = authParams["2.0"].toObject()["token"].toString();

            if (m_ticket.isEmpty()) {
                qWarning() << "Signaling: no ticket in settings";
                return;
            }

            qDebug() << "Signaling: server at" << m_signalingUrl;
            // Automatic nearest-HPB selection: unless a server is pinned manually,
            // probe the candidate pool (Nextcloud-assigned server + the branded
            // build's regional pool) and connect to the NEAREST reachable one — a
            // short, stable WebSocket path is what prevents the long-haul
            // socket-death disconnects. Runs on every (re)connect, so a dead HPB
            // (fails the probe) is naturally skipped on reconnect = failover.
            const bool manualPin = !m_serverOverride.isEmpty()
                || !QSettings(QStringLiteral("TalQ"), QStringLiteral("TalQ"))
                        .value(QStringLiteral("Signaling/overrideServer")).toString().trimmed().isEmpty();
            if (manualPin)
                connectWebSocket();
            else
                selectNearestHpbAndConnect();
        });
}

// Parse the host from a signaling URL (wss://host/path, https://host/path).
static QString hpbHostOf(const QString &url)
{
    const QUrl u(url.trimmed());
    if (!u.host().isEmpty()) return u.host();
    QString s = url.trimmed();
    const int i  = s.indexOf(QStringLiteral("://")); if (i  >= 0) s = s.mid(i + 3);
    const int sl = s.indexOf(QLatin1Char('/'));      if (sl >= 0) s = s.left(sl);
    const int c  = s.lastIndexOf(QLatin1Char(':'));  if (c  >= 0) s = s.left(c);
    return s;
}

// Probe TCP:443 RTT to each candidate HPB and connect to the nearest reachable
// one. :443 is the web front (Caddy/Apache) of every HPB and is always open, so
// unlike the TURN :3478 probe the connect time is a reliable network RTT. The
// Nextcloud-assigned server is always a candidate AND the fail-safe fallback: if
// nothing answers we keep it and connect anyway (never serverless). Probe state
// lives in a shared_ptr owned by the timer functor; the sockets are parented to
// `this`, so a destroyed SignalingClient cleans them up on that same path.
void SignalingClient::selectNearestHpbAndConnect()
{
    QStringList urls;
    urls << m_signalingUrl;                               // Nextcloud baseline
    urls << m_discoveredHpbPool;                          // server-discovered pool (both builds)
    for (const char *const *p = TalQHpb::kPool; *p; ++p)  // branded static pool (empty on generic)
        urls << QString::fromLatin1(*p);

    QStringList cands; QSet<QString> seen;
    for (const QString &u : urls) {
        const QString h = hpbHostOf(u);
        if (h.isEmpty() || seen.contains(h)) continue;
        seen.insert(h); cands << u;
    }
    if (cands.size() <= 1) { connectWebSocket(); return; } // nothing to choose

    // --- Nearest-HPB selection tunables ---------------------------------
    // A SINGLE TCP:443 sample per candidate is far too noisy to choose between
    // POPs whose true RTTs are close (field: turn-bg ~15ms, turn-ru ~150ms,
    // turn-za ~200ms — but DNS/TLS/loss jitter on one 1200ms window let a FAR
    // POP win, stranding a BG user on turn-za / turn-ru). Fix: take several
    // samples per candidate and use the MIN (the true network floor; jitter
    // only ever ADDS latency, so min is the robust estimator), then only SWITCH
    // AWAY from the incumbent (the server we're already on, or the Nextcloud
    // baseline on a cold start) if a challenger is faster by a real MARGIN.
    // This kills jitter-driven flapping while still following a genuinely
    // nearer POP.
    constexpr int kProbeSamples    = 4;    // TCP:443 connects per candidate
    constexpr int kSampleSpacingMs = 120;  // stagger between a candidate's samples
    constexpr int kSelectAfterMs   = 900;  // when to decide (covers 4 staggered samples + RTT)
    constexpr int kSwitchMarginMs  = 30;   // challenger must beat incumbent by this to win

    // The incumbent = the HPB we last successfully connected to (sticky across
    // reconnects), else the Nextcloud-assigned baseline on a cold start. We only
    // leave it for a clearly-closer POP (see kSwitchMarginMs). This is what stops
    // a jitter-driven reconnect from stranding us on a far POP.
    const QString incumbentHost = hpbHostOf(m_lastConnectedHpbHost.isEmpty()
                                            ? m_signalingUrl : m_lastConnectedHpbHost);

    struct Probe {
        QString url, host;
        int minRtt = -1;                 // best (lowest) sample seen so far
        int samplesDone = 0;             // landed RTT samples: a LIVE incumbent is only
                                         // displaced by a challenger with >= 2 samples,
                                         // so one lucky late sample can't flap us.
        bool isIncumbent = false;
        std::vector<QTcpSocket *> socks;  // outstanding sample sockets, cleaned in the sweep
    };
    auto probes = std::make_shared<std::vector<Probe>>();
    for (const QString &u : cands) {
        Probe p; p.url = u; p.host = hpbHostOf(u);
        p.isIncumbent = (!incumbentHost.isEmpty() && p.host == incumbentHost);
        probes->push_back(std::move(p));
    }
    // Shared "selection fired" latch. DNS lookups + sample timers are async and
    // can complete AFTER the selection singleShot has swept the probes — a late
    // callback must not then create a socket on a finalized probe (leak + the
    // old use-after-free on a freed vector). Callbacks capture the `probes`
    // shared_ptr (keeps the vector alive) AND this latch (no-op once done).
    auto selectionDone = std::make_shared<bool>(false);

    // Fire kProbeSamples staggered TCP:443 connects for one candidate, tracking
    // the running MIN. DNS is resolved once up front (connectToHost(QString)
    // would fold resolver latency into the measured RTT — a cold resolver hit
    // could dwarf the true network gap; timing connectToHost(QHostAddress)
    // isolates the TCP handshake RTT).
    for (size_t i = 0; i < probes->size(); ++i) {
        const QString host = (*probes)[i].host;
        QHostInfo::lookupHost(host, this, [this, probes, selectionDone, i](const QHostInfo &info) {
            if (*selectionDone) return;
            if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) return;
            const QHostAddress addr = info.addresses().first();
            for (int s = 0; s < kProbeSamples; ++s) {
                QTimer::singleShot(s * kSampleSpacingMs, this, [this, probes, selectionDone, i, addr]() {
                    if (*selectionDone) return;
                    Probe &pr = (*probes)[i];
                    auto *sock = new QTcpSocket(this);
                    auto t = std::make_shared<QElapsedTimer>();
                    t->start();
                    pr.socks.push_back(sock);
                    // Capture probes+latch by value so a late 'connected' after the
                    // sweep is a safe no-op (vector kept alive; latch gates it).
                    QObject::connect(sock, &QTcpSocket::connected, this,
                                     [probes, selectionDone, i, t, sock]() {
                        if (*selectionDone) { sock->abort(); return; }
                        const int rtt = int(t->elapsed());
                        Probe &pr = (*probes)[i];
                        ++pr.samplesDone;
                        if (pr.minRtt < 0 || rtt < pr.minRtt) pr.minRtt = rtt;
                        sock->abort();
                    });
                    sock->connectToHost(addr, 443);
                });
            }
        });
    }

    QTimer::singleShot(kSelectAfterMs, this, [this, probes, selectionDone]() {
        *selectionDone = true;   // any still-pending callback now no-ops
        // Incumbent RTT (if it answered), the best challenger with >= 2 samples
        // (allowed to DISPLACE a live incumbent), and the best challenger with
        // any sample (only used when the incumbent was silent — something beats
        // nothing). Requiring 2 samples to displace stops a single lucky late
        // sample from re-introducing the jitter this whole rewrite kills.
        QString incumbentUrl; int incumbentRtt = -1;
        QString bestUrl;      int bestRtt = -1;   // >= 2 samples
        QString anyUrl;       int anyRtt  = -1;   // >= 1 sample
        for (Probe &pr : *probes) {
            if (pr.isIncumbent) {
                incumbentUrl = pr.url;
                if (pr.minRtt >= 0) incumbentRtt = pr.minRtt;
            } else if (pr.minRtt >= 0) {
                if (anyRtt < 0 || pr.minRtt < anyRtt) { anyRtt = pr.minRtt; anyUrl = pr.url; }
                if (pr.samplesDone >= 2 && (bestRtt < 0 || pr.minRtt < bestRtt)) {
                    bestRtt = pr.minRtt; bestUrl = pr.url;
                }
            }
            for (QTcpSocket *s : pr.socks) { s->disconnect(); s->abort(); s->deleteLater(); }
            pr.socks.clear();
        }

        // Keep the incumbent unless a >=2-sample challenger beats it by the
        // margin. If the incumbent was silent, take any answering challenger.
        QString chosenUrl; int chosenRtt = -1; const char *why = "no candidate answered";
        if (incumbentRtt >= 0 && bestRtt >= 0 && bestRtt <= incumbentRtt - kSwitchMarginMs) {
            chosenUrl = bestUrl; chosenRtt = bestRtt;
            why = "switched (beat incumbent by margin, >=2 samples)";
        } else if (incumbentRtt >= 0) {
            chosenUrl = incumbentUrl; chosenRtt = incumbentRtt;   // sticky — set URL EXPLICITLY
            why = "sticky (within margin)";
        } else if (anyRtt >= 0) {
            chosenUrl = anyUrl; chosenRtt = anyRtt;
            why = "incumbent silent";
        }

        qInfo().nospace() << "Signaling: HPB select — " << why
                          << " chosen=" << hpbHostOf(chosenUrl.isEmpty() ? m_signalingUrl : chosenUrl)
                          << " rtt=" << chosenRtt << " (min of " << kProbeSamples << ")"
                          << " incumbentRtt=" << incumbentRtt
                          << " bestChallengerRtt=" << bestRtt << " anyChallengerRtt=" << anyRtt;

        if (!chosenUrl.isEmpty()) {
            // Normalise to the BASE url (no trailing "/spreed"): connectWebSocket()
            // appends "/spreed" itself, so a pool url that already carries it would
            // double up to /standalone-signaling/spreed/spreed → 404.
            QString base = chosenUrl.trimmed();
            if (base.endsWith(QLatin1Char('/')))         base.chop(1);
            if (base.endsWith(QLatin1String("/spreed"))) base.chop(7);
            m_signalingUrl   = base;
            m_signalingRttMs = chosenRtt;
        }
        connectWebSocket();
    });
}

void SignalingClient::connectWebSocket()
{
    QString wsUrl = m_signalingUrl;
    wsUrl.replace("https://", "wss://").replace("http://", "ws://");
    if (!wsUrl.endsWith("/"))
        wsUrl += "/";
    wsUrl += "spreed";

    qDebug() << "Signaling: connecting to" << wsUrl;
    m_ws.open(QUrl(wsUrl));
}

void SignalingClient::onConnected()
{
    qDebug() << "Signaling: WebSocket connected, waiting for welcome";
    m_reconnectDelay = 2000;
    // Remember the HPB we actually connected to — the nearest-HPB probe stays
    // sticky to this on the next reconnect unless a challenger wins by margin,
    // so a one-off jittery probe can't strand us on a far POP.
    m_lastConnectedHpbHost = hpbHostOf(m_signalingUrl);
    m_keepAliveTimer.start();   // keep the HPB session alive (see m_keepAliveTimer)
}

void SignalingClient::onDisconnected()
{
    qDebug() << "Signaling: disconnected";
    m_keepAliveTimer.stop();
    bool wasAuth = m_authenticated;
    m_authenticated = false;
    m_roomJoinAcked = false;   // room membership is void until re-acked post-reconnect
    m_sessionId.clear();
    // A1 — if a fast-resume attempt couldn't even establish the socket (e.g. a
    // stale cached signaling URL), it disconnects without ever authenticating.
    // Drop the resume id so the next attempt does a full settings refresh
    // (fresh URL + ticket) instead of looping on the same dead fast path.
    if (m_fastResumePending && !wasAuth) {
        qWarning() << "Signaling: fast-resume socket failed -> full settings refresh next";
        m_fastResumePending = false;
        m_resumeId.clear();
    }
    if (wasAuth) emit connectedChanged();
    reconnect();
}

void SignalingClient::onTextMessage(const QString &msg)
{
    QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
    if (doc.isNull()) {
        qWarning() << "Signaling: received malformed JSON:" << msg.left(200);
        return;
    }
    QJsonObject obj = doc.object();
    QString type = obj["type"].toString();

    // Debug: log all incoming WebSocket messages
    if (type != "hello" && type != "welcome")
        qDebug() << "Signaling: WS <<" << type << msg.left(200);

    if (type == "welcome") {
        qDebug() << "Signaling: received welcome, sending hello";
        sendHello();
    }
    else if (type == "hello") {
        QJsonObject helloObj = obj["hello"].toObject();
        m_sessionId = helloObj["sessionid"].toString();
        // Capture the resume id so a later WS blip can RESUME this session
        // rather than start a new one (which drops us from the call). A fresh
        // hello response carries it; a resume response omits it -> keep ours.
        const QString rid = helloObj["resumeid"].toString();
        if (!rid.isEmpty()) m_resumeId = rid;
        m_authenticated = true;
        m_fastResumePending = false;   // A1 — this attempt authenticated

        const bool wasResume = m_resuming;
        m_resuming = false;
        // A2 — was there a live session BEFORE this hello? A fresh (non-resumed)
        // hello that replaces an existing in-room session is a session RESET.
        // Capture before we mark the new one established so the very first cold
        // hello is never treated as a reset.
        const bool hadPriorSession = m_sessionEstablished;
        m_sessionEstablished = true;

        // Server features (MCU, etc.) are only present in a FRESH hello
        // response; a resume response omits them, so keep the cached value.
        if (helloObj.contains("server")) {
            QJsonArray features = helloObj["server"].toObject()["features"].toArray();
            QStringList featureList;
            for (const auto &f : features)
                featureList << f.toString();
            m_hasMcu = featureList.contains("mcu");
            qDebug() << "Signaling: features:" << featureList.join(", ") << "MCU:" << m_hasMcu;
        }

        emit connectedChanged();

        if (wasResume) {
            // Resumed: the server kept us in the room/call across the blip,
            // so do NOT re-join (that would be a fresh join and the peers
            // would see us leave+rejoin). The call survives untouched.
            qDebug() << "Signaling: session RESUMED" << m_sessionId.left(16) + "..."
                     << "room" << m_currentRoom;
        } else {
            qDebug() << "Signaling: authenticated, session:" << m_sessionId.left(20) + "...";
            // Fresh session -- re-join room if we had one. force=true: this is
            // the ONE legitimate same-room re-join (the same-room guard in
            // joinRoom would otherwise no-op it and we'd never re-enter).
            if (!m_currentRoom.isEmpty())
                joinRoom(m_currentRoom, /*force*/ true);
            // A2 — a fresh session that REPLACES a prior in-room session is a
            // reset: the old MCU publisher is dead and the server call-record
            // points at the dead sid. joinRoom above only re-POSTs the ROOM, so
            // tell CallManager to rebuild the publisher, re-join the CALL, and
            // rebuild subscribers under the new sid. (When idle, CallManager
            // no-ops on state.) Not fired on the first cold hello (hadPrior=false)
            // nor on a clean RESUME (handled in the wasResume branch above).
            if (hadPriorSession && !m_currentRoom.isEmpty()) {
                qWarning() << "Signaling: SESSION RESET (fresh hello replaced an in-room session)"
                           << "-> new sid" << m_sessionId.left(16) + "...";
                emit sessionReset(m_sessionId);
            }
        }
    }
    else if (type == "error") {
        QJsonObject errObj = obj["error"].toObject();
        const QString code = errObj["code"].toString();
        qWarning() << "Signaling: error:" << code << errObj["message"].toString();
        // 0.52.7 — "Not allowed to request offer." is produced ONLY in reply to a
        // requestoffer (no other message yields that text). The HPB error carries
        // NO sessionid / request-id, and the outgoing requestoffer carries none
        // either, so we cannot say WHICH peer it is for — emit a typed,
        // sid-agnostic signal and let CallManager apply it to the peers it
        // currently has a requestoffer outstanding for. Deliberately do NOT add a
        // request-id to the outgoing message: the strukturag server won't echo a
        // field it doesn't implement, and inventing signaling fields has broken
        // calls before. Type-correlation is the only safe correlation here.
        if (code == QLatin1String("not_allowed")
            && errObj["message"].toString().contains(QLatin1String("request offer"),
                                                     Qt::CaseInsensitive)) {
            emit requestOfferRejected();
        }
        if (m_resuming) {
            // Resume rejected (e.g. no_such_session -- the grace lapsed).
            qWarning() << "Signaling: resume rejected -> full re-hello";
            m_resuming = false;
            m_resumeId.clear();
            if (m_fastResumePending) {
                // A1 — we took the fast path and skipped the settings fetch, so
                // we have no FRESH ticket for a clean re-auth (a stale ticket can
                // fail). Drop this socket; the reconnect path then does a full
                // fetchSettings (fresh URL + ticket) -> fresh hello -> re-join.
                m_fastResumePending = false;
                m_ws.close();   // -> onDisconnected -> reconnect -> start() (resumeId now empty)
            } else {
                // Normal path: this round already fetched a fresh ticket, so a
                // fresh hello on the same socket is safe.
                sendHello();
            }
        }
    }
    else if (type == "room") {
        qInfo() << "Signaling: joined room";
        m_roomJoinAcked = true;   // authoritative: HPB confirmed room membership
        emit roomJoined();
    }
    else if (type == "message") {
        QJsonObject messageObj = obj["message"].toObject();
        QJsonObject msgData = messageObj["data"].toObject();
        QString msgType = msgData["type"].toString();
        QString senderSessionId = messageObj["sender"].toObject()["sessionid"].toString();

        // WebRTC signaling messages (session-targeted, dispatch before room filter)
        if (msgType == "offer") {
            QString sdp = msgData["payload"].toObject()["sdp"].toString();
            QString sid = msgData["sid"].toString();
            QString roomType = msgData["roomType"].toString("video");
            qInfo() << "Signaling: received offer from" << senderSessionId.left(20) << "sid=" << sid;
            emit offerReceived(senderSessionId, sdp, sid, roomType);
            return;
        }
        if (msgType == "answer") {
            QString sdp = msgData["payload"].toObject()["sdp"].toString();
            QString roomType = msgData["roomType"].toString("video");
            qInfo() << "Signaling: received answer from" << senderSessionId.left(20);
            emit answerReceived(senderSessionId, sdp, roomType);
            return;
        }
        if (msgType == "candidate") {
            QJsonObject payload = msgData["payload"].toObject();
            // MCU (Janus) sends: payload = {candidate: {candidate: "...", sdpMLineIndex: N}}
            // P2P client sends: payload = {candidate: {candidate: "...", sdpMLineIndex: N}}
            // Extract the inner candidate object if double-nested
            QJsonObject candidate;
            if (payload.contains("candidate") && payload["candidate"].isObject()) {
                candidate = payload["candidate"].toObject();
            } else {
                candidate = payload;
            }
            QString candRoomType = msgData["roomType"].toString("video");
            qInfo() << "Signaling: received candidate from" << senderSessionId.left(20)
                     << "roomType=" << candRoomType;
            emit candidateReceived(senderSessionId, candidate, candRoomType);
            return;
        }
        if (msgType == "endOfCandidates") {
            emit endOfCandidatesReceived(senderSessionId);
            return;
        }
        if (msgType == "mute" || msgType == "unmute") {
            QString media = msgData["payload"].toObject()["name"].toString();
            bool muted = (msgType == "mute");
            emit remoteMuteChanged(senderSessionId, media, muted);
            return;
        }

        if (msgType == "unshareScreen") {
            qDebug() << "Signaling: received unshareScreen from" << senderSessionId.left(20);
            emit screenShareStopped(senderSessionId);
            return;
        }

        // TalQ-private deliberate-hangup hint. A 1:1 MCU call holds the peer in a
        // 28s "Reconnecting" peer-grace on participantLeftCall (to survive a
        // transient drop), which can't tell a real hangup from a blip. The peer
        // sends this the instant the user hangs up so we end immediately instead
        // of waiting out the grace. Best-effort: if it's lost, grace→timeout
        // still ends the call (no regression). The HPB relays it untouched.
        if (msgType == "hangup") {
            qDebug() << "Signaling: received hangup from" << senderSessionId.left(20);
            emit peerHungUp(senderSessionId);
            return;
        }

        // Room-scoped messages (typing indicators)
        QString senderRoom = messageObj["sender"].toObject()["roomid"].toString();
        if (!senderRoom.isEmpty() && senderRoom != m_currentRoom) {
            return;
        }

        if (msgType == "startedTyping") {
            QJsonObject senderObj = messageObj["sender"].toObject();
            QString sender = senderObj["displayname"].toString();
            if (sender.isEmpty()) {
                // HPB may not provide displayname — use userid as fallback key
                // and look up the display name from participant data if available
                sender = m_participantNames.value(
                    senderObj["userid"].toString(),
                    senderObj["userid"].toString());
            }

            if (sender != m_userId && !sender.isEmpty()) {
                m_typingUser = sender;
                m_typingRoom = m_currentRoom;
                emit typingUserChanged();
                m_typingClearTimer.start();
            }
        }
        else if (msgType == "stoppedTyping") {
            if (!m_typingUser.isEmpty()) {
                m_typingUser.clear();
                m_typingRoom.clear();
                emit typingUserChanged();
                m_typingClearTimer.stop();
            }
        }
        else if (msgType == "talq.client") {
            // Client-originated TalQ identification broadcast. Travels the
            // same path as typing indicators (recipient.type=room → echoed
            // back to all room participants as type="message").
            //
            // Prefer userId from the payload itself (we put it there in
            // sendTalqClientHello); fall back to the sender annotation
            // added by the spreedbackend.
            QJsonObject senderObj = messageObj["sender"].toObject();
            QString senderUid = msgData["userid"].toString();
            if (senderUid.isEmpty())
                senderUid = senderObj["userid"].toString();
            const QString senderSid = senderObj["sessionid"].toString();
            if (senderSid == m_sessionId || senderUid == m_userId) {
                // Echo of our own broadcast — ignore.
            } else if (!senderUid.isEmpty()) {
                const QString client = msgData["client"].toString();
                const QString version = msgData["version"].toString();
                const QString info = client + "/" + version;
                updatePeerClient(senderUid, info);  // bug 3 — stamps freshness + persists
                if (!senderSid.isEmpty())
                    m_sessionToUserId[senderSid] = senderUid;
            } else {
                qDebug() << "Signaling: received talq.client without userId — sender=" << senderObj;
            }
        }
    }
    else if (type == "event") {
        QJsonObject event = obj["event"].toObject();
        QString target = event["target"].toString();
        QString eventType = event["type"].toString();
        qDebug() << "Signaling: event target=" << target << "type=" << eventType;

        // HPB broadcasts chat events as room messages with data.type=="chat".
        // The payload looks like:
        //   { target: "room", type: "message",
        //     message: { roomid: "<token>",
        //                data: { type: "chat", chat: { refresh: true, comment: {...} } } } }
        // Emit a refresh signal so MessageListModel can pull the latest state
        // (which includes the X-Chat-Last-Common-Read header). Without this
        // we only learn about read-marker advances when a new message also
        // arrives via the chat long-poll.
        if (target == "room" && eventType == "message") {
            QJsonObject msgObj = event["message"].toObject();
            QJsonObject data = msgObj["data"].toObject();
            const QString dataType = data["type"].toString();
            if (dataType == "chat") {
                QString roomToken = msgObj["roomid"].toString();
                if (!roomToken.isEmpty()) {
                    TLOG_SIG("chat refresh hint for room" << roomToken);
                    emit chatRefreshNeeded(roomToken);
                }
            } else if (dataType == "talq.client") {
                // Defense-in-depth fallback path. The real delivery channel
                // for client-originated room broadcasts is the top-level
                // type="message" branch above (same path typing indicators
                // travel). This event/room/message branch is kept in case a
                // future server variant routes such messages here instead.
                QJsonObject sender = msgObj["sender"].toObject();
                const QString senderSid = sender["sessionid"].toString();
                QString senderUid = data["userid"].toString();
                if (senderUid.isEmpty()) senderUid = sender["userid"].toString();
                if (senderUid.isEmpty()) senderUid = m_sessionToUserId.value(senderSid);
                if (senderSid != m_sessionId && senderUid != m_userId && !senderUid.isEmpty()) {
                    const QString info = data["client"].toString() + "/" + data["version"].toString();
                    updatePeerClient(senderUid, info);  // bug 3 — stamps freshness + persists
                }
            }
        }

        if (target == "room" && eventType == "join") {
            QJsonArray joins = event["join"].toArray();
            bool anyNonSelf = false;
            for (const auto &j : joins) {
                const QJsonObject jo = j.toObject();
                QString sid = jo["sessionid"].toString();
                if (sid.isEmpty()) continue;
                // #bug5 — record the join-event id mappings for EVERY entry.
                // roomsessionid is the NEXTCLOUD session id — the id space the
                // REST call/{token} participant list reports — so this is the
                // exact bridge CallManager's REST poll needs to reach a peer
                // the HPB never emitted a JOINED edge for (a peer ESTABLISHED
                // in the call before we joined/redialed). userid additionally
                // feeds the #bug4 user-keyed adoption (userIdForSession) even
                // when no participants update ever arrives. The server sends
                // these join events as the initial in-the-room list on OUR
                // join too, so the maps are warm before the first poll tick.
                const QString uid   = jo["userid"].toString();
                const QString ncSid = jo["roomsessionid"].toString();
                if (!uid.isEmpty())   m_sessionToUserId[sid] = uid;
                if (!ncSid.isEmpty()) m_ncSessionToHpbSid[ncSid] = sid;
                TLOG_SIG("room join sid=" << sid.left(20) << "user=" << uid
                         << "ncSid=" << ncSid.left(20));
                if (sid != m_sessionId) {
                    qDebug() << "Signaling: room peer joined:" << sid.left(20);
                    emit roomPeerJoined(sid);
                    anyNonSelf = true;
                }
            }
            // Re-announce our TalQ version so the newcomer learns about us.
            // (HPB broadcasts are one-shot — peers that joined before our
            // initial hello already cached it.)
            if (anyNonSelf)
                sendTalqClientHello();
        }

        // #bug2 -- room peer LEFT (HPB room/leave). The payload is an array of
        // session-id strings on this HPB (tolerate the object form defensively).
        // Previously this event was logged and dropped: a vanished session emits
        // no inCall>0 -> 0 transition, so participantLeftCall never fired and a
        // peer whose publisher reconnected (new session/SSRCs) left the other
        // side decoding dead SSRCs forever.
        if (target == "room" && eventType == "leave") {
            const QJsonArray leaves = event["leave"].toArray();
            for (const QJsonValue &l : leaves) {
                QString sid = l.toString();
                if (sid.isEmpty())
                    sid = l.toObject()["sessionid"].toString();
                if (sid.isEmpty() || sid == m_sessionId)
                    continue;
                qDebug() << "Signaling: room peer left:" << sid.left(20);
                m_participantCallFlags.remove(sid);
                // #bug5 — prune this session's NC→HPB mapping so a later REST
                // poll can't resolve a departed session (its user re-appears
                // under a fresh pair of ids on rejoin).
                for (auto it = m_ncSessionToHpbSid.begin(); it != m_ncSessionToHpbSid.end(); ) {
                    if (it.value() == sid) it = m_ncSessionToHpbSid.erase(it);
                    else ++it;
                }
                emit roomPeerLeft(sid);
            }
        }

        if (target == "participants") {
            QJsonObject update = event["update"].toObject();
            QJsonArray users = update["users"].toArray();
            TLOG_SIG("participants update:" << users.size() << "users in room" << update["roomid"].toString());
            bool sawNewPeer = false;
            for (const QJsonValue &val : users) {
                QJsonObject user = val.toObject();
                int inCall = user["inCall"].toInt();
                QString sid = user["sessionId"].toString();
                if (sid.isEmpty()) continue;
                if (sid == m_sessionId) {
                    TLOG_SIG("  skip self sid=" << sid.left(20) << "inCall=" << inCall);
                    continue;
                }

                // Cache userid → displayName for typing indicators, and
                // sessionId → userid for the TalQ peer-identification fallback
                // (so we can resolve a sender's NC userId even on servers that
                // omit userid from broadcast sender annotations).
                QString userId = user["actorId"].toString();
                QString displayName = user["displayName"].toString();
                if (!userId.isEmpty() && !displayName.isEmpty())
                    m_participantNames[userId] = displayName;
                if (!userId.isEmpty())
                    m_sessionToUserId[sid] = userId;
                // #bug5 — some servers include the Nextcloud session id here
                // too; harvest it for the REST-poll NC→HPB bridge (the room
                // join events are the primary source).
                const QString ncSid = user["nextcloudSessionId"].toString();
                if (!ncSid.isEmpty())
                    m_ncSessionToHpbSid[ncSid] = sid;
                // Some servers/participant events omit displayName (only
                // actorId). Emitting an empty name is the "incoming call
                // shows 'Call' instead of the caller" bug. Resolve from
                // the name cache (filled by the room participant list /
                // earlier events), else fall back to the userId itself —
                // anything identifiable beats a generic "Call".
                if (displayName.isEmpty() && !userId.isEmpty())
                    displayName = m_participantNames.value(userId, userId);

                if (!m_participantCallFlags.contains(sid))
                    sawNewPeer = true;
                int prevFlags = m_participantCallFlags.value(sid, 0);
                m_participantCallFlags[sid] = inCall;

                TLOG_SIG("  participant sid=" << sid.left(20) << "inCall=" << inCall << "prev=" << prevFlags << "name=" << displayName);

                if (prevFlags == 0 && inCall > 0) {
                    TLOG_CALL("participant JOINED call sid=" << sid.left(20) << "flags=" << inCall);
                    emit participantJoinedCall(sid, inCall, displayName);
                } else if (prevFlags > 0 && inCall == 0) {
                    TLOG_CALL("participant LEFT call sid=" << sid.left(20));
                    // Prune maps to prevent unbounded growth
                    m_participantCallFlags.remove(sid);
                    if (!userId.isEmpty())
                        m_participantNames.remove(userId);
                    emit participantLeftCall(sid);
                } else if (prevFlags != inCall && inCall > 0) {
                    TLOG_CALL("participant flags CHANGED sid=" << sid.left(20) << prevFlags << "->" << inCall);
                    emit participantFlagsChanged(sid, prevFlags, inCall);
                }
            }
            // A peer we hadn't seen in this room appeared. HPB does not
            // reliably deliver a room/join event for them, so re-announce our
            // TalQ hello here too — this is the more dependable trigger for
            // the mutual peer-identification handshake.
            if (sawNewPeer)
                sendTalqClientHello();
        }
    }
}

void SignalingClient::sendHello()
{
    QJsonObject hello;
    hello["type"] = QString("hello");

    QJsonObject helloData;
    helloData["version"] = QString("1.0");

    // Reconnect-resume: if we hold a resume id from a prior hello, ask the
    // server to RESUME that session (short hello, no auth handshake). On
    // success we stay in the room/call as if the blip never happened; on
    // rejection the "error" handler clears the id and re-calls sendHello()
    // for a full fresh handshake.
    if (!m_resumeId.isEmpty()) {
        helloData["resumeid"] = m_resumeId;
        hello["hello"] = helloData;
        m_resuming = true;
        QString rjson = QJsonDocument(hello).toJson(QJsonDocument::Compact);
        qDebug() << "Signaling: WS >> hello RESUME" << m_resumeId.left(12) + "...";
        m_ws.sendTextMessage(rjson);
        return;
    }
    m_resuming = false;

    // hello v1.0 (userid + ticket). This is a server-accepted protocol
    // version and is the path that actually connects against the
    // standalone signaling backend. A v2.0 (signed-JWT) attempt was
    // rejected by the server at runtime with {"code":"invalid_format"}
    // (the exact v2 envelope this server expects is unverified), so we
    // stay on v1.0 rather than ship a broken handshake — the rest of the
    // flow (room/MCU/offer) is identical regardless of hello version.
    QJsonObject auth;
    auth["url"] = m_api->serverUrl() + "/ocs/v2.php/apps/spreed/api/v3/signaling/backend";

    QJsonObject params;
    params["userid"] = m_userId;
    params["ticket"] = m_ticket;
    auth["params"] = params;

    helloData["auth"] = auth;
    hello["hello"] = helloData;

    QString json = QJsonDocument(hello).toJson(QJsonDocument::Compact);
    qDebug() << "Signaling: WS >> hello v1.0 (creds redacted)";
    m_ws.sendTextMessage(json);
}

void SignalingClient::sendBye()
{
    if (!m_authenticated || m_ws.state() != QAbstractSocket::ConnectedState)
        return;
    QJsonObject msg;
    msg["type"] = QString("bye");
    msg["bye"] = QJsonObject();
    m_ws.sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
    qDebug() << "Signaling: WS >> bye";
}

void SignalingClient::joinRoom(const QString &token, bool force)
{
    // Same-room re-join guard. A redundant joinRoom for the room we are
    // ALREADY in is never harmless: the participants/active POST below mints
    // a FRESH Nextcloud session, and Nextcloud retires the old one — which is
    // the session carrying our in-call flag if a call is running. Field
    // (2026-07-03, Kalin↔Ilko): clicking around the main window re-selected
    // the call's conversation, the re-join replaced the in-call session, and
    // both sides collapsed to "Reconnecting". The reconnect path is the one
    // legitimate same-room re-join (fresh hello genuinely must re-enter the
    // room) — it passes force=true.
    // Gate on m_roomJoinAcked, NOT m_currentRoom: the latter is set
    // optimistically below (before the async POST + WS ack), so keying the
    // no-op on it would latch a FAILED or still-in-flight join as "already
    // joined" — a transient REST hiccup would then leave live-push dead with
    // no retry, and a call could proceed before HPB room membership exists.
    // m_roomJoinAcked is true only once the "room" WS response actually
    // arrived (and false again after any disconnect), so this no-op fires
    // strictly when we are genuinely, confirmed-in the room.
    if (!force && token == m_currentRoom && m_authenticated && m_roomJoinAcked) {
        qInfo() << "Signaling: already in room" << token
                << "— skipping redundant re-join (keeps the in-call session alive)";
        // Waiters (CallManager's call-start flow blocks on roomJoined with a
        // 15s timeout) must still be told the room is ready — being already
        // confirmed-in it IS the success case, not silence.
        emit roomJoined();
        return;
    }

    // 1.0 audit — m_sessionToUserId is otherwise INSERT-ONLY for the whole client
    // life (one entry per session/reconnect, never pruned), so bound it by
    // clearing on a real ROOM SWITCH. NOT on a same-room re-join (a signaling
    // reconnect calls joinRoom for the current room) — the peer-grace
    // correlation (CallManager: "m_sessionToUserId is NOT pruned on leave")
    // must survive a reconnect, so only wipe it when the room actually changes.
    const bool roomChanged = (token != m_currentRoom);
    m_currentRoom = token;
    m_roomJoinAcked = false;   // a join is now in flight; not acked until WS "room"

    // Clear state from previous room
    if (!m_typingUser.isEmpty()) {
        m_typingUser.clear();
        emit typingUserChanged();
        m_typingClearTimer.stop();
    }
    m_participantCallFlags.clear();
    m_participantNames.clear();
    if (roomChanged) {
        m_sessionToUserId.clear();
        m_ncSessionToHpbSid.clear();   // #bug5 — room-scoped, same policy
    }

    // Join as active participant — the response contains the sessionId
    // which the signaling server needs to verify room access
    QJsonObject empty;
    m_api->post("apps/spreed/api/v4/room/" + token + "/participants/active", empty,
        [this, token](bool ok, const QJsonObject &data, int) {
            qDebug() << "Signaling: participants/active response ok=" << ok
                     << "room match=" << (m_currentRoom == token)
                     << "auth=" << m_authenticated
                     << "sessionId=" << data["sessionId"].toString().left(20);
            if (!ok || m_currentRoom != token || !m_authenticated) return;

            // Extract the Nextcloud session ID from the response
            QString ncSessionId = data["sessionId"].toString();

            QJsonObject msg;
            msg["type"] = QString("room");

            QJsonObject room;
            room["roomid"] = token;
            if (!ncSessionId.isEmpty())
                room["sessionid"] = ncSessionId;
            msg["room"] = room;

            m_ws.sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
            qDebug() << "Signaling: joining room" << token << "with session" << ncSessionId.left(20) + "...";

            // Announce our TalQ version to other room participants. Other
            // TalQ clients cache it (by userId) and display it in conversation
            // info / call dialog. The official web client ignores it.
            sendTalqClientHello();
            // Arm the 5-minute periodic re-announce so peers with stale
            // caches (e.g. they joined the room while we were on the
            // pre-upgrade build) eventually heal.
            m_talqClientReannounce.start();
        });
}

void SignalingClient::sendTalqClientHello()
{
    if (m_userId.isEmpty()) {
        TLOG_SIG("sendTalqClientHello skipped — no userId yet");
        return;
    }
    QJsonObject data;
    data["type"] = QStringLiteral("talq.client");
    data["client"] = QStringLiteral("TalQ");
    data["version"] = QStringLiteral(TALQ_VERSION);
    // Self-identify in the payload itself — the spreedbackend's sender
    // annotation on room-broadcasts is unreliable (sessionid only, no userid
    // on some configs), so receivers should prefer this field.
    data["userid"] = m_userId;
    qDebug() << "Signaling: announcing TalQ/" TALQ_VERSION " in room" << m_currentRoom;
    sendBroadcastMessage(data);
}

// QSettings keys treat '/' as a group separator and choke on other characters
// that can appear in federated user IDs, so the userId is percent-encoded.
QString SignalingClient::peerClientInfo(const QString &userId) const
{
    // bug 3 — only report a cached version if it was observed recently. A
    // value loaded from disk that predates the freshness window (or a legacy
    // entry with no recorded timestamp, lastSeen==0) is treated as unknown so
    // the UI shows no chip instead of a confidently-wrong stale number.
    const qint64 lastSeen = m_peerClientSeen.value(userId, 0);
    if (!talq::isPeerVersionFresh(lastSeen,
                                  QDateTime::currentMSecsSinceEpoch(),
                                  kPeerVersionFreshMs))
        return QString();
    return m_peerClientInfo.value(userId);
}

void SignalingClient::updatePeerClient(const QString &userId, const QString &info)
{
    // bug 3 — single entry point that records a peer's TalQ version with a
    // fresh timestamp and persists it. Used by the HPB broadcast handlers and
    // by CallManager's in-call data-channel observation. The timestamp is
    // always refreshed (so a re-announce of the same version keeps it fresh);
    // peerClientInfoChanged is emitted whenever the displayed string changes
    // or a previously-stale value becomes current again, so the chrome
    // repaints.
    if (userId.isEmpty() || userId == m_userId || info.isEmpty())
        return;
    const QString prev = peerClientInfo(userId);   // freshness-gated current display
    m_peerClientInfo[userId] = info;
    m_peerClientSeen[userId] = QDateTime::currentMSecsSinceEpoch();
    persistPeerClient(userId, info);
    if (prev != info) {
        qDebug() << "Signaling: peer TalQ client" << userId << "=" << info;
        emit peerClientInfoChanged(userId, info);
    }
}

// QSettings keys treat '/' as a group separator and choke on other characters
// that can appear in federated user IDs, so the userId is percent-encoded.
void SignalingClient::loadPersistedPeerClients()
{
    QSettings s;
    s.beginGroup(QStringLiteral("peerClients"));
    const QStringList keys = s.childKeys();
    for (const QString &k : keys) {
        const QString uid = QString::fromUtf8(
            QByteArray::fromPercentEncoding(k.toLatin1()));
        const QString info = s.value(k).toString();
        if (!uid.isEmpty() && !info.isEmpty())
            m_peerClientInfo[uid] = info;
    }
    s.endGroup();
    // bug 3 — parallel last-seen timestamps. Entries written by an older build
    // have no timestamp here, so they default to 0 and are treated as stale by
    // peerClientInfo() — which is exactly what prevents the long-stale 0.28.3
    // entry from being shown as current after upgrade.
    s.beginGroup(QStringLiteral("peerClientsSeen"));
    const QStringList seenKeys = s.childKeys();
    for (const QString &k : seenKeys) {
        const QString uid = QString::fromUtf8(
            QByteArray::fromPercentEncoding(k.toLatin1()));
        if (!uid.isEmpty())
            m_peerClientSeen[uid] = s.value(k).toLongLong();
    }
    s.endGroup();
    if (!m_peerClientInfo.isEmpty())
        qDebug() << "Signaling: loaded" << m_peerClientInfo.size()
                 << "persisted peer client(s)";
}

void SignalingClient::persistPeerClient(const QString &userId, const QString &info)
{
    if (userId.isEmpty()) return;
    const QString key = QString::fromLatin1(userId.toUtf8().toPercentEncoding());
    QSettings s;
    s.beginGroup(QStringLiteral("peerClients"));
    s.setValue(key, info);
    s.endGroup();
    // Persist the freshness timestamp alongside (set by updatePeerClient).
    s.beginGroup(QStringLiteral("peerClientsSeen"));
    s.setValue(key, m_peerClientSeen.value(userId, QDateTime::currentMSecsSinceEpoch()));
    s.endGroup();
}

void SignalingClient::sendStartedTyping()
{
    sendRoomMessage("startedTyping");
}

void SignalingClient::sendStoppedTyping()
{
    sendRoomMessage("stoppedTyping");
}

void SignalingClient::sendRoomMessage(const QString &msgType)
{
    if (!m_authenticated || m_currentRoom.isEmpty()) return;
    // Typing privacy. The user can turn "share my typing status" off on the
    // server, and until 0.65.3 TalQ never read that setting and broadcast
    // regardless -- so a user who had explicitly opted out was still telling
    // every room when they were typing. Defaults to sharing when the server
    // does not say, which is what TalQ did before and matches Talk's default.
    if (!m_shareTypingStatus
        && (msgType == QLatin1String("startedTyping")
            || msgType == QLatin1String("stoppedTyping")))
        return;

    QJsonObject msg;
    msg["type"] = QString("message");

    QJsonObject message;
    QJsonObject recipient;
    recipient["type"] = QString("room");
    message["recipient"] = recipient;

    QJsonObject data;
    data["type"] = msgType;
    message["data"] = data;

    msg["message"] = message;

    m_ws.sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

// --- WebRTC call signaling ---

void SignalingClient::sendSessionMessage(const QString &toSessionId, const QString &type,
                                          const QJsonObject &payload, const QString &sid,
                                          const QJsonObject &extraData, const QString &roomType)
{
    if (!m_authenticated) return;

    QJsonObject msg;
    msg["type"] = QString("message");

    QJsonObject message;
    QJsonObject recipient;
    recipient["type"] = QString("session");
    recipient["sessionid"] = toSessionId;
    message["recipient"] = recipient;

    QJsonObject data;
    data["type"] = type;
    data["to"] = toSessionId;
    data["sid"] = sid;
    data["roomType"] = roomType;
    data["payload"] = payload;
    // Merge extra fields (e.g. audiocodec, videocodec for Janus room creation)
    for (auto it = extraData.begin(); it != extraData.end(); ++it)
        data[it.key()] = it.value();
    message["data"] = data;

    msg["message"] = message;

    QString json = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    qDebug() << "Signaling: WS >>" << type << json.left(500);
    m_ws.sendTextMessage(json);
}

void SignalingClient::sendOffer(const QString &toSessionId, const QString &sdp,
                                const QString &sid, const QString &nick,
                                const QString &roomType, const QString &broadcaster,
                                bool mcuCodecHints)
{
    QJsonObject payload;
    payload["type"] = QString("offer");
    payload["sdp"] = sdp;
    if (!nick.isEmpty()) payload["nick"] = nick;
    // Codec preferences passed to the signaling server (offer-codecs) so
    // Janus creates the room with the right codecs; without them Janus
    // sets codec 'none' and rejects media. The screen stream is
    // video-only (VP8) and the publisher must carry a `broadcaster`
    // field = its own session id (upstream Peer.send), or Janus can't
    // associate the screen publisher.
    // We publish hardware **H264** (highest quality at the raised HPB
    // bitrate ceiling, near-zero CPU). Janus is an SFU (no transcode) and
    // forces ONE codec per room from this preference list intersected
    // with what the publisher offers, so the first publisher's list here
    // decides the whole room. "h264,vp8" => H264 when offered (all
    // updated TalQ clients), VP8 only as a safety fallback. By project
    // decision, non-H264 clients simply won't see video — we never
    // transcode or downgrade the conference for one stale client.
    QJsonObject extra;
    // 0.41.9 — omit codec hints for true P2P so the HPB relays the offer
    // session-to-session instead of hijacking it as a Janus MCU publish.
    if (mcuCodecHints) {
        if (roomType == "screen") {
            extra["videocodec"] = QString("h264,vp8");
            if (!broadcaster.isEmpty())
                extra["broadcaster"] = broadcaster;
        } else {
            extra["audiocodec"] = QString("opus");
            extra["videocodec"] = QString("h264,vp8");
        }
    }
    sendSessionMessage(toSessionId, "offer", payload, sid, extra, roomType);
    qDebug() << "Signaling: sent offer to" << toSessionId.left(20)
             << "sid=" << sid.left(10) << "roomType=" << roomType;
}

void SignalingClient::sendAnswer(const QString &toSessionId, const QString &sdp,
                                  const QString &sid, const QString &nick, const QString &roomType)
{
    QJsonObject payload;
    payload["type"] = QString("answer");
    payload["sdp"] = sdp;
    if (!nick.isEmpty()) payload["nick"] = nick;
    sendSessionMessage(toSessionId, "answer", payload, sid, {}, roomType);
    qDebug() << "Signaling: sent answer to" << toSessionId.left(20) << "sid=" << sid.left(10);
}

void SignalingClient::sendCandidate(const QString &toSessionId, const QJsonObject &candidate,
                                     const QString &sid, const QString &roomType)
{
    QJsonObject payload;
    payload["candidate"] = candidate;
    sendSessionMessage(toSessionId, "candidate", payload, sid, {}, roomType);
}

void SignalingClient::sendEndOfCandidates(const QString &toSessionId, const QString &sid,
                                          const QString &roomType)
{
    sendSessionMessage(toSessionId, "endOfCandidates", QJsonObject(), sid, {}, roomType);
}

void SignalingClient::sendSelectStream(const QString &toSessionId, const QString &sid,
                                       int substream, int temporal, const QString &roomType)
{
    QJsonObject payload;
    payload["substream"] = substream;
    payload["temporal"]  = temporal;
    sendSessionMessage(toSessionId, "selectStream", payload, sid, {}, roomType);
    qDebug() << "Signaling: selectStream to" << toSessionId.left(20)
             << "substream=" << substream << "temporal=" << temporal;
}

void SignalingClient::sendBroadcastMessage(const QJsonObject &data)
{
    if (!m_authenticated || m_currentRoom.isEmpty()) return;

    QJsonObject msg;
    msg["type"] = QString("message");

    QJsonObject message;
    QJsonObject recipient;
    recipient["type"] = QString("room");
    message["recipient"] = recipient;
    message["data"] = data;
    msg["message"] = message;

    m_ws.sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
    qDebug() << "Signaling: sent broadcast message" << data["type"].toString();
}

void SignalingClient::sendMinimalMessage(const QString &toSessionId, const QJsonObject &data)
{
    if (!m_authenticated) return;

    QJsonObject msg;
    msg["type"] = QString("message");

    QJsonObject message;
    QJsonObject recipient;
    recipient["type"] = QString("session");
    recipient["sessionid"] = toSessionId;
    message["recipient"] = recipient;
    message["data"] = data;
    msg["message"] = message;

    m_ws.sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
    qDebug() << "Signaling: sent minimal message" << data["type"].toString() << "to" << toSessionId.left(20);
}

void SignalingClient::requestOffer(const QString &sessionId, const QString &roomType)
{
    if (!m_authenticated) return;

    QJsonObject msg;
    msg["type"] = QString("message");

    QJsonObject message;
    QJsonObject recipient;
    recipient["type"] = QString("session");
    recipient["sessionid"] = sessionId;
    message["recipient"] = recipient;

    QJsonObject data;
    data["type"] = QString("requestoffer");
    data["roomType"] = roomType;
    message["data"] = data;

    msg["message"] = message;

    m_ws.sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
    qInfo() << "Signaling: sent requestOffer to" << sessionId.left(20) << "type=" << roomType;
}

void SignalingClient::reconnect()
{
    if (m_api->serverUrl().isEmpty()) return;
    // A1 — when a resume is still possible, race the socket on a short fixed
    // delay so the resume lands inside the server's grace window. Only the cold
    // path (no resume id) uses the exponential backoff, which protects a dead
    // server from a reconnect storm.
    if (!m_resumeId.isEmpty() && !m_signalingUrl.isEmpty()) {
        m_reconnectTimer.start(kResumeReconnectMs);
        return;
    }
    m_reconnectTimer.start(m_reconnectDelay);
    m_reconnectDelay = qMin(m_reconnectDelay * 2, 60000);
}
