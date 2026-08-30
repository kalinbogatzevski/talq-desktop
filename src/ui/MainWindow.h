#pragma once

#include <QMainWindow>
#include <QVector>
#include <QPointer>
#include <QSet>
#include <QStackedWidget>
#include <QLineEdit>
#include <QSettings>
#include <QShortcut>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include "core/ConversationTagLogic.h"
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
class QScrollArea;
class QGridLayout;
class UpdateChecker;
class CtiService;
class QProgressBar;
class QMenu;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // Exposed so main.cpp can surface CTI status as a toast. Without this the
    // "your device is no longer authorised" message is emitted to a signal
    // whose only receiver lives inside the lazily-constructed SettingsDialog,
    // so an agent who never opens Settings is told nothing at all and simply
    // stops getting call pop-ups.
    class CtiService *ctiService() const { return m_cti; }
    class ShiftStatusService *shiftStatus() const { return m_shiftStatus; }

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
    void openSettingsToPhone();

private:
    // Tell ShiftStatusService which colleagues are on screen. Cheap and
    // idempotent -- the service dedupes, clamps and skips anything still
    // fresh, so calling it on every list refresh costs nothing.
    void observeShiftUsers();

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
    // ── Talk 24 conversation tags ──
    // Fetched once per session after login and refreshed whenever the user
    // changes them. All three are no-ops unless the server advertises
    // `conversation-tags`, so nothing here fires against an older server.
    void refreshConversationTags();
    void rebuildTagFilterMenu();
    void pushTagsToSidebar();   // hands the ordered, localised list to the painter
    void openTagManager();      // rename / delete / create for custom tags
    void applyTagFilter(const QString &tagId, const QString &displayName);
    // Builds the "Tags" submenu on a conversation's right-click menu.
    void populateTagAssignMenu(QMenu *parent, const QString &token);
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
    // Talk 24 tags. m_tags is the server's list in display order; the actions
    // are rebuilt into m_filterMenu each time that list changes, and tracked
    // so the previous batch can be removed without disturbing the static
    // Sort-by / Show sections above them.
    QVector<talq::ConversationTag> m_tags;
    QList<QAction *> m_tagFilterActions;
    QWidget *m_welcomeWidget = nullptr;     // persistent host (shown/hidden)
    QScrollArea *m_welcomeScroll = nullptr; // vertical overflow for the board
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
    QLabel *m_welcomePhoneLabel = nullptr;   // PHONE tile (only when configured)
    QLabel *m_wcStatusPill = nullptr;           // "ALL SYSTEMS NOMINAL" pill
    QLabel *m_wcSignalLed = nullptr;            // status LEDs for live subsystems
    QLabel *m_wcPushLed = nullptr;
    QLabel *m_wcGpuLed = nullptr;
    QLabel *m_wcPhoneLed = nullptr;

    // Mission Control telemetry tiles, kept so the grid can REFLOW on resize.
    // A fixed 4-column grid needs ~1400px; the panel is often far narrower,
    // and the surplus columns were simply clipped off the right edge -- which
    // is why SIGNALING, PUSH and GPU were invisible on a 1453px window.
    struct WcTile { QWidget *w = nullptr; int span = 1; };
    QVector<WcTile> m_wcTiles;
    QGridLayout *m_wcGrid = nullptr;
    int m_wcGridCols = 0;              // last applied column count
    // The ONE implementation of forwarding a message. Both entry points (the
    // selection bar and the right-click menu) call this; they were separate
    // copies and fixing only one is exactly how 0.68.1 shipped a fix that did
    // nothing for the menu.
    // The ONE implementation of "put message `messageId` on screen": scroll to
    // it, fetching the surrounding history first if it is outside the loaded
    // window. Used by the search results and by clicking a reply quote.
    void jumpToMessage(int messageId);
    // The ONE implementation of opening the composer to edit a message. Both
    // entry points (up-arrow and the right-click menu) call this; they were
    // separate copies that each loaded the rendered text and so stripped the
    // formatting on save.
    void beginEditingMessage(int messageId, const QString &fallbackPlain);
    void forwardOneMessage(const QVariantMap &msg, const QString &targetToken);
    void relayoutWelcomeTiles();       // recompute columns from the panel width

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
    // Screen-pop for inbound phone calls. Dormant unless the user has
    // paired a device, so an unconfigured install pays nothing for it.
    CtiService *m_cti = nullptr;
    ShiftStatusService *m_shiftStatus = nullptr;
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
    // 0.52.14 — manual "Update now" (Settings). m_userWantsImmediateInstall makes
    // onUpdateReadyToLaunch skip the idle countdown (the call gate still applies);
    // m_updateNowChecking gates the "You're up to date" fallback feedback.
    bool   m_userWantsImmediateInstall = false;
    bool   m_updateNowChecking = false;
    // 0.53.2 — set by EVERY explicit user-initiated install (banner "Install now",
    // Settings "Update now"); persists through the in-call deferral + the call-end
    // retry, and makes maybeLaunchPendingInstaller SKIP the post-call grace — the
    // user asked to install, so only FULLY-AUTOMATIC installs wait out the grace.
    // Cleared on a successful launch. (m_userWantsImmediateInstall can't carry this:
    // it's cleared before the gate runs at every call site.)
    bool   m_explicitInstallRequested = false;
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
    // 0.53.2 — ms-since-epoch of the last moment a CALL was active (refreshed every
    // ~1 s during a call via durationChanged, and on every call-state change). The
    // install gate (maybeLaunchPendingInstaller) defers until kPostCallInstallGraceMs
    // of NO call has elapsed, so an update that landed mid-session NEVER restarts TalQ
    // the instant a call ends — back-to-back test calls keep refreshing this and hold
    // the install off. Replaces the old immediate stateChanged→install fire that
    // restarted out from under an active session (Kalin, live 0.53.x testing).
    qint64 m_lastCallActiveMs = 0;
    static constexpr qint64 kPostCallInstallGraceMs = 180000;  // 3 min of no call
    // 0.40.16 — fire the system-tray "1 min to install" notification
    // exactly once per auto-install cycle. Cleared whenever the cycle
    // resets (cancelled, install fired, gate-blocked, etc).
    bool   m_countdownNotified = false;

    // Search bar
    class QWidget *m_searchBar = nullptr;
    class QLineEdit *m_searchInput = nullptr;
    class QListWidget *m_searchResults = nullptr;
    class QTimer *m_searchDebounce = nullptr;
    // Watches MessageListModel::historyUntilSettled for the current search-hit
    // jump (loadHistoryUntil chases a single model-level target id, so only
    // one jump can ever be in flight). A value member, not a heap allocation:
    // arming a new jump disconnects whatever the previous one left behind
    // first, so a same-room second click can never strand a connection.
    // Watcher for an in-flight loadHistoryUntil chase. ONE member, because
    // the model has ONE m_historyUntilTargetId -- see jumpToMessage().
    QMetaObject::Connection m_jumpConn;

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
    // 0.65.3 — unsent composer text, keyed by conversation token, so switching
    // rooms no longer carries a half-typed message into the wrong one.
    // In-memory by design: see the note at the save/restore site.
    QHash<QString, QString> m_composerDrafts;
    // Talk 24 voice rooms: tokens we have already auto-joined this session.
    // onConversationSelected() re-runs on every sidebar refresh and reselect,
    // so without this latch hanging up would be impossible — the next poll
    // would rejoin the call immediately. Session-scoped on purpose: after a
    // restart, opening the voice room should put you back in the call.
    QSet<QString> m_autoJoinedVoiceRooms;
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
    // 0.52.7 — coalesces per-topic-unread refreshes: live message batches + window
    // activation can fire rapidly, and ThreadListModel::refresh() is an un-coalesced
    // full-room fetch. A single trailing 400 ms refresh avoids overlapping fetches /
    // model-reset churn and a redundant fetch on every message you send.
    QTimer m_topicUnreadDebounce;
    CallWindow *m_callWindow = nullptr;
    SettingsDialog *m_settingsDialog = nullptr;
    QPointer<ImageViewerDialog> m_imageViewer;
};
