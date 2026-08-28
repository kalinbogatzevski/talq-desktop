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

    m_title = new QLabel(this);
    m_title->setAttribute(Qt::WA_TranslucentBackground);
    m_title->setTextFormat(Qt::PlainText);
    root->addWidget(m_title);

    m_subtitle = new QLabel(this);
    m_subtitle->setAttribute(Qt::WA_TranslucentBackground);
    m_subtitle->setTextFormat(Qt::PlainText);
    m_subtitle->setWordWrap(true);
    root->addWidget(m_subtitle);

    m_badgeRow = new QWidget(this);
    auto *badges = new QHBoxLayout(m_badgeRow);
    badges->setContentsMargins(0, 4, 0, 2);
    badges->setSpacing(6);
    badges->addStretch(1);
    root->addWidget(m_badgeRow);

    auto *fieldsHost = new QWidget(this);
    m_fieldsLayout = new QVBoxLayout(fieldsHost);
    m_fieldsLayout->setContentsMargins(0, 4, 0, 0);
    m_fieldsLayout->setSpacing(2);
    root->addWidget(fieldsHost);

    m_more = new QLabel(this);
    m_more->setAttribute(Qt::WA_TranslucentBackground);
    m_more->setTextFormat(Qt::PlainText);
    m_more->setVisible(false);
    root->addWidget(m_more);

    m_actionRow = new QWidget(this);
    auto *actions = new QHBoxLayout(m_actionRow);
    actions->setContentsMargins(0, 10, 0, 0);
    actions->setSpacing(8);
    actions->addStretch(1);

    m_dismissButton = new QPushButton(tr("Dismiss"), m_actionRow);
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
    actions->addWidget(m_dismissButton);
    root->addWidget(m_actionRow);

    applyChrome();
}

void CallerCardPopup::showForCall(const QString &callId, const QString &callerNumber,
                                  const QPoint &position)
{
    m_callId = callId;
    m_callerNumber = callerNumber;
    m_card = CardData();

    // Shown before the lookup returns, deliberately: the phone is ringing now.
    m_title->setText(QString::fromStdString(
        talq::fallbackCardTitle(callerNumber.toStdString())));
    m_subtitle->setText(tr("Looking up…"));
    rebuildBody();
    rebuildActions();

    setState(State::Ringing);
    resizeToContent();
    move(position);
    show();
    raise();
}

void CallerCardPopup::applyCard(const CardData &card)
{
    m_card = card;

    m_title->setText(!card.title.isEmpty()
                         ? card.title
                         : QString::fromStdString(
                               talq::fallbackCardTitle(m_callerNumber.toStdString())));
    m_subtitle->setText(card.subtitle);
    m_subtitle->setVisible(!card.subtitle.isEmpty());

    rebuildBody();
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

void CallerCardPopup::clearLayout(QVBoxLayout *layout)
{
    if (!layout)
        return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
}

void CallerCardPopup::rebuildBody()
{
    const PainterTheme th(m_theme, 1.0);

    // ── badges ──────────────────────────────────────────────────────────────
    if (auto *row = qobject_cast<QHBoxLayout *>(m_badgeRow->layout())) {
        while (row->count() > 1) {                    // keep the trailing stretch
            QLayoutItem *item = row->takeAt(0);
            if (QWidget *w = item->widget())
                w->deleteLater();
            delete item;
        }
        for (const Badge &b : m_card.badges) {
            if (b.text.isEmpty())
                continue;
            auto *chip = new QLabel(QStringLiteral(" %1 ").arg(b.text), m_badgeRow);
            chip->setTextFormat(Qt::PlainText);
            QFont f = chip->font();
            f.setBold(true);
            f.setPointSize(th.fontSizeTiny);
            chip->setFont(f);
            const QColor fill = inkForStyle(b.style);
            chip->setStyleSheet(
                QStringLiteral("color: %1; background: %2; border-radius: 4px;")
                    .arg(th.inkOn(fill).name(), fill.name()));
            row->insertWidget(row->count() - 1, chip);
        }
        m_badgeRow->setVisible(!m_card.badges.isEmpty());
    }

    // ── fields ──────────────────────────────────────────────────────────────
    clearLayout(m_fieldsLayout);

    const int total = m_card.fields.size();
    const int shown = talq::visibleFieldCount(total, m_card.maxFields);
    for (int i = 0; i < shown; ++i) {
        const Field &f = m_card.fields.at(i);
        auto *rowWidget = new QWidget(this);
        auto *row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(10);

        auto *label = new QLabel(f.label, rowWidget);
        label->setTextFormat(Qt::PlainText);
        label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                 .arg(th.textSecondary.name()));
        QFont lf = label->font();
        lf.setPointSize(th.fontSizeSmall);
        label->setFont(lf);

        auto *value = new QLabel(f.value, rowWidget);
        value->setTextFormat(Qt::PlainText);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                 .arg(inkForStyle(f.style).name()));
        QFont vf = value->font();
        vf.setPointSize(th.fontSizeSmall);
        vf.setBold(f.style == QLatin1String("warning") || f.style == QLatin1String("danger"));
        value->setFont(vf);

        row->addWidget(label);
        row->addStretch(1);
        row->addWidget(value);
        m_fieldsLayout->addWidget(rowWidget);
    }

    // Say so rather than silently truncating: an agent who cannot see the
    // count has no way to know the card is not the whole story.
    const int hidden = talq::hiddenFieldCount(total, m_card.maxFields);
    m_more->setVisible(hidden > 0);
    if (hidden > 0) {
        m_more->setText(tr("+%n more…", nullptr, hidden));
        m_more->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                  .arg(th.textMuted.name()));
        QFont mf = m_more->font();
        mf.setPointSize(th.fontSizeTiny);
        m_more->setFont(mf);
    }
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
    auto *row = qobject_cast<QHBoxLayout *>(m_actionRow->layout());
    if (!row)
        return;

    // Drop every generated button, keeping the stretch and the permanent
    // Dismiss control.
    for (int i = row->count() - 1; i >= 0; --i) {
        QWidget *w = row->itemAt(i)->widget();
        if (w && w != m_dismissButton) {
            QLayoutItem *item = row->takeAt(i);
            w->deleteLater();
            delete item;
        }
    }

    // Call back, before the server's own actions. Offered only where the
    // daemon said dialling is configured AND there is a number to dial: a
    // withheld number produces a card with nothing to call.
    //
    // It leads because of WHEN this card is on screen. On a missed call it is
    // the thing the agent wants; on a ringing one it is the thing they must
    // not hit by reflex, which is why it is a ghost button next to the filled
    // primary actions rather than competing with them.
    if (m_canDial && !m_callerNumber.trimmed().isEmpty()) {
        auto *call = new QPushButton(tr("Call back"), m_actionRow);
        call->setProperty("variant", "ghost");
        call->setCursor(Qt::PointingHandCursor);
        call->setFocusPolicy(Qt::NoFocus);
        const QString number = m_callerNumber;
        connect(call, &QPushButton::clicked, this, [this, number]() {
            emit dialRequested(number);
        });
        row->addWidget(call);
    }

    for (const Action &a : m_card.actions) {
        if (a.label.isEmpty() || !a.url.isValid())
            continue;
        auto *btn = new QPushButton(a.label, m_actionRow);
        btn->setProperty("variant", "primary");   // the call to action, filled
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus);
        const QUrl url = a.url;
        connect(btn, &QPushButton::clicked, this, [this, url]() {
            // The owner validates the scheme; the card never opens anything.
            emit openRequested(m_callId, url);
        });
        row->addWidget(btn);
    }
}

QColor CallerCardPopup::inkForStyle(const QString &style) const
{
    const PainterTheme th(m_theme, 1.0);
    switch (talq::cardStyleFromWire(style.toStdString())) {
    case talq::CardStyle::Muted:   return th.textSecondary;
    case talq::CardStyle::Warning: return th.amber;
    case talq::CardStyle::Danger:  return th.danger;
    case talq::CardStyle::Normal:  break;
    }
    return th.textPrimary;
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
    applyChrome();
    rebuildBody();          // badge/field colours are baked into stylesheets
    resizeToContent();
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

    styleLabel(m_status, m_state == State::Missed ? th.danger : th.accent,
               th.fontSizeTiny, true);
    styleLabel(m_title,    th.textPrimary,   th.fontSizeLarge, true);
    styleLabel(m_subtitle, th.textSecondary, th.fontSizeSmall, false);
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
