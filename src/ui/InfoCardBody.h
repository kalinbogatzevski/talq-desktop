#pragma once

#include "painter/PainterTheme.h"

#include <QUrl>
#include <QVector>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QVBoxLayout;

/**
 * The part of a card the SERVER describes: a title, a subtitle, badges, an
 * ordered list of label/value rows, and a set of action buttons.
 *
 * It is a DUMB RENDERER. It does not know what a "balance", a "contract" or a
 * "job title" is, and it does no formatting: every value is a display-ready
 * string the server produced. That is the point of the design -- adding a
 * field, or showing different fields to different roles, becomes a server-side
 * change deployable in minutes, and a deployment with a different backend
 * defines an entirely different card without touching this code.
 *
 * Three things are NOT the server's to choose:
 *  - colour: `style` is a closed vocabulary mapped to AA-validated theme
 *    pairs, so a card can never violate the contrast guarantees;
 *  - what may be opened: this widget only EMITS a url, so one owner validates
 *    the scheme in one place and the card never opens anything itself;
 *  - how many rows fit: capped, so twenty contracts cannot produce a card
 *    taller than the screen.
 *
 * It is a plain child widget rather than a base class on purpose. The caller
 * card is a Qt::Tool that must never take focus, because a call arriving
 * mid-sentence must not eat a keystroke; the person card is a focusable
 * QDialog, because the user asked for it and expects Esc to close it. Those
 * cannot share a base class, but they can both embed this.
 */
class InfoCardBody : public QWidget
{
    Q_OBJECT

public:
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

    /** Everything the server said. */
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

    explicit InfoCardBody(QWidget *parent = nullptr);

    // Renders badges, fields and server actions. Title and subtitle are taken
    // from the card only when it supplies them, so an owner that has already
    // set a local headline (a colleague's name, a bare phone number) keeps it
    // when a card arrives without one.
    void setCard(const CardData &card);
    const CardData &card() const { return m_card; }

    // The width the owner will give this widget, in px. Field values are laid
    // out against it to work out how tall each row must be; without it the
    // rows are sized during the first layout pass, when the widget is still
    // 0 px wide, and every multi-line value ends up a line short.
    void setValueWidthHint(int px);

    void setTitleText(const QString &text);
    void setSubtitleText(const QString &text);

    void setTheme(PainterTheme::Theme theme);

    // Owners insert their own local controls here (Dismiss, Call back,
    // Message). Ordering stays with the owner, which is why the row is
    // exposed rather than the owner handing buttons in.
    QHBoxLayout *actionRow() const;

    // Ink for a wire style, so an owner styling its own labels matches.
    QColor inkForStyle(const QString &style) const;

signals:
    // Carries the URL rather than an index, so the owner validates the scheme
    // in one place and this widget never opens anything itself.
    void openRequested(const QUrl &url);

protected:
    // A click anywhere in a value selects the WHOLE value, rather than
    // dropping a caret where the pointer landed. These are addresses and
    // numbers people want in one piece; part of an email is never useful.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuildBody();       // badges + fields, from m_card
    void rebuildActions();    // one button per server action
    void applyChrome();       // title/subtitle fonts and inks
    void clearLayout(QVBoxLayout *layout);

    CardData m_card;

    QLabel *m_title = nullptr;
    QLabel *m_subtitle = nullptr;
    QWidget *m_badgeRow = nullptr;
    QVBoxLayout *m_fieldsLayout = nullptr;
    QLabel *m_more = nullptr;        // "+3 more" when the list is capped
    int m_widthHint = 0;
    QWidget *m_actionRow = nullptr;

    PainterTheme::Theme m_theme = PainterTheme::Theme::Vivid;
};
