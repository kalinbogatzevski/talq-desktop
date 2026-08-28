#pragma once

#include "painter/PainterTheme.h"

#include <QUrl>
#include <QVector>
#include <QWidget>

class QLabel;
class QPushButton;
class QVBoxLayout;

/**
 * The card shown while a customer's call is ringing.
 *
 * It is a DUMB RENDERER. The server describes the whole card -- title,
 * subtitle, badges, an ordered list of label/value rows, and a set of actions
 * -- and this class draws whatever arrives. It does not know what a "balance"
 * or a "contract" is, and it does no formatting: every value is a
 * display-ready string the server produced.
 *
 * That is deliberate and is the point of the design. Adding a field, or
 * showing different fields to different roles, becomes a server-side change
 * deployable in minutes. Nothing here needs rebuilding or reshipping.
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

    struct Badge {
        QString text;
        QString style;   // normal | muted | warning | danger
    };
    struct Field {
        QString label;
        QString value;
        QString style;
    };
    struct Action {
        QString label;
        QUrl url;
    };

    /** Everything the server said about this caller. */
    struct CardData {
        bool known = false;
        QString title;
        QString subtitle;
        QVector<Badge> badges;
        QVector<Field> fields;
        QVector<Action> actions;
        // How many field rows the server wants shown. 0 = unspecified, in
        // which case the client's default applies. The client enforces a hard
        // ceiling regardless -- see talq::resolveFieldLimit.
        int maxFields = 0;
    };

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
    void rebuildBody();          // badges + fields, from m_card
    void rebuildActions();       // one button per action
    void clearLayout(QVBoxLayout *layout);
    QColor inkForStyle(const QString &style) const;
    void resizeToContent();

    QString m_callId;
    QString m_callerNumber;
    CardData m_card;

    QLabel *m_status = nullptr;      // "Incoming call" / "On this call" / "Missed"
    QLabel *m_title = nullptr;
    QLabel *m_subtitle = nullptr;
    QWidget *m_badgeRow = nullptr;
    QVBoxLayout *m_fieldsLayout = nullptr;
    QLabel *m_more = nullptr;        // "+3 more" when the list is capped
    QWidget *m_actionRow = nullptr;
    QPushButton *m_dismissButton = nullptr;

    bool m_canDial = false;
    State m_state = State::Ringing;
    PainterTheme::Theme m_theme = PainterTheme::Theme::Vivid;
};
