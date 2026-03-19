#include "core/NotificationManager.h"
#include <QApplication>
#include <QQuickWindow>
#include <QIcon>
#include <QFile>
#include <QActionGroup>
#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#endif

NotificationManager::NotificationManager(QObject *parent)
    : QObject(parent)
{
    // Load embedded WAV from resources into memory
    QFile wav(":/notification.wav");
    if (wav.open(QIODevice::ReadOnly)) {
        m_wavData = wav.readAll();
    }

    setupTrayIcon();
}

NotificationManager::~NotificationManager()
{
    delete m_trayMenu;
}

void NotificationManager::setWindow(QQuickWindow *window)
{
    m_window = window;
}

void NotificationManager::setupTrayIcon()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(QIcon(":/logo.png"));
    m_trayIcon->setToolTip("TalQ");

    m_trayMenu = new QMenu();

    auto *showAction = m_trayMenu->addAction("Show TalQ");
    connect(showAction, &QAction::triggered, this, [this]() {
        emit showRequested();
    });

    m_trayMenu->addSeparator();

    // Sound mode submenu
    auto *soundMenu = m_trayMenu->addMenu("Sound");
    auto *soundGroup = new QActionGroup(soundMenu);

    auto *internalAction = soundMenu->addAction("TalQ chime");
    internalAction->setCheckable(true);
    internalAction->setChecked(m_soundMode == "internal");
    internalAction->setActionGroup(soundGroup);
    connect(internalAction, &QAction::triggered, this, [this]() { setSoundMode("internal"); });

    auto *systemAction = soundMenu->addAction("System sound");
    systemAction->setCheckable(true);
    systemAction->setChecked(m_soundMode == "system");
    systemAction->setActionGroup(soundGroup);
    connect(systemAction, &QAction::triggered, this, [this]() { setSoundMode("system"); });

    auto *noneAction = soundMenu->addAction("None");
    noneAction->setCheckable(true);
    noneAction->setChecked(m_soundMode == "none");
    noneAction->setActionGroup(soundGroup);
    connect(noneAction, &QAction::triggered, this, [this]() { setSoundMode("none"); });

    auto *notifAction = m_trayMenu->addAction("Desktop notifications");
    notifAction->setCheckable(true);
    notifAction->setChecked(m_notificationsEnabled);
    connect(notifAction, &QAction::toggled, this, &NotificationManager::setNotificationsEnabled);

    m_trayMenu->addSeparator();

    auto *quitAction = m_trayMenu->addAction("Quit");
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    m_trayIcon->setContextMenu(m_trayMenu);

    connect(m_trayIcon, &QSystemTrayIcon::activated,
            this, &NotificationManager::onTrayActivated);

    m_trayIcon->show();
}

void NotificationManager::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        emit showRequested();
    }
}

void NotificationManager::playInternalSound()
{
#ifdef Q_OS_WIN
    if (!m_wavData.isEmpty()) {
        // Play from memory — bypasses Windows DND/Focus Assist
        PlaySoundA(m_wavData.constData(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
#endif
}

void NotificationManager::playSystemSound()
{
#ifdef Q_OS_WIN
    // Uses Windows system notification sound — respects DND/Focus Assist
    PlaySoundW(L"SystemNotification", NULL, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
#endif
}

void NotificationManager::notify(const QString &title, const QString &message)
{
    bool windowActive = m_window && m_window->isActive();

    if (!windowActive) {
        // Play sound based on mode
        if (m_soundMode == "internal") {
            playInternalSound();
        } else if (m_soundMode == "system") {
            playSystemSound();
        }
        // "none" = no sound

        // Toast notification (always goes through Windows notification center)
        if (m_notificationsEnabled && m_trayIcon) {
            m_trayIcon->showMessage(title, message, QSystemTrayIcon::Information, 4000);
        }
    }
}

void NotificationManager::clearNotifications()
{
    m_unreadCount = 0;
    if (m_trayIcon) {
        m_trayIcon->setToolTip("TalQ");
    }
}

void NotificationManager::updateUnreadCount(int count)
{
    m_unreadCount = count;
    if (m_trayIcon) {
        if (count > 0) {
            m_trayIcon->setToolTip(QString("TalQ — %1 unread").arg(count));
        } else {
            m_trayIcon->setToolTip("TalQ");
        }
    }
}

void NotificationManager::setSoundMode(const QString &mode)
{
    if (m_soundMode != mode) {
        m_soundMode = mode;
        emit soundModeChanged();
    }
}

void NotificationManager::setNotificationsEnabled(bool v)
{
    if (m_notificationsEnabled != v) {
        m_notificationsEnabled = v;
        emit notificationsEnabledChanged();
    }
}
