#pragma once

#include <QDialog>
#include <QTabWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QRadioButton>
#include <QLabel>
#include <QPushButton>
#include <QSettings>

class MediaDeviceManager;
class NotificationManager;
class AppSettings;
class AuthManager;

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(
        MediaDeviceManager *deviceManager,
        NotificationManager *notifications,
        AppSettings *appSettings,
        AuthManager *auth,
        QWidget *parent = nullptr
    );

    // Reload device combos and notification state from managers
    void refresh();

signals:
    void closeToTrayChanged(bool enabled);
    void themeIdChanged(int themeId);   // PainterTheme::Theme as int
    void logoutRequested();
    void checkForUpdatesRequested();

private:
    QWidget *buildAudioVideoTab();
    QWidget *buildNotificationsTab();
    QWidget *buildGeneralTab();
    QWidget *buildAccountTab();
    QWidget *buildUpdatesTab();
    void populateDeviceCombos();
    void loadNotificationSettings();
    void loadGeneralSettings();

    // Backend (not owned)
    MediaDeviceManager *m_deviceManager;
    NotificationManager *m_notifications;
    AppSettings *m_appSettings;
    AuthManager *m_auth;

    QSettings m_settings;
    QTabWidget *m_tabs = nullptr;

    // Audio & Video tab
    QComboBox *m_micCombo = nullptr;
    QComboBox *m_speakerCombo = nullptr;
    QComboBox *m_cameraCombo = nullptr;
    QRadioButton *m_res1080 = nullptr;
    QRadioButton *m_res720 = nullptr;

    // Notifications tab
    QCheckBox *m_notifEnabled = nullptr;
    QRadioButton *m_stylePopup = nullptr;
    QRadioButton *m_styleWindows = nullptr;
    QRadioButton *m_soundInternal = nullptr;
    QRadioButton *m_soundSystem = nullptr;
    QRadioButton *m_soundNone = nullptr;

    // General tab
    QCheckBox *m_autoStart = nullptr;
    QCheckBox *m_startMinimized = nullptr;
    QCheckBox *m_closeToTray = nullptr;
    QComboBox *m_themeCombo = nullptr;

    // Updates tab
    QCheckBox *m_updatesAutoCheck = nullptr;

    // Account tab
    QLabel *m_displayNameLabel = nullptr;
    QLabel *m_serverUrlLabel = nullptr;
    QLabel *m_ncVersionLabel = nullptr;
    QLabel *m_talkVersionLabel = nullptr;
    QLabel *m_talqVersionLabel = nullptr;
};
