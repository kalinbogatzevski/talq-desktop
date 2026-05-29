#pragma once
#include <QAbstractListModel>
#include <QVector>
#include <QJsonArray>
#include <QSet>
#include "core/ApiClient.h"
#include "core/MessageCache.h"

class ConversationListModel;

struct ThreadInfo {
    int threadId = 0;
    QString title;
    QString lastMessage;
    QString lastAuthor;
    qint64 lastActivity = 0;
    int replyCount = 0;
    int iconColor = 0;
    int unreadCount = 0;
    int lastReadMessageId = 0;
    bool isAllMessages = false;
};

class ThreadListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString conversationToken READ conversationToken WRITE setConversationToken NOTIFY tokenChanged)
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(bool hasTopics READ hasTopics NOTIFY hasTopicsChanged)

public:
    enum Roles {
        ThreadIdRole = Qt::UserRole + 1,
        TitleRole,
        LastMessageRole,
        LastAuthorRole,
        LastActivityRole,
        ReplyCountRole,
        IconColorRole,
        UnreadCountRole,
        IsAllMessagesRole,
    };

    explicit ThreadListModel(ApiClient *api, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString conversationToken() const { return m_token; }
    void setConversationToken(const QString &token);
    bool isLoading() const { return m_loading; }
    bool hasTopics() const { return m_threads.size() > 1; }

    int conversationType() const { return m_conversationType; }
    Q_INVOKABLE void setConversationType(int type) { m_conversationType = type; }
    void setCache(MessageCache *cache);

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void markTopicRead(int threadId);
    Q_INVOKABLE void selectTopic(int threadId);
    Q_INVOKABLE int colorForThread(int threadId) const;

    // Best-effort "delete topic". Nextcloud Talk has NO thread-delete endpoint
    // (upstream issue #17146), so a topic is a grouping of messages with no
    // entity to remove. This deletes the topic's MESSAGES (root + replies) via
    // the per-message delete API — bounded by server rules (own messages within
    // the edit window; others' only with moderator rights). Messages that can't
    // be deleted remain; topicDeleteFinished reports the tally and refresh()
    // drops the chip once the topic has no surviving messages.
    Q_INVOKABLE void deleteTopic(int threadId);

    // Client-side hide. Talk has no thread delete and only tombstones messages,
    // so the only reliable way to make a topic disappear from view is to hide
    // it locally. Persisted per-conversation in QSettings and filtered out of
    // the chip list; survives restarts. The topic still exists server-side and
    // on other clients. unhideAllTopics() restores them.
    Q_INVOKABLE void hideTopic(int threadId);
    Q_INVOKABLE void unhideAllTopics();
    int hiddenTopicCount() const { return m_hiddenTopics.size(); }

    // The room's read marker (this user's lastReadMessage for the whole
    // conversation). Per-topic unread = messages in that topic with id > this.
    // Fed by MainWindow; the next refresh()/scan uses it to populate the chip
    // counters. Set it BEFORE refreshing so the counts reflect the latest read
    // position.
    void setRoomLastReadId(int id) { m_roomLastReadId = id; }

    // Lets the model pull the room's read marker itself at scan time (kept in
    // sync by ConversationListModel's poll), so per-topic counts stay current
    // without per-refresh wiring.
    void setConversations(ConversationListModel *c) { m_conversations = c; }

private slots:
    void onCachedThreadsLoaded(const QString &token, const QVector<QJsonObject> &threads);

signals:
    void tokenChanged();
    void loadingChanged();
    void countChanged();
    void hasTopicsChanged();
    // Emitted when deleteTopic() finishes: how many of the topic's messages were
    // deleted vs left behind (server permission/time limits).
    void topicDeleteFinished(int threadId, int deleted, int failed);

private:
    void fetchThreads();
    void loadHiddenTopics();   // from QSettings for m_token
    void saveHiddenTopics();

    ApiClient *m_api;
    QVector<ThreadInfo> m_threads;
    QString m_token;
    bool m_loading = false;
    MessageCache *m_cache = nullptr;
    int m_selectedThreadId = -1;
    int m_conversationType = 0;
    int m_roomLastReadId = 0;
    ConversationListModel *m_conversations = nullptr;
    QSet<int> m_hiddenTopics;   // per-room, persisted; filtered from the chips
};
