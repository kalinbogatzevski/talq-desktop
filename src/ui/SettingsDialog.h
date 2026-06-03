#pragma once

#include <QDialog>
#include <QTabWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QRadioButton>
#include <QLabel>
#include <QPushButton>
#include <QSettings>

class QListWidget;
class QSlider;
class QTimer;

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
    // 0.40.15 — opens directly on the Audio & Video tab. Used by the
    // in-call BACKGROUND dropdown's "Open background settings…" entry.
    void selectAudioVideoTab();

protected:
    // Tear down the live BG preview pipeline (releases the camera so a
    // subsequent call can claim it). Sync also runs on showEvent so the
    // preview restarts naturally if the dialog re-opens.
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;
    // Swallows wheel events on the BG mode combo (avoids the
    // wheel-while-hovering trap that desynced the visible mode from
    // the saved one).
    bool eventFilter(QObject *obj, QEvent *event) override;

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
    // Reads the current Talk/Backgrounds/* settings and either starts /
    // reconfigures the live BG preview (Blur or Image mode) or stops it
    // (Off mode). Called from the mode combo, the strength slider, the
    // image grid, and the dialog show/hide events.
    void syncBgPreview();

    QWidget *buildAudioVideoTab();
    QWidget *buildBackgroundTab();
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
    // Live "is my mic working?" test under the Microphone picker: a capture
    // pipeline on the SELECTED device feeding a level meter. Started on dialog
    // open / device change, stopped on close.
    class MicTester   *m_micTester = nullptr;
    class MicLevelBar *m_micLevel  = nullptr;
    QComboBox *m_speakerCombo = nullptr;
    QComboBox *m_cameraCombo = nullptr;
    QComboBox *m_cameraQualityCombo = nullptr;
    QCheckBox *m_noiseSuppression = nullptr;
    QCheckBox *m_autoGainControl = nullptr;
    QCheckBox *m_echoCancellation = nullptr;
    QCheckBox *m_ssBorderCheck = nullptr;   // #72 — monitor-share border toggle

    // #20 Background section — Off / Blur / Image picker, blur strength
    // slider, 8-bundled-thumbnail grid + Choose… for user-supplied
    // images. Settings keys mirror upstream Talk:
    // Talk/Backgrounds/virtualBackground*.
    QComboBox   *m_bgModeCombo          = nullptr;
    QSlider     *m_bgBlurStrengthSlider = nullptr;
    QLabel      *m_bgImagePathLabel     = nullptr;
    QListWidget *m_bgImageGrid          = nullptr;
    // Container for the "Background image" section (header + grid +
    // browse row). Shown only when mode == Image; hidden in Off/Blur.
    QWidget     *m_bgImageSection       = nullptr;
    // Live camera preview that runs the user's selected BG mode end-to-
    // end without joining a call. Lazy-created the first time the user
    // picks Blur/Image, torn down on dialog close. Owns its own
    // BackgroundEngine so it doesn't fight a call-active engine for the
    // GL context.
    QLabel                  *m_bgPreviewLabel   = nullptr;
    class BgPreviewSource   *m_bgPreviewSource  = nullptr;
    class BackgroundEngine  *m_bgPreviewEngine  = nullptr;
    // Debouncer: dragging the slider used to fire 20 QSettings writes +
    // 20 backgroundSettingsChanged emits in a fraction of a second; the
    // signal handler on the CallManager side reconfigured the publisher
    // pipeline each time. The timer collapses a continuous drag to one
    // write + emit (~200 ms after the last drag tick).
    QTimer    *m_bgSettingsDebounce   = nullptr;

    // Notifications tab
    QCheckBox *m_notifEnabled = nullptr;
    QRadioButton *m_stylePopup = nullptr;
    QRadioButton *m_styleWindows = nullptr;
    QComboBox *m_soundCombo = nullptr;   // None / System default / bundled tones
    QComboBox *m_ringtoneCombo = nullptr; // incoming-call ringtone
    QComboBox *m_outgoingRingtoneCombo = nullptr; // outgoing-call (calling…) tone

    // General tab
    QCheckBox *m_autoStart = nullptr;
    QCheckBox *m_startMinimized = nullptr;
    QCheckBox *m_closeToTray = nullptr;
    QCheckBox *m_detailedLogging = nullptr;
    QComboBox *m_themeCombo = nullptr;

    // Updates tab
    QCheckBox *m_updatesAutoCheck = nullptr;
    QCheckBox *m_updatesBeta = nullptr;
    // 0.40.2 —auto-install on idle. The checkbox gates the whole feature;
    // the combo picks how long the user must have been idle before the
    // background self-restart fires.
    QCheckBox *m_updatesAutoInstall = nullptr;
    QComboBox *m_updatesIdleWait = nullptr;

    // Account tab
    QLabel *m_displayNameLabel = nullptr;
    QLabel *m_serverUrlLabel = nullptr;
    QLabel *m_ncVersionLabel = nullptr;
    QLabel *m_talkVersionLabel = nullptr;
    QLabel *m_talqVersionLabel = nullptr;
};
