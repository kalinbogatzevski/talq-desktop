#include "ChatSummaryDialog.h"

#include "core/ApiClient.h"

#include <QDialogButtonBox>
#include <QJsonObject>
#include <QLabel>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>

namespace {
// The task runs on someone else's hardware; a small local model can take a
// while and a busy queue longer. Poll on a slow cadence and give up after two
// minutes rather than spinning forever with no explanation.
constexpr int kPollIntervalMs = 2000;
constexpr int kMaxPolls       = 60;
}

ChatSummaryDialog::ChatSummaryDialog(ApiClient *api, const QString &token,
                                     int fromMessageId, QWidget *parent)
    : QDialog(parent)
    , m_api(api)
    , m_token(token)
    , m_fromMessageId(fromMessageId)
{
    setWindowTitle(tr("What you missed"));
    setMinimumSize(460, 320);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 14);
    root->setSpacing(10);

    m_text = new QTextBrowser(this);
    m_text->setOpenExternalLinks(false);
    m_text->setFrameShape(QFrame::NoFrame);
    root->addWidget(m_text, 1);

    m_status = new QLabel(tr("Asking the server for a summary…"), this);
    m_status->setProperty("role", "secondary");
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    m_poll = new QTimer(this);
    m_poll->setInterval(kPollIntervalMs);
    connect(m_poll, &QTimer::timeout, this, &ChatSummaryDialog::pollTask);

    requestSummary();
}

void ChatSummaryDialog::fail(const QString &why)
{
    m_poll->stop();
    m_status->setText(why);
}

void ChatSummaryDialog::requestSummary()
{
    m_api->summarizeChat(m_token, m_fromMessageId,
        [this](bool ok, const QJsonObject &data, int status) {
            if (!ok) {
                const QString err = data.value(QStringLiteral("error")).toString();
                if (err == QLatin1String("ai-no-provider"))
                    fail(tr("This server has no summarising service set up."));
                else if (status == 204)
                    // 204 = nothing to summarise. Not an error; say so plainly.
                    fail(tr("There is nothing new to summarise here."));
                else
                    fail(tr("The summary could not be started."));
                return;
            }
            m_taskId = data.value(QStringLiteral("taskId")).toInt();
            if (m_taskId <= 0) {
                fail(tr("The summary could not be started."));
                return;
            }
            m_status->setText(tr("Writing the summary…"));
            m_poll->start();
        });
}

void ChatSummaryDialog::pollTask()
{
    if (++m_polls > kMaxPolls) {
        fail(tr("The summary is taking too long. Try again later."));
        return;
    }

    m_api->fetchTaskResult(m_taskId, [this](bool ok, const QJsonObject &data, int) {
        if (!ok) return;   // transient — the next tick tries again

        const QJsonObject task = data.value(QStringLiteral("task")).toObject();
        const QString status   = task.value(QStringLiteral("status")).toString();

        // TaskProcessing reports status as a STRING constant. Anything that is
        // not terminal means "keep waiting"; only these two end the poll.
        if (status == QLatin1String("STATUS_SUCCESSFUL")) {
            m_poll->stop();
            const QString summary = task.value(QStringLiteral("output")).toObject()
                                        .value(QStringLiteral("output")).toString();
            if (summary.isEmpty()) {
                fail(tr("The summary came back empty."));
                return;
            }
            m_text->setPlainText(summary);
            m_status->setText(tr("Written by the server's AI — check anything important."));
            return;
        }
        if (status == QLatin1String("STATUS_FAILED")
            || status == QLatin1String("STATUS_CANCELLED")) {
            fail(tr("The server could not produce a summary."));
        }
    });
}
