#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QImage>
#include <QUrl>
#include <QUrlQuery>
#include <QVector>
#include <functional>
#include "MentionCandidate.h"
#include "NcFileEntry.h"
#include "Reminder.h"
#include "SearchHit.h"

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
    void del(const QString &path, const QJsonObject &body, Callback callback);
    void setNotificationLevel(const QString &token, int level, Callback callback = nullptr);

    // Raw GET — caller handles the reply (for reading headers)
    QNetworkReply *getRaw(const QString &path, const QUrlQuery &params = QUrlQuery());

    // Raw POST via OCS path
    QNetworkReply *postRaw(const QString &path, const QByteArray &body = QByteArray());

    // POST with absolute path (no OCS prefix) — for non-OCS endpoints like notify_push
    QNetworkReply *postAbsoluteUrl(const QString &path, const QByteArray &body = QByteArray());

    // PUT with absolute path — for WebDAV file uploads
    QNetworkReply *putAbsoluteUrl(const QString &path, const QByteArray &body);

    // GET with absolute path (no OCS prefix, no OCS headers)
    QNetworkReply *getAbsoluteUrl(const QString &path);

    // Fetch a rendering from Nextcloud's preview endpoint. `maxDim` caps the
    // larger edge in pixels (aspect ratio is preserved). `context` is the
    // QObject that owns the callback — if it dies before the reply arrives,
    // the callback is auto-disconnected (prevents use-after-free).
    void fetchFileImage(int fileId, int maxDim, QObject *context,
                        std::function<void(const QImage &, const QString &error)> callback);

    // Fetch mention candidates for a room (Nextcloud Talk v4 API). Same
    // context-safety contract as fetchFileImage.
    void fetchMentions(const QString &token, const QString &search, QObject *context,
                       std::function<void(const QVector<MentionCandidate> &)> callback);

    // Full-text search within a conversation via Nextcloud unified-search.
    void searchInConversation(const QString &token, const QString &query,
                              QObject *context,
                              std::function<void(bool ok, const QVector<SearchHit> &)> callback);

    // WebDAV PROPFIND on /remote.php/dav/files/<user>/<path>. Lists immediate
    // children. `path` is the user-root-relative path, empty for root.
    // Callback gets the HTTP status and an error string on failure so the UI
    // can distinguish offline / auth-expired / server-error.
    void listNextcloudFolder(const QString &path, QObject *context,
                             std::function<void(bool ok,
                                                const QVector<NcFileEntry> &entries,
                                                int httpStatus,
                                                const QString &error)> callback);

    // Share an existing Nextcloud file/folder into the given Talk room.
    // `context` owns the callback — if it dies before the reply arrives,
    // the callback is auto-disconnected (prevents use-after-free). The
    // callback's `message` is the server-provided human-readable reason on
    // failure (from OCS meta.message or the network error string).
    void shareNextcloudFileToChat(const QString &token, const QString &path,
                                  QObject *context,
                                  std::function<void(bool ok,
                                                     int httpStatus,
                                                     const QString &message)> callback);

    // Schedule a reminder for a chat message. NC Talk server persists this
    // and sends a notification when the time comes.
    void setMessageReminder(const QString &token, int messageId,
                            const QDateTime &when,
                            QObject *context,
                            std::function<void(bool ok, const QString &error)> callback);
    void cancelMessageReminder(const QString &token, int messageId,
                               QObject *context,
                               std::function<void(bool ok, const QString &error)> callback);
    void fetchUpcomingReminders(QObject *context,
                                std::function<void(bool ok,
                                                   const QVector<Reminder> &)> callback);

    // Long-poll (custom timeout)
    QNetworkReply *getLongPoll(const QString &path, const QUrlQuery &params, int timeoutSecs);

    // Cancel all pending requests
    void cancelAll();

    // Debug: number of pending network replies
    int pendingCount() const { return m_pendingReplies.size(); }

    // Expose the shared NAM so co-located components can reuse it
    QNetworkAccessManager *networkAccessManager() { return &m_nam; }

signals:
    void serverUrlChanged();
    void authenticatedChanged();

private:
    QNetworkRequest makeRequest(const QString &path, const QUrlQuery &params = QUrlQuery()) const;
    void applyBasicAuth(QNetworkRequest &req) const;
    void handleReply(QNetworkReply *reply, Callback callback);
    void handleArrayReply(QNetworkReply *reply, ArrayCallback callback);
    void trackReply(QNetworkReply *reply);

    QNetworkAccessManager m_nam;
    QString m_serverUrl;
    QString m_user;
    QString m_password;
    QList<QNetworkReply*> m_pendingReplies;
};
