#pragma once

#include <QDialog>

class ApiClient;
class QLabel;
class QTextBrowser;
class QTimer;

/**
 * "Summarise what I missed" — an AI summary of the unread messages in one
 * conversation.
 *
 * TWO-STAGE AND ASYNCHRONOUS BY CONTRACT. `POST /chat/{token}/summarize` does
 * not return a summary; it returns 201 `{taskId}` having *scheduled* one on
 * Nextcloud's TaskProcessing queue. The text has to be collected afterwards
 * from `GET /taskprocessing/task/{id}` once that task reports success. So this
 * dialog opens in a waiting state on purpose — there is nothing to show until
 * a model somewhere else has finished.
 *
 * ⚠ NEEDS A SERVER-SIDE AI PROVIDER. The `chat-summary-api` capability is
 * present only when the server has a TaskProcessing text provider installed;
 * without one the endpoint answers 400 {"error":"ai-no-provider"}. Nothing
 * should reach this dialog on such a server — the caller gates on the
 * capability — but the error is handled anyway, because "the admin removed the
 * provider since login" is a real state.
 */
class ChatSummaryDialog : public QDialog
{
    Q_OBJECT

public:
    ChatSummaryDialog(ApiClient *api, const QString &token, int fromMessageId,
                      QWidget *parent = nullptr);

private:
    void requestSummary();
    void pollTask();
    void fail(const QString &why);

    ApiClient    *m_api;
    QString       m_token;
    int           m_fromMessageId;
    int           m_taskId  = 0;
    int           m_polls   = 0;
    QTimer       *m_poll    = nullptr;
    QTextBrowser *m_text    = nullptr;
    QLabel       *m_status  = nullptr;
};
