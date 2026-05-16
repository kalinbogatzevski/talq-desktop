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
#include "BotInfo.h"
#include "NcFileEntry.h"
#include "NcUser.h"
#include "Reminder.h"
#include "RoomParticipant.h"
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

    // NC Talk bot framework. fetchEnabledBots returns bots configured for
    // the given conversation (available to room participants). fetchAllBots
    // returns every bot installed on the server (admin-only — non-admins
    // get an empty list with ok=true). setBotEnabled flips the state
    // (POST to enable, DELETE to disable; moderator+ required).
    void fetchEnabledBots(const QString &token, QObject *context,
                          std::function<void(bool ok, const QVector<BotInfo> &)> callback);
    void fetchAllBots(QObject *context,
                      std::function<void(bool ok, const QVector<BotInfo> &)> callback);
    void setBotEnabled(const QString &token, int botId, bool enabled, QObject *context,
                       std::function<void(bool ok, int httpStatus)> callback);

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

    // User autocomplete for starting new chats (`itemType=call` scopes to
    // users the caller is allowed to start a conversation with).
    void searchNcUsers(const QString &query, QObject *context,
                       std::function<void(bool ok, const QVector<NcUser> &)> callback);

    // Create a new Talk room. For a one-to-one, pass roomType=1 + invite=<userId>.
    // For a group, pass roomType=2 + roomName; invite participants afterwards
    // via addRoomParticipant. Callback receives the new room's token.
    void createRoom(int roomType, const QString &roomName, const QString &invite,
                    QObject *context,
                    std::function<void(bool ok,
                                       const QString &token,
                                       const QString &error)> callback);

    // Add a user to an existing room.
    void addRoomParticipant(const QString &token, const QString &userId,
                            QObject *context,
                            std::function<void(bool ok, const QString &error)> callback);

    // Room management — rename, description, delete, leave.
    void setRoomName(const QString &token, const QString &name,
                     QObject *context,
                     std::function<void(bool ok, const QString &error)> callback);
    void setRoomDescription(const QString &token, const QString &description,
                            QObject *context,
                            std::function<void(bool ok, const QString &error)> callback);
    void deleteRoom(const QString &token, QObject *context,
                    std::function<void(bool ok, const QString &error)> callback);
    void leaveRoom(const QString &token, QObject *context,
                   std::function<void(bool ok, const QString &error)> callback);

    // Participants.
    void fetchRoomParticipants(const QString &token, QObject *context,
                               std::function<void(bool ok,
                                                  const QVector<RoomParticipant> &)> callback);
    void removeRoomParticipant(const QString &token, qint64 attendeeId,
                               QObject *context,
                               std::function<void(bool ok, const QString &error)> callback);

    // Promote a user-type participant to moderator (or demote back).
    void promoteModerator(const QString &token, qint64 attendeeId,
                          QObject *context,
                          std::function<void(bool ok, const QString &error)> callback);
    void demoteModerator(const QString &token, qint64 attendeeId,
                         QObject *context,
                         std::function<void(bool ok, const QString &error)> callback);

    // Send a chat message and get its new server-side id back — used to
    // seed a topic/thread root when the user hasn't typed anything yet.
    void sendChatMessage(const QString &token, const QString &text,
                         QObject *context,
                         std::function<void(bool ok, int messageId,
                                            const QString &error)> callback);

    // Set the title on a thread rooted at `messageId`. This is what makes a
    // message a named topic instead of just a reply-thread.
    void setChatThreadTitle(const QString &token, int messageId,
                            const QString &title,
                            QObject *context,
                            std::function<void(bool ok, const QString &error)> callback);

    // Long-poll (custom timeout). The headers map lets callers send hints like
    // X-Chat-Last-Common-Read so the server can break the long-poll early when
    // that value changes (a 304 cannot carry custom headers — RFC restriction).
    QNetworkReply *getLongPoll(const QString &path, const QUrlQuery &params, int timeoutSecs,
                               const QMap<QByteArray, QByteArray> &headers = {});

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
