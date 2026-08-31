#pragma once

#include "InfoCardBody.h"
#include "painter/PainterTheme.h"

#include <QDialog>
#include <QPixmap>

class QLabel;
class ShiftChipLabel;
class ShiftStatusService;

/**
 * "Who is this, and are they actually working right now?"
 *
 * Opened by clicking a colleague's avatar. Presence and shift status answer
 * different questions and the interesting case is when they disagree: a green
 * dot at 22:00 does not mean anyone is going to reply.
 *
 * Three layers, each of which appears only if its source exists and each of
 * which is useful without the others:
 *
 *   1. identity  -- avatar, name, Nextcloud presence. Needs no backend at all,
 *                   so this layer is what an OSS user with no ERP still gets,
 *                   and it is designed as a first-class card rather than as a
 *                   fallback.
 *   2. shift     -- the chip, from ShiftStatusService. Client-owned colour, so
 *                   it is identical to the sidebar, header and call screen.
 *   3. server    -- whatever a card endpoint chose to send, rendered by
 *                   InfoCardBody. Job title, extension, anything: the client
 *                   hardcodes no notion of what a colleague record contains.
 *
 * Unlike CallerCardPopup this is a QDialog and it DOES take focus: the user
 * asked for it, so Esc and click-away must dismiss it. The caller card refuses
 * focus for the opposite reason -- an unprompted ring toast must not eat a
 * keystroke out of the message someone is typing.
 */
class PersonCardPopup : public QDialog
{
    Q_OBJECT

public:
    explicit PersonCardPopup(QWidget *parent = nullptr);

    void setShiftStatus(ShiftStatusService *svc);

    // Opens the card anchored to the avatar that was clicked. Everything
    // except the name is filled in as it arrives.
    void showForPerson(const QString &actorId, const QString &displayName,
                       const QRect &anchorGlobal);

    void setPresence(const QString &state, const QString &message);
    void setAvatar(const QPixmap &pixmap);
    void setCard(const InfoCardBody::CardData &card);
    void setTheme(PainterTheme::Theme theme);

    // Who this card is currently about. An async reply must check this before
    // painting: a slow answer for the person the user clicked a moment ago
    // must not land on whoever is on screen now.
    QString actorId() const { return m_actorId; }

signals:
    void openRequested(const QUrl &url);
    void messageRequested(const QString &actorId);
    // "I am open for this person and have no server card yet."
    void cardNeeded(const QString &actorId);
    // "I need a bigger avatar than the 36px the chat cache holds."
    void avatarNeeded(const QString &actorId);

protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void paintEvent(QPaintEvent *e) override;

private:
    void applyChrome();
    void refreshShiftChip();
    void refreshAvatar();
    void resizeToContent();

    QString m_actorId;
    QString m_displayName;
    QPixmap m_avatarPixmap;

    QLabel *m_avatar = nullptr;
    QLabel *m_name = nullptr;
    QLabel *m_presence = nullptr;
    ShiftChipLabel *m_shift = nullptr;
    InfoCardBody *m_body = nullptr;

    ShiftStatusService *m_shiftStatus = nullptr;
    PainterTheme::Theme m_theme = PainterTheme::Theme::Vivid;
};
