#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QUrlQuery>
#include <functional>

/**
 * HTTP client for Nextcloud OCS API.
 * Handles authentication headers, base URL construction, and OCS response unwrapping.
 */
class ApiClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString serverUrl READ serverUrl WRITE setServerUrl NOTIFY serverUrlChanged)
    Q_PROPERTY(bool authenticated READ isAuthenticated NOTIFY authenticatedChanged)

public:
    explicit ApiClient(QObject *parent = nullptr);

    using Callback = std::function<void(bool success, const QJsonObject &data, int statusCode)>;
    using ArrayCallback = std::function<void(bool success, const QJsonArray &data, int statusCode)>;

    // Configuration
    QString serverUrl() const { return m_serverUrl; }
    void setServerUrl(const QString &url);
    void setCredentials(const QString &user, const QString &password);
    bool isAuthenticated() const { return !m_user.isEmpty() && !m_password.isEmpty(); }
    QString user() const { return m_user; }

    // OCS API calls — return data from ocs.data
    void get(const QString &path, const QUrlQuery &params, Callback callback);
    void get(const QString &path, Callback callback);
    void getArray(const QString &path, const QUrlQuery &params, ArrayCallback callback);
    void getArray(const QString &path, ArrayCallback callback);
    void post(const QString &path, const QJsonObject &body, Callback callback);
    void post(const QString &path, Callback callback);
    void put(const QString &path, const QJsonObject &body, Callback callback);
    void del(const QString &path, Callback callback);
    void del(const QString &path, const QUrlQuery &params, Callback callback);

    // Raw GET — caller handles the reply (for reading headers)
    QNetworkReply *getRaw(const QString &path, const QUrlQuery &params = QUrlQuery());

    // Raw POST via OCS path
    QNetworkReply *postRaw(const QString &path, const QByteArray &body = QByteArray());

    // POST with absolute path (no OCS prefix) — for non-OCS endpoints like notify_push
    QNetworkReply *postAbsoluteUrl(const QString &path, const QByteArray &body = QByteArray());

    // PUT with absolute path — for WebDAV file uploads
    QNetworkReply *putAbsoluteUrl(const QString &path, const QByteArray &body);

    // Password accessor (needed for WebDAV auth)
    QString password() const { return m_password; }

    // GET with absolute path (no OCS prefix, no OCS headers)
    QNetworkReply *getAbsoluteUrl(const QString &path);

    // Long-poll (custom timeout)
    QNetworkReply *getLongPoll(const QString &path, const QUrlQuery &params, int timeoutSecs);

    // Cancel all pending requests
    void cancelAll();

signals:
    void serverUrlChanged();
    void authenticatedChanged();

private:
    QNetworkRequest makeRequest(const QString &path, const QUrlQuery &params = QUrlQuery()) const;
    void handleReply(QNetworkReply *reply, Callback callback);
    void handleArrayReply(QNetworkReply *reply, ArrayCallback callback);

    QNetworkAccessManager m_nam;
    QString m_serverUrl;
    QString m_user;
    QString m_password;
    QList<QNetworkReply*> m_pendingReplies;
};
