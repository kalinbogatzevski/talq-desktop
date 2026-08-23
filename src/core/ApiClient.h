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
#include <QTimer>
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
    Q_PROPERTY(bool serverReachable READ isServerReachable NOTIFY serverReachabilityChanged)

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

    // ── Server reachability ───────────────────────────────────────────────
    // Whether the Nextcloud server is answering REST requests at all. This is
    // the authoritative "is the server connection working" signal: it is
    // derived from every finished reply — an HTTP response (any status, even
    // 401/500) proves the server is reachable; a transport failure with no
    // HTTP status (DNS fail, connection refused/timeout, no route) is a
    // "miss", and after a couple of consecutive misses we declare the server
    // offline. Optimistic at startup (true until proven otherwise). The two
    // realtime WebSockets (SignalingClient/PushClient) are secondary — they
    // are absent on servers without an HPB and so can't stand in for this.
    bool isServerReachable() const { return m_serverReachable; }
    // Active lightweight health check — an unauthenticated GET of the
    // server's /status.php (Nextcloud's canonical health endpoint). Feeds the
    // same reachability tracker, so it can both confirm an outage fast and
    // detect recovery. Safe to call any time (window-activation, manual). A
    // no-op while the server URL is unset or a probe is already in flight.
    Q_INVOKABLE void probeReachability();

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
    // "Must-complete" DELETE: bounded backoff retry on confirmed
    // non-delivery (statusCode 0 / 5xx) so a lost request on a high-latency
    // /flaky link still lands (leaveCall, status revert). onDone(ok,status)
    // fires once, on success or after giving up. Never blocks the UI.
    void delMustComplete(const QString &path, const QJsonObject &body,
                         QObject *context,
                         std::function<void(bool ok, int statusCode)> onDone,
                         int attempt = 0);
    void setNotificationLevel(const QString &token, int level, Callback callback = nullptr);

    // Raw GET — caller handles the reply (for reading headers)
    QNetworkReply *getRaw(const QString &path, const QUrlQuery &params = QUrlQuery());

    // Raw POST via OCS path
    QNetworkReply *postRaw(const QString &path, const QByteArray &body = QByteArray());

    // POST with absolute path (no OCS prefix) — for non-OCS endpoints like notify_push
    QNetworkReply *postAbsoluteUrl(const QString &path, const QByteArray &body = QByteArray());

    // PUT with absolute path — for WebDAV file uploads
    QNetworkReply *putAbsoluteUrl(const QString &path, const QByteArray &body);

    // Generic WebDAV verb (MKCOL / MOVE / PUT) with an absolute path, used by the
    // chunked large-file upload. `body` may be empty (MKCOL/MOVE); `headers`
    // carries Destination / OC-Total-Length etc.
    QNetworkReply *davRequest(const QByteArray &verb, const QString &path,
                              const QByteArray &body = QByteArray(),
                              const QMap<QByteArray, QByteArray> &headers = {});

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

    // Full-text search across EVERY conversation (`unified-search`, provider
    // `talk-message`). TalQ only ever searched the room you already had open,
    // so a message you remembered but could not place was unfindable. Each hit
    // carries its own conversation token, because that is the point.
    void searchAllConversations(const QString &query, QObject *context,
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
    // `extraParams` is merged into the create body and exists for Talk 24
    // conversation presets: a preset is applied by sending its `parameters`
    // map (listable, messageExpiration, …) alongside `preset: <identifier>`.
    // It is a trailing default argument specifically so the two pre-existing
    // callers (NewChatDialog's 1:1 + group paths, and CallManager's
    // 1:1-to-group promotion) keep compiling untouched.
    void createRoom(int roomType, const QString &roomName, const QString &invite,
                    QObject *context,
                    std::function<void(bool ok,
                                       const QString &token,
                                       const QString &error)> callback,
                    const QJsonObject &extraParams = QJsonObject());

    // ── Talk 24: conversation tags ────────────────────────────────────────
    // Capability: `conversation-tags`. Tags are PER-USER, not per-room: two
    // participants in the same conversation see different tags. Gate every one
    // of these on AuthManager::supportsConversationTags() — an older server
    // 404s the route, which is indistinguishable from a network failure.
    // NOTE the API version: tags are v4, but presets below are v1.
    void fetchConversationTags(ArrayCallback callback);
    void createConversationTag(const QString &name, Callback callback);
    void renameConversationTag(const QString &tagId, const QString &name, Callback callback);
    void deleteConversationTag(const QString &tagId, Callback callback);
    // REPLACES the conversation's whole tag set — [] unassigns all. To add one
    // tag you must send every tag it already had plus the new one; build the
    // list with talq::toggledTagSet() rather than by hand.
    void assignConversationTags(const QString &token, const QStringList &tagIds,
                                Callback callback);

    // ── Talk 24: conversation presets ─────────────────────────────────────
    // Capability: `conversation-presets`. Returns
    // [{identifier, name, description, parameters:{...}}] — the server's
    // create-time templates, including "voiceroom". Version is v1, NOT v4.
    void fetchRoomPresets(ArrayCallback callback);
    // #78 (add-to-call) -- fetch a room's participants; callback receives the raw
    // participant objects (used to resolve a userId -> attendeeId before ringing).
    // Adding a user reuses the existing addRoomParticipant below (group rooms
    // only; NC rejects it for one-to-one rooms).
    void fetchParticipants(const QString &token, QObject *context,
                           std::function<void(bool ok, const QJsonArray &participants,
                                              const QString &error)> callback);
    // #78 -- ring an existing room attendee into the ongoing call (spreed
    // POST /call/{token}/ring/{attendeeId} -> sendCallNotificationForAttendee).
    void ringAttendee(const QString &token, int attendeeId,
                      QObject *context,
                      std::function<void(bool ok, const QString &error)> callback);

    // Add a participant to an existing room.
    //
    // `source` is Talk's share-type discriminator and MUST match where the id
    // came from: "users" for an account, "groups" for a Nextcloud group,
    // "circles" for a Team. Until 0.65.3 this was hard-coded to "users", so
    // picking a group in the invite UI sent the GROUP's id as if it were an
    // account — the server found no such user and refused, which surfaced as
    // "Room created, some invites failed" in the create dialog and as nothing
    // at all in the existing-room panel. The picker had been advertising
    // groups and Teams with their own glyph the whole time.
    //
    // Defaulted so the many existing user-only call sites are unaffected;
    // anything that offers groups must pass NcUser::source through verbatim.
    void addRoomParticipant(const QString &token, const QString &userId,
                            QObject *context,
                            std::function<void(bool ok, const QString &error)> callback,
                            const QString &source = QStringLiteral("users"));

    // --- 0.65.3 ----------------------------------------------------------
    // ⚠ Room endpoints are v4; chat and thread endpoints are v1. Not
    // interchangeable — sending a chat path at v4 404s silently.

    // Pin/unpin a conversation to the top of the list (`favorites`).
    // POST to set, DELETE to clear; no body either way.
    void setFavorite(const QString &token, bool favorite, Callback callback = {});

    // Per-room "ring me when a call starts here" (`notification-calls`).
    // Independent of the chat notification level.
    void setNotificationCalls(const QString &token, bool notify, Callback callback);

    // The user's Note to self room (`note-to-self`). GET, and it CREATES the
    // room on first call — which is why a TalQ-only user has never had one.
    void fetchNoteToSelf(Callback callback);

    // Remove a pin (`pinned-messages`). Moderator-only server-side, same as
    // pinning.
    void unpinMessage(const QString &token, int messageId, Callback callback = {});

    // Messages either side of one id, in a single request (`chat-get-context`).
    // Replaces walking history backwards a page at a time to reach a search hit.
    // Returns a LIST of TalkChatMessage -> ArrayCallback.
    void fetchMessageContext(const QString &token, int messageId, int limit,
                             ArrayCallback callback);

    // The server's list of recently-active threads (`threads`), instead of
    // inferring the topic list from a window of recent chat messages.
    // Returns a LIST of TalkThreadInfo, so ArrayCallback: the OCS envelope is
    // already unwrapped by the time the callback runs, and `ocs.data` here is
    // an array, not an object.
    void fetchRecentThreads(const QString &token, int limit, ArrayCallback callback);

    // Call recording (`recording-v1`). Both routes are v1 and BOTH are
    // #[RequireLoggedInModeratorParticipant] server-side, so callers must gate
    // on moderator status rather than let a 403 surface as a generic failure.
    // `status` on start is Talk's recording mode: 1 = video, 2 = audio-only
    // (Room::RECORDING_VIDEO / RECORDING_AUDIO).
    void startRecording(const QString &token, int status, Callback callback);
    void stopRecording(const QString &token, Callback callback);

    // Per-TOPIC notification level (`threads`), independent of the room's.
    // Levels are Talk's usual 0 default / 1 always / 2 mention-only / 3 never.
    // v1, and keyed by the topic's ROOT message id.
    void setThreadNotificationLevel(const QString &token, int threadId, int level,
                                    Callback callback);

    // Archive a conversation out of the default list without leaving it
    // (`archived-conversations-v2`), and mark one important so it still
    // notifies while archived (`important-conversations`). POST sets, DELETE
    // clears; no body either way. Both v4.
    void setArchived(const QString &token, bool archived, Callback callback = {});
    void setImportant(const QString &token, bool important, Callback callback = {});

    // --- Polls (`talk-polls`), all v1 -----------------------------------
    // fetchPoll returns the poll incl. `options`, `votes`, `votedSelf` and
    // `status` (0 open / 1 closed / 2 draft). votePoll sends the chosen option
    // indices; an empty list retracts. closePoll ends it (author/moderator).
    //
    // WARNING for createPoll: the server REFUSES a poll in a one-to-one room
    // with 400 {"error":"room"} (PollController.php:96) -- only group and
    // public conversations may have polls. Callers must hide the action there
    // rather than surface that error.
    void fetchPoll(const QString &token, int pollId, Callback callback);
    void votePoll(const QString &token, int pollId, const QList<int> &optionIds,
                  Callback callback);
    void closePoll(const QString &token, int pollId, Callback callback);
    // `draft` saves it as a reusable template instead of posting it. Drafts
    // are moderator-only server-side and gated on `talk-polls-drafts`.
    void createPoll(const QString &token, const QString &question,
                    const QStringList &options, int resultMode, int maxVotes,
                    int threadId, Callback callback, bool draft = false);

    // Every topic this user follows, across ALL conversations (`threads`).
    // Each entry carries its own `thread.roomToken`, so this is the only view
    // in TalQ that crosses conversation boundaries by design.
    void fetchSubscribedThreads(int limit, ArrayCallback callback);

    // Poll drafts (`talk-polls-drafts`): list the room's saved drafts, and
    // publish one as a real poll.
    void fetchPollDrafts(const QString &token, ArrayCallback callback);
    void publishPollDraft(const QString &token, int pollId, Callback callback);

    // AI chat summary (`chat-summary-api`). Needs a TaskProcessing text
    // provider installed on the server; without one the capability is absent
    // and this must never be offered.
    void summarizeChat(const QString &token, int fromMessageId, Callback callback);

    // Nextcloud's TaskProcessing API. The chat summary is scheduled, not
    // computed inline: summarizeChat returns 201 {taskId}, and the text
    // arrives here once the task reaches status STATUS_SUCCESSFUL.
    void fetchTaskResult(int taskId, Callback callback);

    // --- Breakout rooms (`breakout-rooms-v1`), all v1 -------------------
    // Moderator-only. `mode`: 1 automatic (server splits people), 2 manual
    // (attendeeMap decides), 3 free (participants pick). `amount` is how many
    // rooms. configure CREATES them; start/stop open and close them; remove
    // deletes them entirely.
    void configureBreakoutRooms(const QString &token, int mode, int amount,
                                const QString &attendeeMapJson, Callback callback);
    void removeBreakoutRooms(const QString &token, Callback callback);
    void startBreakoutRooms(const QString &token, Callback callback);
    void stopBreakoutRooms(const QString &token, Callback callback);
    // A message sent into every breakout room at once.
    void broadcastToBreakoutRooms(const QString &token, const QString &message,
                                  Callback callback);

    // Room management — rename, description, delete, leave.
    void setRoomName(const QString &token, const QString &name,
                     QObject *context,
                     std::function<void(bool ok, const QString &error)> callback);
    void setRoomDescription(const QString &token, const QString &description,
                            QObject *context,
                            std::function<void(bool ok, const QString &error)> callback);
    // #25 — set a conversation's (group's) avatar from an image file. Multipart
    // POST to the Talk room-avatar API. Moderator-only server-side. On success the
    // caller should invalidate any cached "room/<token>" avatar + reload.
    void setRoomAvatar(const QString &token, const QString &imagePath,
                       QObject *context,
                       std::function<void(bool ok, const QString &error)> callback);
    void deleteRoom(const QString &token, QObject *context,
                    std::function<void(bool ok, const QString &error)> callback);
    void leaveRoom(const QString &token, QObject *context,
                   std::function<void(bool ok, const QString &error)> callback);
    // Clear the ENTIRE chat history (moderator-only; capability "clear-history").
    // Server-side, so it clears for everyone and every device; the server emits
    // a "history_cleared" system message that clients purge their cache on.
    void clearChatHistory(const QString &token, QObject *context,
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
    // When `threadTitle` is non-empty AND the server supports the `threads`
    // capability, Talk creates a new named topic rooted at this message in
    // the SAME call (no follow-up endpoint needed). The earlier two-step
    // approach — send plain message then PUT/POST a "/thread" endpoint —
    // matches no URL Talk actually exposes (v23.0.4 verified) and always
    // 998'd, so what looked like a created topic was just a chat line.
    void sendChatMessage(const QString &token, const QString &text,
                         QObject *context,
                         std::function<void(bool ok, int messageId,
                                            const QString &error)> callback,
                         const QString &threadTitle = QString(),
                         const QString &referenceId = QString());

    // Long-poll (custom timeout). The headers map lets callers send hints like
    // X-Chat-Last-Common-Read so the server can break the long-poll early when
    // that value changes (a 304 cannot carry custom headers — RFC restriction).
    QNetworkReply *getLongPoll(const QString &path, const QUrlQuery &params, int timeoutSecs,
                               const QMap<QByteArray, QByteArray> &headers = {});

    // Drop any server-issued session (cookie jar + cached connections and
    // auth/credential cache). MUST be called when the authenticated user
    // changes: Nextcloud validates a live session cookie BEFORE the
    // Authorization: Basic header, so a cookie left over from the previous
    // account makes every request authenticate as that old user even after
    // credentials change. setCredentials() invokes this automatically on a
    // user change; exposed for callers that need an explicit reset.
    void resetSession();

    // Cancel all pending requests
    void cancelAll();

    // Debug: number of pending network replies
    int pendingCount() const { return m_pendingReplies.size(); }

    // Expose the shared NAM so co-located components can reuse it
    QNetworkAccessManager *networkAccessManager() { return &m_nam; }

signals:
    void serverUrlChanged();
    void authenticatedChanged();
    // Emitted only on an actual online↔offline transition (debounced), never
    // per request. `online == false` means REST calls are not reaching the
    // server — the UI should surface an "offline / reconnecting" state.
    void serverReachabilityChanged(bool online);

private:
    QNetworkRequest makeRequest(const QString &path, const QUrlQuery &params = QUrlQuery()) const;
    void applyBasicAuth(QNetworkRequest &req) const;
    // True when the request demonstrably never reached the server (stale
    // pooled HTTP/2 connection → server GOAWAY after idle, etc.), so it is
    // safe to replay once without risking a double-submit.
    bool isRetryableTransportError(QNetworkReply *reply) const;
    // `resend` re-issues the identical request on a fresh connection; on a
    // retryable transport error the reply is replayed once before failure
    // is surfaced to `callback`.
    void handleReply(QNetworkReply *reply, Callback callback,
                     std::function<QNetworkReply*()> resend = {}, int attempt = 0);
    void handleArrayReply(QNetworkReply *reply, ArrayCallback callback,
                          std::function<QNetworkReply*()> resend = {}, int attempt = 0);
    void trackReply(QNetworkReply *reply);

    // Lifetime wiring shared by every context-taking endpoint.
    //
    // The per-endpoint `finished` handler is deliberately bound to the
    // CALLER's context so it cannot touch a destroyed dialog. But that
    // gating also means Qt severs it when the context dies — and the only
    // reply->deleteLater() lived inside it. The reply, parented to the
    // long-lived m_nam and absent from m_pendingReplies, then completed,
    // buffered its whole body, and survived to app exit. Closing a dialog
    // over a slow link was enough.
    static void bindReplyLifetime(QNetworkReply *reply, QObject *context);

    // Update reachability from a finished reply (called once per reply).
    // Ignores deliberately-cancelled requests so logout/teardown can't be
    // mistaken for an outage.
    void noteNetworkOutcome(QNetworkReply *reply);
    void setReachable(bool online);

    QNetworkAccessManager m_nam;
    QString m_serverUrl;
    QString m_user;
    QString m_password;
    QList<QNetworkReply*> m_pendingReplies;

    // Reachability state. Misses are consecutive transport failures (no HTTP
    // status); kOfflineMisses of them flips us offline. While offline a slow
    // timer actively re-probes /status.php so recovery is noticed promptly
    // rather than only on the next 30 s conversation-list poll.
    bool   m_serverReachable = true;
    int    m_reachMisses = 0;
    bool   m_probeInFlight = false;
    QTimer m_reachProbeTimer;
    static constexpr int kOfflineMisses = 2;
};
