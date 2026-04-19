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
class AvatarProvider;
class FilePreviewProvider;

class ChatPainter;
class SidebarPainter;
class HeaderPainter;
class ThreadsPainter;
class ComposerWidget;
class LoginWidget;
class CallDialog;
class SettingsDialog;
class SelectionBarWidget;
class ImageViewerDialog;
class UpdateChecker;
class QProgressBar;

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
        AvatarProvider *avatarProvider,
        FilePreviewProvider *previewProvider,
        QWidget *parent = nullptr
    );

    void restoreFromTray();
    void openConversation(const QString &token);

    // Must be public for sidebarSqueezedChanged call in cpp
    void sidebarSqueezedChanged();

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
    void openThread(int threadId, const QString &title);
    void closeThread();
    void updateTopicMode(bool active);
    void loadProfileAvatar(QLabel *avatarLabel);
    void buildSearchBar(QWidget *chatCol);
    void runSearchQuery();

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
    AvatarProvider *m_avatarProvider;
    FilePreviewProvider *m_previewProvider;

    // ── UI ──
    QStackedWidget *m_stack = nullptr;

    // Login page
    LoginWidget *m_loginWidget = nullptr;

    // Chat page
    QWidget *m_chatPage = nullptr;
    SidebarPainter *m_sidebar = nullptr;
    QLineEdit *m_searchField = nullptr;
    ThreadsPainter *m_threadsPainter = nullptr;
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
    QWidget *m_profileBar = nullptr;
    QWidget *m_searchRow = nullptr;
    QPushButton *m_homeBtn = nullptr;
    QWidget *m_welcomeWidget = nullptr;
    QSplitter *m_welcomeSplit = nullptr;
    QLabel *m_welcomeNameLabel = nullptr;
    QLabel *m_welcomeServerLabel = nullptr;
    QLabel *m_welcomeNcLabel = nullptr;
    QLabel *m_welcomeTalkLabel = nullptr;
    QLabel *m_welcomeSignalingLabel = nullptr;
    QLabel *m_welcomePushLabel = nullptr;
    QLabel *m_welcomeGpuLabel = nullptr;

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

    // Search bar
    class QWidget *m_searchBar = nullptr;
    class QLineEdit *m_searchInput = nullptr;
    class QListWidget *m_searchResults = nullptr;
    class QTimer *m_searchDebounce = nullptr;

    // State
    bool m_chatMode = false;
    bool m_darkMode = true;
    qreal m_fontScale = 1.0;
    bool m_showTopics = false;
    bool m_sidebarSqueezed = false;
    bool m_closeToTray = true;
    bool m_wasMaximized = false;
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
    CallDialog *m_callDialog = nullptr;
    SettingsDialog *m_settingsDialog = nullptr;
    QPointer<ImageViewerDialog> m_imageViewer;
};
