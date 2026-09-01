#include "CallerCardPopup.h"

#include "core/CtiEventLogic.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

// Wide enough for a company name and a "Balance due  R 1 234.56" row without
// wrapping. Height is NOT fixed -- the card grows with whatever the server
// sends, which is the whole point of the design.
constexpr int kCardWidth = 400;

} // namespace

CallerCardPopup::CallerCardPopup(QWidget *parent)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                      | Qt::WindowDoesNotAcceptFocus | Qt::Tool)
{
    Q_UNUSED(parent);

    // WA_ShowWithoutActivating plus WindowDoesNotAcceptFocus is what keeps the
    // agent's cursor in the message they were typing. Without both, a call
    // arriving mid-sentence eats a keystroke or two.
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFixedWidth(kCardWidth);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 14);
    root->setSpacing(4);

    m_status = new QLabel(this);
    m_status->setAttribute(Qt::WA_TranslucentBackground);
    m_status->setTextFormat(Qt::PlainText);
    root->addWidget(m_status);

    m_body = new InfoCardBody(this);
    // Same fixed-width arithmetic as the person card: root's margins are 20
    // either side, so that is what a value row has to lay out against.
    m_body->setValueWidthHint(kCardWidth - 40);
    // The body only ever emits a url; this class re-emits it with the call id
    // attached, so the owner still validates the scheme in exactly one place.
    connect(m_body, &InfoCardBody::openRequested, this, [this](const QUrl &url) {
        emit openRequested(m_callId, url);
    });
    root->addWidget(m_body);

    m_dismissButton = new QPushButton(tr("Dismiss"), m_body);
    // The app's bare QPushButton is transparent and borderless by design --
    // the filled/outlined looks are OPT-IN via the `variant` property (see
    // AppStyle). Without one, these read as plain text rather than controls,
    // which is exactly how they first shipped.
    m_dismissButton->setProperty("variant", "ghost");
    m_dismissButton->setCursor(Qt::PointingHandCursor);
    m_dismissButton->setFocusPolicy(Qt::NoFocus);
    connect(m_dismissButton, &QPushButton::clicked, this, [this]() {
        emit dismissed(m_callId);
    });
    m_body->actionRow()->addWidget(m_dismissButton);

    applyChrome();
}

void CallerCardPopup::showForCall(const QString &callId, const QString &callerNumber,
                                  const QPoint &position)
{
    m_callId = callId;
    m_callerNumber = callerNumber;

    // Shown before the lookup returns, deliberately: the phone is ringing now.
    m_body->setTitleText(QString::fromStdString(
        talq::fallbackCardTitle(callerNumber.toStdString())));
    m_body->setSubtitleText(tr("Looking up…"));
    m_body->setCard(CardData());
    rebuildActions();

    setState(State::Ringing);
    resizeToContent();
    move(position);
    show();
    raise();
}

void CallerCardPopup::applyCard(const CardData &card)
{
    m_body->setTitleText(!card.title.isEmpty()
                             ? card.title
                             : QString::fromStdString(
                                   talq::fallbackCardTitle(m_callerNumber.toStdString())));
    m_body->setSubtitleText(card.subtitle);
    m_body->setCard(card);

    rebuildActions();
    applyChrome();
    resizeToContent();
}

void CallerCardPopup::applyUnknownCaller()
{
    CardData card;
    card.known = false;
    card.title = QString::fromStdString(
        talq::fallbackCardTitle(m_callerNumber.toStdString()));
    card.subtitle = tr("Not a known customer");
    applyCard(card);
}

void CallerCardPopup::setCanDial(bool canDial)
{
    if (m_canDial == canDial)
        return;
    m_canDial = canDial;
    rebuildActions();
    resizeToContent();
}

void CallerCardPopup::rebuildActions()
{
    auto *row = m_body->actionRow();
    if (!row)
        return;

    // Call back, before the server's own actions. Offered only where the
    // daemon said dialling is configured AND there is a number to dial: a
    // withheld number produces a card with nothing to call.
    //
    // It leads because of WHEN this card is on screen. On a missed call it is
    // the thing the agent wants; on a ringing one it is the thing they must
    // not hit by reflex, which is why it is a ghost button next to the filled
    // primary actions rather than competing with them.
    const bool wantCall = m_canDial && !m_callerNumber.trimmed().isEmpty();
    if (!wantCall) {
        if (m_callButton) {
            row->removeWidget(m_callButton);
            m_callButton->deleteLater();
            m_callButton = nullptr;
        }
        return;
    }
    if (m_callButton)
        return;

    m_callButton = new QPushButton(tr("Call back"), m_body);
    m_callButton->setProperty("variant", "ghost");
    m_callButton->setCursor(Qt::PointingHandCursor);
    m_callButton->setFocusPolicy(Qt::NoFocus);
    connect(m_callButton, &QPushButton::clicked, this, [this]() {
        emit dialRequested(m_callerNumber);
    });
    // Directly after Dismiss, which is where it has always sat: the row leads
    // with a stretch, so inserting at 0 would put this flush against the card's
    // left edge, detached from the other controls.
    row->insertWidget(row->indexOf(m_dismissButton) + 1, m_callButton);
}

void CallerCardPopup::resizeToContent()
{
    // Width is fixed; height follows whatever the server sent. adjustSize()
    // after a rebuild is what lets the card grow for a rich customer and stay
    // compact for a bare number.
    layout()->activate();
    adjustSize();
    setFixedWidth(kCardWidth);
}

void CallerCardPopup::setState(State state)
{
    m_state = state;
    switch (state) {
    case State::Ringing: m_status->setText(tr("Incoming call")); break;
    case State::Active:  m_status->setText(tr("On this call"));  break;
    case State::Missed:  m_status->setText(tr("Missed call"));   break;
    }
    applyChrome();
}

void CallerCardPopup::setTheme(PainterTheme::Theme theme)
{
    m_theme = theme;
    m_body->setTheme(theme);
    applyChrome();
    resizeToContent();
    update();
}

void CallerCardPopup::applyChrome()
{
    const PainterTheme th(m_theme, 1.0);

    if (m_status) {
        QFont f = m_status->font();
        f.setBold(true);
        f.setPointSize(th.fontSizeTiny);
        m_status->setFont(f);
        m_status->setStyleSheet(
            QStringLiteral("color: %1; background: transparent;")
                .arg((m_state == State::Missed ? th.danger : th.accent).name()));
    }
}

void CallerCardPopup::changeEvent(QEvent *event)
{
    // The app sets a new QApplication palette on a live theme switch; re-tint
    // rather than leaving a dark card on a light surface.
    if (event->type() == QEvent::PaletteChange)
        applyChrome();
    QWidget::changeEvent(event);
}

void CallerCardPopup::paintEvent(QPaintEvent *)
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

    // A ringing card gets the accent edge so it reads as live; once the call
    // is over it drops back to the ordinary divider so it stops shouting.
    const QColor border = (m_state == State::Ringing) ? th.accent : th.divider;
    p.setPen(QPen(border, m_state == State::Ringing ? 2 : 1));
    p.drawRoundedRect(bgRect, PainterTheme::radiusCard, PainterTheme::radiusCard);
}
