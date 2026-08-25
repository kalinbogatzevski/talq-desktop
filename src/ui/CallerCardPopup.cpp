#include "CallerCardPopup.h"

#include "core/CtiEventLogic.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

// Wide enough for a company name without wrapping every time, tall enough for
// a name, a company line, badges and two buttons.
constexpr int kCardWidth  = 400;
constexpr int kCardHeight = 168;

} // namespace

CallerCardPopup::CallerCardPopup(QWidget *parent)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                      | Qt::WindowDoesNotAcceptFocus | Qt::Tool)
{
    Q_UNUSED(parent);

    // WA_ShowWithoutActivating plus WindowDoesNotAcceptFocus is what keeps the
    // agent's cursor in the message they were typing. Without both, a call
    // during composition eats a keystroke or two.
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFixedSize(kCardWidth, kCardHeight);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 14);
    root->setSpacing(4);

    m_status = new QLabel(this);
    m_status->setAttribute(Qt::WA_TranslucentBackground);
    m_status->setTextFormat(Qt::PlainText);
    root->addWidget(m_status);

    m_title = new QLabel(this);
    m_title->setAttribute(Qt::WA_TranslucentBackground);
    m_title->setTextFormat(Qt::PlainText);
    m_title->setWordWrap(false);
    root->addWidget(m_title);

    m_subtitle = new QLabel(this);
    m_subtitle->setAttribute(Qt::WA_TranslucentBackground);
    m_subtitle->setTextFormat(Qt::PlainText);
    m_subtitle->setWordWrap(false);
    root->addWidget(m_subtitle);

    auto *badgeRow = new QHBoxLayout();
    badgeRow->setContentsMargins(0, 4, 0, 0);
    badgeRow->setSpacing(8);

    m_outage = new QLabel(this);
    m_outage->setAttribute(Qt::WA_TranslucentBackground);
    m_outage->setTextFormat(Qt::PlainText);
    m_outage->setVisible(false);
    badgeRow->addWidget(m_outage);

    m_badges = new QLabel(this);
    m_badges->setAttribute(Qt::WA_TranslucentBackground);
    m_badges->setTextFormat(Qt::PlainText);
    badgeRow->addWidget(m_badges);
    badgeRow->addStretch(1);
    root->addLayout(badgeRow);

    root->addStretch(1);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(8);
    buttonRow->addStretch(1);

    m_dismissButton = new QPushButton(tr("Dismiss"), this);
    m_dismissButton->setFlat(true);
    m_dismissButton->setCursor(Qt::PointingHandCursor);
    m_dismissButton->setFocusPolicy(Qt::NoFocus);
    connect(m_dismissButton, &QPushButton::clicked, this, [this]() {
        emit dismissed(m_callId);
    });
    buttonRow->addWidget(m_dismissButton);

    m_openButton = new QPushButton(tr("Open customer"), this);
    m_openButton->setCursor(Qt::PointingHandCursor);
    m_openButton->setFocusPolicy(Qt::NoFocus);
    // Nothing to open until the lookup supplies a URL. Showing a button that
    // does nothing is worse than showing none.
    m_openButton->setEnabled(false);
    connect(m_openButton, &QPushButton::clicked, this, [this]() {
        emit openRequested(m_callId);
    });
    buttonRow->addWidget(m_openButton);

    root->addLayout(buttonRow);

    applyChrome();
}

void CallerCardPopup::showForCall(const QString &callId, const QString &callerNumber,
                                  const QPoint &position)
{
    m_callId = callId;
    m_callerNumber = callerNumber;
    m_openUrl = QUrl();
    m_openButton->setEnabled(false);
    m_outage->setVisible(false);
    m_badges->clear();

    // Shown before the lookup returns, deliberately: the phone is ringing now.
    m_title->setText(QString::fromStdString(
        talq::fallbackCardTitle(callerNumber.toStdString())));
    m_subtitle->setText(tr("Looking up…"));

    setState(State::Ringing);
    move(position);
    show();
    raise();
}

void CallerCardPopup::applyCustomer(const QString &displayName, const QString &company,
                                    const QString &ucn, bool isOutage,
                                    int contractCount, int openTicketCount,
                                    const QUrl &openUrl)
{
    if (!displayName.isEmpty())
        m_title->setText(displayName);

    QStringList sub;
    if (!company.isEmpty()) sub << company;
    if (!ucn.isEmpty())     sub << ucn;
    if (!m_callerNumber.isEmpty() && !displayName.isEmpty())
        sub << m_callerNumber;
    m_subtitle->setText(sub.join(QStringLiteral("  ·  ")));

    QStringList badges;
    if (contractCount > 0)
        badges << tr("%n contract(s)", nullptr, contractCount);
    if (openTicketCount > 0)
        badges << tr("%n open ticket(s)", nullptr, openTicketCount);
    m_badges->setText(badges.join(QStringLiteral("  ·  ")));

    // The one thing an agent most needs before speaking: this customer is
    // already affected by a known outage.
    m_outage->setText(tr(" OUTAGE "));
    m_outage->setVisible(isOutage);

    m_openUrl = openUrl;
    m_openButton->setEnabled(openUrl.isValid());

    applyChrome();
}

void CallerCardPopup::applyUnknownCaller()
{
    m_title->setText(QString::fromStdString(
        talq::fallbackCardTitle(m_callerNumber.toStdString())));
    m_subtitle->setText(tr("Not a known customer"));
    m_badges->clear();
    m_outage->setVisible(false);
    m_openButton->setEnabled(false);
}

void CallerCardPopup::setState(State state)
{
    m_state = state;
    switch (state) {
    case State::Ringing:
        m_status->setText(tr("Incoming call"));
        break;
    case State::Active:
        m_status->setText(tr("On this call"));
        break;
    case State::Missed:
        m_status->setText(tr("Missed call"));
        break;
    }
    applyChrome();
}

void CallerCardPopup::setTheme(PainterTheme::Theme theme)
{
    m_theme = theme;
    applyChrome();
    update();
}

void CallerCardPopup::applyChrome()
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

    // The status line is the accent-as-text case, which the theme conformance
    // suite already scores against the card surface.
    styleLabel(m_status,   m_state == State::Missed ? th.danger : th.accent,
               th.fontSizeTiny,   true);
    styleLabel(m_title,    th.textPrimary,   th.fontSizeLarge,  true);
    styleLabel(m_subtitle, th.textSecondary, th.fontSizeSmall,  false);
    styleLabel(m_badges,   th.textSecondary, th.fontSizeSmall,  false);

    // Outage badge: danger fill with inkOn(danger) — an existing, AA-validated
    // pair, so this introduces no new colour combination to register.
    if (m_outage) {
        QFont f = m_outage->font();
        f.setBold(true);
        f.setPointSize(th.fontSizeTiny);
        m_outage->setFont(f);
        m_outage->setStyleSheet(
            QStringLiteral("color: %1; background: %2; border-radius: 4px;")
                .arg(th.inkOn(th.danger).name(), th.danger.name()));
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
