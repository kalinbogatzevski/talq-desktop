#pragma once

#include "painter/PainterTheme.h"

#include <QWidget>
#include <QLabel>
#include <QTimer>

/**
 * Custom notification popup — Telegram-style slide-in notification.
 * Used for both desktop (frameless top-level window) and in-app popups.
 */
class NotificationPopup : public QWidget
{
    Q_OBJECT

public:
    explicit NotificationPopup(QWidget *parent = nullptr);

    void showNotification(const QString &title, const QString &message,
                           const QString &token, const QPoint &position);

    // Drive the popup chrome (card, border, title/body text) from the active
    // theme so an in-app toast never paints a dark box over the light Paper
    // surface. Safe to call before/after show; re-applies live.
    void setTheme(PainterTheme::Theme theme);

signals:
    void clicked(const QString &token);
    void dismissed();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    // Re-tint on a live theme switch (the app sets a new QApplication palette
    // on theme change, exactly the hook TopicTabBar listens on).
    void changeEvent(QEvent *event) override;

private:
    void applyChrome();   // restyle title/body labels from m_theme

    QLabel *m_titleLabel = nullptr;
    QLabel *m_messageLabel = nullptr;
    QTimer m_dismissTimer;
    QString m_token;
    PainterTheme::Theme m_theme = PainterTheme::Theme::Vivid;
    QString m_message;   // un-elided body, re-clamped on resize/theme change
};
