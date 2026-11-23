#include "ScheduledMessagesDialog.h"
#include "core/ApiClient.h"
#include "core/TalkRefusalPolicy.h"

#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

ScheduledMessagesDialog::ScheduledMessagesDialog(ApiClient *api, const QString &token, QWidget *parent)
    : QDialog(parent)
    , m_api(api)
    , m_token(token)
{
    setWindowTitle(tr("Scheduled messages"));
    setMinimumSize(460, 360);
    // Chrome inherited from the app-wide AppStyle sheet (theme-driven).

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto *header = new QLabel(tr("Pending delivery in this conversation:"), this);
    root->addWidget(header);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setSpacing(2);
    root->addWidget(m_list, 1);

    m_empty = new QLabel(tr("No scheduled messages."), this);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setProperty("role", "muted");
    m_empty->hide();
    root->addWidget(m_empty);

    // Inline status line for failed mutations (cancel/save). Theme-driven via
    // the role="danger" QSS token; stays empty/hidden until the server refuses
    // a request, mirroring ConversationInfoDialog's m_status. Looked up by
    // objectName from deleteItem()/editItem() so no header change is needed.
    auto *status = new QLabel(QString(), this);
    status->setObjectName(QStringLiteral("status"));
    status->setProperty("role", "danger");
    status->setWordWrap(true);
    status->hide();
    root->addWidget(status);

    auto *closeBtn = new QPushButton(tr("Close"), this);
    closeBtn->setProperty("variant", "primary");
    auto *btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(0, 4, 0, 0);
    btnRow->setSpacing(8);
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    reload();
}

void ScheduledMessagesDialog::reload()
{
    m_list->clear();
    // Drop any stale failure notice from a prior cancel/save attempt.
    if (auto *status = findChild<QLabel *>(QStringLiteral("status"))) {
        status->clear();
        status->hide();
    }
    if (!m_api || m_token.isEmpty()) { renderEmpty(); return; }

    m_api->getArray("apps/spreed/api/v1/chat/" + m_token + "/schedule",
        [this](bool ok, const QJsonArray &items, int) {
            if (!ok || items.isEmpty()) { renderEmpty(); return; }
            renderItems(items);
        });
}

void ScheduledMessagesDialog::renderEmpty()
{
    m_list->hide();
    m_empty->show();
}

void ScheduledMessagesDialog::renderItems(const QJsonArray &items)
{
    m_empty->hide();
    m_list->show();

    for (const QJsonValue &v : items) {
        const QJsonObject o = v.toObject();
        const qint64 messageId = o.value(QStringLiteral("id")).toVariant().toLongLong();
        const qint64 sendAt    = o.value(QStringLiteral("sendAt")).toVariant().toLongLong();
        // The message exactly as the server holds it. That is what the user
        // TYPED -- the composer posts plain text to /schedule
        // (MessageListModel::scheduleMessage) and the edit below posts plain
        // text back -- so it is not markup. Stripping "<...>" from it deleted
        // what they had written, and the Edit popup was then prefilled from the
        // stripped AND 140-character-truncated copy: re-saving a long scheduled
        // message, even to change only its time, shipped the first 140
        // characters followed by an ellipsis. `fullText` feeds the edit,
        // `text` is only what the row shows.
        const QString fullText = o.value(QStringLiteral("message")).toString();
        QString text = fullText;
        if (text.length() > 140) text = text.left(140) + QStringLiteral("…");

        const QString when = QDateTime::fromSecsSinceEpoch(sendAt)
                                .toString(QStringLiteral("ddd dd MMM yyyy, HH:mm"));

        auto *row = new QWidget(m_list);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(10, 8, 10, 8);
        rowLayout->setSpacing(10);

        auto *textCol = new QVBoxLayout();
        textCol->setSpacing(2);
        auto *whenLabel = new QLabel(QStringLiteral("⏰ %1").arg(when), row);
        whenLabel->setProperty("role", "success");
        auto *textLabel = new QLabel(text.isEmpty()
            ? QStringLiteral("(empty)") : text, row);
        // Plain text, pinned: the default (AutoText) would render a typed
        // "<b>x</b>" as markup and show "&amp;" literally, depending on the guess.
        textLabel->setTextFormat(Qt::PlainText);
        textLabel->setWordWrap(true);
        textCol->addWidget(whenLabel);
        textCol->addWidget(textLabel);
        rowLayout->addLayout(textCol, 1);

        // The edit popup starts from the whole message, byte for byte, so that
        // saving without touching the text changes nothing.
        const QString editPrefill = fullText;
        // Whether it was scheduled silently. The edit has to send it back: the
        // server's edit takes `silent` with a default of false and overwrites
        // the stored flag with whatever arrives (spreed v25.0.7
        // ScheduledMessageService::editMessage), so leaving it out un-silenced
        // the message. An older server that does not list the field reads as
        // false, which is what was sent before.
        const bool silent = o.value(QStringLiteral("silent")).toBool();
        auto *editBtn = new QPushButton(tr("Edit"), row);
        editBtn->setProperty("variant", "default");
        editBtn->setFixedWidth(70);
        connect(editBtn, &QPushButton::clicked, this,
                [this, messageId, editPrefill, sendAt, silent]() {
            editItem(messageId, editPrefill, sendAt, silent);
        });
        rowLayout->addWidget(editBtn);

        auto *delBtn = new QPushButton(tr("Cancel"), row);
        delBtn->setProperty("variant", "danger");
        delBtn->setFixedWidth(80);
        connect(delBtn, &QPushButton::clicked, this, [this, messageId]() {
            deleteItem(messageId);
        });
        rowLayout->addWidget(delBtn);

        auto *item = new QListWidgetItem(m_list);
        item->setSizeHint(row->sizeHint());
        m_list->addItem(item);
        m_list->setItemWidget(item, row);
    }
}

void ScheduledMessagesDialog::deleteItem(qint64 messageId)
{
    if (!m_api || messageId <= 0) return;
    const QString path = QStringLiteral("apps/spreed/api/v1/chat/%1/schedule/%2")
                            .arg(m_token).arg(messageId);
    m_api->del(path, [this](bool ok, const QJsonObject &data, int httpStatus) {
        if (ok) {
            reload();
            return;
        }
        // Server refused the cancel — surface it instead of silently leaving the row in place.
        showRefusal(talq::TalkRequest::CancelScheduled, data, httpStatus);
    });
}

// Says on the status line why the server refused a cancel or an edit (TalkRefusalPolicy.h picks
// the reason; the code itself is never shown). A message that is no longer pending is taken off
// the list: reload() first, because it clears the status line.
void ScheduledMessagesDialog::showRefusal(talq::TalkRequest request, const QJsonObject &data,
                                          int httpStatus)
{
    using talq::TalkRefusal;
    const TalkRefusal why = talq::classifyTalkRefusal(
        request, httpStatus, data.value(QStringLiteral("error")).toString().toStdString());
    const bool cancel = (request == talq::TalkRequest::CancelScheduled);
    QString text;
    switch (why) {
    case TalkRefusal::NoConnection:
        text = cancel ? tr("Couldn't cancel: TalQ could not reach the server.")
                      : tr("Couldn't save changes: TalQ could not reach the server.");
        break;
    case TalkRefusal::ScheduledGone:
        reload();
        text = tr("That message was already sent or cancelled.");
        break;
    case TalkRefusal::NotAMember:
        text = tr("Scheduled messages need you to be a member of this conversation.");
        break;
    case TalkRefusal::PastTime:
        text = tr("Couldn't save changes: that send time has already passed. Pick a later time.");
        break;
    case TalkRefusal::EmptyText:
        text = tr("Couldn't save changes: a scheduled message can't be empty.");
        break;
    case TalkRefusal::TooLong:
        text = tr("Couldn't save changes: the message is too long.");
        break;
    default:
        text = cancel ? tr("Couldn't cancel (HTTP %1).").arg(httpStatus)
                      : tr("Couldn't save changes (HTTP %1).").arg(httpStatus);
        break;
    }
    if (auto *status = findChild<QLabel *>(QStringLiteral("status"))) {
        status->setText(text);
        status->show();
    }
}

void ScheduledMessagesDialog::editItem(qint64 messageId, const QString &currentText,
                                       qint64 currentSendAt, bool silent)
{
    if (!m_api || messageId <= 0) return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Edit scheduled message"));
    dlg.setMinimumWidth(420);
    // Chrome inherited from the app-wide AppStyle sheet (theme-driven).

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(8);

    layout->addWidget(new QLabel(tr("Message"), &dlg));
    auto *editor = new QPlainTextEdit(currentText, &dlg);
    editor->setMinimumHeight(110);
    layout->addWidget(editor);

    layout->addWidget(new QLabel(tr("Send at"), &dlg));
    auto *when = new QDateTimeEdit(
        QDateTime::fromSecsSinceEpoch(currentSendAt), &dlg);
    when->setCalendarPopup(true);
    // Server still rejects past times; require at least a minute out so the
    // user can't accidentally submit something that errors out instantly.
    when->setMinimumDateTime(QDateTime::currentDateTime().addSecs(60));
    when->setDisplayFormat(QStringLiteral("ddd dd MMM yyyy   HH:mm"));
    layout->addWidget(when);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    QJsonObject body;
    body["message"] = editor->toPlainText().trimmed();
    body["sendAt"]  = when->dateTime().toSecsSinceEpoch();
    // Always sent, false included: the server replaces the stored flag with it.
    // No threadTitle: an empty one keeps the stored title, a non-empty one on a
    // message that does not start a topic is refused.
    body["silent"]  = silent;
    const QString path = QStringLiteral("apps/spreed/api/v1/chat/%1/schedule/%2")
                            .arg(m_token).arg(messageId);
    m_api->post(path, body, [this](bool ok, const QJsonObject &data, int httpStatus) {
        if (ok) {
            reload();
            return;
        }
        // Server refused the edit — the inline edit popup has already closed, so report on the
        // list dialog's persistent status line.
        showRefusal(talq::TalkRequest::EditScheduled, data, httpStatus);
    });
}
