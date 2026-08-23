#include "PollDialog.h"

#include "core/ApiClient.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QVBoxLayout>

// ═══════════════════════════════════════════════════════
// PollDialog — view + vote
// ═══════════════════════════════════════════════════════

PollDialog::PollDialog(ApiClient *api, const QString &token, int pollId, QWidget *parent)
    : QDialog(parent)
    , m_api(api)
    , m_token(token)
    , m_pollId(pollId)
{
    setWindowTitle(tr("Poll"));
    setMinimumWidth(420);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 16);
    root->setSpacing(12);

    m_question = new QLabel(tr("Loading…"), this);
    m_question->setWordWrap(true);
    m_question->setProperty("role", "title");
    QFont qf = m_question->font();
    qf.setBold(true);
    qf.setPointSizeF(qf.pointSizeF() + 1.5);
    m_question->setFont(qf);
    root->addWidget(m_question);

    auto *optionsHost = new QWidget(this);
    m_optionsLayout = new QVBoxLayout(optionsHost);
    m_optionsLayout->setContentsMargins(0, 0, 0, 0);
    m_optionsLayout->setSpacing(6);
    root->addWidget(optionsHost);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setProperty("role", "secondary");
    root->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(this);
    m_voteBtn  = buttons->addButton(tr("Vote"), QDialogButtonBox::AcceptRole);
    m_closeBtn = buttons->addButton(tr("End poll"), QDialogButtonBox::DestructiveRole);
    buttons->addButton(QDialogButtonBox::Close);
    m_voteBtn->setEnabled(false);
    m_closeBtn->setVisible(false);
    root->addWidget(buttons);

    connect(m_voteBtn,  &QPushButton::clicked, this, &PollDialog::submitVote);
    connect(m_closeBtn, &QPushButton::clicked, this, &PollDialog::closePoll);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    fetchPoll();
}

void PollDialog::fetchPoll()
{
    m_api->fetchPoll(m_token, m_pollId, [this](bool ok, const QJsonObject &data, int status) {
        if (!ok) {
            // 404 means the poll is gone (the message may have been deleted);
            // anything else is a transport or permission problem. Say which,
            // rather than leaving the dialog on "Loading…" forever.
            m_question->setText(status == 404 ? tr("This poll no longer exists.")
                                              : tr("Could not load this poll."));
            m_status->setText(QString());
            return;
        }
        applyPoll(data);
    });
}

void PollDialog::applyPoll(const QJsonObject &poll)
{
    // Rebuild the option rows from scratch — voting changes counts, and a
    // partial update would be a second source of truth for the same state.
    qDeleteAll(m_optionButtons);
    m_optionButtons.clear();
    while (QLayoutItem *item = m_optionsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    m_question->setText(poll.value(QStringLiteral("question")).toString());
    m_pollStatus = poll.value(QStringLiteral("status")).toInt(0);
    m_maxVotes   = poll.value(QStringLiteral("maxVotes")).toInt(0);

    const QJsonArray options = poll.value(QStringLiteral("options")).toArray();
    const QJsonObject votes  = poll.value(QStringLiteral("votes")).toObject();
    const QJsonArray self    = poll.value(QStringLiteral("votedSelf")).toArray();
    const int numVoters      = poll.value(QStringLiteral("numVoters")).toInt(0);

    QList<int> mine;
    for (const QJsonValue &v : self) mine.append(v.toInt());

    // maxVotes == 1 means pick exactly one, which is a radio group; anything
    // else (including 0 = unlimited) allows multiple, which is check boxes.
    const bool single = (m_maxVotes == 1);
    QButtonGroup *group = single ? new QButtonGroup(this) : nullptr;
    if (group) group->setExclusive(true);

    const bool readOnly = (m_pollStatus != 0);

    for (int i = 0; i < options.size(); ++i) {
        const QString text = options.at(i).toString();
        // `votes` is keyed "option-<index>" and is ABSENT while a hidden-result
        // poll is still open — so a missing key means "not told yet", not zero.
        const QString key = QStringLiteral("option-") + QString::number(i);
        const bool haveCounts = votes.contains(key);
        const int count = votes.value(key).toInt(0);

        QString label = text;
        if (haveCounts)
            label += QStringLiteral("   —   ") + tr("%n vote(s)", nullptr, count);

        QAbstractButton *btn = single ? static_cast<QAbstractButton *>(new QRadioButton(label, this))
                                      : static_cast<QAbstractButton *>(new QCheckBox(label, this));
        btn->setChecked(mine.contains(i));
        btn->setEnabled(!readOnly);
        if (group) group->addButton(btn, i);
        m_optionsLayout->addWidget(btn);
        m_optionButtons.append(btn);

        connect(btn, &QAbstractButton::toggled, this, [this]() {
            if (m_pollStatus == 0) m_voteBtn->setEnabled(true);
        });
    }

    QStringList notes;
    if (m_pollStatus == 1) notes << tr("This poll has ended.");
    if (numVoters > 0)     notes << tr("%n voter(s) so far", nullptr, numVoters);
    if (m_pollStatus == 0 && votes.isEmpty())
        notes << tr("Results stay hidden until the poll ends.");
    if (!mine.isEmpty() && m_pollStatus == 0)
        notes << tr("You have voted — you can change your answer.");
    m_status->setText(notes.join(QStringLiteral("  ·  ")));

    m_voteBtn->setEnabled(false);
    m_voteBtn->setVisible(m_pollStatus == 0);
    // Only the author (or a moderator) may end a poll; the server enforces it,
    // and a 403 here would read as the app being broken, so the button only
    // appears once the response has told us the poll is still open.
    m_closeBtn->setVisible(m_pollStatus == 0);
}

void PollDialog::submitVote()
{
    QList<int> chosen;
    for (int i = 0; i < m_optionButtons.size(); ++i)
        if (m_optionButtons[i]->isChecked()) chosen.append(i);

    m_voteBtn->setEnabled(false);
    m_api->votePoll(m_token, m_pollId, chosen, [this](bool ok, const QJsonObject &data, int) {
        if (!ok) {
            m_status->setText(tr("Your vote was not accepted."));
            m_voteBtn->setEnabled(true);
            return;
        }
        // The vote response IS the updated poll, so re-render from it rather
        // than firing a second request.
        applyPoll(data);
    });
}

void PollDialog::closePoll()
{
    m_api->closePoll(m_token, m_pollId, [this](bool ok, const QJsonObject &data, int) {
        if (!ok) {
            m_status->setText(tr("Could not end this poll."));
            return;
        }
        applyPoll(data);
    });
}

// ═══════════════════════════════════════════════════════
// PollComposerDialog — create
// ═══════════════════════════════════════════════════════

PollComposerDialog::PollComposerDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("New poll"));
    setMinimumWidth(420);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 16);
    root->setSpacing(10);

    root->addWidget(new QLabel(tr("Question"), this));
    m_question = new QLineEdit(this);
    m_question->setPlaceholderText(tr("What do you want to ask?"));
    root->addWidget(m_question);

    root->addWidget(new QLabel(tr("Answers"), this));
    auto *optionsHost = new QWidget(this);
    m_optionsLayout = new QVBoxLayout(optionsHost);
    m_optionsLayout->setContentsMargins(0, 0, 0, 0);
    m_optionsLayout->setSpacing(6);
    root->addWidget(optionsHost);

    // Two rows to start: a poll with one answer is not a poll, and asking the
    // user to press "add" twice before they can type anything is friction for
    // no reason.
    addOptionRow();
    addOptionRow();

    auto *addBtn = new QPushButton(tr("Add answer"), this);
    addBtn->setFlat(true);
    connect(addBtn, &QPushButton::clicked, this, [this]() { addOptionRow(); });
    root->addWidget(addBtn);

    m_multiChoice = new QCheckBox(tr("Allow more than one answer"), this);
    m_hideResults = new QCheckBox(tr("Hide results until the poll ends"), this);
    root->addWidget(m_multiChoice);
    root->addWidget(m_hideResults);

    // Saving as a draft keeps the poll as a reusable template rather than
    // posting it. Hidden unless the server supports drafts — see
    // setDraftsAvailable, called by the owner once capabilities are known.
    m_asDraft = new QCheckBox(tr("Save as a draft instead of posting it"), this);
    m_asDraft->setVisible(false);
    root->addWidget(m_asDraft);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create poll"));
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // A poll needs a question and at least two answers, so Ok stays disabled
    // until it has them rather than letting the server refuse it.
    auto revalidate = [this, buttons]() {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(
            !question().isEmpty() && options().size() >= 2);
    };
    connect(m_question, &QLineEdit::textChanged, this, revalidate);
    for (QLineEdit *e : m_optionEdits)
        connect(e, &QLineEdit::textChanged, this, revalidate);
    revalidate();
}

void PollComposerDialog::addOptionRow()
{
    auto *e = new QLineEdit(this);
    e->setPlaceholderText(tr("Answer %1").arg(m_optionEdits.size() + 1));
    m_optionsLayout->addWidget(e);
    m_optionEdits.append(e);
    // Rows added after construction still have to drive the Ok-button rule.
    connect(e, &QLineEdit::textChanged, this, [this]() {
        if (auto *box = findChild<QDialogButtonBox *>())
            box->button(QDialogButtonBox::Ok)->setEnabled(
                !question().isEmpty() && options().size() >= 2);
    });
}

QString PollComposerDialog::question() const
{
    return m_question ? m_question->text().trimmed() : QString();
}

QStringList PollComposerDialog::options() const
{
    QStringList out;
    for (QLineEdit *e : m_optionEdits) {
        const QString t = e->text().trimmed();
        if (!t.isEmpty()) out << t;
    }
    return out;
}

// Talk: 0 = everyone sees results as they come in, 1 = hidden until it ends.
int PollComposerDialog::resultMode() const
{
    return (m_hideResults && m_hideResults->isChecked()) ? 1 : 0;
}

bool PollComposerDialog::saveAsDraft() const
{
    return m_asDraft && m_asDraft->isVisible() && m_asDraft->isChecked();
}

void PollComposerDialog::setDraftsAvailable(bool v)
{
    if (m_asDraft) m_asDraft->setVisible(v);
}

// 0 = unlimited picks; 1 = exactly one.
int PollComposerDialog::maxVotes() const
{
    return (m_multiChoice && m_multiChoice->isChecked()) ? 0 : 1;
}
