#pragma once

#include <QDialog>

class ApiClient;
class ConversationListModel;
class QListWidget;
class QListWidgetItem;
class QLabel;

/**
 * Every topic you follow, across all your conversations.
 *
 * This is the only view in TalQ that deliberately crosses conversation
 * boundaries. Topics are per-conversation everywhere else, which is right for
 * reading but wrong for catching up: if you follow one topic in each of eight
 * rooms, the sidebar makes you open eight rooms to find out whether any of
 * them moved. The server keeps the subscription list per user
 * (GET /chat/subscribed-threads), so this asks for it directly.
 *
 * Picking a row opens that conversation AND selects that topic, which is the
 * only reason the list is worth showing — a list you cannot act on would just
 * be another place to look.
 */
class SubscribedTopicsDialog : public QDialog
{
    Q_OBJECT

public:
    SubscribedTopicsDialog(ApiClient *api, ConversationListModel *conversations,
                           QWidget *parent = nullptr);

signals:
    // token + topic root message id. The window owns navigation.
    void topicChosen(const QString &token, int threadId);

private:
    void fetchTopics();

    ApiClient             *m_api;
    ConversationListModel *m_conversations;
    QListWidget           *m_list   = nullptr;
    QLabel                *m_status = nullptr;
};
