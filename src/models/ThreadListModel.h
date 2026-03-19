#pragma once
#include <QAbstractListModel>
#include <QVector>
#include <QJsonArray>
#include "core/ApiClient.h"

struct ThreadInfo {
    int threadId = 0;
    QString title;
    QString lastMessage;
    QString lastAuthor;
    qint64 lastActivity = 0;
    int replyCount = 0;
    int iconColor = 0;
};

class ThreadListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString conversationToken READ conversationToken WRITE setConversationToken NOTIFY tokenChanged)
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        ThreadIdRole = Qt::UserRole + 1,
        TitleRole,
        LastMessageRole,
        LastAuthorRole,
        LastActivityRole,
        ReplyCountRole,
        IconColorRole,
    };

    explicit ThreadListModel(ApiClient *api, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString conversationToken() const { return m_token; }
    void setConversationToken(const QString &token);
    bool isLoading() const { return m_loading; }

    Q_INVOKABLE void refresh();

signals:
    void tokenChanged();
    void loadingChanged();
    void countChanged();

private:
    void fetchThreads();

    ApiClient *m_api;
    QVector<ThreadInfo> m_threads;
    QString m_token;
    bool m_loading = false;
};
