#pragma once

#include <QDialog>

class ApiClient;
class QJsonObject;
class QListWidget;
class QLabel;
namespace talq { enum class TalkRequest; }

/**
 * Lists pending scheduled messages for the given conversation and lets the
 * user edit (text and time) or cancel them. Loads on construction via
 *   GET /apps/spreed/api/v1/chat/{token}/schedule
 * edits via
 *   POST /apps/spreed/api/v1/chat/{token}/schedule/{messageId}
 * and deletes via
 *   DELETE /apps/spreed/api/v1/chat/{token}/schedule/{messageId}
 *
 * All of them need the server's `scheduled-messages` capability (Talk 23);
 * MainWindow opens this dialog only when it is advertised.
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
    // Reloads the list when the server confirms. `silent` is the entry's own
    // flag, sent back unchanged (the server overwrites it on every edit).
    void editItem(qint64 messageId, const QString &currentText, qint64 currentSendAt,
                  bool silent);
    // Why a cancel or an edit was refused, on the status line (TalkRefusalPolicy.h).
    void showRefusal(talq::TalkRequest request, const QJsonObject &data, int httpStatus);

    ApiClient *m_api;
    QString    m_token;
    QListWidget *m_list = nullptr;
    QLabel    *m_empty = nullptr;
};
