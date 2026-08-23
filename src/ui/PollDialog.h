#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QList>
#include <QStringList>

class ApiClient;
class QAbstractButton;
class QButtonGroup;
class QLabel;
class QLineEdit;
class QVBoxLayout;
class QPushButton;

/**
 * View a poll, vote in it, and see the result.
 *
 * WHY A DIALOG AND NOT AN IN-BUBBLE SURFACE. A poll is variable-height,
 * multi-option and changes state when anyone votes. The chat is laid out once
 * and scrolled, so a bubble that grew or shrank on a vote would reflow history
 * underneath whoever was reading it; and painting option rows with hover and
 * hit-testing into the bubble is precisely the shape that produced the
 * reaction-pill hit-test drift this codebase already had to fix once (see
 * painter/ReactionLayout.h). The chat therefore paints a fixed-height card
 * (ChatPainter::paintPollCard) and everything interactive happens here, where
 * ordinary widgets handle layout, focus and keyboard for free.
 *
 * Result modes (Talk `resultMode`): 0 = results visible to everyone, 1 = hidden
 * until the poll closes. The server simply omits `votes`/`numVoters` in mode 1
 * while the poll is open, so "no counts" is the normal case, not an error.
 */
class PollDialog : public QDialog
{
    Q_OBJECT

public:
    PollDialog(ApiClient *api, const QString &token, int pollId, QWidget *parent = nullptr);

private:
    void fetchPoll();
    void applyPoll(const QJsonObject &poll);
    void submitVote();
    void closePoll();

    ApiClient  *m_api;
    QString     m_token;
    int         m_pollId;

    QLabel       *m_question = nullptr;
    QLabel       *m_status   = nullptr;
    QVBoxLayout  *m_optionsLayout = nullptr;
    QList<QAbstractButton *> m_optionButtons;   // check boxes or radio buttons
    QPushButton  *m_voteBtn  = nullptr;
    QPushButton  *m_closeBtn = nullptr;

    // 0 = open, 1 = closed, 2 = draft. A closed poll is read-only.
    int  m_pollStatus = 0;
    // 0 means unlimited; 1 makes the options mutually exclusive.
    int  m_maxVotes = 0;
    bool m_isAuthor = false;
};

/**
 * Compose a new poll: a question and two or more options.
 *
 * ⚠ The server REFUSES a poll in a one-to-one conversation with
 * 400 {"error":"room"} (PollController.php:96) — only group and public
 * conversations may have one. The caller must not offer this there; handling
 * the error after the fact would mean letting someone write a poll and only
 * then telling them it was never possible.
 */
class PollComposerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PollComposerDialog(QWidget *parent = nullptr);

    QString     question() const;
    QStringList options() const;   // trimmed, empties dropped
    int         resultMode() const;
    int         maxVotes() const;
    // Save as a reusable draft instead of posting the poll now.
    bool        saveAsDraft() const;
    // Offer the draft option only where the server supports drafts.
    void        setDraftsAvailable(bool v);

private:
    void addOptionRow();

    QLineEdit          *m_question = nullptr;
    QVBoxLayout        *m_optionsLayout = nullptr;
    QList<QLineEdit *>  m_optionEdits;
    QAbstractButton    *m_hideResults = nullptr;
    QAbstractButton    *m_multiChoice = nullptr;
    QAbstractButton    *m_asDraft = nullptr;
};
