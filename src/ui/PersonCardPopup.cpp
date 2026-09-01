#include "PersonCardPopup.h"

#include "core/PersonCardLogic.h"
#include "core/ShiftStatusService.h"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

namespace {

// Narrower than the caller card (400): that one carries account numbers and
// currency rows, this one carries a name and a handful of short facts.
constexpr int kCardWidth = 340;
constexpr int kAvatarPx  = 56;

} // namespace

// A status chip: an outlined pill that leads with a PAINTED ROUNDED SQUARE.
//
// The marker is painted, never a "•" glyph -- the glyph's size and baseline
// vary by font and it renders muddy at small sizes. And every state takes a
// colour: an earlier build made on-shift a neutral grey so it would not shout,
// which left the chip unreadable as a status at all.
//
// SQUARE, not a disc. A disc is the presence token everywhere else in the app
// (SidebarPainter's bottom-right dot, the header status line, the sidebar's
// own-presence row), and `online` and `success` are literally the same green in
// every theme -- so a green disc beside a green word reads as presence no
// matter what the word says. SidebarPainter settled this for the conversation
// list by giving shift the bottom-LEFT corner and a rounded square; the header
// pill and this one now use the same shape, so one mark means one thing
// app-wide.
//
// The OUTLINE is why this is a widget rather than a styled label. On a card the
// chip sits directly under the presence line, and as plain text at the same
// size and weight the two melted into one paragraph -- you could not see where
// "Online" stopped and "On shift" began. A container says "this is a status",
// not "this is another sentence". It stays an outline rather than a fill so the
// card keeps one loud thing at most, and so the label's ink remains
// textPrimary-on-bgSurface, a pair the conformance suite already scores.
class ShiftChipLabel : public QLabel
{
public:
    explicit ShiftChipLabel(QWidget *parent) : QLabel(parent)
    {
        // Room for the disc on the left and breathing space inside the pill.
        setContentsMargins(kDiscZone, 3, 10, 3);
        setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    }

    void setAccent(const QColor &c) { m_accent = c; update(); }

protected:
    void paintEvent(QPaintEvent *e) override
    {
        if (m_accent.isValid() && !text().isEmpty()) {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing);

            const QRectF pill = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(m_accent, 1.0));
            p.drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);

            // Radius is a QUARTER of the side, matching SidebarPainter's 10px
            // marker at 2.5px. A fixed radius rounds a marker this small back
            // into a disc, which is the presence token this must not be.
            const qreal d = qMax(6.0, height() * 0.34);
            p.setPen(Qt::NoPen);
            p.setBrush(m_accent);
            p.drawRoundedRect(QRectF(kDiscZone / 2.0 - d / 2.0, height() / 2.0 - d / 2.0,
                                     d, d), d * 0.25, d * 0.25);
        }
        QLabel::paintEvent(e);
    }

private:
    // Left inset the disc is centred in, and where the text therefore starts.
    static constexpr int kDiscZone = 18;
    QColor m_accent;
};

PersonCardPopup::PersonCardPopup(QWidget *parent)
    : QDialog(parent)
{
    // A frameless QDialog: the one hybrid shape in this tree that both takes
    // focus and paints its own chrome. StatusPopover established it.
    setWindowFlags(Qt::FramelessWindowHint | Qt::Dialog);
    setObjectName("personCard");
    setFixedWidth(kCardWidth);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 14);
    root->setSpacing(4);

    // ── identity row: the layer that needs no backend ───────────────────────
    auto *idRow = new QHBoxLayout;
    idRow->setSpacing(12);

    m_avatar = new QLabel(this);
    m_avatar->setFixedSize(kAvatarPx, kAvatarPx);
    m_avatar->setAttribute(Qt::WA_TranslucentBackground);
    idRow->addWidget(m_avatar, 0, Qt::AlignTop);

    auto *idCol = new QVBoxLayout;
    idCol->setSpacing(2);

    m_name = new QLabel(this);
    m_name->setAttribute(Qt::WA_TranslucentBackground);
    m_name->setTextFormat(Qt::PlainText);
    m_name->setWordWrap(true);
    idCol->addWidget(m_name);

    m_presence = new QLabel(this);
    m_presence->setAttribute(Qt::WA_TranslucentBackground);
    m_presence->setTextFormat(Qt::PlainText);
    m_presence->setWordWrap(true);
    m_presence->setVisible(false);
    idCol->addWidget(m_presence);

    m_shift = new ShiftChipLabel(this);
    m_shift->setAttribute(Qt::WA_TranslucentBackground);
    m_shift->setTextFormat(Qt::PlainText);
    m_shift->setVisible(false);
    // Its own row with a trailing stretch: a pill that spans the card's width
    // is a banner, not a chip. The leading gap separates it from the presence
    // line above -- the two are different kinds of fact and should not touch.
    idCol->addSpacing(4);
    auto *shiftRow = new QHBoxLayout;
    shiftRow->setContentsMargins(0, 0, 0, 0);
    shiftRow->addWidget(m_shift);
    shiftRow->addStretch(1);
    idCol->addLayout(shiftRow);

    idCol->addStretch(1);
    idRow->addLayout(idCol, 1);
    root->addLayout(idRow);

    // ── the layer the server describes ──────────────────────────────────────
    m_body = new InfoCardBody(this);
    connect(m_body, &InfoCardBody::openRequested,
            this, &PersonCardPopup::openRequested);
    root->addWidget(m_body);

    auto *msg = new QPushButton(tr("Message"), m_body);
    // Variants are opt-in; a bare QPushButton in this app is transparent and
    // borderless and would read as plain text.
    msg->setProperty("variant", "ghost");
    msg->setCursor(Qt::PointingHandCursor);
    connect(msg, &QPushButton::clicked, this, [this]() {
        emit messageRequested(m_actorId);
        close();
    });
    m_body->actionRow()->insertWidget(0, msg);

    applyChrome();
}

void PersonCardPopup::setShiftStatus(ShiftStatusService *svc)
{
    if (m_shiftStatus == svc)
        return;
    m_shiftStatus = svc;
    if (!m_shiftStatus)
        return;
    // Without this the chip shows whatever was cached when the card opened,
    // and a first-ever lookup never appears at all: the service has a single
    // in-flight flag and a poll interval equal to its TTL, so a person
    // observed while another request is out may not be fetched for two
    // minutes. ConversationInfoDialog shipped with a comment claiming it
    // re-renders and no such connection; do not repeat that.
    connect(m_shiftStatus, &ShiftStatusService::statusesChanged, this, [this]() {
        if (!isVisible())
            return;
        refreshShiftChip();
        resizeToContent();
    });
}

void PersonCardPopup::showForPerson(const QString &actorId,
                                    const QString &displayName,
                                    const QRect &anchorGlobal)
{
    m_actorId = actorId;
    m_displayName = displayName;

    m_name->setText(QString::fromStdString(
        talq::personCardTitle(displayName.toStdString(), actorId.toStdString())));

    // Wipe the previous occupant before anything async lands. Without this a
    // slow reply for person A paints onto person B's card, which is worse than
    // showing nothing because it is confidently wrong.
    m_presence->clear();
    m_presence->setVisible(false);
    m_avatarPixmap = QPixmap();
    m_body->setCard(InfoCardBody::CardData());
    m_body->setTitleText(QString());
    m_body->setSubtitleText(QString());

    if (m_shiftStatus && !actorId.isEmpty())
        m_shiftStatus->observe({actorId});   // NOT paint-safe; a click is fine
    refreshShiftChip();
    refreshAvatar();

    // A bot's id carries a "bots/" prefix and would be a junk lookup made with
    // the user's own credential. The card still opens -- just without a server
    // layer.
    if (talq::personCardEligibleForErp(actorId.toStdString()))
        emit cardNeeded(actorId);
    if (!actorId.isEmpty())
        emit avatarNeeded(actorId);

    resizeToContent();

    // Positioning, as StatusPopover does it: prefer below the anchor, flip
    // above when that would run off the screen the anchor is actually on, and
    // clamp into the available geometry so a card near an edge is never
    // half-off it.
    QScreen *scr = QGuiApplication::screenAt(anchorGlobal.center());
    if (!scr) scr = QGuiApplication::primaryScreen();
    const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1920, 1080);

    int x = anchorGlobal.left();
    int y = anchorGlobal.bottom() + 6;
    if (y + height() > avail.bottom())
        y = anchorGlobal.top() - height() - 6;
    x = qBound(avail.left() + 4, x, avail.right() - width() - 4);
    y = qBound(avail.top() + 4, y, avail.bottom() - height() - 4);

    move(x, y);
    show();
    raise();
    activateWindow();
}

void PersonCardPopup::setPresence(const QString &state, const QString &message)
{
    const QString line = QString::fromStdString(
        talq::personPresenceLine(state.toStdString(), message.toStdString()));
    m_presence->setText(line);
    m_presence->setVisible(!line.isEmpty());
    resizeToContent();
}

void PersonCardPopup::setAvatar(const QPixmap &pixmap)
{
    m_avatarPixmap = pixmap;
    refreshAvatar();
}

void PersonCardPopup::setCard(const InfoCardBody::CardData &card)
{
    m_body->setCard(card);
    resizeToContent();
}

void PersonCardPopup::setTheme(PainterTheme::Theme theme)
{
    m_theme = theme;
    m_body->setTheme(theme);
    applyChrome();
    refreshShiftChip();
    refreshAvatar();
    resizeToContent();
    update();
}

void PersonCardPopup::applyChrome()
{
    const PainterTheme th(m_theme, 1.0);

    auto styleLabel = [](QLabel *l, const QColor &c, int pt, bool bold) {
        if (!l) return;
        QFont f = l->font();
        f.setBold(bold);
        f.setPointSize(pt);
        l->setFont(f);
        l->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                             .arg(c.name()));
    };

    styleLabel(m_name,     th.textPrimary,   th.fontSizeLarge, true);
    styleLabel(m_presence, th.textSecondary, th.fontSizeSmall, false);
    // Tiny and bold, the same type as the caller card's badges -- a chip reads
    // as a chip because of its type as much as its outline. Matching the
    // presence line's size and weight is what made the two melt together.
    styleLabel(m_shift,    th.textPrimary,   th.fontSizeTiny,  true);
}

void PersonCardPopup::refreshShiftChip()
{
    const PainterTheme th(m_theme, 1.0);

    talq::ShiftState st = talq::ShiftState::Unknown;
    QString serverLabel;
    if (m_shiftStatus && !m_actorId.isEmpty()) {
        st = m_shiftStatus->stateFor(m_actorId);
        serverLabel = m_shiftStatus->labelFor(m_actorId);
    }

    const QString text = QString::fromStdString(
        talq::personShiftChipText(st, serverLabel.toStdString()));
    const bool draw = talq::personShiftChipIsDrawable(st) && !text.isEmpty();

    // Unknown HIDES the chip rather than reserving space for it: the card just
    // gets shorter. Drawing a placeholder would break the rule the whole
    // feature rests on -- Unknown must stay indistinguishable from "refused",
    // "timed out" and "never asked".
    m_shift->setVisible(draw);
    if (!draw)
        return;

    QColor accent;
    switch (st) {
    case talq::ShiftState::OnBreak:  accent = th.amber;     break;
    case talq::ShiftState::OffShift: accent = th.textMuted; break;
    default:                         accent = th.online;    break;
    }
    m_shift->setAccent(accent);
    m_shift->setText(text);   // the pill's own margins make room for the disc
}

void PersonCardPopup::refreshAvatar()
{
    if (!m_avatarPixmap.isNull()) {
        m_avatar->setPixmap(m_avatarPixmap);
        return;
    }

    // The identity hue as an avatar FILL, with inkOn() picking the ink -- the
    // same fallback the chat draws when an avatar image has not arrived, so a
    // person looks the same in both places.
    const PainterTheme th(m_theme, 1.0);
    const QColor fill = PainterTheme::authorColor(m_actorId);

    const qreal dpr = devicePixelRatioF();
    QPixmap pm(QSize(kAvatarPx, kAvatarPx) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawEllipse(QRectF(0, 0, kAvatarPx, kAvatarPx));

    QFont letterFont;
    letterFont.setPixelSize(22);
    letterFont.setWeight(QFont::DemiBold);
    p.setFont(letterFont);
    p.setPen(th.inkOn(fill));   // scored against the fill, never assumed
    const QString source = !m_displayName.trimmed().isEmpty() ? m_displayName.trimmed()
                                                              : m_actorId.trimmed();
    p.drawText(QRectF(0, 0, kAvatarPx, kAvatarPx), Qt::AlignCenter,
               source.isEmpty() ? QStringLiteral("?") : source.left(1).toUpper());
    p.end();

    m_avatar->setPixmap(pm);
}

void PersonCardPopup::resizeToContent()
{
    // Width is fixed; height follows whatever arrived. A card with only its
    // identity layer stays compact instead of leaving room for absent rows.
    layout()->activate();
    adjustSize();
    setFixedWidth(kCardWidth);
}

bool PersonCardPopup::event(QEvent *e)
{
    if (e->type() == QEvent::WindowDeactivate && isVisible())
        close();   // click-away dismissal, like a real dropdown
    return QDialog::event(e);
}

void PersonCardPopup::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QDialog::keyPressEvent(e);
}

void PersonCardPopup::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF shadowRect = rect().adjusted(2, 3, -2, -1);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 70));
    p.drawRoundedRect(shadowRect.adjusted(2, 2, 2, 2), 14, 14);

    const PainterTheme th(m_theme, 1.0);
    const QRectF bgRect = rect().adjusted(4, 4, -4, -4);
    p.setBrush(th.bgSurface);
    // Always the ordinary divider. There is no "live" state here -- nothing is
    // ringing, the user just asked a question.
    p.setPen(QPen(th.divider, 1));
    p.drawRoundedRect(bgRect, PainterTheme::radiusCard, PainterTheme::radiusCard);
}
