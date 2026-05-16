#pragma once

#include <QDialog>
#include "core/Reminder.h"

class ApiClient;
class QListWidget;
class QListWidgetItem;
class QLabel;
class QPushButton;

/**
 * Lists all upcoming reminders from all sources (today: NC Talk; future:
 * ERP). Double-click jumps to the conversation; a trash button per row
 * cancels the reminder server-side.
 */
class UpcomingRemindersDialog : public QDialog
{
    Q_OBJECT
public:
    explicit UpcomingRemindersDialog(ApiClient *api, QWidget *parent = nullptr);

signals:
    // Emitted when the user double-clicks a reminder — caller should open
    // that conversation and scroll to the message.
    void openConversationAt(const QString &token, int messageId);

private slots:
    void refresh();
    void onItemActivated(QListWidgetItem *item);
    void cancelRow(int row);

private:
    void populate(const QVector<Reminder> &items);
    static QString relativeWhen(const QDateTime &when);

    ApiClient   *m_api = nullptr;
    QListWidget *m_list = nullptr;
    QLabel      *m_status = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;

    QVector<Reminder> m_items;
};
