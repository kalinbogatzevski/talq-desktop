#include "core/ApiClient.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QImage>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QXmlStreamReader>
#include <QDebug>

ApiClient::ApiClient(QObject *parent)
    : QObject(parent)
{
}

void ApiClient::setServerUrl(const QString &url)
{
    QString cleaned = url;
    while (cleaned.endsWith('/'))
        cleaned.chop(1);
    if (m_serverUrl != cleaned) {
        m_serverUrl = cleaned;
        emit serverUrlChanged();
    }
}

void ApiClient::setCredentials(const QString &user, const QString &password)
{
    // Zero old credentials before overwriting
    m_password.fill(QChar(0));
    m_user = user;
    m_password = password;
    emit authenticatedChanged();
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
    return req;
}

void ApiClient::applyBasicAuth(QNetworkRequest &req) const
{
    if (m_user.isEmpty()) return;
    QString credentials = m_user + ":" + m_password;
    req.setRawHeader("Authorization", "Basic " + credentials.toUtf8().toBase64());
}

void ApiClient::handleReply(QNetworkReply *reply, Callback callback)
{
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        m_pendingReplies.removeOne(reply);
        reply->deleteLater();

        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "API error:" << reply->errorString() << "status:" << status;
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

void ApiClient::handleArrayReply(QNetworkReply *reply, ArrayCallback callback)
{
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        m_pendingReplies.removeOne(reply);
        reply->deleteLater();

        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (reply->error() != QNetworkReply::NoError) {
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
    auto req = makeRequest(path, params);
    auto *reply = m_nam.get(req);
    m_pendingReplies.append(reply);
    handleReply(reply, callback);
}

void ApiClient::get(const QString &path, Callback callback)
{
    get(path, {}, callback);
}

void ApiClient::getArray(const QString &path, const QUrlQuery &params, ArrayCallback callback)
{
    auto req = makeRequest(path, params);
    auto *reply = m_nam.get(req);
    m_pendingReplies.append(reply);
    handleArrayReply(reply, callback);
}

void ApiClient::getArray(const QString &path, ArrayCallback callback)
{
    getArray(path, {}, callback);
}

void ApiClient::post(const QString &path, const QJsonObject &body, Callback callback)
{
    auto req = makeRequest(path);
    QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);
    auto *reply = m_nam.post(req, data);
    m_pendingReplies.append(reply);
    handleReply(reply, callback);
}

void ApiClient::post(const QString &path, Callback callback)
{
    post(path, {}, callback);
}

void ApiClient::put(const QString &path, const QJsonObject &body, Callback callback)
{
    auto req = makeRequest(path);
    QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);
    auto *reply = m_nam.put(req, data);
    m_pendingReplies.append(reply);
    handleReply(reply, callback);
}

void ApiClient::del(const QString &path, Callback callback)
{
    del(path, QUrlQuery(), callback);
}

void ApiClient::del(const QString &path, const QUrlQuery &params, Callback callback)
{
    auto req = makeRequest(path, params);
    auto *reply = m_nam.deleteResource(req);
    m_pendingReplies.append(reply);
    handleReply(reply, callback);
}

void ApiClient::del(const QString &path, const QJsonObject &body, Callback callback)
{
    auto req = makeRequest(path);
    QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);
    auto *buf = new QBuffer();
    buf->setData(data);
    buf->open(QIODevice::ReadOnly);
    auto *reply = m_nam.sendCustomRequest(req, "DELETE", buf);
    buf->setParent(reply);  // ensure buffer lives until reply completes
    m_pendingReplies.append(reply);
    handleReply(reply, callback);
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

QNetworkReply *ApiClient::postAbsoluteUrl(const QString &path, const QByteArray &body)
{
    QNetworkRequest req{QUrl(m_serverUrl + path)};
    applyBasicAuth(req);
    return m_nam.post(req, body);
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
                           std::function<void(bool, const QString &, const QString &)> callback)
{
    QJsonObject body;
    body["roomType"] = roomType;
    if (!roomName.isEmpty()) body["roomName"] = roomName;
    if (!invite.isEmpty())   body["invite"]   = invite;

    auto req = makeRequest(QStringLiteral("apps/spreed/api/v4/room"));
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson());
    trackReply(reply);
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
            const QString msg = meta.value(QStringLiteral("message")).toString();
            qWarning() << "createRoom: OCS" << ocsStatus << msg;
            callback(false, QString(), msg);
        }
    });
}

void ApiClient::addRoomParticipant(const QString &token, const QString &userId,
                                   QObject *context,
                                   std::function<void(bool, const QString &)> callback)
{
    QJsonObject body;
    body["newParticipant"] = userId;
    body["source"]         = QStringLiteral("users");

    auto req = makeRequest(QStringLiteral("apps/spreed/api/v4/room/") + token + "/participants");
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson());
    trackReply(reply);
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

void ApiClient::setRoomDescription(const QString &token, const QString &description,
                                   QObject *context,
                                   std::function<void(bool, const QString &)> callback)
{
    QJsonObject body;
    body["description"] = description;
    auto req = makeRequest(QStringLiteral("apps/spreed/api/v4/room/") + token + "/description");
    QNetworkReply *reply = m_nam.sendCustomRequest(req, "PUT", QJsonDocument(body).toJson());
    trackReply(reply);
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
                                std::function<void(bool, int, const QString &)> callback)
{
    QJsonObject body;
    body["message"] = text;
    auto req = makeRequest(QStringLiteral("apps/spreed/api/v1/chat/") + token);
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson());
    trackReply(reply);
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

void ApiClient::setChatThreadTitle(const QString &token, int messageId,
                                   const QString &title,
                                   QObject *context,
                                   std::function<void(bool, const QString &)> callback)
{
    // Different NC Talk versions accept different endpoint/param/verb
    // combinations for thread-title setting; attempt them in sequence and
    // report success the moment one works. Implemented as a heap chain that
    // self-deletes on terminal — avoids the shared_ptr self-reference cycle
    // that an earlier lambda-based version leaked on every call.
    struct Attempt {
        QString path;
        QByteArray verb;
        QString paramName;
        QString apiVersion;
    };

    struct Chain {
        ApiClient *self;
        QObject   *context;
        QString    title;
        std::function<void(bool, const QString &)> callback;
        QVector<Attempt> attempts;
        int idx = 0;

        void next() {
            if (idx >= attempts.size()) {
                callback(false,
                    QCoreApplication::translate(
                        "ApiClient",
                        "Server rejected every known thread endpoint shape."));
                delete this;
                return;
            }
            const Attempt &a = attempts[idx];
            QJsonObject body;
            body[a.paramName] = title;
            auto req = self->makeRequest(a.path);
            QNetworkReply *reply = (a.verb == "POST")
                ? self->m_nam.post(req, QJsonDocument(body).toJson())
                : self->m_nam.sendCustomRequest(req, a.verb, QJsonDocument(body).toJson());
            self->trackReply(reply);
            QObject::connect(reply, &QNetworkReply::finished,
                             context ? context : self,
                             [this, reply, ver = a.apiVersion]() {
                reply->deleteLater();
                QJsonObject meta = QJsonDocument::fromJson(reply->readAll()).object()
                    .value(QStringLiteral("ocs")).toObject()
                    .value(QStringLiteral("meta")).toObject();
                const int s = meta.value(QStringLiteral("statuscode")).toInt();
                if (s >= 200 && s < 300) {
                    qDebug() << "setChatThreadTitle: succeeded via" << ver;
                    callback(true, QString());
                    delete this;
                    return;
                }
                qDebug() << "setChatThreadTitle:" << ver << "rejected with" << s
                         << meta.value(QStringLiteral("message")).toString();
                ++idx;
                next();
            });
        }
    };

    auto *chain = new Chain{this, context, title, std::move(callback), {
        { QStringLiteral("apps/spreed/api/v4/chat/") + token + "/" + QString::number(messageId) + "/thread",
          "POST", QStringLiteral("title"),       QStringLiteral("v4") },
        { QStringLiteral("apps/spreed/api/v1/chat/") + token + "/" + QString::number(messageId) + "/thread",
          "POST", QStringLiteral("title"),       QStringLiteral("v1") },
        { QStringLiteral("apps/spreed/api/v4/chat/") + token + "/" + QString::number(messageId) + "/thread",
          "POST", QStringLiteral("threadTitle"), QStringLiteral("v4/threadTitle") },
        { QStringLiteral("apps/spreed/api/v4/chat/") + token + "/" + QString::number(messageId) + "/thread",
          "PUT",  QStringLiteral("title"),       QStringLiteral("v4-PUT") },
    }};
    chain->next();
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

void ApiClient::cancelAll()
{
    for (auto *reply : m_pendingReplies) {
        reply->abort();
    }
    m_pendingReplies.clear();
}
