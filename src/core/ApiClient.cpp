#include "core/ApiClient.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QImage>
#include <QNetworkCookieJar>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QXmlStreamReader>
#include <QDebug>
#include <QTimer>

ApiClient::ApiClient(QObject *parent)
    : QObject(parent)
{
    // While offline, actively re-probe for recovery so "reconnected" is
    // noticed within ~15 s instead of waiting for the next conversation-list
    // poll. Started/stopped by setReachable().
    m_reachProbeTimer.setInterval(15000);
    connect(&m_reachProbeTimer, &QTimer::timeout, this, [this]{ probeReachability(); });
}

void ApiClient::setServerUrl(const QString &url)
{
    QString cleaned = url;
    while (cleaned.endsWith('/'))
        cleaned.chop(1);
    if (m_serverUrl != cleaned) {
        m_serverUrl = cleaned;
        // A server change (login / switch) starts a fresh reachability slate:
        // don't inherit a stale "offline" from the previous server/session.
        m_reachMisses = 0;
        setReachable(true);
        emit serverUrlChanged();
    }
}

void ApiClient::setCredentials(const QString &user, const QString &password)
{
    // A different user (including logout → empty) MUST start from a clean
    // session. Nextcloud authenticates a valid session cookie before the
    // Basic-auth header, so without this a leftover cookie from the old
    // account makes every request resolve to that old user — the app then
    // reports the wrong identity until restarted.
    const bool userChanged = (user != m_user);

    // Zero old credentials before overwriting
    m_password.fill(QChar(0));
    m_user = user;
    m_password = password;
    if (userChanged) {
        // The canonical uid belongs to the OLD account. Keeping it would point
        // every DAV URL at the previous user's home until fetchUserInfo()
        // happens to return, and on logout it would outlive the credential
        // entirely. davUser() falls back to the login name until the real uid
        // arrives.
        m_davUserId.clear();
        resetSession();
    }
    emit authenticatedChanged();
}

void ApiClient::setDavUserId(const QString &uid)
{
    m_davUserId = uid;
}

QString ApiClient::encodeDavPath(const QString &path)
{
    // '/' stays literal so it keeps working as the path separator; everything
    // else outside the unreserved set is percent-encoded. Spaces, parentheses
    // and non-ASCII names all round-trip -- verified against the live server
    // on names containing each.
    QString p = path;
    while (p.startsWith(QLatin1Char('/')))
        p.remove(0, 1);
    return QString::fromLatin1(QUrl::toPercentEncoding(p, "/"));
}

void ApiClient::resetSession()
{
    // Fresh jar drops the previous account's session cookie; clearing the
    // access cache flushes pooled connections plus the cached HTTP
    // auth/credential and SSL-session state so nothing is reused.
    // Parentless so the NAM takes ownership and deletes the prior jar
    // (the internal default, then each replaced one) — no accumulation
    // across repeated account switches.
    m_nam.setCookieJar(new QNetworkCookieJar);
    m_nam.clearAccessCache();
}

QNetworkRequest ApiClient::makeRequest(const QString &path, const QUrlQuery &params) const
{
    // Build full OCS URL
    QString fullPath = path;
    if (!fullPath.startsWith('/'))
        fullPath.prepend('/');

    QUrl url(m_serverUrl + "/ocs/v2.php" + fullPath);
    if (!params.isEmpty())
        url.setQuery(params);

    QNetworkRequest req(url);
    req.setRawHeader("OCS-APIRequest", "true");
    req.setRawHeader("Accept", "application/json");
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyBasicAuth(req);
    // 0.63.1 — a black-holed connection otherwise hangs until the OS TCP
    // timeout even when the caller is alive and waiting. The 0.63.0 reply
    // lifetime work fixed the CONTEXT-death case; this is the orthogonal one.
    // This is Qt's own idle/inactivity timeout (resets on any bytes sent or
    // received; NOT a hard cap on total transfer duration — it also happens
    // to be Qt's documented DefaultTransferTimeoutConstant), so a slow-but-
    // progressing 200-message chat history fetch over a WAN link is fine;
    // only a genuine multi-second stall trips it.
    // Long-poll deliberately overrides this — it is meant to stay open.
    req.setTransferTimeout(30'000);
    return req;
}

void ApiClient::applyBasicAuth(QNetworkRequest &req) const
{
    if (m_user.isEmpty()) return;
    QString credentials = m_user + ":" + m_password;
    req.setRawHeader("Authorization", "Basic " + credentials.toUtf8().toBase64());
}

bool ApiClient::isRetryableTransportError(QNetworkReply *reply) const
{
    // Retry ONLY when the request provably never reached the server, so
    // replaying a POST cannot double-submit. A real HTTP status
    // (404/500/…) means the server answered; never retry those.
    // RemoteHostClosedError / TemporaryNetworkFailureError are NOT safe:
    // the server may have received and acted on the request before the
    // connection dropped (a replayed POST would double-join a call). The
    // only conditions we treat as never-established:
    //   • ContentReSendError — Qt's explicit "safe to resend" signal.
    //   • HTTP/2 GOAWAY before the stream was established — the exact
    //     stale-pooled-connection case this retry exists for; the error
    //     string is the only version-stable signal and it literally says
    //     the stream was never established.
    const QVariant httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (httpStatus.isValid() && httpStatus.toInt() != 0)
        return false;
    if (reply->error() == QNetworkReply::ContentReSendError)
        return true;
    return reply->errorString().contains(
        QStringLiteral("stopped accepting new streams before this stream "
                       "was established"), Qt::CaseInsensitive);
}

void ApiClient::noteNetworkOutcome(QNetworkReply *reply)
{
    if (!reply) return;
    const QNetworkReply::NetworkError err = reply->error();
    // A deliberately-aborted request (logout cancelAll, context death) is not
    // an outage — ignore it so teardown can't trip the offline banner.
    if (err == QNetworkReply::OperationCanceledError) return;

    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // The server answered at the HTTP layer (any status — a 401/404/500 still
    // proves the box is reachable), so we're online. Recovery is instant.
    if (err == QNetworkReply::NoError || httpStatus > 0) {
        m_reachMisses = 0;
        setReachable(true);
        return;
    }

    // Transport failure with no HTTP response: the request never reached the
    // server. Confirm a first miss fast with an active probe (don't wait for
    // the 30 s poll); flip offline once misses cross the threshold.
    if (++m_reachMisses == 1 && !m_probeInFlight)
        QTimer::singleShot(2000, this, [this]{ probeReachability(); });
    if (m_reachMisses >= kOfflineMisses)
        setReachable(false);
}

void ApiClient::setReachable(bool online)
{
    if (m_serverReachable == online) return;
    m_serverReachable = online;
    if (online) {
        m_reachProbeTimer.stop();
        qInfo() << "ApiClient: server reachability -> ONLINE";
    } else {
        m_reachProbeTimer.start();   // re-probe for recovery while down
        qWarning() << "ApiClient: server reachability -> OFFLINE"
                   << "(server not answering REST requests)";
    }
    emit serverReachabilityChanged(online);
}

void ApiClient::probeReachability()
{
    if (m_serverUrl.isEmpty() || m_probeInFlight) return;
    m_probeInFlight = true;
    // status.php is Nextcloud's canonical, unauthenticated health endpoint —
    // tiny JSON, no OCS envelope, present on every server. Feeds the same
    // reachability tracker as normal traffic via noteNetworkOutcome().
    QNetworkRequest req{QUrl(m_serverUrl + QStringLiteral("/status.php"))};
    req.setTransferTimeout(10000);
    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]{
        m_probeInFlight = false;
        noteNetworkOutcome(reply);
        reply->deleteLater();
    });
}

void ApiClient::handleReply(QNetworkReply *reply, Callback callback,
                            std::function<QNetworkReply*()> resend, int attempt)
{
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, callback, resend, attempt]() {
        m_pendingReplies.removeOne(reply);
        reply->deleteLater();

        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        noteNetworkOutcome(reply);

        if (reply->error() != QNetworkReply::NoError) {
            // First call after an idle period (e.g. joining a call) rides a
            // pooled HTTP/2 connection the server has since closed; the
            // request fails before it ever reaches the server. Drop the
            // dead connection and replay once — otherwise the join silently
            // fails and the user sees the call "drop immediately".
            if (attempt == 0 && resend && isRetryableTransportError(reply)) {
                qWarning() << "API transient transport error:"
                           << reply->errorString()
                           << "— retrying once on a fresh connection";
                m_nam.clearConnectionCache();
                QNetworkReply *r2 = resend();
                m_pendingReplies.append(r2);
                handleReply(r2, callback, resend, attempt + 1);
                return;
            }
            // Capture the response BODY on error — Qt's errorString() is just
            // "server replied:" with an empty tail, so the server's real reason
            // (e.g. an OCS meta.message like "Call not found" / a participant-
            // session error) is otherwise lost. This is what lets us diagnose a
            // failed POST call/{token} from the log alone.
            const QByteArray errBody = reply->readAll();
            QString ocsMsg;
            if (!errBody.isEmpty()) {
                const QJsonObject m = QJsonDocument::fromJson(errBody)
                    .object().value("ocs").toObject().value("meta").toObject();
                ocsMsg = m.value("message").toString();
            }
            qWarning() << "API error:" << reply->errorString() << "status:" << status
                       << "ocsMsg:" << (ocsMsg.isEmpty() ? QStringLiteral("(none)") : ocsMsg)
                       << "body:" << QString::fromUtf8(errBody.left(500));
            callback(false, {}, status);
            return;
        }

        QByteArray body = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isNull()) {
            qWarning() << "Invalid JSON response";
            callback(false, {}, status);
            return;
        }

        // Unwrap OCS envelope: { ocs: { meta: {...}, data: {...} } }
        QJsonObject root = doc.object();
        QJsonObject ocs = root["ocs"].toObject();
        QJsonObject meta = ocs["meta"].toObject();
        int ocsStatus = meta["statuscode"].toInt();

        if (ocsStatus >= 200 && ocsStatus < 300) {
            callback(true, ocs["data"].toObject(), ocsStatus);
        } else {
            qWarning() << "OCS error:" << meta["message"].toString() << "code:" << ocsStatus;
            callback(false, ocs["data"].toObject(), ocsStatus);
        }
    });
}

void ApiClient::handleArrayReply(QNetworkReply *reply, ArrayCallback callback,
                                 std::function<QNetworkReply*()> resend, int attempt)
{
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, callback, resend, attempt]() {
        m_pendingReplies.removeOne(reply);
        reply->deleteLater();

        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        noteNetworkOutcome(reply);

        if (reply->error() != QNetworkReply::NoError) {
            if (attempt == 0 && resend && isRetryableTransportError(reply)) {
                qWarning() << "API transient transport error:"
                           << reply->errorString()
                           << "— retrying once on a fresh connection";
                m_nam.clearConnectionCache();
                QNetworkReply *r2 = resend();
                m_pendingReplies.append(r2);
                handleArrayReply(r2, callback, resend, attempt + 1);
                return;
            }
            const QByteArray errBody = reply->readAll();
            QString ocsMsg;
            if (!errBody.isEmpty()) {
                const QJsonObject m = QJsonDocument::fromJson(errBody)
                    .object().value("ocs").toObject().value("meta").toObject();
                ocsMsg = m.value("message").toString();
            }
            qWarning() << "API error:" << reply->errorString() << "status:" << status
                       << "ocsMsg:" << (ocsMsg.isEmpty() ? QStringLiteral("(none)") : ocsMsg)
                       << "body:" << QString::fromUtf8(errBody.left(500));
            callback(false, {}, status);
            return;
        }

        QByteArray body = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(body);
        QJsonObject root = doc.object();
        QJsonObject ocs = root["ocs"].toObject();
        QJsonObject meta = ocs["meta"].toObject();
        int ocsStatus = meta["statuscode"].toInt();

        if (ocsStatus >= 200 && ocsStatus < 300) {
            // data can be array or object — handle both
            QJsonValue dataVal = ocs["data"];
            if (dataVal.isArray()) {
                callback(true, dataVal.toArray(), ocsStatus);
            } else {
                callback(true, QJsonArray{dataVal.toObject()}, ocsStatus);
            }
        } else {
            callback(false, {}, ocsStatus);
        }
    });
}

void ApiClient::get(const QString &path, const QUrlQuery &params, Callback callback)
{
    auto resend = [this, path, params]() {
        return m_nam.get(makeRequest(path, params));
    };
    auto *reply = resend();
    m_pendingReplies.append(reply);
    handleReply(reply, callback, resend);
}

void ApiClient::get(const QString &path, Callback callback)
{
    get(path, {}, callback);
}

void ApiClient::getArray(const QString &path, const QUrlQuery &params, ArrayCallback callback)
{
    auto resend = [this, path, params]() {
        return m_nam.get(makeRequest(path, params));
    };
    auto *reply = resend();
    m_pendingReplies.append(reply);
    handleArrayReply(reply, callback, resend);
}

void ApiClient::getArray(const QString &path, ArrayCallback callback)
{
    getArray(path, {}, callback);
}

void ApiClient::post(const QString &path, const QJsonObject &body, Callback callback)
{
    QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);
    auto resend = [this, path, data]() {
        return m_nam.post(makeRequest(path), data);
    };
    auto *reply = resend();
    m_pendingReplies.append(reply);
    handleReply(reply, callback, resend);
}

void ApiClient::post(const QString &path, Callback callback)
{
    post(path, {}, callback);
}

void ApiClient::put(const QString &path, const QJsonObject &body, Callback callback)
{
    QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);
    auto resend = [this, path, data]() {
        return m_nam.put(makeRequest(path), data);
    };
    auto *reply = resend();
    m_pendingReplies.append(reply);
    handleReply(reply, callback, resend);
}

void ApiClient::del(const QString &path, Callback callback)
{
    del(path, QUrlQuery(), callback);
}

void ApiClient::del(const QString &path, const QUrlQuery &params, Callback callback)
{
    auto resend = [this, path, params]() {
        return m_nam.deleteResource(makeRequest(path, params));
    };
    auto *reply = resend();
    m_pendingReplies.append(reply);
    handleReply(reply, callback, resend);
}

void ApiClient::del(const QString &path, const QJsonObject &body, Callback callback)
{
    QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);
    // Fresh buffer per send: the previous one is parented to (and dies
    // with) the failed reply, so a retry must build its own.
    auto resend = [this, path, data]() {
        auto *buf = new QBuffer();
        buf->setData(data);
        buf->open(QIODevice::ReadOnly);
        auto *reply = m_nam.sendCustomRequest(makeRequest(path), "DELETE", buf);
        buf->setParent(reply);  // ensure buffer lives until reply completes
        return reply;
    };
    auto *reply = resend();
    m_pendingReplies.append(reply);
    handleReply(reply, callback, resend);
}

void ApiClient::delMustComplete(const QString &path, const QJsonObject &body,
                                QObject *context,
                                std::function<void(bool, int)> onDone, int attempt)
{
    // Reusable "must-complete" DELETE: sends, and on a CONFIRMED
    // non-delivery (statusCode 0 = no HTTP reply, or 5xx) retries with
    // bounded backoff. NOT speculative — only after the reply says it
    // didn't land. For requests whose loss has user-visible consequences
    // on a high-latency/flaky link (leaveCall → other party stuck in the
    // call; status revert → stuck "In a call"). 4xx (incl. 404) is treated
    // as "the server processed us" → done. Never blocks the UI.
    QPointer<ApiClient> self(this);
    del(path, body, [self, path, body, context, onDone, attempt]
                    (bool ok, const QJsonObject &, int statusCode) {
        if (ok) { if (onDone) onDone(true, statusCode); return; }
        const bool transient = (statusCode == 0 || statusCode >= 500);
        constexpr int kMaxAttempts = 4;
        if (transient && attempt + 1 < kMaxAttempts) {
            const int delayMs = 1000 * (1 << attempt);  // 1s, 2s, 4s
            QObject *ctx = context ? context : self.data();
            QTimer::singleShot(delayMs, ctx, [self, path, body, context, onDone, attempt]() {
                if (self) self->delMustComplete(path, body, context, onDone, attempt + 1);
            });
        } else {
            if (onDone) onDone(false, statusCode);
        }
    });
}

QNetworkReply *ApiClient::getRaw(const QString &path, const QUrlQuery &params)
{
    auto req = makeRequest(path, params);
    auto *reply = m_nam.get(req);
    trackReply(reply);
    return reply;
}

QNetworkReply *ApiClient::postRaw(const QString &path, const QByteArray &body)
{
    auto req = makeRequest(path);
    auto *reply = m_nam.post(req, body);
    trackReply(reply);
    return reply;
}

QNetworkReply *ApiClient::getLongPoll(const QString &path, const QUrlQuery &params, int timeoutSecs,
                                      const QMap<QByteArray, QByteArray> &headers)
{
    auto req = makeRequest(path, params);
    req.setTransferTimeout((timeoutSecs + 5) * 1000); // extra 5s grace
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        req.setRawHeader(it.key(), it.value());
    auto *reply = m_nam.get(req);
    trackReply(reply);
    return reply;
}

QNetworkReply *ApiClient::getAbsoluteUrl(const QString &path)
{
    QNetworkRequest req{QUrl(m_serverUrl + path)};
    applyBasicAuth(req);
    // Not added to m_pendingReplies — caller manages lifetime
    return m_nam.get(req);
}

QNetworkReply *ApiClient::putAbsoluteUrl(const QString &path, const QByteArray &body)
{
    QNetworkRequest req{QUrl(m_serverUrl + path)};
    applyBasicAuth(req);
    return m_nam.put(req, body);
}

QNetworkReply *ApiClient::davRequest(const QByteArray &verb, const QString &path,
                                     const QByteArray &body,
                                     const QMap<QByteArray, QByteArray> &headers)
{
    QNetworkRequest req{QUrl(m_serverUrl + path)};
    applyBasicAuth(req);
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        req.setRawHeader(it.key(), it.value());
    if (body.isEmpty())
        return m_nam.sendCustomRequest(req, verb);   // MKCOL / MOVE — no payload
    // PUT a chunk: own a QBuffer that outlives the request.
    auto *buf = new QBuffer();
    buf->setData(body);
    buf->open(QIODevice::ReadOnly);
    auto *reply = m_nam.sendCustomRequest(req, verb, buf);
    buf->setParent(reply);
    return reply;
}

QNetworkReply *ApiClient::postAbsoluteUrl(const QString &path, const QByteArray &body)
{
    QNetworkRequest req{QUrl(m_serverUrl + path)};
    applyBasicAuth(req);
    return m_nam.post(req, body);
}

void ApiClient::bindReplyLifetime(QNetworkReply *reply, QObject *context)
{
    if (!reply) return;
    // Unconditional and independent of the context: this is the ONLY
    // deleteLater that always runs.
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
    // A caller that dies mid-flight should also stop paying for the
    // transfer — nobody is left to read the body.
    //
    // Qt::QueuedConnection is deliberate, not the connect() default: emit
    // destroyed(this) runs BEFORE ~QObject disconnects the connections where
    // `context` is the RECEIVER (that cleanup is a later pass in the same
    // destructor), so at the moment this fires, the per-endpoint
    // `finished`->lambda connection gated on `context` is still live. A
    // Direct connection here would call reply->abort() synchronously,
    // inside context's destructor — and if abort() synchronously re-emits
    // finished() (backend-dependent), that still-live connection would
    // invoke `callback` against a `context` whose derived-class state has
    // already been torn down: the exact use-after-free the context gating
    // exists to prevent. Queuing defers abort() until after context's
    // destructor (and its connection cleanup) has fully completed, so the
    // gated connection is already gone by the time it runs — safe
    // regardless of whether the backend's finished() is sync or async.
    if (context)
        connect(context, &QObject::destroyed, reply, &QNetworkReply::abort,
                Qt::QueuedConnection);
}

void ApiClient::fetchFileImage(int fileId, int maxDim, QObject *context,
                               std::function<void(const QImage &, const QString &)> callback)
{
    // Nextcloud's preview endpoint returns a rendering capped to (x,y); a=1
    // preserves aspect ratio. maxDim controls the larger edge — callers pass
    // the viewport size so we don't waste bandwidth on 4K thumbnails.
    QUrl url(m_serverUrl + "/index.php/core/preview");
    QUrlQuery q;
    q.addQueryItem("fileId", QString::number(fileId));
    q.addQueryItem("x", QString::number(maxDim));
    q.addQueryItem("y", QString::number(maxDim));
    q.addQueryItem("a", "1");
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setRawHeader("OCS-APIRequest", "true");
    applyBasicAuth(req);

    auto *reply = m_nam.get(req);
    bindReplyLifetime(reply, context);
    // Use `context` as the receiver so the lambda is auto-disconnected if
    // the caller (e.g. ImageViewerDialog) is destroyed mid-flight.
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            qWarning() << "fetchFileImage: HTTP" << status << reply->errorString();
            callback(QImage(), reply->errorString());
            return;
        }
        QImage img = QImage::fromData(reply->readAll());
        if (img.isNull())
            qWarning() << "fetchFileImage: decode failed — content-type:"
                       << reply->header(QNetworkRequest::ContentTypeHeader).toString();
        callback(img, img.isNull() ? QStringLiteral("decode failed") : QString());
    });
}

void ApiClient::fetchMentions(const QString &token, const QString &search,
                              QObject *context,
                              std::function<void(const QVector<MentionCandidate> &)> callback)
{
    // Build the full URL as a string so Nextcloud subpath installations
    // (e.g. https://host/nextcloud) don't have their prefix stripped by QUrl::setPath.
    // Mentions endpoint lives under chat API v1, not v4. v4 is for the room
    // API; the spreed chat endpoints (mentions, share, schedule, …) are all
    // under /api/v1/chat/. A wrong version here returns HTTP 404 silently,
    // which manifests as "mention popup never appears" — the empty candidate
    // list takes the same path as "no matching users".
    QUrl url(m_serverUrl + QStringLiteral("/ocs/v2.php/apps/spreed/api/v1/chat/")
             + token + QStringLiteral("/mentions"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("search"), search);
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("20"));
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setRawHeader("OCS-APIRequest", "true");
    req.setRawHeader("Accept", "application/json");
    applyBasicAuth(req);

    QNetworkReply *reply = m_nam.get(req);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QVector<MentionCandidate> out;

        if (reply->error() != QNetworkReply::NoError) {
            int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 403 || status == 404)
                qWarning() << "fetchMentions:" << status << reply->errorString();
            callback(out);
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (doc.isNull()) {
            qWarning() << "fetchMentions: non-JSON response (reverse proxy HTML login page?)";
            callback(out);
            return;
        }
        // Nextcloud OCS returns HTTP 200 even for server-side errors; the real
        // status lives in ocs.meta.statuscode. 100 and 200 are success.
        QJsonObject ocs = doc.object().value(QStringLiteral("ocs")).toObject();
        int ocsStatus = ocs.value(QStringLiteral("meta")).toObject()
                           .value(QStringLiteral("statuscode")).toInt(200);
        if (ocsStatus != 100 && ocsStatus != 200) {
            QString msg = ocs.value(QStringLiteral("meta")).toObject()
                             .value(QStringLiteral("message")).toString();
            qWarning() << "fetchMentions: OCS status" << ocsStatus << msg;
            callback(out);
            return;
        }
        QJsonArray data = ocs.value(QStringLiteral("data")).toArray();
        for (const QJsonValue &v : data) {
            QJsonObject o = v.toObject();
            MentionCandidate c;
            c.id     = o.value(QStringLiteral("id")).toString();
            c.label  = o.value(QStringLiteral("label")).toString();
            c.source = MentionCandidate::parseSource(
                          o.value(QStringLiteral("source")).toString());
            if (!c.id.isEmpty()) out.append(c);
        }
        callback(out);
    });
}

namespace {
// Shared parser for both /bot/{token} and /bot/admin response shapes.
QVector<BotInfo> parseBotList(const QJsonArray &data)
{
    QVector<BotInfo> out;
    for (const QJsonValue &v : data) {
        QJsonObject o = v.toObject();
        BotInfo b;
        b.id          = o.value(QStringLiteral("id")).toInt();
        b.name        = o.value(QStringLiteral("name")).toString();
        b.description = o.value(QStringLiteral("description")).toString();
        b.state       = o.value(QStringLiteral("state")).toInt();
        b.features    = o.value(QStringLiteral("features")).toInt();
        b.errorMessage = o.value(QStringLiteral("error_message")).toString();
        if (b.id != 0) out.append(b);
    }
    return out;
}
} // namespace

void ApiClient::fetchEnabledBots(const QString &token, QObject *context,
                                 std::function<void(bool, const QVector<BotInfo> &)> callback)
{
    QUrl url(m_serverUrl + QStringLiteral("/ocs/v2.php/apps/spreed/api/v1/bot/") + token);
    QNetworkRequest req(url);
    req.setRawHeader("OCS-APIRequest", "true");
    req.setRawHeader("Accept", "application/json");
    applyBasicAuth(req);

    QNetworkReply *reply = m_nam.get(req);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QVector<BotInfo> out;
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "fetchEnabledBots:" << reply->errorString();
            callback(false, out);
            return;
        }
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject ocs = doc.object().value(QStringLiteral("ocs")).toObject();
        int status = ocs.value(QStringLiteral("meta")).toObject()
                        .value(QStringLiteral("statuscode")).toInt(200);
        if (status != 100 && status != 200) { callback(false, out); return; }
        callback(true, parseBotList(ocs.value(QStringLiteral("data")).toArray()));
    });
}

void ApiClient::fetchAllBots(QObject *context,
                             std::function<void(bool, const QVector<BotInfo> &)> callback)
{
    QUrl url(m_serverUrl + QStringLiteral("/ocs/v2.php/apps/spreed/api/v1/bot/admin"));
    QNetworkRequest req(url);
    req.setRawHeader("OCS-APIRequest", "true");
    req.setRawHeader("Accept", "application/json");
    applyBasicAuth(req);

    QNetworkReply *reply = m_nam.get(req);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QVector<BotInfo> out;
        if (reply->error() != QNetworkReply::NoError) {
            // 403 just means "not admin" — caller should fall back to the
            // per-room list. Pass ok=true with empty so caller treats it
            // as "no server bots visible to you" rather than an error.
            int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (httpStatus == 403) { callback(true, out); return; }
            qWarning() << "fetchAllBots:" << reply->errorString();
            callback(false, out);
            return;
        }
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject ocs = doc.object().value(QStringLiteral("ocs")).toObject();
        int status = ocs.value(QStringLiteral("meta")).toObject()
                        .value(QStringLiteral("statuscode")).toInt(200);
        if (status != 100 && status != 200) { callback(false, out); return; }
        callback(true, parseBotList(ocs.value(QStringLiteral("data")).toArray()));
    });
}

void ApiClient::setBotEnabled(const QString &token, int botId, bool enabled,
                              QObject *context,
                              std::function<void(bool, int)> callback)
{
    QUrl url(m_serverUrl + QStringLiteral("/ocs/v2.php/apps/spreed/api/v1/bot/")
             + token + QStringLiteral("/") + QString::number(botId));
    QNetworkRequest req(url);
    req.setRawHeader("OCS-APIRequest", "true");
    req.setRawHeader("Accept", "application/json");
    applyBasicAuth(req);

    QNetworkReply *reply = enabled
        ? m_nam.sendCustomRequest(req, "POST", QByteArray())
        : m_nam.sendCustomRequest(req, "DELETE");
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        callback(reply->error() == QNetworkReply::NoError, httpStatus);
    });
}

void ApiClient::searchInConversation(const QString &token, const QString &query,
                                     QObject *context,
                                     std::function<void(bool, const QVector<SearchHit> &)> callback)
{
    QUrl url(m_serverUrl + QStringLiteral("/ocs/v2.php/search/providers/talk-message-current/search"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("term"), query);
    q.addQueryItem(QStringLiteral("from"), QStringLiteral("/call/") + token);
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("30"));
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setRawHeader("OCS-APIRequest", "true");
    req.setRawHeader("Accept", "application/json");
    applyBasicAuth(req);

    QNetworkReply *reply = m_nam.get(req);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QVector<SearchHit> hits;
        if (reply->error() != QNetworkReply::NoError) {
            int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            qWarning() << "searchInConversation: HTTP" << status << reply->errorString();
            callback(false, hits);
            return;
        }
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonArray entries = doc.object().value("ocs").toObject()
                                .value("data").toObject()
                                .value("entries").toArray();
        for (const QJsonValue &v : entries) {
            QJsonObject e = v.toObject();
            QJsonObject attrs = e.value("attributes").toObject();
            SearchHit h;
            h.messageId = attrs.value("messageId").toString().toInt();
            h.timestamp = attrs.value("timestamp").toString().toLongLong();
            h.actorName = e.value("title").toString();
            h.snippet   = e.value("subline").toString();
            if (h.messageId > 0) hits.append(h);
        }
        callback(true, hits);
    });
}

void ApiClient::searchAllConversations(const QString &query, QObject *context,
                                       std::function<void(bool, const QVector<SearchHit> &)> callback)
{
    // `talk-message` rather than `talk-message-current`, and NO `from` scope --
    // that pair is what makes this search the whole account instead of one room.
    QUrl url(m_serverUrl + QStringLiteral("/ocs/v2.php/search/providers/talk-message/search"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("term"), query);
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("30"));
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setRawHeader("OCS-APIRequest", "true");
    req.setRawHeader("Accept", "application/json");
    applyBasicAuth(req);

    QNetworkReply *reply = m_nam.get(req);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QVector<SearchHit> hits;
        if (reply->error() != QNetworkReply::NoError) {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            qWarning() << "searchAllConversations: HTTP" << status << reply->errorString();
            callback(false, hits);
            return;
        }
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonArray entries = doc.object().value("ocs").toObject()
                                      .value("data").toObject()
                                      .value("entries").toArray();
        for (const QJsonValue &v : entries) {
            const QJsonObject e = v.toObject();
            const QJsonObject attrs = e.value("attributes").toObject();
            SearchHit h;
            // MessageSearch.php:313 adds the room token as `conversation`.
            h.conversationToken = attrs.value("conversation").toString();
            h.messageId = attrs.value("messageId").toString().toInt();
            h.timestamp = attrs.value("timestamp").toString().toLongLong();
            h.actorName = e.value("title").toString();
            h.snippet   = e.value("subline").toString();
            if (h.messageId > 0 && !h.conversationToken.isEmpty())
                hits.append(h);
        }
        callback(true, hits);
    });
}

void ApiClient::listNextcloudFolder(const QString &path, QObject *context,
                                    std::function<void(bool, const QVector<NcFileEntry> &,
                                                       int, const QString &)> callback)
{
    if (m_user.isEmpty() || m_serverUrl.isEmpty()) {
        callback(false, {}, 0, tr("Not signed in to Nextcloud"));
        return;
    }

    QString p = path;
    while (p.startsWith(QLatin1Char('/'))) p.remove(0, 1);
    while (p.endsWith(QLatin1Char('/'))) p.chop(1);

    const QString userPrefix = QStringLiteral("/remote.php/dav/files/") + m_user;
    const QString encPath = p.isEmpty() ? QString() : QString::fromLatin1(QUrl::toPercentEncoding(p, "/"));
    QUrl url(m_serverUrl + userPrefix + (encPath.isEmpty() ? "/" : "/" + encPath));

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/xml; charset=utf-8");
    req.setRawHeader("Depth", "1");
    applyBasicAuth(req);

    static const QByteArray body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<d:propfind xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\">\n"
        "  <d:prop>\n"
        "    <d:resourcetype/>\n"
        "    <d:getcontentlength/>\n"
        "    <d:getcontenttype/>\n"
        "    <d:getlastmodified/>\n"
        "    <d:displayname/>\n"
        "    <oc:fileid/>\n"
        "  </d:prop>\n"
        "</d:propfind>\n";

    QBuffer *buf = new QBuffer();
    buf->setData(body);
    buf->open(QIODevice::ReadOnly);

    QNetworkReply *reply = m_nam.sendCustomRequest(req, "PROPFIND", buf);
    buf->setParent(reply);
    trackReply(reply);
    bindReplyLifetime(reply, context);

    const QString requestPathNormalized = p.isEmpty() ? QStringLiteral("/") : "/" + p;
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback, userPrefix, requestPathNormalized]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "listNextcloudFolder:" << reply->errorString() << "status:" << status;
            callback(false, {}, status, reply->errorString());
            return;
        }
        // PROPFIND success is HTTP 207 Multi-Status. A plain 200 is almost
        // certainly an auth redirect or captive-portal HTML page — the XML
        // parser would quietly produce zero entries, which the UI then shows
        // as "Empty folder." Reject up front.
        if (status != 207) {
            callback(false, {}, status,
                tr("Nextcloud returned HTTP %1 instead of 207 Multi-Status").arg(status));
            return;
        }
        QVector<NcFileEntry> entries;
        int responseCount = 0;
        QXmlStreamReader r(reply->readAll());
        NcFileEntry cur;
        bool inResponse = false;
        bool inResourceType = false;
        while (!r.atEnd()) {
            r.readNext();
            if (r.isStartElement()) {
                const auto name = r.name();
                if (name == QLatin1String("response")) {
                    cur = NcFileEntry();
                    inResponse = true;
                    ++responseCount;
                } else if (inResponse) {
                    if (name == QLatin1String("href")) {
                        QString decoded = QUrl::fromPercentEncoding(r.readElementText().toUtf8());
                        if (decoded.startsWith(userPrefix))
                            decoded = decoded.mid(userPrefix.size());
                        if (decoded.endsWith(QLatin1Char('/')) && decoded.size() > 1)
                            decoded.chop(1);
                        cur.path = decoded.isEmpty() ? QStringLiteral("/") : decoded;
                        cur.name = cur.path.section(QLatin1Char('/'), -1);
                    } else if (name == QLatin1String("displayname")) {
                        QString d = r.readElementText();
                        if (!d.isEmpty()) cur.name = d;
                    } else if (name == QLatin1String("resourcetype")) {
                        inResourceType = true;
                    } else if (inResourceType && name == QLatin1String("collection")) {
                        cur.isDir = true;
                    } else if (name == QLatin1String("getcontentlength")) {
                        cur.size = r.readElementText().toLongLong();
                    } else if (name == QLatin1String("getcontenttype")) {
                        cur.mimeType = r.readElementText();
                    } else if (name == QLatin1String("getlastmodified")) {
                        cur.lastModified = QDateTime::fromString(
                            r.readElementText(), Qt::RFC2822Date);
                    } else if (name == QLatin1String("fileid")) {
                        cur.fileId = r.readElementText().toLongLong();
                    }
                }
            } else if (r.isEndElement()) {
                const auto name = r.name();
                if (name == QLatin1String("resourcetype")) {
                    inResourceType = false;
                } else if (name == QLatin1String("response")) {
                    inResponse = false;
                    // PROPFIND Depth:1 echoes the requested folder itself as the first
                    // <response> — skip it so the caller only sees children.
                    if (cur.path != requestPathNormalized
                        && !(requestPathNormalized == QStringLiteral("/") && cur.path.isEmpty())) {
                        entries.push_back(cur);
                    }
                }
            }
        }
        if (r.hasError()) {
            qWarning() << "listNextcloudFolder: XML parse error:" << r.errorString();
            callback(false, {}, status,
                tr("Nextcloud returned an unexpected response (%1)").arg(r.errorString()));
            return;
        }
        if (responseCount == 0) {
            // 207 with no <response> is not "empty folder" — an empty folder
            // still returns the parent entry. Surface this as a real failure.
            callback(false, {}, status, tr("Nextcloud returned no folder entries"));
            return;
        }
        callback(true, entries, status, QString());
    });
}

void ApiClient::shareNextcloudFileToChat(const QString &token, const QString &path,
                                         QObject *context,
                                         std::function<void(bool, int, const QString &)> callback)
{
    // Files Sharing API, shareType=10 (Talk room). Spreed's /chat/{token}/share
    // endpoint only accepts generic shareable objects (polls etc.) and 400s on
    // file paths.
    QJsonObject body;
    body["path"]      = path;
    body["shareType"] = 10;
    body["shareWith"] = token;

    auto req = makeRequest(QStringLiteral("apps/files_sharing/api/v1/shares"));
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson());
    trackReply(reply);
    bindReplyLifetime(reply, context);

    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError && status == 0) {
            qWarning() << "shareNextcloudFileToChat: network error:" << reply->errorString();
            callback(false, 0, reply->errorString());
            return;
        }
        QJsonObject ocs = QJsonDocument::fromJson(reply->readAll()).object()
                              .value(QStringLiteral("ocs")).toObject();
        QJsonObject meta = ocs.value(QStringLiteral("meta")).toObject();
        const int ocsStatus = meta.value(QStringLiteral("statuscode")).toInt();
        const QString message = meta.value(QStringLiteral("message")).toString();
        if (ocsStatus >= 200 && ocsStatus < 300) {
            callback(true, ocsStatus, QString());
        } else {
            qWarning() << "shareNextcloudFileToChat: OCS" << ocsStatus << message;
            callback(false, ocsStatus, message);
        }
    });
}

void ApiClient::setMessageReminder(const QString &token, int messageId,
                                   const QDateTime &when,
                                   QObject *context,
                                   std::function<void(bool, const QString &)> callback)
{
    QJsonObject body;
    body["timestamp"] = static_cast<qint64>(when.toSecsSinceEpoch());

    auto req = makeRequest(QStringLiteral("apps/spreed/api/v1/chat/")
                           + token + "/" + QString::number(messageId) + "/reminder");
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson());
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QJsonObject ocs = QJsonDocument::fromJson(reply->readAll()).object()
                              .value(QStringLiteral("ocs")).toObject();
        QJsonObject meta = ocs.value(QStringLiteral("meta")).toObject();
        const int ocsStatus = meta.value(QStringLiteral("statuscode")).toInt();
        if (ocsStatus >= 200 && ocsStatus < 300) { callback(true, QString()); return; }
        callback(false, meta.value(QStringLiteral("message")).toString());
    });
}

void ApiClient::cancelMessageReminder(const QString &token, int messageId,
                                      QObject *context,
                                      std::function<void(bool, const QString &)> callback)
{
    auto req = makeRequest(QStringLiteral("apps/spreed/api/v1/chat/")
                           + token + "/" + QString::number(messageId) + "/reminder");
    QNetworkReply *reply = m_nam.sendCustomRequest(req, "DELETE");
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QJsonObject ocs = QJsonDocument::fromJson(reply->readAll()).object()
                              .value(QStringLiteral("ocs")).toObject();
        QJsonObject meta = ocs.value(QStringLiteral("meta")).toObject();
        const int ocsStatus = meta.value(QStringLiteral("statuscode")).toInt();
        if (ocsStatus >= 200 && ocsStatus < 300) { callback(true, QString()); return; }
        callback(false, meta.value(QStringLiteral("message")).toString());
    });
}

void ApiClient::fetchUpcomingReminders(QObject *context,
                                       std::function<void(bool, const QVector<Reminder> &)> callback)
{
    auto req = makeRequest(QStringLiteral("apps/spreed/api/v1/chat/upcoming-reminders"));
    QNetworkReply *reply = m_nam.get(req);
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QVector<Reminder> out;
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "fetchUpcomingReminders:" << reply->errorString();
            callback(false, out);
            return;
        }
        QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).object()
                             .value(QStringLiteral("ocs")).toObject()
                             .value(QStringLiteral("data")).toArray();
        for (const QJsonValue &v : arr) {
            QJsonObject o = v.toObject();
            Reminder r;
            r.source      = Reminder::NextcloudTalk;
            r.when        = QDateTime::fromSecsSinceEpoch(
                o.value(QStringLiteral("reminderTimestamp")).toVariant().toLongLong());
            r.token       = o.value(QStringLiteral("token")).toString();
            r.messageId   = o.value(QStringLiteral("id")).toInt();
            r.actorName   = o.value(QStringLiteral("actorDisplayName")).toString();
            r.messageText = o.value(QStringLiteral("message")).toString();
            out.push_back(r);
        }
        callback(true, out);
    });
}

void ApiClient::searchNcUsers(const QString &query, QObject *context,
                              std::function<void(bool, const QVector<NcUser> &)> callback)
{
    // Autocomplete for new-conversation invite. Nextcloud's core endpoint
    // expects itemId=new when itemType=call has no existing room — matches
    // what the NC Talk web frontend sends. shareTypes 0/1/7 = user/group/circle.
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("search"),       query);
    q.addQueryItem(QStringLiteral("itemType"),     QStringLiteral("call"));
    q.addQueryItem(QStringLiteral("itemId"),       QStringLiteral("new"));
    q.addQueryItem(QStringLiteral("shareTypes[]"), QStringLiteral("0"));
    q.addQueryItem(QStringLiteral("shareTypes[]"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("shareTypes[]"), QStringLiteral("7"));
    q.addQueryItem(QStringLiteral("limit"),        QStringLiteral("25"));
    auto req = makeRequest(QStringLiteral("core/autocomplete/get"), q);
    QNetworkReply *reply = m_nam.get(req);
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QVector<NcUser> out;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "searchNcUsers: HTTP" << status << reply->errorString()
                       << "body:" << reply->readAll().left(500);
            callback(false, out);
            return;
        }
        QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).object()
                             .value(QStringLiteral("ocs")).toObject()
                             .value(QStringLiteral("data")).toArray();
        for (const QJsonValue &v : arr) {
            QJsonObject o = v.toObject();
            const QString source = o.value(QStringLiteral("source")).toString();
            // Accept users directly, and circles/groups as collective invitees
            // (for direct mode the NewChatDialog will filter to source=users).
            if (source != QStringLiteral("users")
                && source != QStringLiteral("groups")
                && source != QStringLiteral("circles")) continue;
            NcUser u;
            u.id          = o.value(QStringLiteral("id")).toString();
            u.displayName = o.value(QStringLiteral("label")).toString();
            u.source      = source;
            if (!u.id.isEmpty()) out.push_back(u);
        }
        callback(true, out);
    });
}

void ApiClient::createRoom(int roomType, const QString &roomName, const QString &invite,
                           QObject *context,
                           std::function<void(bool, const QString &, const QString &)> callback,
                           const QJsonObject &extraParams)
{
    QJsonObject body;
    body["roomType"] = roomType;
    if (!roomName.isEmpty()) body["roomName"] = roomName;
    if (!invite.isEmpty())   body["invite"]   = invite;
    // Preset parameters (Talk 24). Merged AFTER the three base keys but the
    // base keys win on collision: a preset that carries a roomType must not be
    // able to turn a 1:1 the user explicitly asked for into a group room.
    for (auto it = extraParams.begin(); it != extraParams.end(); ++it) {
        if (!body.contains(it.key()))
            body.insert(it.key(), it.value());
    }

    auto req = makeRequest(QStringLiteral("apps/spreed/api/v4/room"));
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson());
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QJsonObject ocs = QJsonDocument::fromJson(reply->readAll()).object()
                              .value(QStringLiteral("ocs")).toObject();
        QJsonObject meta = ocs.value(QStringLiteral("meta")).toObject();
        const int ocsStatus = meta.value(QStringLiteral("statuscode")).toInt();
        if (ocsStatus >= 200 && ocsStatus < 300) {
            QString token = ocs.value(QStringLiteral("data")).toObject()
                                .value(QStringLiteral("token")).toString();
            callback(true, token, QString());
        } else {
            const int httpStatus = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QString msg = meta.value(QStringLiteral("message")).toString();
            if (msg.isEmpty()) {
                if (reply->error() != QNetworkReply::NoError)
                    msg = reply->errorString();
                else if (httpStatus == 403)
                    msg = QStringLiteral("Server refused (HTTP 403): this "
                          "account is not allowed to create conversations. "
                          "Check Talk admin setting \"Allow users to start "
                          "conversations\" / group restriction.");
                else
                    msg = QStringLiteral("Server refused (HTTP %1, OCS %2).")
                              .arg(httpStatus).arg(ocsStatus);
            }
            qWarning() << "createRoom failed: http" << httpStatus
                       << "ocs" << ocsStatus
                       << "neterr" << reply->error() << msg;
            callback(false, QString(), msg);
        }
    });
}

// ── Talk 24: conversation tags ───────────────────────────────────────────
// These deliberately go through the generic verbs (which share handleReply's
// OCS unwrap, transport-retry and error surfacing) rather than hand-rolling a
// reply handler like createRoom above does. Callers MUST gate them on
// AuthManager::supportsConversationTags(); against an older server the route
// simply does not exist and the 404 is indistinguishable from a real failure.
// Version note: tags are v4 — fetchRoomPresets() below is v1.

void ApiClient::fetchConversationTags(ArrayCallback callback)
{
    getArray(QStringLiteral("apps/spreed/api/v4/tags"), std::move(callback));
}

void ApiClient::createConversationTag(const QString &name, Callback callback)
{
    QJsonObject body;
    body[QStringLiteral("name")] = name;
    post(QStringLiteral("apps/spreed/api/v4/tags"), body, std::move(callback));
}

void ApiClient::renameConversationTag(const QString &tagId, const QString &name,
                                      Callback callback)
{
    QJsonObject body;
    body[QStringLiteral("name")] = name;
    put(QStringLiteral("apps/spreed/api/v4/tags/%1").arg(tagId), body, std::move(callback));
}

void ApiClient::deleteConversationTag(const QString &tagId, Callback callback)
{
    del(QStringLiteral("apps/spreed/api/v4/tags/%1").arg(tagId), std::move(callback));
}

void ApiClient::assignConversationTags(const QString &token, const QStringList &tagIds,
                                       Callback callback)
{
    // The endpoint SETS the whole list; an empty array unassigns every tag.
    // That is why the array is always sent, even when empty — omitting the key
    // is not the same request.
    QJsonArray ids;
    for (const QString &id : tagIds)
        ids.append(id);
    QJsonObject body;
    body[QStringLiteral("tagIds")] = ids;
    post(QStringLiteral("apps/spreed/api/v4/room/%1/tags").arg(token), body,
         std::move(callback));
}

// ── Talk 24: conversation presets ────────────────────────────────────────
void ApiClient::fetchRoomPresets(ArrayCallback callback)
{
    // v1, not v4 (Talk 24 lib/Controller/PresetController.php:52). Using v4
    // here yields a 404 that looks exactly like "your server is too old".
    getArray(QStringLiteral("apps/spreed/api/v1/presets/room"), std::move(callback));
}

void ApiClient::fetchParticipants(const QString &token, QObject *context,
                                  std::function<void(bool, const QJsonArray &, const QString &)> callback)
{
    auto req = makeRequest(QStringLiteral("apps/spreed/api/v4/room/") + token + "/participants");
    QNetworkReply *reply = m_nam.get(req);
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this, [reply, callback]() {
        reply->deleteLater();
        const QJsonObject ocs = QJsonDocument::fromJson(reply->readAll()).object()
                                    .value(QStringLiteral("ocs")).toObject();
        const int s = ocs.value(QStringLiteral("meta")).toObject()
                         .value(QStringLiteral("statuscode")).toInt();
        if (s >= 200 && s < 300)
            callback(true, ocs.value(QStringLiteral("data")).toArray(), QString());
        else
            callback(false, {}, ocs.value(QStringLiteral("meta")).toObject()
                                    .value(QStringLiteral("message")).toString());
    });
}

void ApiClient::ringAttendee(const QString &token, int attendeeId, QObject *context,
                             std::function<void(bool, const QString &)> callback)
{
    auto req = makeRequest(QStringLiteral("apps/spreed/api/v4/call/") + token
                           + QStringLiteral("/ring/") + QString::number(attendeeId));
    QNetworkReply *reply = m_nam.post(req, QByteArray());
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this, [reply, callback]() {
        reply->deleteLater();
        const QJsonObject ocs = QJsonDocument::fromJson(reply->readAll()).object()
                                    .value(QStringLiteral("ocs")).toObject();
        const int s = ocs.value(QStringLiteral("meta")).toObject()
                         .value(QStringLiteral("statuscode")).toInt();
        if (s >= 200 && s < 300) callback(true, QString());
        else callback(false, ocs.value(QStringLiteral("meta")).toObject()
                                 .value(QStringLiteral("message")).toString());
    });
}

void ApiClient::addRoomParticipant(const QString &token, const QString &userId,
                                   QObject *context,
                                   std::function<void(bool, const QString &)> callback,
                                   const QString &source)
{
    QJsonObject body;
    body["newParticipant"] = userId;
    // Whatever the picker said this id was. See the header for why this was a
    // literal until 0.65.3 and what it broke.
    body["source"]         = source.isEmpty() ? QStringLiteral("users") : source;

    auto req = makeRequest(QStringLiteral("apps/spreed/api/v4/room/") + token + "/participants");
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson());
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QJsonObject ocs = QJsonDocument::fromJson(reply->readAll()).object()
                              .value(QStringLiteral("ocs")).toObject();
        QJsonObject meta = ocs.value(QStringLiteral("meta")).toObject();
        const int ocsStatus = meta.value(QStringLiteral("statuscode")).toInt();
        if (ocsStatus >= 200 && ocsStatus < 300) { callback(true, QString()); return; }
        callback(false, meta.value(QStringLiteral("message")).toString());
    });
}

void ApiClient::setRoomName(const QString &token, const QString &name,
                            QObject *context,
                            std::function<void(bool, const QString &)> callback)
{
    QJsonObject body;
    body["roomName"] = name;
    auto req = makeRequest(QStringLiteral("apps/spreed/api/v4/room/") + token);
    QNetworkReply *reply = m_nam.sendCustomRequest(req, "PUT", QJsonDocument(body).toJson());
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QJsonObject meta = QJsonDocument::fromJson(reply->readAll()).object()
                               .value(QStringLiteral("ocs")).toObject()
                               .value(QStringLiteral("meta")).toObject();
        const int s = meta.value(QStringLiteral("statuscode")).toInt();
        if (s >= 200 && s < 300) { callback(true, QString()); return; }
        callback(false, meta.value(QStringLiteral("message")).toString());
    });
}

void ApiClient::setRoomAvatar(const QString &token, const QString &imagePath,
                              QObject *context,
                              std::function<void(bool, const QString &)> callback)
{
    // Nextcloud's avatar service REQUIRES a SQUARE image and rejects anything
    // else with HTTP 400 ("image is not square"). Users pick arbitrary photos,
    // so do what every Talk/Nextcloud client does: center-crop to the largest
    // centered square and scale to a sane size, then upload the result.
    QImage img(imagePath);
    if (img.isNull()) {
        if (callback) callback(false, QStringLiteral("Couldn't read the selected image."));
        return;
    }
    const int side = qMin(img.width(), img.height());
    QImage square = img.copy((img.width() - side) / 2, (img.height() - side) / 2, side, side);
    if (square.width() > 512)
        square = square.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const bool png = imagePath.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive);
    QByteArray imgBytes;
    QBuffer imgBuf(&imgBytes);
    imgBuf.open(QIODevice::WriteOnly);
    if (!square.save(&imgBuf, png ? "PNG" : "JPEG", png ? -1 : 90) || imgBytes.isEmpty()) {
        if (callback) callback(false, QStringLiteral("Couldn't process the selected image."));
        return;
    }
    imgBuf.close();
    qInfo().nospace() << "ApiClient: setRoomAvatar uploading center-cropped "
                      << square.width() << "x" << square.height() << " square ("
                      << imgBytes.size() << " bytes, " << (png ? "PNG" : "JPEG")
                      << ") to room " << token;

    // Multipart POST; do NOT use makeRequest() (it forces JSON content-type).
    QUrl url(m_serverUrl + QStringLiteral("/ocs/v2.php/apps/spreed/api/v1/room/")
             + token + QStringLiteral("/avatar"));
    QNetworkRequest req(url);
    req.setRawHeader("OCS-APIRequest", "true");
    req.setRawHeader("Accept", "application/json");
    applyBasicAuth(req);

    auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart imagePart;
    imagePart.setHeader(QNetworkRequest::ContentTypeHeader,
                        png ? QStringLiteral("image/png") : QStringLiteral("image/jpeg"));
    imagePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                        QStringLiteral("form-data; name=\"file\"; filename=\"avatar.%1\"")
                            .arg(png ? QStringLiteral("png") : QStringLiteral("jpg")));
    imagePart.setBody(imgBytes);
    multiPart->append(imagePart);

    QNetworkReply *reply = m_nam.post(req, multiPart);
    multiPart->setParent(reply);         // multipart (+ file) freed with the reply
    trackReply(reply);
    // Redundant with the unconditional deleteLater below (this endpoint already
    // frees the reply regardless of context), but bindReplyLifetime also wires
    // abort-on-context-destroyed, which this endpoint was missing: without it an
    // avatar upload kept running (and paying for bandwidth) after the dialog
    // that started it was closed.
    bindReplyLifetime(reply, context);
    // Free the reply (+ multipart + the OPEN QFile) even if `context` (the dialog)
    // is destroyed mid-upload — bind cleanup to `this`, NOT the caller, so the file
    // descriptor never leaks on a close-during-upload.
    connect(reply, &QNetworkReply::finished, this, [reply]() { reply->deleteLater(); });
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        const QByteArray body = reply->readAll();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject ocs = QJsonDocument::fromJson(body).object()
                                    .value(QStringLiteral("ocs")).toObject();
        const QJsonObject meta = ocs.value(QStringLiteral("meta")).toObject();
        const int s = meta.value(QStringLiteral("statuscode")).toInt();
        // The spreed avatar controller returns the real failure reason in
        // ocs.data.message (NOT ocs.meta.message, which it leaves empty), so read
        // both and surface whichever is populated to the user.
        const QString metaMsg = meta.value(QStringLiteral("message")).toString();
        const QString dataMsg = ocs.value(QStringLiteral("data")).toObject()
                                    .value(QStringLiteral("message")).toString();
        qInfo().nospace() << "ApiClient: setRoomAvatar HTTP " << http << " ocs " << s
                          << " metaMsg=\"" << metaMsg << "\" dataMsg=\"" << dataMsg
                          << "\" body=" << QString::fromUtf8(body.left(400));
        const QString err = !dataMsg.isEmpty() ? dataMsg : metaMsg;
        if (s >= 200 && s < 300) { if (callback) callback(true, QString()); return; }
        if (callback) callback(false, err);
    });
}

void ApiClient::setRoomDescription(const QString &token, const QString &description,
                                   QObject *context,
                                   std::function<void(bool, const QString &)> callback)
{
    QJsonObject body;
    body["description"] = description;
    auto req = makeRequest(QStringLiteral("apps/spreed/api/v4/room/") + token + "/description");
    QNetworkReply *reply = m_nam.sendCustomRequest(req, "PUT", QJsonDocument(body).toJson());
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QJsonObject meta = QJsonDocument::fromJson(reply->readAll()).object()
                               .value(QStringLiteral("ocs")).toObject()
                               .value(QStringLiteral("meta")).toObject();
        const int s = meta.value(QStringLiteral("statuscode")).toInt();
        if (s >= 200 && s < 300) { callback(true, QString()); return; }
        callback(false, meta.value(QStringLiteral("message")).toString());
    });
}

void ApiClient::deleteRoom(const QString &token, QObject *context,
                           std::function<void(bool, const QString &)> callback)
{
    auto req = makeRequest(QStringLiteral("apps/spreed/api/v4/room/") + token);
    QNetworkReply *reply = m_nam.sendCustomRequest(req, "DELETE");
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QJsonObject meta = QJsonDocument::fromJson(reply->readAll()).object()
                               .value(QStringLiteral("ocs")).toObject()
                               .value(QStringLiteral("meta")).toObject();
        const int s = meta.value(QStringLiteral("statuscode")).toInt();
        if (s >= 200 && s < 300) { callback(true, QString()); return; }
        callback(false, meta.value(QStringLiteral("message")).toString());
    });
}

void ApiClient::clearChatHistory(const QString &token, QObject *context,
                                 std::function<void(bool, const QString &)> callback)
{
    // DELETE /chat/{token} clears the whole conversation for everyone. The
    // server emits a "history_cleared" system message which every client (this
    // device + the user's other PCs + the peer) purges its local cache on.
    auto req = makeRequest(QStringLiteral("apps/spreed/api/v1/chat/") + token);
    QNetworkReply *reply = m_nam.sendCustomRequest(req, "DELETE");
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QJsonObject meta = QJsonDocument::fromJson(reply->readAll()).object()
                               .value(QStringLiteral("ocs")).toObject()
                               .value(QStringLiteral("meta")).toObject();
        const int s = meta.value(QStringLiteral("statuscode")).toInt();
        if (s >= 200 && s < 300) { callback(true, QString()); return; }
        callback(false, meta.value(QStringLiteral("message")).toString());
    });
}

void ApiClient::leaveRoom(const QString &token, QObject *context,
                          std::function<void(bool, const QString &)> callback)
{
    auto req = makeRequest(QStringLiteral("apps/spreed/api/v4/room/") + token + "/participants/self");
    QNetworkReply *reply = m_nam.sendCustomRequest(req, "DELETE");
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QJsonObject meta = QJsonDocument::fromJson(reply->readAll()).object()
                               .value(QStringLiteral("ocs")).toObject()
                               .value(QStringLiteral("meta")).toObject();
        const int s = meta.value(QStringLiteral("statuscode")).toInt();
        if (s >= 200 && s < 300) { callback(true, QString()); return; }
        callback(false, meta.value(QStringLiteral("message")).toString());
    });
}

void ApiClient::fetchRoomParticipants(const QString &token, QObject *context,
                                      std::function<void(bool, const QVector<RoomParticipant> &)> callback)
{
    auto req = makeRequest(QStringLiteral("apps/spreed/api/v4/room/") + token + "/participants");
    QNetworkReply *reply = m_nam.get(req);
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QVector<RoomParticipant> out;
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "fetchRoomParticipants:" << reply->errorString();
            callback(false, out);
            return;
        }
        QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).object()
                             .value(QStringLiteral("ocs")).toObject()
                             .value(QStringLiteral("data")).toArray();
        for (const QJsonValue &v : arr) {
            QJsonObject o = v.toObject();
            RoomParticipant p;
            p.userId          = o.value(QStringLiteral("actorId")).toString();
            p.displayName     = o.value(QStringLiteral("displayName")).toString();
            p.participantType = o.value(QStringLiteral("participantType")).toInt();
            p.attendeeId      = o.value(QStringLiteral("attendeeId")).toVariant().toLongLong();
            p.status          = o.value(QStringLiteral("status")).toString();
            out.push_back(p);
        }
        callback(true, out);
    });
}

void ApiClient::removeRoomParticipant(const QString &token, qint64 attendeeId,
                                      QObject *context,
                                      std::function<void(bool, const QString &)> callback)
{
    QJsonObject body;
    body["attendeeId"] = attendeeId;
    auto req = makeRequest(QStringLiteral("apps/spreed/api/v4/room/") + token + "/attendees");
    QNetworkReply *reply = m_nam.sendCustomRequest(req, "DELETE", QJsonDocument(body).toJson());
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QJsonObject meta = QJsonDocument::fromJson(reply->readAll()).object()
                               .value(QStringLiteral("ocs")).toObject()
                               .value(QStringLiteral("meta")).toObject();
        const int s = meta.value(QStringLiteral("statuscode")).toInt();
        if (s >= 200 && s < 300) { callback(true, QString()); return; }
        callback(false, meta.value(QStringLiteral("message")).toString());
    });
}

void ApiClient::promoteModerator(const QString &token, qint64 attendeeId,
                                 QObject *context,
                                 std::function<void(bool, const QString &)> callback)
{
    QJsonObject body;
    body["attendeeId"] = attendeeId;
    auto req = makeRequest(QStringLiteral("apps/spreed/api/v4/room/") + token + "/moderators");
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson());
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QJsonObject meta = QJsonDocument::fromJson(reply->readAll()).object()
                               .value(QStringLiteral("ocs")).toObject()
                               .value(QStringLiteral("meta")).toObject();
        const int s = meta.value(QStringLiteral("statuscode")).toInt();
        if (s >= 200 && s < 300) { callback(true, QString()); return; }
        callback(false, meta.value(QStringLiteral("message")).toString());
    });
}

void ApiClient::demoteModerator(const QString &token, qint64 attendeeId,
                                QObject *context,
                                std::function<void(bool, const QString &)> callback)
{
    QJsonObject body;
    body["attendeeId"] = attendeeId;
    auto req = makeRequest(QStringLiteral("apps/spreed/api/v4/room/") + token + "/moderators");
    QNetworkReply *reply = m_nam.sendCustomRequest(req, "DELETE", QJsonDocument(body).toJson());
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QJsonObject meta = QJsonDocument::fromJson(reply->readAll()).object()
                               .value(QStringLiteral("ocs")).toObject()
                               .value(QStringLiteral("meta")).toObject();
        const int s = meta.value(QStringLiteral("statuscode")).toInt();
        if (s >= 200 && s < 300) { callback(true, QString()); return; }
        callback(false, meta.value(QStringLiteral("message")).toString());
    });
}

void ApiClient::sendChatMessage(const QString &token, const QString &text,
                                QObject *context,
                                std::function<void(bool, int, const QString &)> callback,
                                const QString &threadTitle,
                                const QString &referenceId)
{
    QJsonObject body;
    body["message"] = text;
    // #80 -- a locale-independent marker so a TalQ recipient can recognise a
    // machine-sent message (e.g. the "on another call" busy reply) regardless
    // of the sender's UI language. Talk echoes referenceId back on the message.
    if (!referenceId.isEmpty())
        body["referenceId"] = referenceId;
    // Talk's send-message takes `threadTitle` as a top-level form/JSON field
    // on the original POST. When non-empty (and replyTo == 0), the server's
    // ChatController creates a brand-new thread rooted at this message in
    // the same call — there is NO separate "create thread" endpoint. The
    // single-call shape mirrors upstream Talk's web client.
    if (!threadTitle.isEmpty())
        body["threadTitle"] = threadTitle;
    auto req = makeRequest(QStringLiteral("apps/spreed/api/v1/chat/") + token);
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson());
    trackReply(reply);
    bindReplyLifetime(reply, context);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, callback]() {
        reply->deleteLater();
        QJsonObject ocs = QJsonDocument::fromJson(reply->readAll()).object()
                              .value(QStringLiteral("ocs")).toObject();
        QJsonObject meta = ocs.value(QStringLiteral("meta")).toObject();
        const int s = meta.value(QStringLiteral("statuscode")).toInt();
        if (s >= 200 && s < 300) {
            const int id = ocs.value(QStringLiteral("data")).toObject()
                              .value(QStringLiteral("id")).toInt();
            callback(true, id, QString());
        } else {
            callback(false, 0, meta.value(QStringLiteral("message")).toString());
        }
    });
}

void ApiClient::trackReply(QNetworkReply *reply)
{
    m_pendingReplies.append(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_pendingReplies.removeOne(reply);
    });
}

void ApiClient::setNotificationLevel(const QString &token, int level, Callback callback)
{
    QJsonObject body;
    body["level"] = level;
    QString path = "/apps/spreed/api/v4/room/" + token + "/notify";
    if (callback) {
        put(path, body, callback);
    } else {
        put(path, body, [](bool, const QJsonObject &, int) {});
    }
}

// --- 0.65.3 room + chat endpoints ----------------------------------------
//
// ⚠ Note the API versions differ and are NOT interchangeable: room endpoints
// are v4, chat and thread endpoints are v1. Every path below was read off the
// #[ApiRoute] attribute in Talk 24.0.4's own controllers, because upstream's
// prose docs have been wrong about this more than once.

void ApiClient::setFavorite(const QString &token, bool favorite, Callback callback)
{
    // POST adds, DELETE removes — the room id is the whole request, there is
    // no body either way (RoomController.php:954 / :973).
    const QString path = QStringLiteral("apps/spreed/api/v4/room/") + token
                         + QStringLiteral("/favorite");
    Callback cb = callback ? callback : Callback([](bool, const QJsonObject &, int) {});
    if (favorite)
        post(path, QJsonObject{}, cb);
    else
        del(path, cb);
}

void ApiClient::setNotificationCalls(const QString &token, bool notify, Callback callback)
{
    // `level` here is Talk's ParticipantService notification-calls level:
    // 1 = ring me, 0 = do not (RoomController.php:1026). It is deliberately
    // separate from the chat notificationLevel, so a room can be
    // mentions-only for chat and still ring for calls.
    QJsonObject body;
    body[QStringLiteral("level")] = notify ? 1 : 0;
    const QString path = QStringLiteral("apps/spreed/api/v4/room/") + token
                         + QStringLiteral("/notify-calls");
    post(path, body, callback ? callback : Callback([](bool, const QJsonObject &, int) {}));
}

void ApiClient::fetchNoteToSelf(Callback callback)
{
    // GET, not POST — the endpoint is "give me my note-to-self room", and it
    // CREATES the room on first call (RoomController.php:552). That is why a
    // TalQ-only user never had one: nothing has ever called this, so the room
    // was only ever created by opening the web UI.
    get(QStringLiteral("apps/spreed/api/v4/room/note-to-self"), callback);
}

void ApiClient::unpinMessage(const QString &token, int messageId, Callback callback)
{
    const QString path = QStringLiteral("apps/spreed/api/v1/chat/") + token
                         + QLatin1Char('/') + QString::number(messageId)
                         + QStringLiteral("/pin");
    del(path, callback ? callback : Callback([](bool, const QJsonObject &, int) {}));
}

void ApiClient::fetchMessageContext(const QString &token, int messageId, int limit,
                                    ArrayCallback callback)
{
    // The messages either side of `messageId` in one request. TalQ's
    // search-result jump used to walk history backwards a page at a time with
    // a hard 5-page cap, so a hit older than ~500 messages simply never
    // scrolled into view; the same path serves notification click-through.
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    const QString path = QStringLiteral("apps/spreed/api/v1/chat/") + token
                         + QLatin1Char('/') + QString::number(messageId)
                         + QStringLiteral("/context");
    getArray(path, q, callback);
}

void ApiClient::fetchRecentThreads(const QString &token, int limit, ArrayCallback callback)
{
    // The server's own list of recently-active threads. TalQ derived this by
    // scanning the last 200 chat messages, which silently dropped any topic
    // whose root message had scrolled past that window.
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    const QString path = QStringLiteral("apps/spreed/api/v1/chat/") + token
                         + QStringLiteral("/threads/recent");
    getArray(path, q, callback);
}

void ApiClient::startRecording(const QString &token, int status, Callback callback)
{
    QJsonObject body;
    body[QStringLiteral("status")] = status;
    post(QStringLiteral("apps/spreed/api/v1/recording/") + token, body,
         callback ? callback : Callback([](bool, const QJsonObject &, int) {}));
}

void ApiClient::stopRecording(const QString &token, Callback callback)
{
    del(QStringLiteral("apps/spreed/api/v1/recording/") + token,
        callback ? callback : Callback([](bool, const QJsonObject &, int) {}));
}

void ApiClient::setThreadNotificationLevel(const QString &token, int threadId, int level,
                                           Callback callback)
{
    QJsonObject body;
    body[QStringLiteral("level")] = level;
    const QString path = QStringLiteral("apps/spreed/api/v1/chat/") + token
                         + QStringLiteral("/threads/") + QString::number(threadId)
                         + QStringLiteral("/notify");
    post(path, body, callback ? callback : Callback([](bool, const QJsonObject &, int) {}));
}

void ApiClient::setArchived(const QString &token, bool archived, Callback callback)
{
    const QString path = QStringLiteral("apps/spreed/api/v4/room/") + token
                         + QStringLiteral("/archive");
    Callback cb = callback ? callback : Callback([](bool, const QJsonObject &, int) {});
    if (archived) post(path, QJsonObject{}, cb);
    else          del(path, cb);
}

void ApiClient::setImportant(const QString &token, bool important, Callback callback)
{
    const QString path = QStringLiteral("apps/spreed/api/v4/room/") + token
                         + QStringLiteral("/important");
    Callback cb = callback ? callback : Callback([](bool, const QJsonObject &, int) {});
    if (important) post(path, QJsonObject{}, cb);
    else           del(path, cb);
}

static QString pollPath(const QString &token, int pollId = 0)
{
    QString p = QStringLiteral("apps/spreed/api/v1/poll/") + token;
    if (pollId > 0) p += QLatin1Char('/') + QString::number(pollId);
    return p;
}

void ApiClient::fetchPoll(const QString &token, int pollId, Callback callback)
{
    get(pollPath(token, pollId), callback);
}

void ApiClient::votePoll(const QString &token, int pollId, const QList<int> &optionIds,
                         Callback callback)
{
    QJsonArray ids;
    for (int id : optionIds) ids.append(id);
    QJsonObject body;
    body[QStringLiteral("optionIds")] = ids;
    post(pollPath(token, pollId), body, callback);
}

void ApiClient::closePoll(const QString &token, int pollId, Callback callback)
{
    del(pollPath(token, pollId), callback);
}

void ApiClient::createPoll(const QString &token, const QString &question,
                           const QStringList &options, int resultMode, int maxVotes,
                           int threadId, Callback callback, bool draft)
{
    QJsonArray opts;
    for (const QString &o : options) opts.append(o);
    QJsonObject body;
    body[QStringLiteral("question")]   = question;
    body[QStringLiteral("options")]    = opts;
    body[QStringLiteral("resultMode")] = resultMode;   // 0 public, 1 hidden until closed
    body[QStringLiteral("maxVotes")]   = maxVotes;     // 0 = unlimited
    // Keep a poll created while a topic is open inside that topic, the same way
    // messages and file shares now do.
    if (threadId > 0) body[QStringLiteral("threadId")] = threadId;
    if (draft) body[QStringLiteral("draft")] = true;
    post(pollPath(token), body, callback);
}

void ApiClient::fetchSubscribedThreads(int limit, ArrayCallback callback)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    getArray(QStringLiteral("apps/spreed/api/v1/chat/subscribed-threads"), q, callback);
}

void ApiClient::fetchPollDrafts(const QString &token, ArrayCallback callback)
{
    getArray(QStringLiteral("apps/spreed/api/v1/poll/") + token + QStringLiteral("/drafts"),
             callback);
}

void ApiClient::publishPollDraft(const QString &token, int pollId, Callback callback)
{
    post(QStringLiteral("apps/spreed/api/v1/poll/") + token
             + QStringLiteral("/draft/") + QString::number(pollId),
         QJsonObject{}, callback);
}

void ApiClient::summarizeChat(const QString &token, int fromMessageId, Callback callback)
{
    QJsonObject body;
    body[QStringLiteral("fromMessageId")] = fromMessageId;
    post(QStringLiteral("apps/spreed/api/v1/chat/") + token + QStringLiteral("/summarize"),
         body, callback);
}

void ApiClient::fetchTaskResult(int taskId, Callback callback)
{
    // NOT under apps/spreed - this is a core Nextcloud endpoint.
    get(QStringLiteral("taskprocessing/task/") + QString::number(taskId), callback);
}

static QString breakoutPath(const QString &token, const QString &suffix = QString())
{
    return QStringLiteral("apps/spreed/api/v1/breakout-rooms/") + token + suffix;
}

void ApiClient::configureBreakoutRooms(const QString &token, int mode, int amount,
                                       const QString &attendeeMapJson, Callback callback)
{
    QJsonObject body;
    body[QStringLiteral("mode")]   = mode;
    body[QStringLiteral("amount")] = amount;
    // Sent as a JSON *string*, not an object -- the server signature is
    // `string $attendeeMap = '[]'` and decodes it itself.
    body[QStringLiteral("attendeeMap")] =
        attendeeMapJson.isEmpty() ? QStringLiteral("[]") : attendeeMapJson;
    post(breakoutPath(token), body, callback);
}

void ApiClient::removeBreakoutRooms(const QString &token, Callback callback)
{
    del(breakoutPath(token), callback);
}

void ApiClient::startBreakoutRooms(const QString &token, Callback callback)
{
    post(breakoutPath(token, QStringLiteral("/rooms")), QJsonObject{}, callback);
}

void ApiClient::stopBreakoutRooms(const QString &token, Callback callback)
{
    del(breakoutPath(token, QStringLiteral("/rooms")), callback);
}

void ApiClient::broadcastToBreakoutRooms(const QString &token, const QString &message,
                                         Callback callback)
{
    QJsonObject body;
    body[QStringLiteral("message")] = message;
    post(breakoutPath(token, QStringLiteral("/broadcast")), body, callback);
}

void ApiClient::cancelAll()
{
    for (auto *reply : m_pendingReplies) {
        reply->abort();
    }
    m_pendingReplies.clear();
}
