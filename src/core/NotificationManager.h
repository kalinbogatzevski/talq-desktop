#pragma once

#include <QObject>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QByteArray>
#include <QPixmap>

class QWidget;

/**
 * Manages system tray icon, desktop notifications, and notification sounds.
 *
 * Sound modes:
 * - "internal" (default): plays embedded TalQ chime from resources
 * - "system": plays Windows system notification sound
 * - "none": silent
 */
class NotificationManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString soundMode READ soundMode WRITE setSoundMode NOTIFY soundModeChanged)
    Q_PROPERTY(bool notificationsEnabled READ isNotificationsEnabled WRITE setNotificationsEnabled NOTIFY notificationsEnabledChanged)

public:
    explicit NotificationManager(QObject *parent = nullptr);
    ~NotificationManager() override;

    void setWindow(QWidget *window);

    QString soundMode() const { return m_soundMode; }
    void setSoundMode(const QString &mode);
    bool isNotificationsEnabled() const { return m_notificationsEnabled; }
    void setNotificationsEnabled(bool v);

    Q_INVOKABLE void notify(const QString &title, const QString &message, bool alwaysSound = false, const QString &token = QString());
    Q_INVOKABLE void clearNotifications();
    Q_INVOKABLE void updateUnreadCount(int count);

    Q_PROPERTY(QString notifStyle READ notifStyle WRITE setNotifStyle NOTIFY notifStyleChanged)

    QString notifStyle() const { return m_notifStyle; }
    void setNotifStyle(const QString &style);

signals:
    void soundModeChanged();
    void notificationsEnabledChanged();
    void notifStyleChanged();
    void showRequested();
    void popupRequested(const QString &title, const QString &message, const QString &token);
    void desktopPopupRequested(const QString &title, const QString &message, const QString &token);

private:
    void setupTrayIcon();
    void playInternalSound();
    void playSystemSound();
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);

    QSystemTrayIcon *m_trayIcon = nullptr;
    QMenu *m_trayMenu = nullptr;
    QWidget *m_window = nullptr;
    QString m_soundMode = "internal";  // "internal", "system", "none"
    QString m_notifStyle = "popup";    // "popup" (Telegram-style) or "windows" (toast)
    bool m_notificationsEnabled = true;
    int m_unreadCount = 0;
    QByteArray m_wavData;  // embedded WAV loaded from resources
    QPixmap m_baseIcon;
};
