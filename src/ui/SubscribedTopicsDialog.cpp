#include "SubscribedTopicsDialog.h"

#include "core/ApiClient.h"
#include "models/ConversationListModel.h"

#include <QDialogButtonBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

namespace {
constexpr int kTokenRole    = Qt::UserRole + 1;
constexpr int kThreadIdRole = Qt::UserRole + 2;
}

SubscribedTopicsDialog::SubscribedTopicsDialog(ApiClient *api,
                                               ConversationListModel *conversations,
                                               QWidget *parent)
    : QDialog(parent)
    , m_api(api)
    , m_conversations(conversations)
{
    setWindowTitle(tr("Topics you follow"));
    setMinimumSize(480, 420);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 14);
    root->setSpacing(10);

    m_list = new QListWidget(this);
    m_list->setAlternatingRowColors(false);
    m_list->setUniformItemSizes(false);
    root->addWidget(m_list, 1);

    m_status = new QLabel(tr("Loading…"), this);
    m_status->setProperty("role", "secondary");
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) {
        if (!it) return;
        const QString token = it->data(kTokenRole).toString();
        const int threadId  = it->data(kThreadIdRole).toInt();
        if (token.isEmpty() || threadId <= 0) return;   // the empty-state row
        emit topicChosen(token, threadId);
        accept();
    });

    fetchTopics();
}

void SubscribedTopicsDialog::fetchTopics()
{
    m_api->fetchSubscribedThreads(100, [this](bool ok, const QJsonArray &list, int) {
        m_list->clear();
        if (!ok) {
            m_status->setText(tr("Could not load your followed topics."));
            return;
        }

        int shown = 0;
        for (const QJsonValue &v : list) {
            const QJsonObject o  = v.toObject();
            const QJsonObject th = o.value(QStringLiteral("thread")).toObject();
            const int    id      = th.value(QStringLiteral("id")).toInt();
            const QString token  = th.value(QStringLiteral("roomToken")).toString();
            if (id <= 0 || token.isEmpty()) continue;

            const QString title = th.value(QStringLiteral("title")).toString();
            const int replies   = th.value(QStringLiteral("numReplies")).toInt();
            // The conversation NAME, not its token — a token means nothing to
            // the reader, and naming the room is the entire point of a
            // cross-conversation list. Falls back to the token only if the
            // sidebar has not heard of the room yet.
            QString room = m_conversations ? m_conversations->displayNameForToken(token)
                                           : QString();
            if (room.isEmpty()) room = token;

            const QJsonObject last = o.value(QStringLiteral("last")).toObject();
            const QString preview  = last.value(QStringLiteral("message")).toString();
            const QString author   = last.value(QStringLiteral("actorDisplayName")).toString();

            QString line = QStringLiteral("%1\n%2 · %3")
                               .arg(title.isEmpty() ? tr("(untitled topic)") : title,
                                    room,
                                    tr("%n repl(y|ies)", nullptr, replies));
            if (!preview.isEmpty()) {
                line += QStringLiteral("\n%1%2")
                            .arg(author.isEmpty() ? QString()
                                                  : author + QStringLiteral(": "),
                                 preview.left(120));
            }

            auto *item = new QListWidgetItem(line, m_list);
            item->setData(kTokenRole, token);
            item->setData(kThreadIdRole, id);
            ++shown;
        }

        if (shown == 0) {
            // A non-selectable placeholder rather than an empty box, so "you
            // follow nothing yet" is distinguishable from "this is broken".
            auto *empty = new QListWidgetItem(
                tr("You are not following any topics yet.\n"
                   "Right-click a topic and set its notifications to follow it."),
                m_list);
            empty->setFlags(Qt::NoItemFlags);
            empty->setTextAlignment(Qt::AlignCenter);
            m_status->setText(QString());
            return;
        }
        m_status->setText(tr("%n topic(s) · click one to open it", nullptr, shown));
    });
}
