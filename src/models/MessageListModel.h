#pragma once

#include <QAbstractListModel>
#include <QVector>
#include <QHash>
#include <QSet>
#include <QList>
#include <QJsonArray>
#include <QTimer>
#include <memory>
#include "models/Message.h"
#include "core/ApiClient.h"
#include "core/MessagePoller.h"

class MessageCache;
class ConversationListModel;
struct ChunkUploadState;   // chunked large-file upload state (defined in the .cpp)

/**
 * QAbstractListModel for chat messages in a conversation.
 * Handles initial history load + live polling for new messages.
 */
class MessageListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
    Q_PROPERTY(QString conversationToken READ conversationToken WRITE setConversationToken NOTIFY conversationTokenChanged)
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    Q_PROPERTY(int threadId READ threadId WRITE setThreadId NOTIFY threadIdChanged)
    Q_PROPERTY(bool hideThreadMessages READ hideThreadMessages WRITE setHideThreadMessages NOTIFY hideThreadMessagesChanged)
    Q_PROPERTY(bool hasMoreHistory READ hasMoreHistory NOTIFY hasMoreHistoryChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        ActorNameRole,
        ActorIdRole,
        MessageTextRole,
        TimestampRole,
        IsSystemRole,
        MessageTypeRole,
        IsGroupedRole,      // true if same author & close in time to previous
        ReplyToTextRole,
        ReplyToAuthorRole,
        ReactionsRole,
        ReactionsSelfRole,
        PollIdRole,
        PollQuestionRole,
        TimeStringRole,
        ShowDateSeparatorRole,  // true if this message starts a new day
        DateStringRole,         // "Today", "Yesterday", "18 Mar 2026"
        IsReadRole,             // true if all participants have read this message
        SendStatusRole,         // "sent", "sending", "failed"
        ThreadIdRole,
        FileNameRole,
        FilePathRole,
        FileMimeRole,
        FileSizeRole,
        FileLinkRole,
        FilePreviewRole,
        HasFileRole,
        FileIdRole,
        FileHideDownloadRole,   // sharer asked that this not be offered for download
        LastEditTimestampRole,
        SilentRole,             // sender suppressed notifications for this message
        ReferenceIdRole,        // #80 -- client referenceId (machine marker, e.g. "talq/busy")
        ReplyToIdRole,          // parent message id, so a click on the quote can jump to it
    };

    explicit MessageListModel(ApiClient *api, MessageCache *cache, QObject *parent = nullptr);

    /// Public accessor so ChatPainter can make authenticated requests
    ApiClient *api() const { return m_api; }
    ~MessageListModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool isLoading() const { return m_loading; }
    bool isConnected() const { return m_connected; }
    bool hasMoreHistory() const { return m_hasMoreHistory; }
    QString conversationToken() const { return m_token; }
    void setConversationToken(const QString &token);

    // #26 - in a one_to_one room that has exactly one bot enabled, the
    // user's messages should reach the bot without requiring an
    // @-mention prefix (nobody else to confuse them with). The composer
    // / model sets this slug on entry to such a room; sendMessage
    // prepends "@<slug> " if the user's text doesn't already contain @.
    // Empty = no auto-prepend.
    void setAutoMentionBot(const QString &mentionSlug);
    QString autoMentionBot() const { return m_autoMentionBot; }

    int threadId() const { return m_threadId; }
    void setThreadId(int id);

    bool hideThreadMessages() const { return m_hideThreadMessages; }
    void setHideThreadMessages(bool hide);

    int unreadBoundary() const { return m_unreadBoundary; }
    void setConversationListModel(ConversationListModel *c) { m_conversations = c; }

    Q_INVOKABLE void sendMessage(const QString &text, int replyToId = 0, bool silent = false);
    // Schedule a message for future delivery via POST /chat/{token}/schedule.
    // sendAt is the absolute unix timestamp (seconds) when the server should
    // deliver. silent==true suppresses notifications for the recipient.
    Q_INVOKABLE void scheduleMessage(const QString &text, qint64 sendAt,
                                      int replyToId = 0, bool silent = false);
    Q_INVOKABLE void markAsRead();
    // Mark this message and everything newer as unread. Implemented as a
    // POST /read with lastReadMessage = messageId - 1 (the docs accept any
    // integer; server clamps to existing IDs). The conversation's unread
    // count and the "New messages" divider both update on the next
    // conversation refresh.
    Q_INVOKABLE void markAsUnread(int messageId);
    Q_INVOKABLE void sendFile(const QString &filePath);
    // The user pressed Retry on a message that did not send. It does NOT blindly
    // re-POST: it asks the server whether the message is already there first
    // (see the send outbox below), because "failed" can mean "delivered".
    Q_INVOKABLE void retryMessage(int tempId);
    // The user gave up on an unsent message. Drops it from the room and from the
    // outbox. If its POST was somehow still live and lands, it simply arrives as
    // a normal message through the poller.
    Q_INVOKABLE void discardFailed(int tempId);
    // A hint that sending may work now: the server answered after an outage, the
    // window regained focus, a room was opened. Pulls the next retry forward;
    // it never bypasses the check-then-resend gate and never fires faster than
    // SendRetryPolicy::kMinSpacingMs after an entry's last activity.
    void kickOutbox();
    // Drop every unsent message and its row. For logout / account change: unsent
    // text belongs to the SESSION, exactly like the composer drafts (see the
    // loggedInChanged handler in MainWindow) -- it must not be re-sent as, or
    // shown to, the next account that signs in on this machine.
    void clearOutbox();
    // True while any unsent message is still being retried (a given-up one is not
    // counted: it waits for the user and must not hold up an update forever). The
    // auto-update install gate reads this: restarting would throw them away.
    bool hasPendingSends() const;
    // False while the message's last attempt is still in flight (its answer is on
    // its way, Retry has nothing to do yet) or when it is no longer tracked.
    bool canRetry(int tempId) const;
    // The row of the message that newMessagesAtEnd() just announced. Row 0 is the
    // newest message only when no unsent message is pinned in front of it, so a
    // listener must not assume index(0).
    int latestArrivedRow() const;
    Q_INVOKABLE void addReaction(int messageId, const QString &emoji);
    Q_INVOKABLE void loadHistory();
    Q_INVOKABLE void loadHistoryUntil(int messageId);
    // 0.65.3 — licenses the single-request context fetch used by
    // loadHistoryUntil(). False keeps the historic backwards page walk.
    void setContextCapable(bool capable) { m_contextCapable = capable; }
    Q_INVOKABLE void deleteMessage(int messageId);
    Q_INVOKABLE void editMessage(int messageId, const QString &newText);
    Q_INVOKABLE void pinMessage(int messageId);
    Q_INVOKABLE QString messageLink(int messageId) const;
    Q_INVOKABLE bool pasteClipboardImage();
    Q_INVOKABLE void sendFileWithCaption(const QString &filePath, const QString &caption);
    Q_INVOKABLE void promptFileSend(const QString &filePath);
    Q_INVOKABLE void cleanupTempFile(const QString &filePath);
    // openWhenDone splits the two gestures that reach this. Clicking the file
    // bubble means "show me this", so it opens; the context-menu Download
    // means "put a copy on my disk", so it saves and says where. Same bytes,
    // different intent.
    Q_INVOKABLE void downloadFile(int fileId, const QString &fileName,
                                  bool openWhenDone = true);
    // Upload and share a recording as a VOICE MESSAGE rather than a plain
    // audio attachment. See shareUploadedFile for what the flag buys.
    Q_INVOKABLE void sendVoiceMessage(const QString &filePath);
    // Write the ORIGINAL bytes of an attachment to a caller-chosen path. The
    // image viewer's "Save as..." used to re-encode its on-screen QImage --
    // which is a server-side preview render capped to the screen, so a 4000px
    // photo saved as a screen-sized JPEG and a .gif/.webp/.heic saved through
    // a png/jpg/bmp filter. Same fetch as downloadFile, different destination.
    Q_INVOKABLE void saveFileAs(int fileId, const QString &fileName,
                                const QString &destPath);
    // An attachment's ORIGINAL bytes, in memory -- for a caller that needs the
    // content rather than a file on disk (ImageClipboard). Same path lookup as
    // downloadFile. Failure is reported ONLY through `done`, never through
    // errorOccurred: that raises a modal box, and this caller has a fallback,
    // so the box would announce a failure the user never actually meets. More
    // than `maxBytes` aborts the transfer and fails, and so does a transfer
    // that stalls for 30 s. `done` is not called at all once `context` is
    // gone; deleting `context` also aborts the transfer.
    void fetchFileBytes(int fileId, qint64 maxBytes, QObject *context,
                        std::function<void(const QByteArray &bytes, const QString &error)> done);

    // Upload progress (0.0 to 1.0, -1 = no upload)
    Q_PROPERTY(double uploadProgress READ uploadProgress NOTIFY uploadProgressChanged)
    Q_PROPERTY(QString uploadFileName READ uploadFileName NOTIFY uploadProgressChanged)
    double uploadProgress() const { return m_uploadProgress; }
    QString uploadFileName() const { return m_uploadFileName; }
    Q_INVOKABLE void createTopic(const QString &title);
    Q_INVOKABLE void refresh();  // trigger immediate poll for read status, new messages
    Q_INVOKABLE void sendMessageToToken(const QString &targetToken, const QString &text);
    // 0.65.3 — forward an attachment by re-sharing its path into another
    // conversation. Talk 24 has no forward endpoint, so this is what
    // forwarding a file actually is; before this, a forwarded image arrived
    // as the literal text "[File: name]".
    Q_INVOKABLE void shareExistingFile(const QString &path, const QString &targetToken);
    // The exact body to POST when forwarding `messageId`, or empty when this
    // message is not forwardable as text (not in the model yet, or it carries a
    // rich object that must be re-shared rather than described). Reads the
    // server's own markup out of rawJson -- see ForwardLogic.h for why the
    // rendered text is not an acceptable source.
    // The message's ORIGINAL markup, with {mention-*} restored to the @"id"
    // syntax the server re-parses (which is also exactly what the composer
    // itself inserts). Empty when there is no server copy yet or the message
    // carries a rich object that is not prose. This is what anything needing
    // the source text must use -- forwarding, and editing.
    Q_INVOKABLE QString rawBodyFor(int messageId) const;
    Q_INVOKABLE QString forwardBodyFor(int messageId) const;
    // The user's server-side attachment folder (config.attachments.folder).
    // Empty restores the historic "Talk".
    void setAttachmentFolder(const QString &folder) { m_attachmentFolder = folder; }
    QString attachmentFolder() const;

signals:
    void historyUntilSettled(int messageId, bool found);
    void loadingChanged();
    void conversationTokenChanged();
    void messageSent();
    void errorOccurred(const QString &error);
    void newMessagesAtEnd();
    void connectedChanged();
    void threadIdChanged();
    void hideThreadMessagesChanged();
    void uploadProgressChanged();
    void hasMoreHistoryChanged();
    void pasteReady(const QString &filePath, int width, int height);
    // An attachment finished writing to disk. `opened` says whether the file
    // was also handed to the OS, so a listener can tell the user where it went
    // WITHOUT announcing a file that is already open in front of them.
    void fileDownloaded(const QString &savePath, bool opened);
    // A saveFileAs() destination finished writing. Distinct from
    // fileDownloaded so the "Saved to Downloads" toast never fires for a file
    // the user explicitly placed somewhere else.
    void fileSavedAs(const QString &savePath);
    void unreadBoundaryChanged();
    // Emitted after POST /chat/{token}/schedule succeeds — UI uses this to
    // confirm the schedule (toast / banner) without polling the queue.
    void messageScheduled(qint64 sendAt);

private slots:
    void onMessagesReceived(const QJsonArray &messages);
    void onLastCommonReadChanged(int messageId);

    void updateReactions(int messageId, const QJsonObject &data);
    // bug 8 — a reaction/reaction_revoked system message (from the long-poll or
    // a refresh) carries the target comment + its updated reactions map in
    // `parent`. Apply that delta to the target message instead of dropping the
    // event, so reactions added by OTHER clients appear (and survive cache
    // reload). Never shown as a visible message.
    void applyReactionSystemMessage(const QJsonObject &systemMessageJson);
    // A peer's edit arrives as a system message whose `parent` carries the new
    // body. Apply it in place and re-cache, the same shape as the reaction
    // path -- the long-poll only ever returns messages newer than the last
    // known id, so nothing else will ever correct a stale body.
    void applyEditSystemMessage(const QJsonObject &systemMessageJson);
    // One funnel for reaction / edit / delete / call-join rows, shared by every
    // path that ingests server data. Returns true when the row was an event
    // rather than content and must not be admitted to the list.
    bool absorbControlMessage(const QJsonObject &obj, const Message &m);
    // Remove a single message row by id (local only — no server call). Used to
    // hide deletion tombstones live when a "message_deleted" event arrives.
    void removeMessageById(int id);

    // Purge the whole conversation locally (model + cache). Driven by the
    // server's "history_cleared" system message (DELETE /chat/{token}), so it
    // fires on every device — the actor's and the peer's — not just the caller.
    void clearLocalHistory();

private:
    void startPoller();
    void trimOldMessages();
    void refreshLatest();
    // Download helpers. resolveDavPathById recovers a live DAV path for a
    // fileId when the message's cached `path` is missing or degraded;
    // fetchDavFile streams the bytes to disk; uniqueDownloadPath picks the
    // destination without ever overwriting.
    void resolveDavPathById(int fileId, std::function<void(const QString &)> done);
    void fetchDavFile(const QString &davPath, int fileId, const QString &fileName,
                      bool openWhenDone, const QString &destPath = QString());
    // Shared entry: resolve a path for `fileId` and hand it to fetchDavFile.
    void startFileFetch(int fileId, const QString &fileName, bool openWhenDone,
                        const QString &destPath);
    // The DAV path for `fileId`: the loaded message's own, else recovered over
    // DAV SEARCH. `done` gets an empty string when neither finds it. The one
    // lookup behind both startFileFetch and fetchFileBytes.
    void withDavPath(int fileId, std::function<void(const QString &)> done);
    static QString uniqueDownloadPath(const QString &fileName);
    // File-upload helpers (used by sendFileWithCaption). shareUploadedFile posts
    // the Talk share once the bytes are on the server (shared by the single-PUT
    // and chunked paths); uploadFileChunked streams a large file via Nextcloud
    // chunked-upload v2 so we never readAll() the whole file into memory.
    // Filenames queued by sendVoiceMessage, consumed by shareUploadedFile.
    QSet<QString> m_voiceMessageFiles;
    void shareUploadedFile(const QString &fileName, const QString &token,
                           const QString &caption);
    void uploadFileChunked(const QString &readPath, const QString &fileName,
                           const QString &token, const QString &caption,
                           const QString &tempCopy);
    // Uploads the next chunk (or MOVE-assembles + shares when done). Re-invoked
    // from each chunk's finished() callback — no self-referential std::function,
    // so the state is released cleanly when the last reply completes.
    void uploadNextChunk(std::shared_ptr<ChunkUploadState> st);
    void failChunkedUpload(const std::shared_ptr<ChunkUploadState> &st, const QString &msg);
    // Lightweight pull just to refresh X-Chat-Last-Common-Read.
    // Used by m_readMarkerTimer because this server's HPB doesn't relay
    // read-marker events (only new-message events), so the long-poll never
    // breaks early on a pure-read advance.
    void refreshReadMarker();

    // ---- send outbox -------------------------------------------------------
    // Every composer message the server has not yet confirmed. It lives OUTSIDE
    // m_messages on purpose: m_messages is wiped by a room switch, a topic-tab
    // switch and a history clear, which used to take the user's unsent text with
    // it (field log 2026-10-03 11:19:58). The row in m_messages is only the
    // outbox entry's picture; setConversationToken() redraws it on return.
    //
    // The rules (all in core/SendRetryPolicy.h so they are unit-tested):
    //  * an attempt is in flight until its reply callback fires, however the row
    //    looks -- the 20 s "failed" flip is display only and cannot abort the POST;
    //  * never re-POST on a guess: probe the room's recent history for our
    //    referenceId first, because Talk 24 saves a comment BEFORE its HPB notify
    //    and does not deduplicate on referenceId;
    //  * strictly oldest-first per room, so a retry never overtakes an older
    //    message that is still waiting.
    struct OutboxEntry {
        QString     token;
        QString     owner;               // account (user + server) that wrote it
        int         tempId = 0;          // negative, unique for the session
        QString     text;                // exactly as POSTed (auto-@bot prefix included)
        QString     referenceId;
        int         replyToId = 0;       // goes in the body
        QJsonObject replyTo;             // the parent as drawn in the quote; empty if not loaded
        int         threadId = 0;
        bool        silent = false;
        qint64      sentAtSecs = 0;      // CLIENT clock; shown as the bubble's time only
        qint64      sentAtMs = 0;
        // The newest SERVER id this room had in the model when the message was
        // sent. A message the server saved afterwards has a larger id, which is
        // what lets a history window PROVE it covers the send (X-Chat-Last-Given).
        // 0 when the room had none loaded: coverage then cannot be proven.
        int         knownNewestId = 0;

        int         postAttempts = 0;
        bool        everPosted = false;  // a POST has actually been started (not just queued)
        quint64     attemptSeq = 0;      // names the CURRENT attempt for its callback and timer
        bool        inFlight = false;
        qint64      inFlightSinceMs = 0;
        bool        slow = false;        // 20 s without an answer -- display only
        bool        permanent = false;   // given up; only a manual Retry continues
        bool        manual = false;      // the user pressed Retry on this message
        bool        needFreshConnection = false;   // last failure was a transport one
        qint64      nextTryMs = 0;
        qint64      lastActivityMs = 0;
        int         probeFailures = 0;
        int         lastStatus = 0;
    };

    OutboxEntry *findEntry(int tempId);
    QString statusFor(const OutboxEntry &e) const;
    Message tempMessageFor(const OutboxEntry &e) const;
    QJsonObject postBodyFor(const OutboxEntry &e) const;
    bool entryVisibleInView(const OutboxEntry &e) const;
    void refreshRowStatus(int tempId);
    void startAttempt(int tempId);
    void onAttemptSlow(int tempId, quint64 seq);
    void onAttemptFinished(int tempId, quint64 seq, const QString &token, bool ok,
                           const QJsonObject &data, int status);
    // Who is signed in right now: user + server. An entry is only ever probed,
    // sent or redrawn for the account that wrote it.
    QString outboxOwner() const;
    void deliverReal(const QString &token, int tempId, const Message &real);
    bool dropOutboxEntry(int tempId);
    void removeTempRow(int tempId);     // the row only; the outbox entry is the caller's business
    void outboxTick();
    void probeToken(const QString &token);
    void sendNextQueued(const QString &token);
    void syncOutboxTimer();
    void reinsertOutboxTemps();
    // The newest / oldest SERVER id in the list, or 0 when only pending temps
    // (negative ids) are there. The poll cursor and the scroll-back cursor must
    // never be seeded from a temp.
    int  newestRealId() const;
    int  oldestRealId() const;
    bool hasRealMessages() const { return newestRealId() > 0; }

    QVector<OutboxEntry> m_outbox;            // send order, oldest first
    QHash<QString, QList<int>> m_sendQueue;   // per room: tempIds cleared to POST, in order
    QSet<QString> m_probing;                  // rooms with a history probe in flight
    QTimer m_outboxTimer;                     // 2 s tick while any entry is still active
    quint64 m_attemptCounter = 0;
    int m_latestArrivedRow = 0;               // see latestArrivedRow()

    ApiClient *m_api;
    MessageCache *m_cache;
    MessagePoller *m_poller;
    QVector<Message> m_messages;
    QSet<int> m_messageIds;  // persistent set for O(1) dedup — updated incrementally
    QString m_token;
    // #26 — auto-mention slug for the current room (see header comment
    // above setAutoMentionBot). Lower-cased actorId without the "bots/"
    // prefix; rebuilt on every setConversationToken via the bot list.
    QString m_autoMentionBot;
    bool m_loading = false;
    bool m_hasMoreHistory = true;
    int m_oldestMessageId = 0;
    // 0.41.2-beta — gap-fill state. When refreshLatest or the poller
    // reveals new messages whose IDs are not contiguous with the
    // cached set (e.g. the user's other device sent 200 messages
    // since this client last polled, and refreshLatest only returns
    // the latest 50), m_gapFillCursor is set to the OLDEST of the
    // newly-fetched batch and m_gapFillTargetId to the cached
    // newest-pre-refresh. The fetcher pages older until the gap
    // closes or m_gapFillPagesRemaining is exhausted.
    int m_gapFillCursor          = 0;
    int m_gapFillTargetId        = 0;
    int m_gapFillPagesRemaining  = 0;
    class QNetworkReply *m_gapFillReply = nullptr;
    void runGapFillStep();
    // 0.41.3-beta — when a real message arrives whose referenceId
    // matches a still-pending optimistic temp, remove the temp from
    // the model. Returns true if a temp was removed (caller should
    // skip the temp's id from m_messageIds and add the real msg
    // fresh). Mirrors upstream `getTemporaryReferences` matching.
    bool replaceTempByReferenceId(const Message &real);
    // 0.41.3-beta — client-side thread filter. Replaces the previous
    // server-side `threadId=` query param which the upstream Talk web
    // client does NOT use on the long-poll (it pulls everything and
    // filters in the chat store). When m_threadId>0, only messages
    // where threadId==m_threadId OR id==m_threadId (the seed) pass.
    // When m_threadId==0 AND m_hideThreadMessages is true, only
    // non-thread messages pass.
    bool passesThreadFilter(const Message &m) const;

    // bug 1 — the SINGLE reconciliation authority for the model's core
    // invariant: m_messages strictly newest-first by id (index 0 = newest)
    // and m_messageIds an exact mirror. Every ingest path calls this so the
    // ordering guarantee can never drift between paths. Returns true if a
    // corrective re-sort was performed (cheap O(n) check when already ordered).
    bool enforceNewestFirstInvariant();
    // 0.65.3 - the pre-context page walk, kept as the fallback for servers
    // without `chat-get-context` and for a refused context request.
    void beginPagedHistoryUntil(int messageId);
    // Whether this server supports `chat-get-context`. Set by the owner;
    // false means page-walk only, i.e. exactly the 0.65.2 behaviour.
    bool m_contextCapable = false;
    // Server-side attachment folder; empty = the historic "Talk".
    QString m_attachmentFolder;

    int m_lastCommonRead = 0;
    int m_unreadBoundary = 0;
    ConversationListModel *m_conversations = nullptr;
    int m_threadId = 0;
    bool m_hideThreadMessages = false;
    bool m_connected = true;  // assume connected until proven otherwise
    double m_uploadProgress = -1;
    QString m_uploadFileName;
    QNetworkReply *m_historyReply = nullptr;   // cancel on chat switch
    QNetworkReply *m_refreshReply = nullptr;   // cancel on chat switch
    QNetworkReply *m_readMarkerReply = nullptr;   // light read-marker probe
    QTimer m_readMarkerTimer;  // 5s tick while a chat is open
    int m_generation = 0;  // incremented on conversation switch; stale callbacks bail out
    int m_historyUntilTargetId = 0;
    int m_historyUntilRemainingPages = 0;
};
