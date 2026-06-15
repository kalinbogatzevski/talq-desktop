#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QStackedWidget>
#include <QLineEdit>
#include <QSettings>
#include <QShortcut>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include "painter/PainterTheme.h"

class ApiClient;
class AuthManager;
class ConversationListModel;
class MessageListModel;
class ThreadListModel;
class NotificationManager;
class PushClient;
class SignalingClient;
class MediaDeviceManager;
class CallManager;
class UserStatusManager;
class DebugMonitor;
class AppSettings;

class ChatPainter;
class SidebarPainter;
class HeaderPainter;
class ThreadsPainter;
class ComposerWidget;
class LoginWidget;
class CallWindow;
class SettingsDialog;
class StatusPopover;
class StatusDot;
class SelectionBarWidget;
class ImageViewerDialog;
class UpdateChecker;
class QProgressBar;
class QMenu;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(
        ApiClient *api,
        AuthManager *auth,
        ConversationListModel *conversations,
        MessageListModel *messages,
        ThreadListModel *threads,
        NotificationManager *notifications,
        PushClient *push,
        SignalingClient *signaling,
        MediaDeviceManager *deviceManager,
        CallManager *callManager,
        UserStatusManager *userStatus,
        DebugMonitor *debug,
        AppSettings *appSettings,
        QWidget *parent = nullptr
    );
    ~MainWindow() override;

    void restoreFromTray();
    void openConversation(const QString &token);

    // Must be public for sidebarSqueezedChanged call in cpp
    void sidebarSqueezedChanged();

    // Accessors for DebugMonitor instrumentation
    ChatPainter *chatPainter() const { return m_chatPainter; }
    SidebarPainter *sidebar() const { return m_sidebar; }

protected:
    void closeEvent(QCloseEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;   // hide status popover on minimize
    void hideEvent(QHideEvent *event) override;  // hide status popover with window
    void showEvent(QShowEvent *event) override;  // re-assert taskbar unread badge on (re)show
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;  // catch TaskbarButtonCreated → re-apply overlay
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void onUpdateReadyToLaunch(const QString &installerPath);
    void maybeLaunchPendingInstaller();
    // 0.40.15 — opens (or builds) SettingsDialog and switches to the
    // Audio & Video tab (the home of the blur slider + image picker).
    // Triggered from CallWindow's "Open background settings…" menu.
    void openSettingsToBackgrounds();

private:
    // 0.40.15 — lazy-build SettingsDialog with all the connections the
    // sidebar button used to set up inline. Shared by the button and by
    // openSettingsToBackgrounds so the wiring can't drift.
    void ensureSettingsDialog();
    // 0.40.2 —fires every few seconds while an update is staged AND
    // auto-install-on-idle is enabled. Reads the system idle counter,
    // checks the hard gates (no active call, empty composer, no upload),
    // updates the banner text/countdown, and finally calls
    // maybeLaunchPendingInstaller once the user has been idle for the
    // configured window.
    void onUpdateAutoInstallTick();

private:
    void buildChatPage();
    void buildLoginPage();
    void switchToChat();
    void switchToLogin();
    void onConversationSelected(const QString &token, const QString &name,
                                 const QString &userId, int convType,
                                 const QString &userStatus,
                                 const QString &statusMessage,
                                 const QString &statusIcon);
    void saveWindowState();
    void restoreChatGeometry();
    void applyDarkPalette();
    void applyFontScale(qreal scale);
    void applyTheme(bool dark);     // compat shim: dark→Vivid, light→Paper
    void applyThemeId(PainterTheme::Theme t);  // real path: set+persist+propagate
    void openThread(int threadId, const QString &title);
    void closeThread();
    void updateTopicMode(bool active);
    void loadProfileAvatar(QLabel *avatarLabel);
    void openStatusPopover();
    void refreshStatusIndicator();
    void buildSearchBar(QWidget *chatCol);
    void runSearchQuery();
    void scheduleReminder(int messageId, const QDateTime &when);
    QDateTime askReminderTime();
    void openUpcomingReminders();
    void openNewChatDialog();
    void openConversationInfo();
    void createNewTopic();
    void buildWelcomeContent();    // (re)build Mission Control content; theme-aware
    void showWelcome();            // rebuild-if-dirty, then show + refresh
    void restyleChrome();          // re-apply theme tokens to QSS-styled chrome
    void showThemeToast(const QString &name);  // brief "Theme: X" overlay
    void refreshWelcomeStatus();   // repaint Mission Control telemetry/LEDs/pill
    // Show/hide the offline banner + desktop-notify on server up/down.
    void onServerReachabilityChanged(bool online);

    // ── Pointers to backend (not owned) ──
    ApiClient *m_api;
    AuthManager *m_auth;
    ConversationListModel *m_conversations;
    MessageListModel *m_messages;
    ThreadListModel *m_threads;
    NotificationManager *m_notifications;
    PushClient *m_push;
    SignalingClient *m_signaling;
    MediaDeviceManager *m_deviceManager;
    CallManager *m_callManager;
    UserStatusManager *m_userStatus;
    DebugMonitor *m_debug;
    AppSettings *m_appSettings;

    // ── UI ──
    QStackedWidget *m_stack = nullptr;

    // Login page
    LoginWidget *m_loginWidget = nullptr;

    // Chat page
    QWidget *m_chatPage = nullptr;
    SidebarPainter *m_sidebar = nullptr;
    QLineEdit *m_searchField = nullptr;
    ThreadsPainter *m_threadsPainter = nullptr;
    class TopicTabBar *m_topicTabBar = nullptr;
    QWidget *m_threadsPanel = nullptr;
    HeaderPainter *m_header = nullptr;
    ChatPainter *m_chatPainter = nullptr;
    ComposerWidget *m_composer = nullptr;
    SelectionBarWidget *m_selectionBar = nullptr;
    QWidget *m_uploadBar = nullptr;
    QLabel *m_uploadLabel = nullptr;
    QWidget *m_uploadProgress = nullptr;
    QSplitter *m_splitter = nullptr;
    // Debounce for splitter-width persistence: splitterMoved fires once
    // per pixel during a drag, so we collapse to one QSettings write
    // ~250 ms after the user releases the handle. Owned by `this`.
    class QTimer *m_splitterSaveDebounce = nullptr;
    // Rate-limit window-activate user-status refresh to one fetch per
    // 5 s. Stored as milliseconds-since-epoch; 0 = "never refreshed".
    qint64 m_lastActivationStatusRefreshMs = 0;
    QWidget *m_sidebarCol = nullptr;
    QLabel *m_profileNameLabel = nullptr;
    QLabel *m_profileAvatarLabel = nullptr;
    QPushButton *m_statusPill = nullptr;     // glanceable "● Online ▾" readout
    StatusDot *m_statusDot = nullptr;        // overlay on own avatar
    StatusPopover *m_statusPopover = nullptr;
    QPushButton *m_settingsBtn = nullptr;
    QPushButton *m_themeBtn = nullptr;
    QWidget *m_profileBar = nullptr;
    QWidget *m_searchRow = nullptr;
    QPushButton *m_homeBtn = nullptr;
    QPushButton *m_filterBtn = nullptr;     // sidebar sort/filter menu trigger
    QMenu *m_filterMenu = nullptr;          // themed in restyleChrome
    QWidget *m_welcomeWidget = nullptr;     // persistent host (shown/hidden)
    QWidget *m_welcomeContent = nullptr;    // themed content, rebuilt on theme change
    bool m_welcomeDirty = false;            // theme changed while welcome hidden → rebuild on next show
    QLabel *m_themeToast = nullptr;         // transient "Theme: X" overlay
    QLabel *m_welcomeNameLabel = nullptr;
    QLabel *m_welcomeServerLabel = nullptr;     // Mission Control tile values
    QLabel *m_welcomeNcLabel = nullptr;
    QLabel *m_welcomeTalkLabel = nullptr;
    QLabel *m_welcomeSignalingLabel = nullptr;
    QLabel *m_welcomePushLabel = nullptr;
    QLabel *m_welcomeGpuLabel = nullptr;
    QLabel *m_wcStatusPill = nullptr;           // "ALL SYSTEMS NOMINAL" pill
    QLabel *m_wcSignalLed = nullptr;            // status LEDs for live subsystems
    QLabel *m_wcPushLed = nullptr;
    QLabel *m_wcGpuLed = nullptr;

    // Connection-status strip — a quiet, Telegram-style "Connecting…" bar
    // shown whenever ApiClient reports the Nextcloud server unreachable (REST
    // calls not landing). Inserted at the top of the chat column so it sits
    // above both the Home screen and an open conversation. It is purely
    // informational: it never grabs focus or disables any widget, so cached
    // chat history stays fully readable while offline.
    QWidget *m_offlineBanner = nullptr;
    QLabel  *m_offlineLabel = nullptr;
    QTimer   m_offlineAnimTimer;           // animates the trailing "…" while offline
    int      m_offlineDots = 0;

    // Auto-update banner
    UpdateChecker *m_updateChecker = nullptr;
    QWidget       *m_updateBanner = nullptr;
    class QLabel  *m_updateLabel = nullptr;
    QProgressBar  *m_updateProgress = nullptr;
    class QPushButton  *m_updateInstallBtn = nullptr;
    class QPushButton  *m_updateLaterBtn = nullptr;
    class QPushButton  *m_updateWhatsNewBtn = nullptr;
    class QPushButton  *m_updateCloseBtn = nullptr;
    QString        m_pendingInstallerPath;
    // Self-heal one-shot: a launch failure (AV quarantine, stale lock,
    // truncated download) triggers ONE silent re-download + retry. The
    // second failure surfaces to the user. Reset on every fresh
    // updateAvailable so a later session gets a clean two-strike budget.
    bool           m_updateRelaunchAttempted = false;
    QString        m_pendingUpdateNotes;
    // Version string of the most-recently-offered update. Used to gate
    // the self-heal one-shot reset: m_updateRelaunchAttempted only
    // resets when a NEW version arrives, not on every periodic re-emit
    // of the same manifest. Without this, an AV-quarantine environment
    // could see endless silent retries because each periodic poll
    // re-emits updateAvailable and clears the flag.
    QString        m_lastOfferedVersion;
    bool           m_updateBannerActive = false;   // true from updateAvailable until user dismisses

    // 0.40.2 —auto-install-on-idle state. The tick polls the system
    // idle counter and per-tick hard gates; one-shot for the lifetime
    // of the currently-pending installer. Cancelled when the user
    // clicks "Cancel auto-install", picks "Install now" (which bypasses
    // the gate), or dismisses the banner with Later.
    QTimer m_autoInstallTick;
    bool   m_autoInstallActive = false;
    bool   m_autoInstallCancelledForSession = false;
    // 0.40.6 — ms-since-epoch when the download landed (the moment the
    // user could realistically see the countdown banner). The tick
    // clamps the effective idle time to (now - this) so a user who was
    // already idle the whole download still gets to see the countdown
    // run, rather than the install firing on the first tick because
    // GetLastInputInfo was already past the threshold.
    qint64 m_autoInstallReadyAtMs = 0;
    // 0.40.16 — TalQ-input idle metric. ms-since-epoch of the last
    // mouse/key/wheel event routed through our QApplication (set by
    // eventFilter). Replaces the previous GetLastInputInfo-based idle
    // gate so input that goes to OTHER apps (a browser, an IDE) no
    // longer resets the auto-install countdown. Initialised in the
    // ctor to "now" so a fresh launch doesn't immediately count as
    // idle for the past 49 days.
    qint64 m_lastTalqInputMs = 0;
    // 0.40.16 — fire the system-tray "1 min to install" notification
    // exactly once per auto-install cycle. Cleared whenever the cycle
    // resets (cancelled, install fired, gate-blocked, etc).
    bool   m_countdownNotified = false;

    // Search bar
    class QWidget *m_searchBar = nullptr;
    class QLineEdit *m_searchInput = nullptr;
    class QListWidget *m_searchResults = nullptr;
    class QTimer *m_searchDebounce = nullptr;

    // State
    bool m_chatMode = false;
    bool m_darkMode = true;
    PainterTheme::Theme m_themeId = PainterTheme::Theme::Vivid;
    qreal m_fontScale = 1.0;
    bool m_showTopics = false;
    bool m_sidebarSqueezed = false;
    bool m_closeToTray = true;
    // Windows: RegisterWindowMessage id for the "TaskbarButtonCreated"
    // broadcast Windows sends whenever our taskbar button is created (first
    // show, restore-from-tray, Explorer restart). nativeEvent() watches for it
    // and re-applies the unread overlay badge, which SetOverlayIcon otherwise
    // loses each time the button is destroyed. 0 until registered / non-Windows.
    unsigned int m_taskbarButtonCreatedMsg = 0;
    bool m_wasMaximized = false;
    bool m_wasFullScreen = false;  // remembered separately because isMaximized() is false in fullscreen
    bool m_geometrySaveEnabled = false;
    QString m_activeConvToken;
    int m_activeThreadId = 0;
    QString m_activeThreadTitle;
    int m_activeThreadColor = 0;
    bool m_isInTopicMode = false;
    int m_replyToId = 0;
    QString m_replyToAuthor;
    QString m_replyToText;
    int m_editingMessageId = 0;

    QSettings m_settings;
    QTimer m_saveGeometryTimer;
    CallWindow *m_callWindow = nullptr;
    SettingsDialog *m_settingsDialog = nullptr;
    QPointer<ImageViewerDialog> m_imageViewer;
};
