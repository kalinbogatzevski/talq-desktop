#include "ScheduledMessagesDialog.h"
#include "core/ApiClient.h"

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

    auto *closeBtn = new QPushButton(tr("Close"), this);
    closeBtn->setProperty("variant", "primary");
    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    reload();
}

void ScheduledMessagesDialog::reload()
{
    m_list->clear();
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
        QString text = o.value(QStringLiteral("message")).toString();
        // Strip rich-text markers — the API may have already substituted
        // {actor}/{file} placeholders into HTML-like spans for the message.
        text.remove(QRegularExpression("<[^>]*>"));
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
        textLabel->setWordWrap(true);
        textCol->addWidget(whenLabel);
        textCol->addWidget(textLabel);
        rowLayout->addLayout(textCol, 1);

        // Capture the plain text for the edit popup — server returns the
        // already-substituted version (HTML-ish), but we round-trip the raw
        // user content. For now use the plain-stripped version we computed
        // above; the round-trip is the user's recall, not a perfect copy.
        const QString editPrefill = text;
        auto *editBtn = new QPushButton(tr("Edit"), row);
        editBtn->setFixedWidth(70);
        connect(editBtn, &QPushButton::clicked, this, [this, messageId, editPrefill, sendAt]() {
            editItem(messageId, editPrefill, sendAt);
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
    m_api->del(path, [this](bool ok, const QJsonObject &, int) {
        if (ok) reload();
    });
}

void ScheduledMessagesDialog::editItem(qint64 messageId, const QString &currentText, qint64 currentSendAt)
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
    const QString path = QStringLiteral("apps/spreed/api/v1/chat/%1/schedule/%2")
                            .arg(m_token).arg(messageId);
    m_api->post(path, body, [this](bool ok, const QJsonObject &, int) {
        if (ok) reload();
    });
}
