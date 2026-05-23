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
    // #20 — fires when any Talk/Backgrounds/* QSetting is changed via
    // the Backgrounds section. CallManager listens to live-apply during
    // calls instead of waiting for the next call's pipeline rebuild.
    void backgroundSettingsChanged();

private:
    QWidget *buildAudioVideoTab();
    QWidget *buildNotificationsTab();
    QWidget *buildGeneralTab();
    QWidget *buildAccountTab();
    QWidget *buildUpdatesTab();
    void populateDeviceCombos();
    void populateCameraQualityCombo();  // fills from selected camera's caps
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
    QComboBox *m_cameraQualityCombo = nullptr;
    QCheckBox *m_noiseSuppression = nullptr;

    // #20 Background section (#20 Phase 3.1) — Off / Blur / Image picker,
    // blur strength slider, image-path label + Choose… button. Settings
    // keys mirror upstream Talk: Talk/Backgrounds/virtualBackground*.
    QComboBox *m_bgModeCombo         = nullptr;
    QSlider   *m_bgBlurStrengthSlider = nullptr;
    QLabel    *m_bgImagePathLabel    = nullptr;

    // Notifications tab
    QCheckBox *m_notifEnabled = nullptr;
    QRadioButton *m_stylePopup = nullptr;
    QRadioButton *m_styleWindows = nullptr;
    QComboBox *m_soundCombo = nullptr;   // None / System default / bundled tones
    QComboBox *m_ringtoneCombo = nullptr; // incoming-call ringtone

    // General tab
    QCheckBox *m_autoStart = nullptr;
    QCheckBox *m_startMinimized = nullptr;
    QCheckBox *m_closeToTray = nullptr;
    QCheckBox *m_detailedLogging = nullptr;
    QComboBox *m_themeCombo = nullptr;

    // Updates tab
    QCheckBox *m_updatesAutoCheck = nullptr;
    QCheckBox *m_updatesBeta = nullptr;

    // Account tab
    QLabel *m_displayNameLabel = nullptr;
    QLabel *m_serverUrlLabel = nullptr;
    QLabel *m_ncVersionLabel = nullptr;
    QLabel *m_talkVersionLabel = nullptr;
    QLabel *m_talqVersionLabel = nullptr;
};
