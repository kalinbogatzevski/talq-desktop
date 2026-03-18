#pragma once

#include <QAbstractListModel>
#include <QVector>
#include "models/Conversation.h"
#include "core/ApiClient.h"

/**
 * QAbstractListModel exposing conversations to QML.
 * Fetches from Talk API and keeps the list sorted (favorites first, then by activity).
 */
class ConversationListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        TokenRole = Qt::UserRole + 1,
        DisplayNameRole,
        TypeRole,
        UnreadCountRole,
        UnreadMentionRole,
        FavoriteRole,
        LastMessageRole,
        LastAuthorRole,
        LastActivityRole,
        ActorIdRole,
    };

    explicit ConversationListModel(ApiClient *api, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE QString tokenAt(int index) const;
    Q_INVOKABLE int lastReadMessageForToken(const QString &token) const;

    bool isLoading() const { return m_loading; }

signals:
    void loadingChanged();
    void countChanged();
    void errorOccurred(const QString &error);

private:
    ApiClient *m_api;
    QVector<Conversation> m_conversations;
    bool m_loading = false;
};
