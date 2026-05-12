#pragma once

#include <QDialog>

class ApiClient;
class QListWidget;
class QLabel;

/**
 * Lists pending scheduled messages for the given conversation and lets the
 * user cancel them. Loads on construction via
 *   GET /apps/spreed/api/v1/chat/{token}/schedule
 * and deletes via
 *   DELETE /apps/spreed/api/v1/chat/{token}/schedule/{messageId}
 *
 * Edit is intentionally out of scope for the first cut — Talk's web client
 * supports it, but the round-trip dialog is larger than a single button on
 * a row. Once we have a "compose at this future time" reuse path, edit will
 * fall out cheaply.
 */
class ScheduledMessagesDialog : public QDialog
{
    Q_OBJECT

public:
    ScheduledMessagesDialog(ApiClient *api, const QString &token, QWidget *parent = nullptr);

private:
    void reload();
    void renderEmpty();
    void renderItems(const QJsonArray &items);
    void deleteItem(qint64 messageId);
    // Opens an inline edit popup (text + datetime) for the given queue
    // entry, and on accept POSTs to /chat/{token}/schedule/{messageId}.
    // Reloads the list when the server confirms.
    void editItem(qint64 messageId, const QString &currentText, qint64 currentSendAt);

    ApiClient *m_api;
    QString    m_token;
    QListWidget *m_list = nullptr;
    QLabel    *m_empty = nullptr;
};
