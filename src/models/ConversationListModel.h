#pragma once

#include <QAbstractListModel>
#include <QVector>
#include <QTimer>
#include "models/Conversation.h"
#include "core/ApiClient.h"

/**
 * QAbstractListModel exposing conversations to QML.
 * Fetches from Talk API and keeps the list sorted (favorites first, then by activity).
 * Auto-refreshes every 30 seconds to catch new messages in all conversations.
 */
class ConversationListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int totalUnread READ totalUnread NOTIFY totalUnreadChanged)

public:
    enum Roles {
        TokenRole = Qt::UserRole + 1,
        DisplayNameRole,
        TypeRole,
        UnreadCountRole,
        UnreadMentionRole,
        // 0.65.3 - someone mentioned YOU, as opposed to @all.
        // unreadMention is true for both, so without this an @all in a
        // busy room lights the same alarm badge as a direct mention.
        UnreadMentionDirectRole,
        FavoriteRole,
        LastMessageRole,
        LastAuthorRole,
        LastActivityRole,
        ActorIdRole,
        UserStatusRole,     // "online", "away", "dnd", "offline"
        UserStatusMessageRole,
        UserStatusIconRole,
        HasTopicsRole,
        NotificationLevelRole,
        ParticipantTypeRole,
        TagIdsRole,        // QStringList — Talk 24 conversation tags
        AttributesRole,    // int bit-flags — Talk 24 (bit 0 = voice room)
        DescriptionRole,   // room description, so the info dialog can round-trip it
        ArchivedRole,      // out of the default list, but still joined
        ImportantRole,     // keeps notifying even while archived
    };

    explicit ConversationListModel(ApiClient *api, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void startAutoRefresh();
    Q_INVOKABLE void stopAutoRefresh();
    // Force-refresh the user_status map outside the 60 s poll. MainWindow
    // calls this on window-activate so the sidebar dots update as soon as
    // the user returns to TalQ, rather than waiting up to a minute.
    Q_INVOKABLE void refreshUserStatuses();
    Q_INVOKABLE QString tokenAt(int index) const;
    Q_INVOKABLE int lastReadMessageForToken(const QString &token) const;
    Q_INVOKABLE QString userStatusForToken(const QString &token) const;
    Q_INVOKABLE QString userStatusMessageForToken(const QString &token) const;
    Q_INVOKABLE QString userStatusIconForToken(const QString &token) const;
    // Conversation type (1 = one_to_one, 2 = group, 3 = public, etc.).
    // Returns 0 if the token isn't in the cached list. Used by the
    // composer to decide whether to auto-prepend a bot mention in 1:1s.
    Q_INVOKABLE int conversationTypeForToken(const QString &token) const;
    // Talk 24 conversation attribute bit-flags (bit 0 = voice room). Returns 0
    // for an unknown token and on any pre-24 server, which reads as "no
    // attributes" — see talq::isVoiceRoom(), which additionally gates on the
    // presets capability before this value is allowed to mean anything.
    Q_INVOKABLE int attributesForToken(const QString &token) const;
    // Ids of the user's tags on this conversation (Talk 24). Empty on older
    // servers and for untagged conversations — the two are indistinguishable
    // by design, since both mean "show no tags".
    QStringList tagIdsForToken(const QString &token) const;
    Q_INVOKABLE void clearUnreadForToken(const QString &token);
    // Mirror a server-side read advance into our cache without a round-trip:
    // sets lastReadMessage and clears unreadMessages for `token`. Used after
    // a successful POST /chat/{token}/read so the next conversation switch
    // doesn't reload a stale lastReadMessage (which would re-show the
    // "New messages" divider).
    Q_INVOKABLE void markReadAt(const QString &token, int lastReadMessageId);
    Q_INVOKABLE void setHasTopics(const QString &token, bool has);
    Q_INVOKABLE void setNotificationLevel(int index, int level);
    // --- 0.65.3 ---------------------------------------------------------
    // Favourite state. TalQ has always PAINTED favourites (dot, top-of-list
    // sort, its own filter and section) but had no way to set one, so a
    // TalQ-only user had to open the web UI to pin anything.
    Q_INVOKABLE bool favoriteAt(int index) const;
    Q_INVOKABLE void setFavorite(int index, bool favorite);
    // Per-room call ringing (`notification-calls`), separate from the chat
    // notification level.
    Q_INVOKABLE bool notificationCallsAt(int index) const;
    Q_INVOKABLE void setNotificationCalls(int index, bool notify);
    // Whether this user may start a call here. Defaults true for an unknown
    // token so callers behave as they did before this was readable.
    Q_INVOKABLE bool canStartCallForToken(const QString &token) const;
    // Talk participant type (1 owner, 2 moderator, 3 user, 6 guest-moderator…);
    // used to hide moderator-only actions rather than let them 403.
    Q_INVOKABLE int participantTypeForToken(const QString &token) const;
    // Talk's callRecording state for a room (0 none / 1 video / 2 audio /
    // 3 video-starting / 4 audio-starting / 5 failed). 0 for an unknown
    // token, which reads as "not recording".
    Q_INVOKABLE int callRecordingForToken(const QString &token) const;
    // SIP dial-in state and this user's dial-in PIN for a room.
    Q_INVOKABLE int sipEnabledForToken(const QString &token) const;
    Q_INVOKABLE QString attendeePinForToken(const QString &token) const;
    // Display name for a token, so a cross-conversation search hit can say
    // WHICH conversation it is in. Empty for an unknown token.
    Q_INVOKABLE QString displayNameForToken(const QString &token) const;
    Q_INVOKABLE bool archivedAt(int index) const;
    Q_INVOKABLE void setArchived(int index, bool archived);
    Q_INVOKABLE bool importantAt(int index) const;
    Q_INVOKABLE void setImportant(int index, bool important);
    Q_INVOKABLE void updateLastMessage(const QString &token, const QString &author, const QString &text);

    bool isLoading() const { return m_loading; }
    int totalUnread() const { return m_totalUnread; }

signals:
    void loadingChanged();
    void countChanged();
    void totalUnreadChanged();
    void errorOccurred(const QString &error);
    // Emitted when a conversation gets new unread messages
    void newUnreadMessage(const QString &conversationName, const QString &lastMessage, const QString &token);
    // Emitted when someone starts a call in a conversation
    void incomingCallDetected(const QString &conversationName, const QString &token, int callFlag);

private:
    int indexOfToken(const QString &token) const;
    void fetchUserStatuses();
    // Re-sort in place after a change that moves a row (favouriting).
    void resort();

    struct UserStatus {
        QString state;
        QString message;
        QString icon;
        bool operator==(const UserStatus &other) const {
            return state == other.state && message == other.message && icon == other.icon;
        }
        bool operator!=(const UserStatus &other) const { return !(*this == other); }
    };

    ApiClient *m_api;
    QVector<Conversation> m_conversations;
    QHash<QString, UserStatus> m_userStatuses;  // userId → UserStatus
    QHash<QString, bool> m_callState;        // persistent call state — survives across refreshes
    bool m_callStateSeeded = false;          // true after first load — prevents ringing for stale calls
    QTimer m_autoRefreshTimer;
    QTimer m_statusPollTimer;
    // Watchdog for the m_loading latch. m_loading is cleared only inside the
    // /room reply callback; if that reply never completes (a hung/half-open
    // connection with no transfer timeout), m_loading would stay true forever
    // and EVERY later refresh() would early-return at `if (m_loading) return;`
    // — the list freezes until an app restart. The watchdog force-clears the
    // latch and retries so it can self-heal.
    QTimer m_loadWatchdog;
    bool m_loading = false;
    // Set when a refresh() is requested while one is already in flight (e.g. a
    // push arrives mid-load). Instead of silently dropping it, we run one more
    // refresh after the in-flight one completes, so new rooms are never missed.
    bool m_pendingRefresh = false;
    int m_totalUnread = 0;
};
