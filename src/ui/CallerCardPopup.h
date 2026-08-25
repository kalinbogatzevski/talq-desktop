#pragma once

#include "painter/PainterTheme.h"

#include <QUrl>
#include <QWidget>

class QLabel;
class QPushButton;

/**
 * The card shown while a customer's call is ringing.
 *
 * Three deliberate differences from a chat toast:
 *
 *  - It NEVER steals focus. The agent is probably mid-sentence in a message.
 *  - It is dismissed by CALL STATE, not by a timer. A card that disappears
 *    after a few seconds while the caller is still waiting is worse than no
 *    card, because by then the agent has come to rely on it.
 *  - It carries actions, because knowing who is calling is only half of it.
 */
class CallerCardPopup : public QWidget
{
    Q_OBJECT

public:
    enum class State {
        Ringing,   // the phone is ringing right now
        Active,    // this agent answered — the card stays until dismissed
        Missed,    // rang out; lingers briefly so it can be acted on
    };

    explicit CallerCardPopup(QWidget *parent = nullptr);

    // Called as soon as the ring arrives, before the ERP has answered. The
    // card appears immediately with the bare number rather than waiting on a
    // lookup that might be slow or might fail.
    void showForCall(const QString &callId, const QString &callerNumber,
                     const QPoint &position);

    // Fills in whatever the lookup returned. Safe to never arrive.
    void applyCustomer(const QString &displayName, const QString &company,
                       const QString &ucn, bool isOutage,
                       int contractCount, int openTicketCount,
                       const QUrl &openUrl);

    // The lookup failed or the caller is not a known customer. The card stays
    // up — this is exactly when the agent knows least.
    void applyUnknownCaller();

    void setState(State state);
    QString callId() const { return m_callId; }
    QUrl openUrl() const { return m_openUrl; }

    void setTheme(PainterTheme::Theme theme);

signals:
    void dismissed(const QString &callId);
    void openRequested(const QString &callId);

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void applyChrome();
    void relayoutBadges();

    QString m_callId;
    QString m_callerNumber;
    QUrl m_openUrl;

    QLabel *m_title = nullptr;      // customer name, or the bare number
    QLabel *m_subtitle = nullptr;   // company / UCN
    QLabel *m_status = nullptr;     // "Ringing on 131" / "On this call" / "Missed"
    QLabel *m_badges = nullptr;     // contracts, tickets
    QLabel *m_outage = nullptr;     // painted only when the customer is affected
    QPushButton *m_openButton = nullptr;
    QPushButton *m_dismissButton = nullptr;

    State m_state = State::Ringing;
    PainterTheme::Theme m_theme = PainterTheme::Theme::Vivid;
};
