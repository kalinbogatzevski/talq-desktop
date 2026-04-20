#include "UpcomingRemindersDialog.h"

#include "core/ApiClient.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QVBoxLayout>
#include <QDateTime>

namespace {
constexpr int kIndexRole = Qt::UserRole + 1;
}

UpcomingRemindersDialog::UpcomingRemindersDialog(ApiClient *api, QWidget *parent)
    : QDialog(parent), m_api(api)
{
    setWindowTitle(tr("Upcoming reminders"));
    resize(520, 420);
    setStyleSheet(
        "QDialog { background: #1a1a18; color: #e4e0da; }"
        "QLabel { color: #e4e0da; }"
        "QListWidget { background: #222220; border: 1px solid #2a2a26;"
        " border-radius: 6px; color: #e4e0da; padding: 4px; }"
        "QListWidget::item { padding: 10px 12px; border-radius: 4px; }"
        "QListWidget::item:hover { background: #2a2a26; }"
        "QListWidget::item:selected { background: #2a2a26; }"
        "QPushButton { background: #2a2a26; color: #e4e0da; border: none;"
        " border-radius: 6px; padding: 6px 16px; font-size: 13px; }"
        "QPushButton:hover { background: #3a3a34; }"
        "QPushButton#danger { color: #ff6b6b; }"
    );

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(10);

    m_list = new QListWidget(this);
    outer->addWidget(m_list, 1);

    m_status = new QLabel(tr("Loading\u2026"), this);
    m_status->setStyleSheet("color: #8a8680; font-size: 12px;");
    outer->addWidget(m_status);

    auto *btnRow = new QHBoxLayout();
    m_refreshBtn = new QPushButton(tr("Refresh"), this);
    btnRow->addWidget(m_refreshBtn);
    btnRow->addStretch();
    m_closeBtn = new QPushButton(tr("Close"), this);
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
        auto *item = new QListWidgetItem(line, m_list);
        item->setData(kIndexRole, i);
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
