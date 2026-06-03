#include "UpcomingRemindersDialog.h"

#include "core/ApiClient.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QDateTime>

namespace {
constexpr int kIndexRole = Qt::UserRole + 1;
}

UpcomingRemindersDialog::UpcomingRemindersDialog(ApiClient *api, QWidget *parent)
    : QDialog(parent), m_api(api)
{
    setWindowTitle(tr("Upcoming reminders"));
    resize(520, 420);
    // Chrome inherited from the app-wide AppStyle sheet (theme-driven).

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(10);

    m_list = new QListWidget(this);
    outer->addWidget(m_list, 1);

    m_status = new QLabel(tr("Loading\u2026"), this);
    m_status->setProperty("role", "secondary");
    outer->addWidget(m_status);

    auto *btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(0, 4, 0, 0);
    btnRow->setSpacing(8);
    m_refreshBtn = new QPushButton(tr("Refresh"), this);
    m_refreshBtn->setProperty("variant", "default");
    btnRow->addWidget(m_refreshBtn);
    btnRow->addStretch();
    m_closeBtn = new QPushButton(tr("Close"), this);
    m_closeBtn->setProperty("variant", "primary");
    m_closeBtn->setDefault(true);
    btnRow->addWidget(m_closeBtn);
    outer->addLayout(btnRow);

    connect(m_list, &QListWidget::itemActivated,
            this, &UpcomingRemindersDialog::onItemActivated);
    connect(m_refreshBtn, &QPushButton::clicked, this, &UpcomingRemindersDialog::refresh);
    connect(m_closeBtn,   &QPushButton::clicked, this, &QDialog::accept);

    refresh();
}

void UpcomingRemindersDialog::refresh()
{
    m_list->clear();
    m_status->setText(tr("Loading\u2026"));
    m_api->fetchUpcomingReminders(this,
        [this](bool ok, const QVector<Reminder> &items) {
            if (!ok) { m_status->setText(tr("Couldn\u2019t load reminders.")); return; }
            populate(items);
        });
}

void UpcomingRemindersDialog::populate(const QVector<Reminder> &items)
{
    m_items = items;
    std::sort(m_items.begin(), m_items.end(),
              [](const Reminder &a, const Reminder &b) { return a.when < b.when; });

    for (int i = 0; i < m_items.size(); ++i) {
        const Reminder &r = m_items[i];
        QString preview = r.messageText;
        preview.replace(QLatin1Char('\n'), QLatin1Char(' '));
        if (preview.size() > 80) preview = preview.left(80) + QStringLiteral("\u2026");
        QString line = QStringLiteral("\u23F0  %1\n      %2 \u2014 %3")
                           .arg(relativeWhen(r.when),
                                r.actorName.isEmpty() ? tr("Unknown") : r.actorName,
                                preview.isEmpty() ? tr("(message)") : preview);

        // Row widget so each reminder carries a Cancel affordance (wired to the
        // existing cancelRow()). The row label keeps the two-line preview; the
        // item still activates on row click to open the conversation.
        auto *row = new QWidget(m_list);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(10, 8, 10, 8);
        rowLayout->setSpacing(10);

        auto *textLabel = new QLabel(line, row);
        textLabel->setWordWrap(true);
        rowLayout->addWidget(textLabel, 1);

        auto *cancelBtn = new QPushButton(tr("Cancel reminder"), row);
        cancelBtn->setProperty("variant", "danger");
        connect(cancelBtn, &QPushButton::clicked, this, [this, i]() {
            cancelRow(i);
        });
        rowLayout->addWidget(cancelBtn);

        auto *item = new QListWidgetItem(m_list);
        item->setData(kIndexRole, i);
        item->setSizeHint(row->sizeHint());
        m_list->addItem(item);
        m_list->setItemWidget(item, row);
    }
    m_status->setText(m_items.isEmpty()
        ? tr("No upcoming reminders.")
        : tr("%n reminder(s)", nullptr, m_items.size()));
}

void UpcomingRemindersDialog::onItemActivated(QListWidgetItem *item)
{
    if (!item) return;
    int idx = item->data(kIndexRole).toInt();
    if (idx < 0 || idx >= m_items.size()) return;
    const Reminder &r = m_items[idx];
    emit openConversationAt(r.token, r.messageId);
    accept();
}

void UpcomingRemindersDialog::cancelRow(int row)
{
    if (row < 0 || row >= m_items.size()) return;
    const Reminder r = m_items[row];
    m_api->cancelMessageReminder(r.token, r.messageId, this,
        [this](bool ok, const QString &) {
            if (ok) refresh();
        });
}

QString UpcomingRemindersDialog::relativeWhen(const QDateTime &when)
{
    const qint64 secs = QDateTime::currentDateTime().secsTo(when);
    if (secs < 0) return when.toString(QStringLiteral("ddd d MMM, HH:mm")) + tr(" (overdue)");
    if (secs < 60 * 60)                 return tr("in %n min", nullptr, int(secs / 60));
    if (secs < 24 * 60 * 60)            return tr("in %n h", nullptr, int(secs / 3600));
    if (when.date() == QDate::currentDate().addDays(1))
        return tr("tomorrow %1").arg(when.toString(QStringLiteral("HH:mm")));
    if (secs < 7 * 24 * 60 * 60)
        return when.toString(QStringLiteral("ddd HH:mm"));
    return when.toString(QStringLiteral("d MMM, HH:mm"));
}
