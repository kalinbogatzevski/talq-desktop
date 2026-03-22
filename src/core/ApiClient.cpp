#include "core/ApiClient.h"
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

    // Basic auth
    if (!m_user.isEmpty()) {
        QString credentials = m_user + ":" + m_password;
        QByteArray encoded = credentials.toUtf8().toBase64();
        req.setRawHeader("Authorization", "Basic " + encoded);
    }

    return req;
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
    del(path, {}, callback);
}

void ApiClient::del(const QString &path, const QUrlQuery &params, Callback callback)
{
    auto req = makeRequest(path, params);
    auto *reply = m_nam.deleteResource(req);
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
    QUrl url(m_serverUrl + path);
    QNetworkRequest req(url);
    if (!m_user.isEmpty()) {
        QString credentials = m_user + ":" + m_password;
        req.setRawHeader("Authorization", "Basic " + credentials.toUtf8().toBase64());
    }
    auto *reply = m_nam.get(req);
    // Not added to m_pendingReplies — caller manages lifetime
    return reply;
}

QNetworkReply *ApiClient::putAbsoluteUrl(const QString &path, const QByteArray &body)
{
    QUrl url(m_serverUrl + path);
    QNetworkRequest req(url);
    if (!m_user.isEmpty()) {
        QString credentials = m_user + ":" + m_password;
        req.setRawHeader("Authorization", "Basic " + credentials.toUtf8().toBase64());
    }
    auto *reply = m_nam.put(req, body);
    return reply;
}

QNetworkReply *ApiClient::postAbsoluteUrl(const QString &path, const QByteArray &body)
{
    QUrl url(m_serverUrl + path);
    QNetworkRequest req(url);
    if (!m_user.isEmpty()) {
        QString credentials = m_user + ":" + m_password;
        req.setRawHeader("Authorization", "Basic " + credentials.toUtf8().toBase64());
    }
    auto *reply = m_nam.post(req, body);
    return reply;
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
    put("/apps/spreed/api/v4/room/" + token + "/notify", body,
        callback ? callback : Callback([](bool, const QJsonObject &, int) {}));
}

void ApiClient::cancelAll()
{
    for (auto *reply : m_pendingReplies) {
        reply->abort();
    }
    m_pendingReplies.clear();
}
