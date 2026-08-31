#pragma once

#include "InfoCardBody.h"
#include "painter/PainterTheme.h"

#include <QUrl>
#include <QVector>
#include <QWidget>

class QLabel;
class QPushButton;

/**
 * The card shown while a customer's call is ringing.
 *
 * The server describes the card -- title, subtitle, badges, an ordered list of
 * label/value rows, and a set of actions -- and InfoCardBody draws whatever
 * arrives. This class owns only what is specific to a CALL: the state line,
 * the Dismiss and Call back controls, the chrome that goes accent-edged while
 * ringing, and the call id that every signal carries.
 *
 * Adding a field, or showing different fields to different roles, is therefore
 * a server-side change deployable in minutes. Nothing here needs rebuilding.
 *
 * Three behaviours that are NOT the server's to choose:
 *  - colour: `style` is a closed vocabulary mapped to AA-validated theme
 *    pairs, so a card can never violate the contrast guarantees;
 *  - what may be opened: only http/https, checked by the owner;
 *  - how many rows fit: capped, so twenty contracts cannot produce a card
 *    taller than the screen.
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

    // The card payload is shared with every other card surface, so it lives in
    // InfoCardBody. Aliased here so this class's API is unchanged by that.
    using Badge    = InfoCardBody::Badge;
    using Field    = InfoCardBody::Field;
    using Action   = InfoCardBody::Action;
    using CardData = InfoCardBody::CardData;

    explicit CallerCardPopup(QWidget *parent = nullptr);

    // Shown the moment the phone rings, before the lookup has answered. The
    // card appears immediately with the bare number rather than waiting on a
    // request that might be slow or might fail.
    void showForCall(const QString &callId, const QString &callerNumber,
                     const QPoint &position);

    // Fills the card in from whatever the server sent. Safe to never arrive.
    void applyCard(const CardData &card);

    // The lookup failed, or this caller is not a known customer. The card
    // stays up: that is exactly when the agent knows least.
    void applyUnknownCaller();

    void setState(State state);

    // Whether this site can place calls. The card is otherwise a dumb
    // renderer of what the server sent, and this is the one exception: the
    // ABILITY to dial is a property of this desktop and its daemon, not of
    // the customer record, so the server has nothing to say about it.
    void setCanDial(bool canDial);
    QString callId() const { return m_callId; }

    void setTheme(PainterTheme::Theme theme);

signals:
    void dismissed(const QString &callId);
    // Ring this desk phone and connect it to the caller.
    void dialRequested(const QString &number);
    // Carries the URL rather than an index, so the owner validates the scheme
    // in one place and the card never opens anything itself.
    void openRequested(const QString &callId, const QUrl &url);

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void applyChrome();
    void rebuildActions();       // the local Call back control
    void resizeToContent();

    QString m_callId;
    QString m_callerNumber;

    QLabel *m_status = nullptr;      // "Incoming call" / "On this call" / "Missed"
    InfoCardBody *m_body = nullptr;  // everything the server described
    QPushButton *m_dismissButton = nullptr;
    QPushButton *m_callButton = nullptr;

    bool m_canDial = false;
    State m_state = State::Ringing;
    PainterTheme::Theme m_theme = PainterTheme::Theme::Vivid;
};
