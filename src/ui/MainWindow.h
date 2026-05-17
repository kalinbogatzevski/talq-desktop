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
        DebugMonitor *debug,
        AppSettings *appSettings,
        QWidget *parent = nullptr
    );

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
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void onUpdateReadyToLaunch(const QString &installerPath);
    void maybeLaunchPendingInstaller();

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
    QWidget *m_sidebarCol = nullptr;
    QLabel *m_profileNameLabel = nullptr;
    QLabel *m_profileAvatarLabel = nullptr;
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
    QString        m_pendingUpdateNotes;
    bool           m_updateBannerActive = false;   // true from updateAvailable until user dismisses

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
