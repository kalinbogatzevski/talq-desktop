#pragma once

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

signals:
    void clicked(const QString &token);
    void dismissed();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    QLabel *m_titleLabel = nullptr;
    QLabel *m_messageLabel = nullptr;
    QTimer m_dismissTimer;
    QString m_token;
};
