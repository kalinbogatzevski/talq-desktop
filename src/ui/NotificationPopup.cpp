#include "NotificationPopup.h"
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

NotificationPopup::NotificationPopup(QWidget *parent)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                      | Qt::WindowDoesNotAcceptFocus | Qt::Tool)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFixedSize(360, 92);
    setCursor(Qt::PointingHandCursor);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(12);

    auto *iconLabel = new QLabel(this);
    QPixmap icon(":/logo.png");
    iconLabel->setPixmap(icon.scaled(40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    iconLabel->setFixedSize(40, 40);
    iconLabel->setAttribute(Qt::WA_TranslucentBackground);
    layout->addWidget(iconLabel, 0, Qt::AlignTop);

    auto *textCol = new QVBoxLayout();
    textCol->setContentsMargins(0, 0, 0, 0);
    textCol->setSpacing(3);

    m_titleLabel = new QLabel(this);
    m_titleLabel->setStyleSheet(
        "color: #f4efe6; font-size: 13px; font-weight: 600;"
        " letter-spacing: 0.1px; background: transparent;");
    m_titleLabel->setAttribute(Qt::WA_TranslucentBackground);
    m_titleLabel->setTextFormat(Qt::PlainText);
    m_titleLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    // Single-line, no word wrap — elide manually in showNotification so we
    // always show the room name and never let it eat vertical space.
    m_titleLabel->setWordWrap(false);
    textCol->addWidget(m_titleLabel);

    m_messageLabel = new QLabel(this);
    m_messageLabel->setStyleSheet(
        "color: #b9b4ac; font-size: 12px; background: transparent;");
    m_messageLabel->setAttribute(Qt::WA_TranslucentBackground);
    m_messageLabel->setTextFormat(Qt::PlainText);
    m_messageLabel->setWordWrap(true);
    m_messageLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    textCol->addWidget(m_messageLabel);
    textCol->addStretch();

    layout->addLayout(textCol, 1);

    m_dismissTimer.setSingleShot(true);
    m_dismissTimer.setInterval(6000);
    connect(&m_dismissTimer, &QTimer::timeout, this, [this]() {
        hide();
        emit dismissed();
    });
}

void NotificationPopup::showNotification(const QString &title, const QString &message,
                                          const QString &token, const QPoint &position)
{
    m_token = token;

    // Elide title to fit the visible label width — measured in the label's
    // own font so it accounts for the bundled Inter metrics.
    const int textW = width() - (18 + 40 + 12 + 18);   // margins + icon + spacing
    QFontMetrics titleFm(m_titleLabel->font());
    m_titleLabel->setText(titleFm.elidedText(title, Qt::ElideRight, textW));

    // Collapse newlines so the 2-line budget goes to actual content, not
    // format whitespace from forwarded messages.
    QString msg = message;
    msg.replace(QLatin1Char('\r'), QLatin1Char(' '));
    msg.replace(QLatin1Char('\n'), QLatin1Char(' '));
    msg = msg.trimmed();
    m_messageLabel->setText(msg);

    move(position);
    show();
    raise();
    m_dismissTimer.start();
}

void NotificationPopup::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        hide();
        emit clicked(m_token);
    }
}

void NotificationPopup::enterEvent(QEnterEvent *)
{
    m_dismissTimer.stop();
}

void NotificationPopup::leaveEvent(QEvent *)
{
    m_dismissTimer.start();
}

void NotificationPopup::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Drop shadow
    QRectF shadowRect = rect().adjusted(2, 3, -2, -1);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 70));
    p.drawRoundedRect(shadowRect.adjusted(2, 2, 2, 2), 14, 14);

    // Body — warm-dispatch surface + tinted border
    QRectF bgRect = rect().adjusted(4, 4, -4, -4);
    p.setBrush(QColor("#1f1b17"));
    p.setPen(QPen(QColor("#2e271f"), 1));
    p.drawRoundedRect(bgRect, 12, 12);
}
