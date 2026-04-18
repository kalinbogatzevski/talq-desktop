#include "core/ApiClient.h"
#include <QBuffer>
#include <QImage>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
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

QNetworkReply *ApiClient::getLongPoll(const QString &path, const QUrlQuery &params, int timeoutSecs)
{
    auto req = makeRequest(path, params);
    req.setTransferTimeout((timeoutSecs + 5) * 1000); // extra 5s grace
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

void ApiClient::fetchFileImage(int fileId,
                               std::function<void(const QImage &, const QString &)> callback)
{
    // Nextcloud's preview endpoint returns a full image rendering at requested size.
    // a=1 keeps aspect ratio; x/y are max dimensions.
    QUrl url(m_serverUrl + "/index.php/core/preview");
    QUrlQuery q;
    q.addQueryItem("fileId", QString::number(fileId));
    q.addQueryItem("x", "4096");
    q.addQueryItem("y", "4096");
    q.addQueryItem("a", "1");
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setRawHeader("OCS-APIRequest", "true");
    applyBasicAuth(req);

    auto *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(QImage(), reply->errorString());
            return;
        }
        QImage img = QImage::fromData(reply->readAll());
        callback(img, img.isNull() ? QStringLiteral("decode failed") : QString());
    });
}

void ApiClient::fetchMentions(const QString &token,
                              const QString &search,
                              std::function<void(const QVector<MentionCandidate> &)> callback)
{
    // Build the full URL as a string so Nextcloud subpath installations
    // (e.g. https://host/nextcloud) don't have their prefix stripped by QUrl::setPath.
    QUrl url(m_serverUrl + QStringLiteral("/ocs/v2.php/apps/spreed/api/v4/chat/")
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
    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
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
            c.source = o.value(QStringLiteral("source")).toString();
            if (!c.id.isEmpty()) out.append(c);
        }
        callback(out);
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

void ApiClient::cancelAll()
{
    for (auto *reply : m_pendingReplies) {
        reply->abort();
    }
    m_pendingReplies.clear();
}
