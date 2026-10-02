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
    // Own status is Do Not Disturb: notify() and notifySilently() raise NOTHING
    // -- no popup, no sound. Only notifyFeedback() still shows (still silent).
    void setDoNotDisturb(bool on) { m_doNotDisturb = on; }
    bool isDoNotDisturb() const { return m_doNotDisturb; }

    // Something arrived from OUTSIDE (a message, a second caller, a server
    // notice). Silent in Do Not Disturb: no popup and no sound. This is the
    // default on purpose -- a new caller is quiet in DND unless it opts out.
    Q_INVOKABLE void notify(const QString &title, const QString &message, bool alwaysSound = false, const QString &token = QString());
    // Feedback about something the user is doing RIGHT NOW (their own outgoing
    // call, a screen share they started, a reminder they just set). Do Not
    // Disturb must not swallow it -- a share that silently failed is worse than
    // a toast -- but it never makes a sound in DND. Use sparingly: anything
    // that came from another person or from the server belongs in notify().
    void notifyFeedback(const QString &title, const QString &message, bool alwaysSound = false, const QString &token = QString());
    Q_INVOKABLE void clearNotifications();
    Q_INVOKABLE void updateUnreadCount(int count);

    // Light or clear the tray's connection-fault disc. Stays lit until the
    // fault clears -- it is the surface the user cannot dismiss.
    void setConnectionAlarm(bool on, const QString &reason = QString());

    // Raise a connection-fault popup. Never plays a sound and is not
    // suppressed when the window has focus; returns whether it was actually
    // shown. See the implementation for why notify() is wrong for this.
    bool notifyConnectionFault(const QString &title, const QString &message);
    // A popup that never makes a sound, shown whether or not TalQ is focused;
    // respects notificationsEnabled and Do Not Disturb. For app notices that are
    // not messages. Returns whether it went out.
    bool notifySilently(const QString &title, const QString &message);
    // Re-assert the Windows taskbar overlay badge for the CURRENT unread count,
    // bypassing updateUnreadCount()'s "count unchanged" early-return. MainWindow
    // calls this whenever the taskbar button is (re)created — first show,
    // restore-from-tray, Explorer restart — because ITaskbarList3::SetOverlayIcon
    // only persists while the button exists. No-op on non-Windows.
    void reapplyTaskbarOverlay();

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
    // The single writer of the tray icon + tooltip: composites the unread
    // badge and the fault disc in one pass so neither can erase the other.
    void refreshTrayIcon();
    void loadSoundForId(const QString &id);  // fills m_wavData from :/sounds/<id>.wav
    void playInternalSound();
    void playSystemSound();
    // The one body behind notify() and notifyFeedback(); `feedback` is the only
    // difference (whether Do Not Disturb still lets the popup through).
    void deliver(const QString &title, const QString &message, bool alwaysSound,
                 const QString &token, bool feedback);
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);

    QSystemTrayIcon *m_trayIcon = nullptr;
    QMenu *m_trayMenu = nullptr;
    QWidget *m_window = nullptr;
    QString m_soundId = "chime";       // see header doc for valid values
    QString m_notifStyle = "popup";    // "popup" (Telegram-style) or "windows" (toast)
    bool m_notificationsEnabled = true;
    bool m_doNotDisturb = false;
    int m_unreadCount = 0;
    QByteArray m_wavData;  // bytes of the currently-selected tone (empty for none/system)
    QPixmap m_baseIcon;
    bool m_connectionAlarm = false;
    QString m_alarmReason;

#ifdef Q_OS_WIN
    // Taskbar overlay (the small badge on the app's taskbar button).
    // Qt6 dropped QtWinExtras, so we go straight to ITaskbarList3.
    void updateTaskbarOverlay(int count);
    ITaskbarList3 *m_taskbar = nullptr;
#endif
};
