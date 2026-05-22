#pragma once

#include <QObject>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QByteArray>
#include <QPixmap>
#include <QVector>

class QWidget;

#ifdef Q_OS_WIN
struct ITaskbarList3;
#endif

/**
 * Manages system tray icon, desktop notifications, and notification sounds.
 *
 * Sound id values (persisted as QSettings key Notifications/soundId):
 * - "none"     — silent
 * - "system"   — Windows system notification sound (PlaySoundW SND_ALIAS)
 * - "<tone>"   — one of the bundled tones from bundledTones() (default
 *               "chime"); bytes loaded from :/sounds/<tone>.wav
 */
class NotificationManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString soundId READ soundId WRITE setSoundId NOTIFY soundIdChanged)
    Q_PROPERTY(bool notificationsEnabled READ isNotificationsEnabled WRITE setNotificationsEnabled NOTIFY notificationsEnabledChanged)

public:
    explicit NotificationManager(QObject *parent = nullptr);
    ~NotificationManager() override;

    void setWindow(QWidget *window);

    QString soundId() const { return m_soundId; }
    void setSoundId(const QString &id);
    // Single source of truth for the bundled tone roster. Order here is the
    // order shown in the Settings combo and the tray sound submenu.
    // Each pair: { id, displayLabel }.
    static QVector<QPair<QString, QString>> bundledTones();
    // Play the currently-selected sound once (for the Settings audition).
    void playCurrentSound();
    bool isNotificationsEnabled() const { return m_notificationsEnabled; }
    void setNotificationsEnabled(bool v);

    Q_INVOKABLE void notify(const QString &title, const QString &message, bool alwaysSound = false, const QString &token = QString());
    Q_INVOKABLE void clearNotifications();
    Q_INVOKABLE void updateUnreadCount(int count);

    Q_PROPERTY(QString notifStyle READ notifStyle WRITE setNotifStyle NOTIFY notifStyleChanged)

    QString notifStyle() const { return m_notifStyle; }
    void setNotifStyle(const QString &style);

signals:
    void soundIdChanged();
    void notificationsEnabledChanged();
    void notifStyleChanged();
    void showRequested();
    void desktopPopupRequested(const QString &title, const QString &message, const QString &token);

private:
    void setupTrayIcon();
    void loadSoundForId(const QString &id);  // fills m_wavData from :/sounds/<id>.wav
    void playInternalSound();
    void playSystemSound();
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);

    QSystemTrayIcon *m_trayIcon = nullptr;
    QMenu *m_trayMenu = nullptr;
    QWidget *m_window = nullptr;
    QString m_soundId = "chime";       // see header doc for valid values
    QString m_notifStyle = "popup";    // "popup" (Telegram-style) or "windows" (toast)
    bool m_notificationsEnabled = true;
    int m_unreadCount = 0;
    QByteArray m_wavData;  // bytes of the currently-selected tone (empty for none/system)
    QPixmap m_baseIcon;

#ifdef Q_OS_WIN
    // Taskbar overlay (the small badge on the app's taskbar button).
    // Qt6 dropped QtWinExtras, so we go straight to ITaskbarList3.
    void updateTaskbarOverlay(int count);
    ITaskbarList3 *m_taskbar = nullptr;
#endif
};
