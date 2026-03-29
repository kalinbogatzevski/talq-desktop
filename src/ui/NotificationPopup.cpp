#include "NotificationPopup.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QScreen>
#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPropertyAnimation>

NotificationPopup::NotificationPopup(QWidget *parent)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFixedSize(340, 76);
    setCursor(Qt::PointingHandCursor);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(10);

    // App icon
    auto *iconLabel = new QLabel(this);
    QPixmap icon(":/logo.png");
    iconLabel->setPixmap(icon.scaled(36, 36, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    iconLabel->setFixedSize(36, 36);
    layout->addWidget(iconLabel, 0, Qt::AlignTop);

    // Text column
    auto *textCol = new QVBoxLayout();
    textCol->setSpacing(2);

    m_titleLabel = new QLabel(this);
    m_titleLabel->setStyleSheet("font-size: 13px; font-weight: 600; color: #f0f2f5; background: transparent;");
    m_titleLabel->setMaximumWidth(250);
    textCol->addWidget(m_titleLabel);

    m_messageLabel = new QLabel(this);
    m_messageLabel->setStyleSheet("font-size: 12px; color: #b0b4bc; background: transparent;");
    m_messageLabel->setWordWrap(true);
    m_messageLabel->setMaximumWidth(250);
    m_messageLabel->setMaximumHeight(32);
    textCol->addWidget(m_messageLabel);

    layout->addLayout(textCol, 1);

    // Dismiss timer
    m_dismissTimer.setSingleShot(true);
    m_dismissTimer.setInterval(5000);
    connect(&m_dismissTimer, &QTimer::timeout, this, [this]() {
        hide();
        emit dismissed();
    });
}

void NotificationPopup::showNotification(const QString &title, const QString &message,
                                          const QString &token, const QPoint &position)
{
    m_token = token;
    m_titleLabel->setText(title);

    // Truncate message
    QString msg = message;
    if (msg.length() > 100)
        msg = msg.left(100) + QStringLiteral("...");
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

    // Shadow
    QRectF shadowRect = rect().adjusted(2, 2, -2, -2);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 60));
    p.drawRoundedRect(shadowRect.adjusted(2, 2, 2, 2), 14, 14);

    // Background
    QRectF bgRect = rect().adjusted(4, 4, -4, -4);
    p.setBrush(QColor("#2a2f3a"));
    p.setPen(QPen(QColor("#3a4050"), 1));
    p.drawRoundedRect(bgRect, 12, 12);
}
