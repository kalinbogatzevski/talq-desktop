#include "MainWindow.h"
#include "CallWindow.h"
#include "SettingsDialog.h"
#include "CodenameBlurb.h"
#ifndef TALQ_VERSION_NAME
#define TALQ_VERSION_NAME ""   // per-release codename (set in CMake)
#endif
#include "LoginWidget.h"
#include "ComposerWidget.h"
#include "SelectionBarWidget.h"
#include "ConversationPickerDialog.h"
#include "ScheduledMessagesDialog.h"
#include "ImageViewerDialog.h"
#include "ConversationInfoDialog.h"
#include "NewChatDialog.h"
#include "TopicTabBar.h"
#include "UpcomingRemindersDialog.h"
#include "painter/ChatPainter.h"
#include "painter/SidebarPainter.h"
#include "painter/HeaderPainter.h"
#include "painter/ThreadsPainter.h"
#include "painter/PainterTheme.h"
#include "core/ApiClient.h"
#include "core/SearchHit.h"
#include "core/AuthManager.h"
#include "core/ShutdownWatchdog.h"
#include "core/NotificationManager.h"
#include "core/PushClient.h"
#include "core/SignalingClient.h"
#include "core/CallManager.h"
#include "core/Diagnostics.h"
#include "core/EncodeTier.h"
#include "core/UserStatusManager.h"
#include "ui/StatusPopover.h"
#include "ui/AppStyle.h"
#include "core/DebugMonitor.h"
#include "core/AppSettings.h"
#include "core/MediaDeviceManager.h"
#include "core/UpdateChecker.h"
#include <gst/gst.h>
#include "models/ConversationListModel.h"
#include "models/MessageListModel.h"
#include "models/ThreadListModel.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QCloseEvent>
#include <QShowEvent>
#include <QScreen>
#include <QMenu>
#include <QActionGroup>
#include <QDesktopServices>
#include <QDialog>
#include <QClipboard>
#include <QRegularExpression>
#include <QToolTip>
#include <QWidgetAction>
#include <QMessageBox>
#include <QNetworkReply>
#include <QLabel>
#include <QFontMetrics>
#include <QHideEvent>
#include <QListWidget>
#include <QUrl>
#include <QTextBrowser>
#include <QTextDocument>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QFile>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QProgressBar>
#include <QProcess>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

// Strip HTML tags from message text, substituting a file placeholder when empty.
static QString plainBodyText(const QVariantMap &msg)
{
    QString html = msg.value("messageText").toString();
    QTextDocument doc;
    doc.setHtml(html);
    QString body = doc.toPlainText().trimmed();
    if (body.isEmpty() && msg.value("hasFile").toBool())
        body = QStringLiteral("[File: %1]").arg(msg.value("fileName").toString());
    return body;
}

MainWindow::~MainWindow()
{
    qInfo() << "[SHUTDOWN] ~MainWindow begin";   // 0.51.15 TEMP hang diag
    // m_callWindow is a parentless top-level (so it gets its own Windows
    // taskbar button); it has no QObject parent to auto-delete it.
    delete m_callWindow;
    m_callWindow = nullptr;
}

MainWindow::MainWindow(
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
    QWidget *parent)
    : QMainWindow(parent)
    , m_api(api)
    , m_auth(auth)
    , m_conversations(conversations)
    , m_messages(messages)
    , m_threads(threads)
    , m_notifications(notifications)
    , m_push(push)
    , m_signaling(signaling)
    , m_deviceManager(deviceManager)
    , m_callManager(callManager)
    , m_userStatus(userStatus)
    , m_debug(debug)
    , m_appSettings(appSettings)
    , m_settings("TalQ", "TalQ")
{
    // Window setup. Pre-release builds get a visible " — PRE-RELEASE"
    // suffix in the title bar so beta testers always know which channel
    // they're on (gated by the TALQ_PRERELEASE compile define that
    // build-release.sh --beta sets).
    QString winTitle =
#ifdef TALQ_BRAND_123NET
        "123NET TalQ "
#else
        "TalQ "
#endif
        + QApplication::applicationVersion();
#ifdef TALQ_PRERELEASE
    winTitle += QStringLiteral(" — PRE-RELEASE");
#endif
    setWindowTitle(winTitle);
    setWindowIcon(QIcon(":/logo.png"));
    setMinimumSize(380, 400);
    resize(380, 420);

    // Read settings
    m_settings.beginGroup("Theme");
    m_darkMode = m_settings.value("darkMode", true).toBool();
    m_themeId = PainterTheme::themeFromId(
        m_settings.value("theme", PainterTheme::themeId(PainterTheme::Theme::Vivid)).toString(),
        PainterTheme::Theme::Vivid);
    m_fontScale = m_settings.value("fontScale", 1.0).toReal();
    m_settings.endGroup();

    m_settings.beginGroup("General");
    m_closeToTray = m_settings.value("closeToTray", true).toBool();
    m_settings.endGroup();

    applyDarkPalette();

    // Stack for login/chat
    m_stack = new QStackedWidget(this);
    setCentralWidget(m_stack);

    buildLoginPage();
    buildChatPage();

    m_stack->setCurrentWidget(m_loginWidget);

    // Geometry save timer (debounced)
    m_saveGeometryTimer.setSingleShot(true);
    m_saveGeometryTimer.setInterval(300);
    connect(&m_saveGeometryTimer, &QTimer::timeout, this, &MainWindow::saveWindowState);

    // 0.40.2 —auto-install-on-idle tick. The timer is armed only while
    // an update is downloaded and the user hasn't opted out; the slot
    // handles its own stop condition.
    connect(&m_autoInstallTick, &QTimer::timeout,
            this, &MainWindow::onUpdateAutoInstallTick);

    // 0.40.16 — TalQ-input idle metric. Global event filter on qApp so
    // every mouse/key/wheel event that flows through OUR app event loop
    // timestamps m_lastTalqInputMs. Events targeted at other processes
    // never reach this filter — so background activity in a browser /
    // IDE / etc. no longer resets the auto-install countdown. Seed to
    // "now" so a freshly-launched TalQ doesn't read as idle-since-epoch.
    m_lastTalqInputMs = QDateTime::currentMSecsSinceEpoch();
    qApp->installEventFilter(this);

    // Dark title bar on Windows
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(winId());
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode));
    // Register for the taskbar-button-created broadcast so nativeEvent() can
    // re-apply the unread overlay badge whenever Windows (re)creates our
    // taskbar button. Without this, a badge set while we were hidden in the
    // tray is dropped and never reappears on restore — which is why the badge
    // never showed on the taskbar button despite the tray badge working.
    m_taskbarButtonCreatedMsg = RegisterWindowMessageW(L"TaskbarButtonCreated");
#endif

    // ── Keyboard shortcuts ──
    auto *zoomIn = new QShortcut(QKeySequence("Ctrl+="), this);
    connect(zoomIn, &QShortcut::activated, this, [this]() {
        applyFontScale(qMin(m_fontScale + 0.1, 2.0));
    });

    auto *zoomOut = new QShortcut(QKeySequence("Ctrl+-"), this);
    connect(zoomOut, &QShortcut::activated, this, [this]() {
        applyFontScale(qMax(m_fontScale - 0.1, 0.7));
    });

    auto *zoomReset = new QShortcut(QKeySequence("Ctrl+0"), this);
    connect(zoomReset, &QShortcut::activated, this, [this]() {
        applyFontScale(1.0);
    });

    auto *toggleDark = new QShortcut(QKeySequence("Ctrl+D"), this);
    connect(toggleDark, &QShortcut::activated, this, [this]() {
        applyThemeId(PainterTheme::cycle(m_themeId));
    });

    // ── Auth signals ──
    connect(m_auth, &AuthManager::restoringChanged, this, [this]() {
        if (!m_auth->isRestoringSession()) {
            if (m_auth->isLoggedIn()) {
                switchToChat();
            } else {
                switchToLogin();
            }
        }
    });

    connect(m_auth, &AuthManager::loggedInChanged, this, [this]() {
        if (m_auth->isRestoringSession()) return;
        if (m_auth->isLoggedIn()) {
            switchToChat();
        } else {
            saveWindowState();
            switchToLogin();
        }
    });

    // Notify manager
    m_notifications->setWindow(this);
    connect(m_notifications, &NotificationManager::showRequested, this, &MainWindow::restoreFromTray);

    // Center on screen
    if (auto *screen = QApplication::primaryScreen()) {
        QRect geo = screen->availableGeometry();
        move(geo.center() - QPoint(width()/2, height()/2));
    }
}

void MainWindow::buildLoginPage()
{
    m_loginWidget = new LoginWidget(m_auth, m_stack);
    m_stack->addWidget(m_loginWidget);
}

void MainWindow::buildChatPage()
{
    m_chatPage = new QWidget(m_stack);
    auto *mainLayout = new QHBoxLayout(m_chatPage);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ── Sidebar column ──
    m_sidebarCol = new QWidget(m_chatPage);
    auto *sidebarCol = m_sidebarCol;
    auto *sidebarLayout = new QVBoxLayout(sidebarCol);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    sidebarLayout->setSpacing(0);

    // Warm-dispatch search field — pill-shaped, low-contrast until focused.
    m_searchField = new QLineEdit(sidebarCol);
    m_searchField->setObjectName("sbSearch");
    m_searchField->setPlaceholderText(tr("Search conversations\u2026"));
    m_searchField->setMinimumHeight(32);

    m_homeBtn = new QPushButton(QStringLiteral("\uE80F"), sidebarCol);  // Home
    m_homeBtn->setObjectName("sbIcon");
    m_homeBtn->setFixedSize(32, 32);
    m_homeBtn->setFocusPolicy(Qt::NoFocus);
    m_homeBtn->setCursor(Qt::PointingHandCursor);
    m_homeBtn->setToolTip(tr("Home"));

    // Sort / filter trigger — funnel glyph, menu of exclusive sort & show modes.
    m_filterBtn = new QPushButton(QStringLiteral(""), sidebarCol);  // Filter (funnel)
    m_filterBtn->setObjectName("sbIcon");
    m_filterBtn->setFixedSize(32, 32);
    m_filterBtn->setFocusPolicy(Qt::NoFocus);
    m_filterBtn->setCursor(Qt::PointingHandCursor);
    m_filterBtn->setToolTip(tr("Sort and filter conversations"));

    m_filterMenu = new QMenu(m_filterBtn);
    auto *sortGroup = new QActionGroup(m_filterMenu);
    sortGroup->setExclusive(true);
    m_filterMenu->addSection(tr("Sort by"));
    struct ModeDef { const char *label; int mode; };
    const ModeDef sortDefs[] = {
        { QT_TR_NOOP("Recent activity"), SidebarPainter::SortRecent },
        { QT_TR_NOOP("Unread first"),    SidebarPainter::SortUnread },
        { QT_TR_NOOP("Name (A–Z)"), SidebarPainter::SortName },
    };
    for (const auto &d : sortDefs) {
        QAction *a = m_filterMenu->addAction(tr(d.label));
        a->setCheckable(true);
        a->setData(d.mode);
        sortGroup->addAction(a);
    }
    auto *filterGroup = new QActionGroup(m_filterMenu);
    filterGroup->setExclusive(true);
    m_filterMenu->addSection(tr("Show"));
    const ModeDef filterDefs[] = {
        { QT_TR_NOOP("All conversations"), SidebarPainter::FilterAll },
        { QT_TR_NOOP("Unread"),            SidebarPainter::FilterUnread },
        { QT_TR_NOOP("Favorites"),         SidebarPainter::FilterFavorites },
        { QT_TR_NOOP("Direct messages"),   SidebarPainter::FilterDirect },
        { QT_TR_NOOP("Groups"),            SidebarPainter::FilterGroups },
    };
    for (const auto &d : filterDefs) {
        QAction *a = m_filterMenu->addAction(tr(d.label));
        a->setCheckable(true);
        a->setData(d.mode);
        filterGroup->addAction(a);
    }
    connect(m_filterBtn, &QPushButton::clicked, this, [this]() {
        m_filterMenu->popup(m_filterBtn->mapToGlobal(
            QPoint(0, m_filterBtn->height() + 2)));
    });

    m_searchRow = new QWidget(sidebarCol);
    auto *searchRowLayout = new QHBoxLayout(m_searchRow);
    searchRowLayout->setContentsMargins(10, 6, 10, 6);
    searchRowLayout->setSpacing(6);
    searchRowLayout->addWidget(m_homeBtn);
    searchRowLayout->addWidget(m_searchField, 1);
    searchRowLayout->addWidget(m_filterBtn);
    auto *searchRow = m_searchRow;

    // ── User profile header ──
    m_profileBar = new QWidget(sidebarCol);
    auto *profileBar = m_profileBar;
    profileBar->setFixedHeight(56);
    profileBar->installEventFilter(this);

    auto *profileLayout = new QHBoxLayout(profileBar);
    profileLayout->setContentsMargins(14, 10, 10, 10);
    profileLayout->setSpacing(10);

    auto *profileAvatar = new QLabel(profileBar);
    profileAvatar->setObjectName("sbAvatar");
    profileAvatar->setFixedSize(36, 36);
    profileLayout->addWidget(profileAvatar);

    m_profileNameLabel = new QLabel(profileBar);
    m_profileNameLabel->setObjectName("sbName");

    // Glanceable status readout under the name; click to open the picker.
    m_statusPill = new QPushButton(profileBar);
    m_statusPill->setObjectName("sbStatusPill");
    m_statusPill->setFlat(true);
    m_statusPill->setCursor(Qt::PointingHandCursor);
    m_statusPill->setFocusPolicy(Qt::NoFocus);
    m_statusPill->setToolTip(tr("Set your status"));
    connect(m_statusPill, &QPushButton::clicked, this, &MainWindow::openStatusPopover);

    auto *idCol = new QWidget(profileBar);
    auto *idLay = new QVBoxLayout(idCol);
    idLay->setContentsMargins(0, 0, 0, 0);
    idLay->setSpacing(1);
    idLay->addWidget(m_profileNameLabel);
    idLay->addWidget(m_statusPill, 0, Qt::AlignLeft);
    profileLayout->addWidget(idCol, 1);

    // Own presence dot, overlaid on the avatar's bottom-right corner
    // (same vocabulary as the contacts' dots).
    m_statusDot = new StatusDot(profileAvatar);
    m_statusDot->move(profileAvatar->width() - m_statusDot->width(),
                      profileAvatar->height() - m_statusDot->height());
    m_statusDot->show();

    auto *newChatBtn = new QPushButton(QStringLiteral("\uE710"), profileBar);  // Add
    newChatBtn->setObjectName("sbIcon");
    newChatBtn->setFixedSize(30, 30);
    newChatBtn->setFocusPolicy(Qt::NoFocus);
    newChatBtn->setToolTip(tr("New chat"));
    newChatBtn->setCursor(Qt::PointingHandCursor);
    profileLayout->addWidget(newChatBtn);
    connect(newChatBtn, &QPushButton::clicked, this, &MainWindow::openNewChatDialog);

    // Theme toggle \u2014 shows the icon for the destination state (sun while
    // dark, moon while light). Same code path as Ctrl+D and the Settings
    // checkbox; applyTheme keeps all three in sync.
    m_themeBtn = new QPushButton(profileBar);
    m_themeBtn->setObjectName("sbIcon");
    m_themeBtn->setFixedSize(30, 30);
    m_themeBtn->setFocusPolicy(Qt::NoFocus);
    m_themeBtn->setToolTip(tr("Toggle dark/light theme (Ctrl+D)"));
    m_themeBtn->setCursor(Qt::PointingHandCursor);
    m_themeBtn->setText(QStringLiteral("\uE790"));   // color/palette swatch
    m_themeBtn->setToolTip(tr("Theme: %1 (Ctrl+D to cycle)")
                               .arg(PainterTheme::themeLabel(m_themeId)));
    profileLayout->addWidget(m_themeBtn);
    connect(m_themeBtn, &QPushButton::clicked, this,
            [this]() { applyThemeId(PainterTheme::cycle(m_themeId)); });

    m_settingsBtn = new QPushButton(QStringLiteral("\uE713"), profileBar);     // Settings (gear)
    auto *settingsBtn = m_settingsBtn;
    settingsBtn->setObjectName("sbIcon");
    settingsBtn->setFixedSize(30, 30);
    settingsBtn->setFocusPolicy(Qt::NoFocus);
    settingsBtn->setToolTip(tr("Settings"));
    settingsBtn->setCursor(Qt::PointingHandCursor);
    profileLayout->addWidget(settingsBtn);

    connect(settingsBtn, &QPushButton::clicked, this, [this]() {
        ensureSettingsDialog();
        m_settingsDialog->refresh();
        m_settingsDialog->exec();
    });

    sidebarLayout->addWidget(profileBar);
    sidebarLayout->addWidget(searchRow);

    m_profileAvatarLabel = profileAvatar;
    m_profileAvatarLabel->setCursor(Qt::PointingHandCursor);
    // Make avatar clickable — open settings on click
    m_profileAvatarLabel->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_profileAvatarLabel->installEventFilter(this);

    // Update profile when auth changes
    connect(m_auth, &AuthManager::loggedInChanged, this, [this]() {
        if (m_auth->isLoggedIn()) {
            m_profileNameLabel->setText(m_auth->displayName());
            loadProfileAvatar(m_profileAvatarLabel);
        }
    });

    // Also reload avatar/name once user info is fetched (userId may arrive late)
    connect(m_auth, &AuthManager::userInfoChanged, this, [this]() {
        if (m_auth->isLoggedIn()) {
            m_profileNameLabel->setText(m_auth->displayName());
            if (!m_auth->userId().isEmpty())
                loadProfileAvatar(m_profileAvatarLabel);
        }
    });

    // Also set immediately if already logged in
    if (m_auth->isLoggedIn()) {
        m_profileNameLabel->setText(m_auth->displayName());
        loadProfileAvatar(m_profileAvatarLabel);
    }

    connect(m_userStatus, &UserStatusManager::statusChanged,
            this, &MainWindow::refreshStatusIndicator);
    refreshStatusIndicator();

    m_sidebar = new SidebarPainter(sidebarCol);
    m_sidebar->setModel(m_conversations);
    m_sidebar->setApi(m_api);
    m_sidebar->setSignaling(m_signaling);
    m_sidebar->setTheme(m_themeId);
    sidebarLayout->addWidget(m_sidebar, 1);

    connect(m_searchField, &QLineEdit::textChanged, m_sidebar, &SidebarPainter::setFilterText);
    connect(m_sidebar, &SidebarPainter::conversationClicked, this, &MainWindow::onConversationSelected);

    // ── Restore persisted sort / filter, sync menu checks, wire changes ──
    m_settings.beginGroup("Sidebar");
    int savedSort = m_settings.value("sortMode", SidebarPainter::SortRecent).toInt();
    int savedFilter = m_settings.value("filterMode", SidebarPainter::FilterAll).toInt();
    m_settings.endGroup();
    if (savedSort < SidebarPainter::SortRecent || savedSort > SidebarPainter::SortName)
        savedSort = SidebarPainter::SortRecent;
    if (savedFilter < SidebarPainter::FilterAll || savedFilter > SidebarPainter::FilterGroups)
        savedFilter = SidebarPainter::FilterAll;
    m_sidebar->setSortMode(savedSort);
    m_sidebar->setFilterMode(savedFilter);
    for (QAction *a : sortGroup->actions())
        if (a->data().toInt() == savedSort) a->setChecked(true);
    for (QAction *a : filterGroup->actions())
        if (a->data().toInt() == savedFilter) a->setChecked(true);

    connect(sortGroup, &QActionGroup::triggered, this, [this](QAction *a) {
        const int m = a->data().toInt();
        m_sidebar->setSortMode(m);
        m_settings.beginGroup("Sidebar");
        m_settings.setValue("sortMode", m);
        m_settings.endGroup();
        restyleChrome();
    });
    connect(filterGroup, &QActionGroup::triggered, this, [this](QAction *a) {
        const int m = a->data().toInt();
        m_sidebar->setFilterMode(m);
        m_settings.beginGroup("Sidebar");
        m_settings.setValue("filterMode", m);
        m_settings.endGroup();
        restyleChrome();
    });

    // Home button and clickable-logo → return to welcome screen
    connect(m_homeBtn, &QPushButton::clicked, m_sidebar, &SidebarPainter::homeRequested);
    connect(m_sidebar, &SidebarPainter::homeRequested, this, [this]() {
        m_sidebar->setSelectedIndex(-1);
        if (m_chatPainter->selectionMode())
            m_chatPainter->exitSelectionMode();
        closeThread();
        // Drop topic-mode state. Otherwise the "All messages / General / …"
        // tab bar from the previous group chat stays visible over the welcome
        // screen until the user picks another conversation.
        updateTopicMode(false);
        // Clear the active conversation in the message model (empty token = no conversation)
        m_messages->setConversationToken(QString());
        m_activeConvToken.clear();
        m_chatPainter->hide();
        if (m_composer) m_composer->hide();
        m_header->hide();
        showWelcome();
        if (m_updateBannerActive) {
            m_updateBanner->show();
            m_updateBanner->raise();
        }
    });
    connect(m_sidebar, &SidebarPainter::contextMenuRequested, this, [this](int modelIndex, int notifLevel, qreal, qreal) {
        auto *menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        auto *action = menu->addAction(notifLevel == 3 ? "Unmute" : "Mute");
        connect(action, &QAction::triggered, this, [this, modelIndex, notifLevel]() {
            int newLevel = (notifLevel == 3) ? 0 : 3;
            m_conversations->setNotificationLevel(modelIndex, newLevel);
        });
        menu->popup(QCursor::pos());
    });

    sidebarCol->setMinimumWidth(200);
    sidebarCol->setMaximumWidth(500);

    // ── Topics panel ──
    m_threadsPanel = new QWidget(m_chatPage);
    auto *threadsLayout = new QVBoxLayout(m_threadsPanel);
    threadsLayout->setContentsMargins(0, 0, 0, 0);
    threadsLayout->setSpacing(0);

    m_threadsPainter = new ThreadsPainter(m_threadsPanel);
    m_threadsPainter->setModel(m_threads);
    m_threadsPainter->setTheme(m_themeId);
    m_threadsPainter->setSelectedThreadId(-1);
    threadsLayout->addWidget(m_threadsPainter, 1);
    m_threadsPanel->hide();

    connect(m_threadsPainter, &ThreadsPainter::backClicked, this, [this]() {
        m_sidebarSqueezed = false;
        sidebarSqueezedChanged();
    });

    connect(m_threadsPainter, &ThreadsPainter::threadClicked, this, [this](int threadId, const QString &title) {
        if (threadId == 0) {
            closeThread();
            m_messages->setHideThreadMessages(true);
        } else {
            openThread(threadId, title);
            m_messages->setHideThreadMessages(false);
        }
        m_threads->selectTopic(threadId);
        m_activeThreadColor = m_threads->colorForThread(threadId);
        m_header->setActiveThreadColor(m_activeThreadColor);
    });

    connect(m_threadsPainter, &ThreadsPainter::newTopicClicked,
            this, &MainWindow::createNewTopic);

    // ── Chat area ──
    auto *chatCol = new QWidget(m_chatPage);
    auto *chatLayout = new QVBoxLayout(chatCol);
    chatLayout->setContentsMargins(0, 0, 0, 0);
    chatLayout->setSpacing(0);

    m_header = new HeaderPainter(chatCol);
    m_header->setTheme(m_themeId);
    m_header->setApi(m_api);
    m_header->setSignaling(m_signaling);
    chatLayout->addWidget(m_header);

    // Topic tabs (Telegram-style horizontal strip below the header).
    m_topicTabBar = new TopicTabBar(chatCol);
    m_topicTabBar->setModel(m_threads);
    m_topicTabBar->setTheme(m_themeId);   // bug 10 — theme the chips from PainterTheme
    chatLayout->addWidget(m_topicTabBar);
    connect(m_topicTabBar, &TopicTabBar::threadSelected, this,
            [this](int threadId, const QString &title) { openThread(threadId, title); });
    connect(m_topicTabBar, &TopicTabBar::allMessagesSelected, this,
            &MainWindow::closeThread);
    connect(m_topicTabBar, &TopicTabBar::newTopicRequested,
            this, &MainWindow::createNewTopic);

    connect(m_header, &HeaderPainter::expandSidebarClicked, this, [this]() {
        m_sidebarSqueezed = false;
        sidebarSqueezedChanged();
    });
    connect(m_header, &HeaderPainter::backClicked, this, &MainWindow::closeThread);
    connect(m_header, &HeaderPainter::audioCallClicked, this, [this]() {
        m_callManager->setRemotePeerInfo(m_header->conversationName(), m_header->conversationUserId());
        m_callManager->startCall(m_messages->conversationToken(), false);
    });
    connect(m_header, &HeaderPainter::videoCallClicked, this, [this]() {
        m_callManager->setRemotePeerInfo(m_header->conversationName(), m_header->conversationUserId());
        m_callManager->startCall(m_messages->conversationToken(), true);
    });

    // ── Mission Control home: persistent host; content rebuilt per theme ──
    m_welcomeWidget = new QWidget(chatCol);
    m_welcomeWidget->setObjectName("welcomeHost");
    auto *welcomeHostLayout = new QVBoxLayout(m_welcomeWidget);
    welcomeHostLayout->setContentsMargins(0, 0, 0, 0);
    welcomeHostLayout->setSpacing(0);
    chatLayout->addWidget(m_welcomeWidget, 1);
    // Telemetry stays live across theme rebuilds (UniqueConnection keeps a
    // single connection; refreshWelcomeStatus guards on the rebuilt labels).
    connect(m_signaling, &SignalingClient::connectedChanged, this,
            &MainWindow::refreshWelcomeStatus, Qt::UniqueConnection);
    connect(m_push, &PushClient::connectedChanged, this,
            &MainWindow::refreshWelcomeStatus, Qt::UniqueConnection);
    buildWelcomeContent();

    // Give MessageListModel access to ConversationListModel so it can snapshot
    // the per-user lastReadMessage on conversation switch (for the unread divider).
    m_messages->setConversationListModel(m_conversations);

    // Chat content (hidden until conversation selected)
    m_chatPainter = new ChatPainter(chatCol);
    m_chatPainter->setModel(m_messages);
    m_chatPainter->setMyUserId(m_auth->userId());
    m_chatPainter->setSignaling(m_signaling);
    m_chatPainter->setTheme(m_themeId);
    m_chatPainter->setFontScale(m_fontScale);
    m_chatPainter->hide();
    chatLayout->addWidget(m_chatPainter, 1);

    // ── Connection-status strip (Telegram-style "Connecting…"; restyleChrome) ──
    // Deliberately quiet: a thin, centred, muted bar — NOT an alarming error
    // panel — so it reads as "reconnecting in the background." It never grabs
    // focus or disables a widget, so the cached chat list + history stay fully
    // usable while offline.
    m_offlineBanner = new QWidget(chatCol);
    m_offlineBanner->setObjectName("offlineRoot");
    m_offlineBanner->hide();
    m_offlineBanner->setFixedHeight(26);
    auto *offLay = new QHBoxLayout(m_offlineBanner);
    offLay->setContentsMargins(12, 0, 12, 0);
    offLay->setSpacing(0);
    m_offlineLabel = new QLabel(m_offlineBanner);
    m_offlineLabel->setObjectName("offlineLabel");
    m_offlineLabel->setAlignment(Qt::AlignCenter);
    m_offlineLabel->setText(tr("Connecting…"));
    offLay->addWidget(m_offlineLabel, 1);
    chatLayout->insertWidget(0, m_offlineBanner);

    // Animate the trailing dots (Telegram cycles "Connecting" with 1–3 dots) so
    // the strip reads as actively working, not stuck. Runs only while offline.
    m_offlineAnimTimer.setInterval(450);
    connect(&m_offlineAnimTimer, &QTimer::timeout, this, [this]{
        if (!m_offlineLabel) return;
        m_offlineDots = (m_offlineDots % 3) + 1;
        m_offlineLabel->setText(tr("Connecting") + QString(m_offlineDots, QChar(u'.')));
    });

    // React to live reachability changes from the REST layer (the
    // authoritative "server connection working" signal).
    connect(m_api, &ApiClient::serverReachabilityChanged, this,
            &MainWindow::onServerReachabilityChanged, Qt::UniqueConnection);
    // Reflect whatever state the ApiClient already holds (e.g. it went offline
    // before the chat page was built).
    if (m_api && !m_api->isServerReachable())
        onServerReachabilityChanged(false);

    // ── Auto-update banner (prominent; styled by restyleChrome from tokens) ──
    m_updateBanner = new QWidget(chatCol);
    m_updateBanner->setObjectName("ubRoot");
    m_updateBanner->hide();
    m_updateBanner->setFixedHeight(52);
    auto *ubLay = new QHBoxLayout(m_updateBanner);
    ubLay->setContentsMargins(18, 6, 12, 6);
    ubLay->setSpacing(12);

    auto *ubGlyph = new QLabel(QStringLiteral(""), m_updateBanner);  // download
    ubGlyph->setObjectName("ubGlyph");
    ubLay->addWidget(ubGlyph);

    m_updateLabel = new QLabel(m_updateBanner);
    m_updateLabel->setObjectName("ubLabel");
    m_updateLabel->setTextFormat(Qt::RichText);
    ubLay->addWidget(m_updateLabel, 1);

    m_updateProgress = new QProgressBar(m_updateBanner);
    m_updateProgress->setObjectName("ubProgress");
    m_updateProgress->setRange(0, 100);
    m_updateProgress->setFixedWidth(200);
    m_updateProgress->hide();
    ubLay->addWidget(m_updateProgress);

    m_updateWhatsNewBtn = new QPushButton(tr("What's new"), m_updateBanner);
    m_updateWhatsNewBtn->setObjectName("ubGhost");
    m_updateWhatsNewBtn->setFlat(true);
    m_updateWhatsNewBtn->setCursor(Qt::PointingHandCursor);
    ubLay->addWidget(m_updateWhatsNewBtn);

    m_updateInstallBtn = new QPushButton(tr("Install now"), m_updateBanner);
    m_updateInstallBtn->setObjectName("ubInstall");
    m_updateInstallBtn->setCursor(Qt::PointingHandCursor);
    ubLay->addWidget(m_updateInstallBtn);

    m_updateLaterBtn = new QPushButton(tr("Later"), m_updateBanner);
    m_updateLaterBtn->setObjectName("ubGhost");
    m_updateLaterBtn->setFlat(true);
    m_updateLaterBtn->setCursor(Qt::PointingHandCursor);
    ubLay->addWidget(m_updateLaterBtn);

    m_updateCloseBtn = new QPushButton(QStringLiteral("\u2715"), m_updateBanner);
    m_updateCloseBtn->setObjectName("ubClose");
    m_updateCloseBtn->setFlat(true);
    m_updateCloseBtn->setFixedSize(26, 26);
    m_updateCloseBtn->setCursor(Qt::PointingHandCursor);
    ubLay->addWidget(m_updateCloseBtn);

    chatLayout->insertWidget(0, m_updateBanner);

    buildSearchBar(chatCol);

    // Upload progress bar
    m_uploadBar = new QWidget(chatCol);
    m_uploadBar->setObjectName(QStringLiteral("uploadBar"));
    m_uploadBar->setFixedHeight(36);   // surface from restyleChrome (theme)
    m_uploadBar->hide();
    auto *uploadOuterLayout = new QVBoxLayout(m_uploadBar);
    uploadOuterLayout->setContentsMargins(0, 0, 0, 0);
    uploadOuterLayout->setSpacing(0);

    auto *uploadRow = new QWidget(m_uploadBar);
    auto *uploadLayout = new QHBoxLayout(uploadRow);
    uploadLayout->setContentsMargins(16, 6, 16, 4);
    uploadLayout->setSpacing(8);
    auto *clipIcon = new QLabel(QStringLiteral("\U0001F4CE"), uploadRow);
    clipIcon->setStyleSheet("font-size: 14px; background: transparent;");
    uploadLayout->addWidget(clipIcon);
    m_uploadLabel = new QLabel(uploadRow);
    m_uploadLabel->setProperty("role", "secondary");
    m_uploadLabel->setStyleSheet("font-size: 12px; background: transparent;");
    uploadLayout->addWidget(m_uploadLabel, 1);
    auto *percentLabel = new QLabel(uploadRow);
    percentLabel->setProperty("role", "success");
    percentLabel->setStyleSheet("font-size: 12px; font-weight: 600; background: transparent;");
    uploadLayout->addWidget(percentLabel);
    uploadOuterLayout->addWidget(uploadRow, 1);

    // Accent progress line (themed in restyleChrome via #uploadProgress).
    m_uploadProgress = new QWidget(m_uploadBar);
    m_uploadProgress->setObjectName(QStringLiteral("uploadProgress"));
    m_uploadProgress->setGeometry(0, 34, 0, 2);

    chatLayout->addWidget(m_uploadBar);

    connect(m_messages, &MessageListModel::uploadProgressChanged, this, [this, percentLabel]() {
        double progress = m_messages->uploadProgress();
        if (progress < 0) {
            m_uploadBar->hide();
            return;
        }
        m_uploadBar->show();
        m_uploadLabel->setText(m_messages->uploadFileName());
        percentLabel->setText(QStringLiteral("%1%").arg(qRound(progress * 100)));
        // Use the full upload bar width for the progress line
        int barW = qRound(m_uploadBar->width() * qMax(0.0, progress));
        m_uploadProgress->setGeometry(0, m_uploadBar->height() - 2, barW, 2);
    });

    m_composer = new ComposerWidget(chatCol);
    m_composer->setSignaling(m_signaling);
    m_composer->setMessageModel(m_messages);
    m_composer->hide();
    chatLayout->addWidget(m_composer);

    // Restore the saved zoom through the SAME path a manual zoom uses, now
    // that both the chat painter and composer exist. Previously only the
    // chat painter got m_fontScale at startup (line ~577); the composer
    // stayed at 1.0 until the user pressed a zoom shortcut again.
    applyFontScale(m_fontScale);

    m_selectionBar = new SelectionBarWidget(chatCol);
    m_selectionBar->hide();
    chatLayout->addWidget(m_selectionBar);

    connect(m_composer, &ComposerWidget::sendMessage, this, [this](const QString &text, bool silent) {
        // 0.40.9 — only pass `replyTo` when the user explicitly clicked
        // "Reply" on a specific message. Topic membership goes through
        // m_messages->m_threadId (set by openThread) and the server-side
        // `threadId` parameter, so we no longer overload `replyTo` with
        // the thread root id — that produced a fake "↳ replying to
        // 📌 Refunds" badge on every topic message.
        m_messages->sendMessage(text, m_replyToId, silent);
        m_replyToId = 0;
        m_replyToAuthor.clear();
        m_replyToText.clear();
        m_composer->hideReplyBar();
    });
    connect(m_composer, &ComposerWidget::scheduleRequested, this,
            [this](const QString &text, qint64 sendAt, bool silent) {
        // scheduleMessage already wires m_threadId on the body itself,
        // so the same rule applies: replyTo only on explicit replies.
        m_messages->scheduleMessage(text, sendAt, m_replyToId, silent);
        m_replyToId = 0;
        m_replyToAuthor.clear();
        m_replyToText.clear();
        m_composer->hideReplyBar();
    });
    connect(m_composer, &ComposerWidget::manageScheduledRequested, this, [this]() {
        if (!m_messages || m_messages->conversationToken().isEmpty()) return;
        auto *dlg = new ScheduledMessagesDialog(m_api, m_messages->conversationToken(), this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->open();  // non-modal; user can keep typing while reviewing the queue
    });
    connect(m_messages, &MessageListModel::messageScheduled, this,
            [this](qint64 sendAt) {
        // Quick visual confirmation — the scheduled message won't appear in
        // the chat until the server delivers it, so users need something
        // immediate to know the schedule landed. A tooltip near the send
        // button is unobtrusive and self-dismissing.
        const QString when = QDateTime::fromSecsSinceEpoch(sendAt)
                                .toString(QStringLiteral("ddd dd MMM, HH:mm"));
        QToolTip::showText(QCursor::pos(),
                           tr("✓ Scheduled for %1").arg(when),
                           m_composer, QRect(), 3000);
    });
    connect(m_composer, &ComposerWidget::replyBarCancelled, this, [this]() {
        m_replyToId = 0;
        m_replyToAuthor.clear();
        m_replyToText.clear();
    });
    connect(m_composer, &ComposerWidget::editMessageRequested, this,
            [this](const QString &newText) {
        if (m_editingMessageId <= 0) return;
        m_messages->editMessage(m_editingMessageId, newText);
        m_editingMessageId = 0;
    });
    connect(m_composer, &ComposerWidget::editingBarCancelled, this, [this]() {
        m_editingMessageId = 0;
    });
    // Up-arrow on an empty composer → edit the newest own editable message
    // (Telegram-style). Skip system rows, files (not editable), and unacked
    // optimistic rows. The server enforces the edit time-window; if it's too
    // old the edit PUT fails and editMessage surfaces the error.
    connect(m_composer, &ComposerWidget::editLastRequested, this, [this]() {
        if (!m_messages || !m_auth) return;
        const QString self = m_auth->userId();
        const int rows = m_messages->rowCount();
        for (int i = 0; i < rows; ++i) {   // index 0 = newest
            const QModelIndex idx = m_messages->index(i);
            if (m_messages->data(idx, MessageListModel::ActorIdRole).toString() != self)
                continue;
            if (m_messages->data(idx, MessageListModel::IsSystemRole).toBool())
                continue;
            if (m_messages->data(idx, MessageListModel::HasFileRole).toBool())
                continue;   // edit isn't allowed on file messages
            const int id = m_messages->data(idx, MessageListModel::IdRole).toInt();
            if (id <= 0)
                continue;   // optimistic/not-yet-acked row
            QTextDocument doc;
            doc.setHtml(m_messages->data(idx, MessageListModel::MessageTextRole).toString());
            const QString plain = doc.toPlainText().trimmed();
            if (plain.isEmpty())
                continue;
            m_editingMessageId = id;
            m_composer->showEditingBar(plain);
            break;
        }
    });
    // Dismiss the "New messages" divider the moment the user engages with
    // the composer (click, focus, type, or send).
    connect(m_composer, &ComposerWidget::userInteracted,
            m_chatPainter, &ChatPainter::dismissUnreadSeparator);
    connect(m_composer, &ComposerWidget::sendMessage,
            m_chatPainter, [this](const QString &, bool) {
        m_chatPainter->dismissUnreadSeparator();
    });
    // Dismissing the divider visually isn't enough: the server still has
    // the old lastReadMessage, so a chat switch round-trips through
    // setUnreadBoundary and re-shows the divider. Advance the server marker
    // so the next conversation refresh records the chat as fully read.
    connect(m_chatPainter, &ChatPainter::unreadSeparatorDismissed,
            this, [this]() {
        if (m_messages) m_messages->markAsRead();
    });

    // Drag-and-drop files onto chat → show confirmation in composer
    connect(m_chatPainter, &ChatPainter::fileDropped, m_composer, &ComposerWidget::showPendingFile);

    // Infinite scroll: load older history when user scrolls near top.
    connect(m_chatPainter, &ChatPainter::moreHistoryRequested, m_messages, &MessageListModel::loadHistory);

    // Selection mode
    connect(m_chatPainter, &ChatPainter::selectionModeChanged, this, [this](bool active) {
        if (active) {
            m_composer->hide();
            m_selectionBar->show();
            m_chatPainter->setFocus();
        } else {
            m_selectionBar->hide();
            if (m_chatMode)
                m_composer->show();
        }
    });

    connect(m_chatPainter, &ChatPainter::selectionChanged, this, [this](int count) {
        m_selectionBar->setCount(count);
        m_selectionBar->setDeleteVisible(m_chatPainter->allSelectedOwn());
    });

    connect(m_selectionBar, &SelectionBarWidget::cancelClicked, this, [this]() {
        m_chatPainter->exitSelectionMode();
    });

    connect(m_selectionBar, &SelectionBarWidget::copyClicked, this, [this]() {
        auto messages = m_chatPainter->selectedMessages();
        QString text;
        for (const auto &msg : messages) {
            QString body = plainBodyText(msg);
            text += QStringLiteral("[%1, %2]\n%3\n\n")
                .arg(msg.value("actorName").toString(),
                     msg.value("timeString").toString(),
                     body);
        }
        if (!text.isEmpty())
            QApplication::clipboard()->setText(text.trimmed());
        m_chatPainter->exitSelectionMode();
    });

    connect(m_selectionBar, &SelectionBarWidget::deleteClicked, this, [this]() {
        auto messages = m_chatPainter->selectedMessages();
        int count = messages.size();
        auto reply = QMessageBox::question(this,
            QStringLiteral("Delete %1 message%2").arg(count).arg(count == 1 ? "" : "s"),
            QStringLiteral("Are you sure you want to delete %1 message%2?")
                .arg(count).arg(count == 1 ? "" : "s"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            for (const auto &msg : messages)
                m_messages->deleteMessage(msg.value("messageId").toInt());
            m_chatPainter->exitSelectionMode();
        }
    });

    connect(m_selectionBar, &SelectionBarWidget::forwardClicked, this, [this]() {
        auto messages = m_chatPainter->selectedMessages();
        if (messages.isEmpty()) return;

        auto *picker = new ConversationPickerDialog(m_conversations, m_activeConvToken, this);
        if (picker->exec() == QDialog::Accepted) {
            QString targetToken = picker->selectedToken();
            for (const auto &msg : messages) {
                QString body = plainBodyText(msg);
                if (!body.isEmpty())
                    m_messages->sendMessageToToken(targetToken, body);
            }
            m_chatPainter->exitSelectionMode();
        }
        picker->deleteLater();
    });

    auto *copyShortcut = new QShortcut(QKeySequence::Copy, m_chatPainter);
    connect(copyShortcut, &QShortcut::activated, this, [this]() {
        if (m_chatPainter->selectionMode())
            emit m_selectionBar->copyClicked();
    });

    // Right-click context menu on messages
    connect(m_chatPainter, &ChatPainter::contextMenuRequested, this, [this](const QVariantMap &msg, const QPoint &globalPos) {
        int msgId = msg.value("messageId").toInt();
        bool isOwn = msg.value("isOwn").toBool();
        QString text = msg.value("messageText").toString();
        QString author = msg.value("actorName").toString();
        int fileId = msg.value("fileId").toInt();
        QString fileName = msg.value("fileName").toString();
        bool hasFile = msg.value("hasFile").toBool();
        QString fileMime = msg.value("fileMime").toString();

        PainterTheme th(m_themeId, m_fontScale);
        auto hx = [](const QColor &c){ return c.name(QColor::HexRgb); };

        auto *menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->setStyleSheet(QStringLiteral(
            "QMenu {"
            "  background: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 10px;"
            "  padding: 6px 0;"
            "}"
            "QMenu::item {"
            "  padding: 6px 16px 6px 12px;"
            "  color: %3;"
            "  font-size: 13px;"
            "}"
            "QMenu::item:selected {"
            "  background: %4;"
            "  color: %5;"
            "}"
            "QMenu::separator {"
            "  height: 1px;"
            "  background: %2;"
            "  margin: 4px 8px;"
            "}"
        ).arg(hx(th.bgSecondary), hx(th.divider), hx(th.textSecondary),
              hx(th.bgHover), hx(th.textPrimary)));

        // Emoji quick-react row
        auto *emojiRow = new QWidgetAction(menu);
        auto *emojiWidget = new QWidget(menu);
        emojiWidget->setStyleSheet(QStringLiteral("background: %1; border-radius: 8px; margin: 2px;")
            .arg(hx(th.bgSurface)));
        auto *emojiLayout = new QHBoxLayout(emojiWidget);
        emojiLayout->setContentsMargins(6, 4, 6, 4);
        emojiLayout->setSpacing(2);
        QStringList emojis = {"\U0001F44D", "\u2764\uFE0F", "\U0001F602", "\U0001F62E", "\U0001F622", "\U0001F389"};
        for (const auto &emoji : emojis) {
            auto *btn = new QPushButton(emoji, emojiWidget);
            btn->setFixedSize(34, 34);
            btn->setFlat(true);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setStyleSheet(QStringLiteral(
                "QPushButton { font-size: 18px; border: none; border-radius: 8px; background: transparent; }"
                "QPushButton:hover { background: %1; }"
            ).arg(hx(th.bgHover)));
            connect(btn, &QPushButton::clicked, this, [this, msgId, emoji, menu]() {
                m_messages->addReaction(msgId, emoji);
                menu->close();
            });
            emojiLayout->addWidget(btn);
        }
        emojiRow->setDefaultWidget(emojiWidget);
        menu->addAction(emojiRow);
        menu->addSeparator();

        // File actions
        if (hasFile) {
            menu->addAction(QStringLiteral("\u2B07\uFE0F  Download"), this, [this, fileId, fileName]() {
                m_messages->downloadFile(fileId, fileName);
            });
            menu->addAction(QStringLiteral("\u2601\uFE0F  Open in Nextcloud"), this, [this, fileId]() {
                QDesktopServices::openUrl(QUrl(m_api->serverUrl() + "/f/" + QString::number(fileId)));
            });
        }

        // Standard actions
        menu->addAction(QStringLiteral("\U0001F4CB  Copy"), this, [text]() {
            QString plain = text;
            static const QRegularExpression htmlRe("<[^>]*>");
            plain.remove(htmlRe);
            QApplication::clipboard()->setText(plain);
        });
        menu->addAction(QStringLiteral("\u21A9\uFE0F  Reply"), this, [this, msgId, author, text]() {
            m_replyToId = msgId;
            m_replyToAuthor = author;
            m_replyToText = text;
            // Show the reply preview bar above the composer \u2014 same as the
            // hover-icon path. Without this the reply was armed but invisible,
            // so it looked like nothing happened (field report 2026-06-04).
            QString plain = text;
            static const QRegularExpression htmlRe("<[^>]*>");
            plain.remove(htmlRe);
            if (plain.length() > 60) plain = plain.left(60) + "...";
            m_composer->showReplyBar(author, plain);
            m_composer->setFocus();
        });
        menu->addAction(QStringLiteral("\u2197\uFE0F  Forward"), this, [this, msg]() {
            QString body = plainBodyText(msg);
            if (body.isEmpty()) return;
            auto *picker = new ConversationPickerDialog(m_conversations, m_activeConvToken, this);
            if (picker->exec() == QDialog::Accepted) {
                m_messages->sendMessageToToken(picker->selectedToken(), body);
            }
            picker->deleteLater();
        });
        menu->addAction(QStringLiteral("\U0001F4CC  Pin"), this, [this, msgId]() {
            m_messages->pinMessage(msgId);
        });
        menu->addAction(QStringLiteral("\U0001F517  Copy link"), this, [this, msgId]() {
            QString link = m_messages->messageLink(msgId);
            QApplication::clipboard()->setText(link);
        });
        // "Mark as unread" only makes sense for incoming messages — your own
        // messages are read by definition the moment you send them, and the
        // server's lastReadMessage tracks the current user only.
        if (!isOwn) {
            menu->addAction(QStringLiteral("\U0001F4E9  Mark as unread"), this, [this, msgId]() {
                m_messages->markAsUnread(msgId);
            });
        }
        // Threads are available in every real conversation type — 1:1,
        // group, public. Earlier `>= 2` gate was too conservative: the
        // upstream Talk web client offers threads in 1:1 chats, and our
        // createNewTopic flow is just "send a seed message + name it",
        // which is type-agnostic. Server-side capability gates the rest.
        if (m_header->conversationType() >= 1) {
            menu->addAction(QStringLiteral("\U0001F4AC  Thread"), this, [this, msgId]() {
                openThread(msgId, "Thread");
            });
        }

        auto *remindSubmenu = menu->addMenu(QStringLiteral("\u23F0  Remind me\u2026"));
        remindSubmenu->setStyleSheet(menu->styleSheet());
        struct QuickPick { const char *label; int hours; };
        const auto addQuick = [this, msgId, remindSubmenu](const QString &label,
                                                            const QDateTime &when) {
            remindSubmenu->addAction(label, this, [this, msgId, when]() {
                scheduleReminder(msgId, when);
            });
        };
        QDateTime now = QDateTime::currentDateTime();
        addQuick(tr("In 20 minutes"), now.addSecs(20 * 60));
        addQuick(tr("In 1 hour"),      now.addSecs(60 * 60));
        addQuick(tr("In 3 hours"),     now.addSecs(3 * 60 * 60));
        QDateTime tomorrow = now.addDays(1);
        tomorrow.setTime(QTime(8, 0));
        addQuick(tr("Tomorrow 8:00"), tomorrow);
        QDateTime nextWeek = now.addDays(7 - now.date().dayOfWeek() + 1);   // next Monday
        nextWeek.setTime(QTime(9, 0));
        addQuick(tr("Next Monday 9:00"), nextWeek);
        remindSubmenu->addSeparator();
        remindSubmenu->addAction(tr("Custom time\u2026"), this, [this, msgId]() {
            QDateTime when = askReminderTime();
            if (when.isValid()) scheduleReminder(msgId, when);
        });

        if (isOwn) {
            menu->addSeparator();
            if (!hasFile) {
                menu->addAction(QStringLiteral("\u270F\uFE0F  Edit"), this, [this, msgId, msg]() {
                    m_editingMessageId = msgId;
                    QString plain = plainBodyText(msg);
                    m_composer->showEditingBar(plain);
                });
            }
            menu->addAction(QStringLiteral("\U0001F5D1\uFE0F  Delete"), this, [this, msgId]() {
                auto reply = QMessageBox::question(this, "Delete message",
                    "Are you sure you want to delete this message?",
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (reply == QMessageBox::Yes)
                    m_messages->deleteMessage(msgId);
            });
        }

        menu->addSeparator();
        menu->addAction(QStringLiteral("\u2610  Select"), this, [this, msgId]() {
            m_chatPainter->enterSelectionMode(msgId);
        });

        menu->popup(globalPos);
    });

    // Reply from hover button
    connect(m_chatPainter, &ChatPainter::replyRequested, this, [this](int msgId, const QString &author, const QString &text) {
        m_replyToId = msgId;
        m_replyToAuthor = author;
        m_replyToText = text;
        // Show reply bar in composer
        QString plain = text;
        static const QRegularExpression htmlRe("<[^>]*>");
        plain.remove(htmlRe);
        if (plain.length() > 60) plain = plain.left(60) + "...";
        m_composer->showReplyBar(author, plain);
        m_composer->setFocus();
    });

    // React from hover button — show quick emoji menu next to button
    connect(m_chatPainter, &ChatPainter::reactRequested, this, [this](int msgId, const QPoint &globalPos) {
        PainterTheme th(m_themeId, m_fontScale);
        auto hx = [](const QColor &c){ return c.name(QColor::HexRgb); };

        auto *menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->setStyleSheet(QStringLiteral(
            "QMenu { background: %1; border: 1px solid %2; border-radius: 20px; padding: 4px; }"
        ).arg(hx(th.bgSecondary), hx(th.divider)));
        auto *emojiRow = new QWidgetAction(menu);
        auto *w = new QWidget(menu);
        auto *layout = new QHBoxLayout(w);
        layout->setContentsMargins(4, 2, 4, 2);
        layout->setSpacing(2);
        QStringList emojis = {"\U0001F44D", "\u2764\uFE0F", "\U0001F602", "\U0001F62E", "\U0001F622", "\U0001F389"};
        for (const auto &emoji : emojis) {
            auto *btn = new QPushButton(emoji, w);
            btn->setFixedSize(34, 34);
            btn->setFlat(true);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setStyleSheet(QStringLiteral(
                "QPushButton { font-size: 18px; border: none; border-radius: 8px; }"
                "QPushButton:hover { background: %1; }"
            ).arg(hx(th.bgHover)));
            connect(btn, &QPushButton::clicked, this, [this, msgId, emoji, menu]() {
                m_messages->addReaction(msgId, emoji);
                menu->close();
            });
            layout->addWidget(btn);
        }
        emojiRow->setDefaultWidget(w);
        menu->addAction(emojiRow);
        // Position next to the button, not under it
        menu->popup(QPoint(globalPos.x() + 16, globalPos.y() - 20));
    });

    // Chat mouse interaction — wheel and click are handled by ChatPainter directly
    // Link/file clicks from ChatPainter signals
    connect(m_chatPainter, &ChatPainter::linkActivated, this, [](const QString &url) {
        QDesktopServices::openUrl(QUrl(url));
    });
    connect(m_chatPainter, &ChatPainter::fileClicked, this, [this](int fileId, const QString &mime, const QString &fileName) {
        if (mime.startsWith("image/")) {
            QImage placeholder = m_chatPainter->cachedPreview(fileId);
            if (!m_imageViewer)
                m_imageViewer = new ImageViewerDialog(m_api, nullptr);
            m_imageViewer->setImage(fileId, fileName, placeholder);
            m_imageViewer->show();
            m_imageViewer->raise();
            m_imageViewer->activateWindow();
        } else {
            m_messages->downloadFile(fileId, fileName);
        }
    });
    connect(m_chatPainter, &ChatPainter::reactionClicked, this, [this](int msgId, const QString &emoji) {
        m_messages->addReaction(msgId, emoji);
    });

    // Splitter: sidebar | topics | chat
    m_splitter = new QSplitter(Qt::Horizontal, m_chatPage);
    m_splitter->addWidget(sidebarCol);
    m_splitter->addWidget(m_threadsPanel);
    m_splitter->addWidget(chatCol);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setStretchFactor(2, 1);
    m_splitter->setSizes({280, 0, 700});
    m_splitter->setHandleWidth(1);

    // Restore the previous session's sidebar / chat-pane widths if the user
    // ever dragged the handle. The setSizes() above is the seed; if the
    // QSettings blob is present and not empty, restoreState() overwrites
    // it. On first run (no blob) we keep the defaults. Use
    // splitterMoved for live save instead of waiting for close — a crash
    // or kill loses the new width otherwise.
    {
        const QByteArray saved =
            m_settings.value("WindowGeometry/splitterState").toByteArray();
        if (!saved.isEmpty())
            m_splitter->restoreState(saved);
    }
    connect(m_splitter, &QSplitter::splitterMoved, this,
            [this](int, int) {
        // splitterMoved fires per pixel during drag — collapse to one
        // QSettings write after the user releases by deferring through a
        // 250 ms single-shot timer. Same debounce shape as the
        // background-settings live-apply path.
        if (!m_splitterSaveDebounce) {
            m_splitterSaveDebounce = new QTimer(this);
            m_splitterSaveDebounce->setSingleShot(true);
            m_splitterSaveDebounce->setInterval(250);
            connect(m_splitterSaveDebounce, &QTimer::timeout, this, [this]() {
                if (m_splitter)
                    m_settings.setValue("WindowGeometry/splitterState",
                                        m_splitter->saveState());
            });
        }
        m_splitterSaveDebounce->start();
    });

    mainLayout->addWidget(m_splitter);

    // Initial chrome styling from the active theme (re-applied on theme change
    // by applyThemeId → restyleChrome).
    restyleChrome();

    // ── Model signals ──
    connect(m_messages, &MessageListModel::conversationTokenChanged, this, [this]() {
        closeThread();
        // bug 1 — pin to bottom across the whole open sequence (cache load,
        // refreshLatest reset, and the ~1s-later poll delivery), so a message
        // that lands just after open isn't left below the fold by the
        // open-time reset churn. Cleared automatically on user scroll-up.
        m_chatPainter->pinToBottom();
    });

    // 0.52.7 — coalesced per-topic-unread refresh (see m_topicUnreadDebounce).
    m_topicUnreadDebounce.setSingleShot(true);
    m_topicUnreadDebounce.setInterval(400);
    connect(&m_topicUnreadDebounce, &QTimer::timeout, this, [this]() {
        if (m_threads && m_showTopics)
            m_threads->refresh();
    });
    connect(m_messages, &MessageListModel::newMessagesAtEnd, this, [this]() {
        if (m_chatPainter->atBottom())
            m_chatPainter->scrollToBottom();
        // 0.52.7 — a live message just landed in the open conversation; recompute
        // the per-topic unread so the topic bar's counter updates (it previously
        // only refreshed on conversation-open / window activation, so a reply in
        // another topic while you watched showed no count). Debounced + gated on
        // m_showTopics so a 1:1 room never fetches and rapid batches / your own
        // sends collapse into one trailing fetch (the currently-open topic is also
        // pinned to 0 in fetchThreads, so this can't flicker the topic you're in).
        if (m_showTopics)
            m_topicUnreadDebounce.start();
    });
    connect(m_messages, &MessageListModel::errorOccurred, this, [this](const QString &error) {
        QMessageBox::warning(this, "Error", error);
    });
    connect(m_messages, &MessageListModel::messageSent, this, [this]() {
        m_chatPainter->scrollToBottom();
    });

    connect(m_messages, &MessageListModel::loadingChanged, this, [this]() {
        m_header->setLoading(m_messages->isLoading());
    });

    // Update header peer status when user statuses refresh
    connect(m_conversations, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex &, const QModelIndex &, const QList<int> &roles) {
        if (!roles.contains(ConversationListModel::UserStatusRole) || m_activeConvToken.isEmpty())
            return;
        // Find the active conversation and update the header
        int count = m_conversations->rowCount();
        for (int i = 0; i < count; ++i) {
            QModelIndex idx = m_conversations->index(i, 0);
            if (idx.data(ConversationListModel::TokenRole).toString() == m_activeConvToken) {
                QString status  = idx.data(ConversationListModel::UserStatusRole).toString();
                QString message = idx.data(ConversationListModel::UserStatusMessageRole).toString();
                QString icon    = idx.data(ConversationListModel::UserStatusIconRole).toString();
                m_header->setPeerStatus(status, message, icon);
                break;
            }
        }
    });

    // Topics panel visibility: show in any real conversation (1:1,
    // group, public) the server allows threads in, regardless of
    // whether topics exist yet — otherwise users can't discover the
    // "+ New topic" button to create the first one.
    connect(m_threads, &ThreadListModel::hasTopicsChanged, this, [this]() {
        const bool isRealConv = m_header->conversationType() >= 1;
        const bool active     = isRealConv && m_auth->hasThreadsSupport();
        updateTopicMode(active);
    });

    // Keep header updated with typing/call state
    connect(m_signaling, &SignalingClient::typingUserChanged, this, [this]() {
        m_header->setTypingUser(m_signaling->typingUser());
        bool typing = !m_signaling->typingUser().isEmpty()
                    && m_signaling->typingRoom() == m_messages->conversationToken();
        m_header->setIsTyping(typing);
    });

    connect(m_callManager, &CallManager::stateChanged, this, [this]() {
        m_header->setCallState(m_callManager->state());
    });
    connect(m_callManager, &CallManager::stateChanged,
            this, &MainWindow::maybeLaunchPendingInstaller);
    connect(m_callManager, &CallManager::durationChanged, this, [this]() {
        m_header->setCallDuration(m_callManager->callDuration());
    });

    // Clear any "in call" user-status automation. We listen to
    // callServerLeaveAcked (fires after the server has processed the
    // DELETE /call) rather than callEnded (fires synchronously, the
    // moment the user hangs up — at which point the server may still
    // be transitioning and DELETE /revert/call would 404). callEnded
    // closes the call UI immediately; the revert hooks the right
    // moment server-side. Idempotent if nothing was actually stuck.
    connect(m_callManager, &CallManager::callServerLeaveAcked, this,
            [this]() {
        if (m_userStatus) m_userStatus->revertStuckCall();
        // 0.40.15 — also re-poll peer user-statuses so the conversation
        // list / 1:1 header drops the "in a call" tag on the other party
        // immediately rather than waiting up to 60 s for the next poll.
        // The OTHER party's status reverts server-side independently
        // (their own TalQ does DELETE /revert/call), but the only thing
        // syncing it into our local conversation cache is the 60-s
        // status-poll timer in ConversationListModel. Fire one extra
        // poll right after our own leave-ack so a peer who just left
        // is reflected within ~one round-trip instead of a minute.
        if (m_conversations) m_conversations->refreshUserStatuses();
        // Catch peers who leave a couple of seconds after we do
        // (a 2-party hang-up cascade): one more poll 4 s later.
        QTimer::singleShot(4000, this, [this]() {
            if (m_conversations) m_conversations->refreshUserStatuses();
        });
    });

    // Call dialog (shows/hides automatically via CallManager::stateChanged).
    // Parent is nullptr ON PURPOSE: a Qt::Window with a QWidget parent is
    // an *owned* window on Windows and shares the owner's taskbar button,
    // so when another app covers the call window it can't be raised from
    // the taskbar. A parentless top-level gets its own taskbar button.
    // MainWindow is the app-lifetime singleton that holds this pointer;
    // it is deleted in ~MainWindow.
    m_callWindow = new CallWindow(m_callManager, m_api, nullptr);

    // 0.40.15 — "Open background settings…" entry on the in-call
    // BACKGROUND dropdown jumps to Settings → Audio & Video (tab 0,
    // home of the blur slider + image picker).
    connect(m_callWindow, &CallWindow::backgroundSettingsRequested,
            this, &MainWindow::openSettingsToBackgrounds);

    // Update userId when logged in
    connect(m_auth, &AuthManager::userInfoChanged, this, [this]() {
        m_chatPainter->setMyUserId(m_auth->userId());
    });

    // Update NC/Talk version labels when server info arrives (async after login)
    connect(m_auth, &AuthManager::serverInfoChanged, this, [this]() {
        if (m_welcomeNcLabel) {
            QString ncVer = m_auth->nextcloudVersion();
            m_welcomeNcLabel->setText(ncVer.isEmpty() ? QStringLiteral("Nextcloud")
                                                      : QStringLiteral("Nextcloud ") + ncVer);
        }
        if (m_welcomeTalkLabel) {
            QString talkVer = m_auth->talkVersion();
            m_welcomeTalkLabel->setText(talkVer.isEmpty() ? QStringLiteral("Talk")
                                                          : QStringLiteral("Talk ") + talkVer);
        }
    });

    // Auto-upgrade: share the existing network manager used by ApiClient.
    m_updateChecker = new UpdateChecker(m_api->networkAccessManager(), this);

    connect(m_updateChecker, &UpdateChecker::updateAvailable,
            this, [this](const UpdateChecker::Manifest &m) {
        // 0.52.14 — manual "Update now" in flight: cancel the up-to-date fallback,
        // tell the Settings button an update is on the way, and FORCE the download
        // ourselves. The auto-install acceptUpdate() below is gated on
        // autoInstall-enabled / not-cancelled / new-version, so without this an
        // update found via "Update now" with auto-install off (or a re-click of an
        // already-offered version) would never download — leaving the flag stuck
        // true and a later install skipping the idle countdown. acceptUpdate() is
        // idempotent (no-op unless a pending update exists; re-arms the download).
        if (m_userWantsImmediateInstall && m_updateNowChecking) {
            m_updateNowChecking = false;
            if (m_settingsDialog) m_settingsDialog->setUpdateNowStatus(tr("Update found — installing…"));
            if (m_updateChecker) m_updateChecker->acceptUpdate();
        }
        // Reset the self-heal one-shot ONLY when a genuinely new
        // version is offered. Periodic poll re-emits the same
        // manifest; without the version guard, an AV-quarantine
        // environment would silently re-download forever because
        // every re-emit cleared the flag.
        // 0.52.10 — the 5-min poll re-emits the SAME manifest. Gate both the
        // self-heal reset AND the auto-download (below) on a genuinely NEW
        // version, or the auto-installer re-pulls the same ~46 MB installer on
        // every poll until install/restart (the "downloads twice" Kalin saw).
        const bool isNewVersion = (m.version != m_lastOfferedVersion);
        if (isNewVersion) {
            m_updateRelaunchAttempted = false;
            m_lastOfferedVersion = m.version;
        }
        m_pendingUpdateNotes = m.notes;
        // Append a "PRE-RELEASE" emphasis when the offered update came
        // from the beta channel — bold + separator dot, no inline hex
        // (anti-drift: typography carries the signal, not bespoke color).
        QString uText = tr("<b>Update available.</b> TalQ v%1 is ready "
                           "to install.").arg(m.version);
        if (m.prerelease)
            uText += QStringLiteral(" &nbsp;·&nbsp; <b>PRE-RELEASE</b>");
        m_updateLabel->setText(uText);
        m_updateProgress->hide();
        m_updateInstallBtn->setText(tr("Install now"));
        m_updateInstallBtn->show();
        m_updateLaterBtn->show();
        m_updateWhatsNewBtn->show();
        m_updateBannerActive = true;
        m_updateBanner->show();
        m_updateBanner->raise();    // ensure it's above sibling painters on Z-order

        // 0.40.4 — when auto-install is enabled (default ON), kick the
        // download immediately without waiting for a manual "Install
        // now" click. After download completes, onUpdateReadyToLaunch
        // parks the relaunch behind the idle gate. Manual buttons stay
        // visible during the download in case the user wants to cancel
        // the auto-flow for this session — the [Cancel auto-install]
        // path takes over once the download finishes. Auto-download
        // does NOT trigger again if the user already opted out for
        // this session.
        const bool autoInstallEnabled = QSettings()
            .value(QStringLiteral("updates/autoInstall"), true).toBool();
        if (autoInstallEnabled && !m_autoInstallCancelledForSession && isNewVersion) {
            m_updateChecker->acceptUpdate();
        }
    });
    connect(m_updateChecker, &UpdateChecker::downloadProgress,
            this, [this](qreal pct) {
        m_updateProgress->setValue(int(pct));
        m_updateProgress->show();
        m_updateInstallBtn->hide();
        m_updateLaterBtn->hide();
        m_updateWhatsNewBtn->hide();
        m_updateLabel->setText(tr("Downloading update\u2026"));
    });
    connect(m_updateChecker, &UpdateChecker::downloadFailed,
            this, [this](const QString &reason) {
        m_updateLabel->setText(tr("Update failed: %1").arg(reason));
        m_updateProgress->hide();
        m_updateInstallBtn->setText(tr("Retry"));
        m_updateInstallBtn->show();
        m_updateLaterBtn->show();
        m_updateWhatsNewBtn->hide();
        // 0.52.14 — a "Update now" request must not leave its flags armed if the
        // download fails (else a later completed download would skip the idle
        // countdown). Clear them and report back to the Settings button.
        if (m_userWantsImmediateInstall || m_updateNowChecking) {
            m_userWantsImmediateInstall = false;
            m_updateNowChecking = false;
            if (m_settingsDialog) m_settingsDialog->setUpdateNowStatus(tr("Update failed — try again"));
        }
    });
    connect(m_updateChecker, &UpdateChecker::readyToLaunch,
            this, &MainWindow::onUpdateReadyToLaunch);

    connect(m_updateInstallBtn, &QPushButton::clicked, this, [this]() {
        // 0.40.2 —if we're mid-auto-install countdown, "Install now"
        // bypasses the idle gate and triggers immediately. Otherwise
        // it's the classic "accept the offered update" path that kicks
        // off the download.
        if (m_autoInstallActive && !m_pendingInstallerPath.isEmpty()) {
            m_autoInstallActive = false;
            m_autoInstallTick.stop();
            m_updateLabel->setText(tr("Update downloaded — relaunching…"));
            m_updateInstallBtn->hide();
            m_updateLaterBtn->hide();
            m_updateWhatsNewBtn->hide();
            maybeLaunchPendingInstaller();
            return;
        }
        m_updateChecker->acceptUpdate();
    });
    connect(m_updateLaterBtn, &QPushButton::clicked, this, [this]() {
        // 0.40.2 —during the auto-install wait the [Later] button is
        // relabelled "Cancel auto-install". Clicking it stops the
        // countdown for this session but keeps the banner up with the
        // classic [Install now] / [Later] choice so the user can still
        // install on their own schedule.
        if (m_autoInstallActive) {
            m_autoInstallActive = false;
            m_autoInstallCancelledForSession = true;
            m_autoInstallTick.stop();
            m_updateLabel->setText(
                tr("<b>Update ready.</b> Click <i>Install now</i> when "
                   "you're ready."));
            m_updateInstallBtn->setText(tr("Install now"));
            m_updateInstallBtn->show();
            m_updateLaterBtn->setText(tr("Later"));
            return;
        }
        m_updateBannerActive = false;
        m_updateBanner->hide();
        m_updateChecker->deferUpdate();
    });
    connect(m_updateCloseBtn, &QPushButton::clicked, this, [this]() {
        m_updateBannerActive = false;
        m_updateBanner->hide();
        m_updateChecker->deferUpdate();
    });
    connect(m_updateWhatsNewBtn, &QPushButton::clicked, this, [this]() {
        PainterTheme t(m_themeId, m_fontScale);
        auto hx = [](const QColor &c){ return c.name(QColor::HexRgb); };

        auto *dlg = new QDialog(this);
        dlg->setWindowTitle(tr("What's new"));
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->resize(640, 560);
        dlg->setStyleSheet(QString(
            "QDialog{background:%1;}"
            "QPushButton{background:%2;color:%3;border:none;border-radius:8px;"
            "padding:8px 18px;font-size:13px;font-weight:600;}"
            "QPushButton:hover{background:%4;}")
            .arg(hx(t.bgPrimary), hx(t.bgSurface), hx(t.textPrimary),
                 hx(t.bgSecondary)));

        auto *lay = new QVBoxLayout(dlg);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);

        auto *hdr = new QLabel(QStringLiteral("●  WHAT'S NEW"), dlg);
        hdr->setStyleSheet(QString(
            "color:%1;font-size:10px;font-weight:bold;letter-spacing:2px;"
            "padding:16px 26px;border-bottom:1px solid %2;background:%3;")
            .arg(hx(t.textTime), hx(t.divider), hx(t.bgSurface)));
        lay->addWidget(hdr);

        auto *tb = new QTextBrowser(dlg);
        tb->setFrameShape(QFrame::NoFrame);
        tb->setOpenExternalLinks(true);
        tb->setStyleSheet(QString(
            "QTextBrowser{background:%1;color:%2;border:none;"
            "padding:6px 26px 24px;font-size:13px;}")
            .arg(hx(t.bgPrimary), hx(t.textPrimary)));
        // Markdown -> QTextDocument uses minimal default CSS (cramped headings,
        // no theme color). A document style sheet, set BEFORE setMarkdown,
        // gives the release notes real hierarchy and on-theme colors.
        tb->document()->setDefaultStyleSheet(QString(
            "h1{font-size:21px;font-weight:700;color:%1;margin:18px 0 8px;}"
            "h2{font-size:17px;font-weight:700;color:%1;margin:22px 0 8px;}"
            "h3{font-size:13px;font-weight:700;color:%2;margin:16px 0 4px;}"
            "p{color:%1;margin:7px 0;}"
            "li{color:%1;margin:5px 0;}"
            "strong,b{color:%3;font-weight:700;}"
            "code{color:%3;}"
            "a{color:%3;text-decoration:none;}")
            .arg(hx(t.textPrimary), hx(t.textSecondary), hx(t.accent)));
        tb->setMarkdown(m_pendingUpdateNotes);
        tb->setReadOnly(true);
        lay->addWidget(tb, 1);

        auto *footer = new QHBoxLayout();
        footer->setContentsMargins(26, 14, 26, 18);
        footer->addStretch();
        auto *closeBtn = new QPushButton(tr("Close"), dlg);
        closeBtn->setCursor(Qt::PointingHandCursor);
        connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::accept);
        footer->addWidget(closeBtn);
        lay->addLayout(footer);

        dlg->show();
    });

    m_updateChecker->start();

    m_stack->addWidget(m_chatPage);
}

void MainWindow::buildSearchBar(QWidget *chatCol)
{
    m_searchBar = new QWidget(chatCol);
    m_searchBar->hide();
    auto *lay = new QHBoxLayout(m_searchBar);
    lay->setContentsMargins(8, 4, 8, 4);
    m_searchInput = new QLineEdit(m_searchBar);
    m_searchInput->setPlaceholderText(tr("Search in this conversation\u2026"));
    lay->addWidget(m_searchInput, 1);
    auto *closeBtn = new QPushButton("\u2715", m_searchBar);
    closeBtn->setFixedSize(28, 28);
    closeBtn->setFlat(true);
    connect(closeBtn, &QPushButton::clicked, this, [this]() {
        m_searchBar->hide();
        if (m_searchResults) m_searchResults->hide();
    });
    lay->addWidget(closeBtn);

    auto *v = qobject_cast<QVBoxLayout*>(chatCol->layout());
    if (v) v->insertWidget(0, m_searchBar);

    m_searchDebounce = new QTimer(this);
    m_searchDebounce->setSingleShot(true);
    m_searchDebounce->setInterval(300);
    connect(m_searchDebounce, &QTimer::timeout, this, &MainWindow::runSearchQuery);
    connect(m_searchInput, &QLineEdit::textChanged, this, [this](const QString &) {
        m_searchDebounce->start();
    });

    m_searchResults = new QListWidget(this);
    m_searchResults->setWindowFlags(Qt::Popup);
    {
        PainterTheme th(m_themeId, m_fontScale);
        auto hx = [](const QColor &c){ return c.name(QColor::HexRgb); };
        m_searchResults->setStyleSheet(QStringLiteral(
            "QListWidget { background: %1; color: %2; border: 1px solid %3; border-radius: 6px; }"
            "QListWidget::item { padding: 6px 10px; }"
            "QListWidget::item:selected { background: %4; }"
        ).arg(hx(th.bgSecondary), hx(th.textPrimary), hx(th.divider), hx(th.bgSelected)));
    }

    connect(m_searchResults, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) {
        int msgId = it->data(Qt::UserRole).toInt();
        if (msgId <= 0) return;
        m_searchResults->hide();
        m_chatPainter->scrollToMessage(msgId);
        if (!m_messages) return;
        bool foundLocal = false;
        for (int i = 0; i < m_messages->rowCount(); ++i) {
            if (m_messages->data(m_messages->index(i), MessageListModel::IdRole).toInt() == msgId) {
                foundLocal = true; break;
            }
        }
        if (!foundLocal) {
            auto *c = new QMetaObject::Connection;
            *c = connect(m_messages, &MessageListModel::historyUntilSettled, this,
                [this, msgId, c](int settledId, bool ok) {
                    if (settledId != msgId) return;
                    QObject::disconnect(*c);
                    delete c;
                    if (ok) m_chatPainter->scrollToMessage(msgId);
                });
            m_messages->loadHistoryUntil(msgId);
        }
    });

    connect(m_header, &HeaderPainter::searchRequested, this, [this]() {
        m_searchBar->show();
        m_searchInput->setFocus();
        m_searchInput->selectAll();
    });

    connect(m_header, &HeaderPainter::remindersRequested,
            this, &MainWindow::openUpcomingReminders);

    connect(m_header, &HeaderPainter::infoRequested,
            this, &MainWindow::openConversationInfo);

    m_searchInput->installEventFilter(this);
}

void MainWindow::runSearchQuery()
{
    QString q = m_searchInput->text().trimmed();
    if (q.length() < 2 || !m_messages || m_messages->conversationToken().isEmpty()) {
        m_searchResults->hide();
        return;
    }
    QString token = m_messages->conversationToken();
    m_api->searchInConversation(token, q, this,
        [this, token](bool ok, const QVector<SearchHit> &hits) {
            if (!m_messages || m_messages->conversationToken() != token) return;
            if (!ok) { m_searchResults->hide(); return; }
            m_searchResults->clear();
            int limit = qMin(30, int(hits.size()));
            for (int i = 0; i < limit; ++i) {
                const SearchHit &h = hits[i];
                QString rowText = QStringLiteral("%1\n%2").arg(h.actorName, h.snippet);
                auto *it = new QListWidgetItem(rowText);
                it->setData(Qt::UserRole, h.messageId);
                m_searchResults->addItem(it);
            }
            if (m_searchResults->count() == 0) {
                m_searchResults->hide();
                return;
            }
            QPoint p = m_searchInput->mapToGlobal(QPoint(0, m_searchInput->height()));
            m_searchResults->resize(m_searchInput->width(),
                                    qMin(6, m_searchResults->count()) * 46 + 8);
            m_searchResults->move(p);
            m_searchResults->show();
        });
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    // 0.40.16 — TalQ-input idle metric (see ctor comment). Fires for
    // every event flowing through our app event loop; we only timestamp
    // the user-actuated input types. Wheel + clicks + keystrokes; bare
    // MouseMove deliberately excluded so the mouse drifting across a
    // TalQ window edge doesn't reset the countdown.
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::KeyPress:
    case QEvent::Wheel:
        m_lastTalqInputMs = QDateTime::currentMSecsSinceEpoch();
        // bug 13 — local input is the clearest "user is back" signal; restore
        // an auto-set Away to Online immediately (no-op unless auto-away).
        if (m_userStatus)
            m_userStatus->tryRestoreFromAutoAway();
        break;
    default:
        break;
    }

    // Escape in search input — close search bar
    if (obj == m_searchInput && event->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent*>(event);
        if (k->key() == Qt::Key_Escape) {
            m_searchBar->hide();
            if (m_searchResults) m_searchResults->hide();
            return true;
        }
    }
    // Avatar click -> open the status picker (Settings lives on the gear).
    if (obj == m_profileAvatarLabel && event->type() == QEvent::MouseButtonRelease) {
        openStatusPopover();
        return true;
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::sidebarSqueezedChanged()
{
    m_sidebar->setSqueezed(m_sidebarSqueezed);
    m_header->setSidebarSqueezed(m_sidebarSqueezed);

    // In squeezed mode: hide search and settings, keep avatar only
    m_searchField->setVisible(!m_sidebarSqueezed);
    if (m_homeBtn) m_homeBtn->setVisible(!m_sidebarSqueezed);
    m_settingsBtn->setVisible(!m_sidebarSqueezed);
    m_profileNameLabel->setVisible(!m_sidebarSqueezed);
    if (m_statusPill) m_statusPill->setVisible(!m_sidebarSqueezed);

    if (m_sidebarSqueezed) {
        // Center avatar in the narrow bar
        auto *lay = qobject_cast<QHBoxLayout *>(m_profileBar->layout());
        if (lay) lay->setContentsMargins(0, 8, 0, 8);
    } else {
        auto *lay = qobject_cast<QHBoxLayout *>(m_profileBar->layout());
        if (lay) lay->setContentsMargins(12, 8, 12, 8);
    }

    // Adjust sidebar constraints for squeeze mode
    m_sidebarCol->setMinimumWidth(m_sidebarSqueezed ? 56 : 200);
    m_sidebarCol->setMaximumWidth(m_sidebarSqueezed ? 56 : 500);

    int sideW = m_sidebarSqueezed ? 56 : 280;
    int topicsW = m_showTopics ? 240 : 0;
    m_splitter->setSizes({sideW, topicsW, m_splitter->width() - sideW - topicsW});
}

void MainWindow::onConversationSelected(const QString &token, const QString &name,
                                         const QString &userId, int convType,
                                         const QString &userStatus,
                                         const QString &statusMessage,
                                         const QString &statusIcon)
{
    if (m_chatPainter->selectionMode())
        m_chatPainter->exitSelectionMode();

    // 0.52.10 — a "reconnecting" blip re-selects the ALREADY-active conversation;
    // capture that BEFORE updating m_activeConvToken so we can tell a genuine
    // navigation from a programmatic re-select.
    const bool sameConvReselect = (token == m_activeConvToken);
    m_activeConvToken = token;

    // A live call stays watchable while you browse chat: dock it to a compact
    // corner PiP when you navigate into a conversation. Double-click the PiP (or
    // end the call) to bring it back full.
    // 0.52.10 — three guards on when to dock:
    //  (1) ONLY an ACTIVE call. A ringing (Incoming/Connecting) call must present
    //      centered on the main screen, never get yanked to the corner — Pavel saw
    //      an incoming call docked bottom-right.
    //  (2) NOT a fullscreen call (it has its own screen real estate, typically a
    //      2nd monitor — 0.52.7 field bug).
    //  (3) NOT a programmatic re-select of the SAME conversation — that's the
    //      "reconnecting" blip, and it yanked a windowed call to the corner
    //      mid-share (Kalin↔Ilko).
    if (m_callWindow && m_callManager->state() == CallManager::Active
        && !m_callWindow->isFullscreen() && !sameConvReselect)
        m_callWindow->enterPipDock();

    // Switch from welcome to chat
    m_welcomeWidget->hide();
    m_header->show();
    m_chatPainter->show();
    m_composer->show();
    if (m_updateBannerActive) {
        m_updateBanner->show();
        m_updateBanner->raise();
    }

    m_header->setConversationName(name);
    m_header->setConversationUserId(userId);
    m_header->setConversationType(convType);
    m_header->setPeerStatus(userStatus, statusMessage, statusIcon);

    // Pass sidebar's cached avatar to header (avoids duplicate HTTP fetch)
    QString avatarKey = (convType == 1) ? userId : ("room/" + token);
    QImage cached = m_sidebar->cachedAvatar(avatarKey);
    if (!cached.isNull())
        m_header->setAvatarImage(cached);
    m_header->setActiveThreadId(0);
    m_header->setActiveThreadTitle("");
    m_header->setIsInTopicMode(false);
    m_isInTopicMode = false;

    m_conversations->clearUnreadForToken(token);
    // joinRoom handles both REST (/participants/active) and signaling WebSocket
    m_signaling->joinRoom(token);
    m_threads->setConversationType(convType);
    m_threads->setConversationToken(token);
    m_threadsPainter->setGroupName(name);

    m_messages->setThreadId(0);
    m_messages->setHideThreadMessages(false);
    m_messages->setConversationToken(token);

    m_header->setConversationToken(token);
    m_header->setMessageCount(m_messages->rowCount());
    m_header->setCallsAvailable(m_callManager->callsAvailable());
    m_header->setCallsUnavailableReason(m_callManager->callsUnavailableReason());

    // Show the topics panel for any real conversation (1:1, group,
    // public) once the server advertises threads support. Earlier
    // `>= 2` gate excluded 1:1 chats, but topics there are just a
    // sub-conversation between two people — same mechanism, same UX,
    // useful for separating distinct conversation threads with the
    // same contact. The panel hosts the "+ New topic" button — if we
    // wait for m_threads to signal hasTopicsChanged, empty rooms
    // never get it.
    const bool topicsVisible = (convType >= 1) && m_auth->hasThreadsSupport();
    updateTopicMode(topicsVisible);
}

void MainWindow::openThread(int threadId, const QString &title)
{
    m_activeThreadId = threadId;
    m_activeThreadTitle = title;
    m_header->setActiveThreadId(threadId);
    m_header->setActiveThreadTitle(title);
    m_messages->setThreadId(threadId);
    m_composer->setTopicName(title);
    // 0.40.9 — sync the topic-bar selection so the active topic chip is
    // visibly highlighted instead of "All messages" staying lit.
    if (m_topicTabBar) m_topicTabBar->setSelectedThreadId(threadId);
}

void MainWindow::closeThread()
{
    m_activeThreadId = 0;
    m_activeThreadTitle = "";
    m_header->setActiveThreadId(0);
    m_header->setActiveThreadTitle("");
    m_messages->setThreadId(0);
    m_composer->setTopicName("");
    if (m_topicTabBar) m_topicTabBar->setSelectedThreadId(0);
}

void MainWindow::updateTopicMode(bool active)
{
    m_showTopics = active;
    m_isInTopicMode = active;
    m_header->setIsInTopicMode(active);
    m_messages->setHideThreadMessages(active);
    // The old 3rd column is gone — topics live in m_topicTabBar above the
    // chat now. Keep m_threadsPanel hidden unconditionally so the two UIs
    // don't double up.
    m_threadsPanel->setVisible(false);
    if (m_topicTabBar) m_topicTabBar->setVisible(active);

    if (active) {
        m_conversations->setHasTopics(m_activeConvToken, true);
    }
}

void MainWindow::switchToChat()
{
    m_chatMode = true;
    m_stack->setCurrentWidget(m_chatPage);
    restoreChatGeometry();
    m_conversations->refresh();

    // Show the Mission Control home (rebuilds only if a theme change happened
    // while it was hidden) and populate telemetry.
    showWelcome();
    m_chatPainter->hide();
    m_composer->hide();
}

// Repaint the Mission Control board: tile values, status LEDs, system pill.
// Called on entry to the chat page and whenever signaling/push flip.
void MainWindow::refreshWelcomeStatus()
{
    if (!m_welcomeNameLabel) return;   // welcome screen not built yet
    // Don't do off-screen work: while a chat is open the welcome host is
    // hidden, and signaling/push churn must not touch its widgets.
    if (m_welcomeWidget && !m_welcomeWidget->isVisible()) return;
    PainterTheme th(m_themeId, m_fontScale);
    auto hx = [](const QColor &c){ return c.name(QColor::HexRgb); };
    const QString mono = QStringLiteral("'Consolas','Cascadia Mono',monospace");

    const bool sigOn  = m_signaling && m_signaling->isConnected();
    const bool pushOn = m_push && m_push->isConnected();
    const QString gpu = m_callManager ? m_callManager->gpuAccelStatus()
                                      : QStringLiteral("Software only");
    const bool gpuOn  = (gpu != QLatin1String("Software only"));
    // Show ALL real GPUs (a 2-GPU laptop should read "NVIDIA RTX 3070 + Intel
    // UHD"), with the hardware-accel codec on the sub-line. Falls back to the
    // accel string if adapter enumeration is unavailable.
    const QStringList gpuNames = talq::gpuAdapterNames();
    const QString gpuBig = gpuNames.isEmpty() ? gpu
                                              : gpuNames.join(QStringLiteral(" + "));

    QString url = m_auth ? m_auth->serverUrl() : QString();
    url.remove(QRegularExpression(QStringLiteral("^https?://")));

    auto val = [&](const QString &big, const QString &sub) {
        return QString(
            "<span style='font-size:18px;font-weight:600;color:%1'>%2</span>"
            "<br><span style='font-family:%3;font-size:11px;color:%4'>%5</span>")
            .arg(hx(th.textPrimary), big.toHtmlEscaped(), mono,
                 hx(th.textSecondary), sub.toHtmlEscaped());
    };

    // Real reachability — not just "a URL is configured". The ApiClient
    // tracks whether REST calls are actually landing on the server.
    const bool srvOn = !url.isEmpty() && (!m_api || m_api->isServerReachable());

    m_welcomeNameLabel->setText(QStringLiteral("Welcome back, ")
        + (m_auth ? m_auth->displayName() : QString()));
    m_welcomeServerLabel->setText(val(url.isEmpty() ? QStringLiteral("offline") : url,
                                      url.isEmpty() ? QStringLiteral("not connected")
                                      : srvOn       ? QStringLiteral("reachable")
                                                    : QStringLiteral("unreachable")));
    m_welcomeNcLabel->setText(val(m_auth ? m_auth->nextcloudVersion() : QStringLiteral("?"),
                                  QStringLiteral("core api")));
    m_welcomeTalkLabel->setText(val(m_auth ? m_auth->talkVersion() : QStringLiteral("?"),
                                    QStringLiteral("capabilities")));
    m_welcomeSignalingLabel->setText(val(sigOn ? QStringLiteral("Connected")
                                                : QStringLiteral("Disconnected"),
                                         sigOn ? QStringLiteral("HPB realtime")
                                               : QStringLiteral("offline")));
    m_welcomePushLabel->setText(val(pushOn ? QStringLiteral("Real-time")
                                           : QStringLiteral("Polling"),
                                    pushOn ? QStringLiteral("websocket up")
                                           : QStringLiteral("fallback")));
    // Encode-load cap (same rule PublishPipeline applies, via EncodeTier.h): on
    // a weak/iGPU/software encoder TalQ caps the camera SEND resolution so it
    // can't saturate the GPU. Surface it on the GPU tile so the user knows why
    // their outgoing video is limited; the full reason is the tooltip.
    const talq::EncodeTierCap encCap = talq::encodeTierCap(
        m_callManager ? m_callManager->gpuClass() : talq::GpuClass::Software);
    QString gpuSub = gpuOn ? QStringLiteral("hardware accelerated · %1").arg(gpu)
                           : QStringLiteral("software only");
    if (encCap.maxSendHeight > 0)
        gpuSub += QStringLiteral(" · camera ≤%1p").arg(encCap.maxSendHeight);
    m_welcomeGpuLabel->setText(val(gpuBig, gpuSub));
    m_welcomeGpuLabel->setToolTip(encCap.homeText);   // empty = no tooltip

    auto setLed = [&](QLabel *led, bool ok) {
        if (led) led->setStyleSheet(QString("color:%1;font-size:9px;")
                                    .arg(hx(ok ? th.online : th.amber)));
    };
    setLed(m_wcSignalLed, sigOn);
    setLed(m_wcPushLed, pushOn);
    setLed(m_wcGpuLed, gpuOn);

    if (m_wcStatusPill) {
        const bool nominal = srvOn && sigOn && pushOn && gpuOn;
        const QColor c = nominal ? th.online : th.amber;
        m_wcStatusPill->setText(nominal
            ? QStringLiteral("●  ALL SYSTEMS NOMINAL")
            : QStringLiteral("●  DEGRADED"));
        m_wcStatusPill->setStyleSheet(QString(
            "color:%1;font-size:11px;font-weight:bold;letter-spacing:1px;"
            "border:1px solid %1;border-radius:999px;padding:6px 13px;")
            .arg(hx(c)));
    }
}

// React to the REST layer's reachability verdict, Telegram-style: just show or
// hide the quiet "Connecting…" strip and refresh the Home tile. Intentionally
// NO desktop/OS notification — chat apps don't toast you for a transient
// connection blip — and nothing here disables any widget, so the cached chat
// list and history stay fully usable while offline.
void MainWindow::onServerReachabilityChanged(bool online)
{
    if (online) {
        m_offlineAnimTimer.stop();
        if (m_offlineBanner) m_offlineBanner->hide();
    } else {
        m_offlineDots = 0;
        if (m_offlineLabel) m_offlineLabel->setText(tr("Connecting…"));
        if (m_offlineBanner) {
            m_offlineBanner->show();
            m_offlineBanner->raise();   // keep above sibling painter widgets
        }
        if (!m_offlineAnimTimer.isActive()) m_offlineAnimTimer.start();
    }

    refreshWelcomeStatus();   // update the Home "server" tile + status pill
}

// Builds (or rebuilds, on theme change) the Mission Control content inside
// the persistent welcome host. Deleting and recreating the content widget
// keeps every themed stylesheet correct without chasing individual widgets.
void MainWindow::buildWelcomeContent()
{
    if (!m_welcomeWidget) return;
    m_welcomeDirty = false;
    if (m_welcomeContent) { delete m_welcomeContent; m_welcomeContent = nullptr; }

    PainterTheme wt(m_themeId, m_fontScale);
    auto wcss = [](const QColor &c){ return c.name(QColor::HexRgb); };
    const QString wmono = QStringLiteral("'Consolas','Cascadia Mono',monospace");

    auto *root = new QWidget(m_welcomeWidget);
    m_welcomeContent = root;
    root->setObjectName("welcomeRoot");
    root->setStyleSheet(QString(
        "QWidget#welcomeRoot{background:%1;} QLabel{background:transparent;}")
        .arg(wcss(wt.bgPrimary)));
    m_welcomeWidget->layout()->addWidget(root);
    auto *welcomeLayout = new QVBoxLayout(root);
    welcomeLayout->setContentsMargins(34, 26, 38, 22);
    welcomeLayout->setSpacing(15);

    // Command bar: wordmark, build tags, system-status pill.
    auto *cmdBar = new QHBoxLayout();
    cmdBar->setSpacing(10);
    auto *brand = new QLabel(QString(
        "<span style='font-size:19px;font-weight:700;color:%1;'>Tal</span>"
        "<span style='font-size:19px;font-weight:700;color:%2;'>Q</span>")
        .arg(wcss(wt.textPrimary), wcss(wt.accent)), root);
    cmdBar->addWidget(brand);
    auto makeTag = [&](const QString &t) {
        auto *l = new QLabel(t, root);
        l->setStyleSheet(QString(
            "color:%1;font-family:%2;font-size:10px;font-weight:bold;"
            "letter-spacing:1px;border:1px solid %3;border-radius:6px;padding:4px 8px;")
            .arg(wcss(wt.textTime), wmono, wcss(wt.divider)));
        return l;
    };
    cmdBar->addWidget(makeTag("BUILD " + QApplication::applicationVersion()));
#ifdef TALQ_BRAND_123NET
    cmdBar->addWidget(makeTag(QStringLiteral("123NET")));
#endif
    // Per-release codename pill. A celebratory moment, so it wears the warm
    // amber (secondary), not the accent (the accent stays the one "needs
    // you" signal). Hidden when no codename is set for the release.
    const QString verName = QStringLiteral(TALQ_VERSION_NAME);
    if (!verName.isEmpty()) {
        auto *codename = new QLabel(QStringLiteral("✦ ") + verName.toUpper(), root);
        codename->setToolTip(codenameBlurb(verName));   // the real story, not a generic label
        codename->setStyleSheet(QString(
            "color:%1;font-family:%2;font-size:10px;font-weight:bold;"
            "letter-spacing:1px;border:1px solid %1;border-radius:6px;padding:4px 8px;")
            .arg(wcss(wt.amber), wmono));
        cmdBar->addWidget(codename);
    }
    cmdBar->addStretch();
    m_wcStatusPill = new QLabel(QStringLiteral("●  ALL SYSTEMS NOMINAL"), root);
    m_wcStatusPill->setStyleSheet(QString(
        "color:%1;font-size:11px;font-weight:bold;letter-spacing:1px;"
        "border:1px solid %1;border-radius:999px;padding:6px 13px;")
        .arg(wcss(wt.online)));
    cmdBar->addWidget(m_wcStatusPill);
    welcomeLayout->addLayout(cmdBar);

    // Greeting (still the empty state: who you are, what to do next). On the
    // branded build the 123NET logo fills the top-right free space beside the
    // two-line greeting, just below the status pill.
    auto *greetRow = new QHBoxLayout();
    greetRow->setSpacing(16);
    auto *greetCol = new QVBoxLayout();
    greetCol->setSpacing(4);
    m_welcomeNameLabel = new QLabel(root);
    {
        QFont wf = m_welcomeNameLabel->font();
        wf.setPixelSize(25);
        wf.setWeight(QFont::DemiBold);
        m_welcomeNameLabel->setFont(wf);
    }
    m_welcomeNameLabel->setStyleSheet(QString("color:%1;").arg(wcss(wt.textPrimary)));
    greetCol->addWidget(m_welcomeNameLabel);
    auto *subLine = new QLabel(QStringLiteral(
        "No conversation selected. Pick one from the sidebar to jump back in."),
        root);
    subLine->setStyleSheet(QString("font-size:14px;color:%1;").arg(wcss(wt.textSecondary)));
    greetCol->addWidget(subLine);
    greetRow->addLayout(greetCol);
    greetRow->addStretch();
    {
#ifdef TALQ_BRAND_123NET
        QPixmap lp(QStringLiteral(":/123net-logo.png"));
#else
        QPixmap lp(QStringLiteral(":/logo.png"));
#endif
        if (!lp.isNull()) {
            auto *brandLogo = new QLabel(root);
            // Match the two-line greeting height (25px name + 14px sub + gap).
            brandLogo->setPixmap(lp.scaledToHeight(56, Qt::SmoothTransformation));
            greetRow->addWidget(brandLogo, 0, Qt::AlignRight | Qt::AlignVCenter);
        }
    }
    welcomeLayout->addLayout(greetRow);

    // Telemetry grid: SERVER (wide) / SIGNALING / PUSH / NEXTCLOUD / TALK / GPU.
    auto *grid = new QGridLayout();
    grid->setSpacing(11);
    auto makeTile = [&](const QString &key, QLabel **valOut, QLabel **ledOut) {
        auto *tile = new QFrame(root);
        tile->setObjectName("mcTile");
        tile->setStyleSheet(QString(
            "QFrame#mcTile{background:%1;border:1px solid %2;border-radius:13px;}")
            .arg(wcss(wt.bgSurface), wcss(wt.divider)));
        tile->setMinimumHeight(92);
        auto *tl = new QVBoxLayout(tile);
        tl->setContentsMargins(15, 13, 16, 14);
        tl->setSpacing(6);
        auto *kRow = new QHBoxLayout();
        kRow->setSpacing(7);
        auto *led = new QLabel(QStringLiteral("●"), tile);
        led->setStyleSheet(QString("color:%1;font-size:9px;").arg(wcss(wt.online)));
        kRow->addWidget(led);
        auto *kl = new QLabel(key, tile);
        kl->setStyleSheet(QString(
            "color:%1;font-size:10px;font-weight:bold;letter-spacing:2px;")
            .arg(wcss(wt.textTime)));
        kRow->addWidget(kl);
        kRow->addStretch();
        tl->addLayout(kRow);
        tl->addStretch();
        auto *val = new QLabel(tile);
        val->setTextFormat(Qt::RichText);
        val->setStyleSheet(QString("color:%1;").arg(wcss(wt.textPrimary)));
        tl->addWidget(val);
        if (valOut) *valOut = val;
        if (ledOut) *ledOut = led;
        return tile;
    };
    grid->addWidget(makeTile(QStringLiteral("SERVER"),    &m_welcomeServerLabel,    nullptr),        0, 0, 1, 2);
    grid->addWidget(makeTile(QStringLiteral("SIGNALING"), &m_welcomeSignalingLabel, &m_wcSignalLed), 0, 2);
    grid->addWidget(makeTile(QStringLiteral("PUSH"),      &m_welcomePushLabel,      &m_wcPushLed),   0, 3);
    grid->addWidget(makeTile(QStringLiteral("NEXTCLOUD"), &m_welcomeNcLabel,        nullptr),        1, 0);
    grid->addWidget(makeTile(QStringLiteral("TALK"),      &m_welcomeTalkLabel,      nullptr),        1, 1);
    grid->addWidget(makeTile(QStringLiteral("GPU"),       &m_welcomeGpuLabel,       &m_wcGpuLed),    1, 2, 1, 2);
    for (int c = 0; c < 4; ++c) grid->setColumnStretch(c, 1);
    welcomeLayout->addLayout(grid);

    // Subsystems strip: GStreamer codec/transport availability.
    {
        auto *strip = new QFrame(root);
        strip->setObjectName("mcStrip");
        strip->setStyleSheet(QString(
            "QFrame#mcStrip{background:%1;border:1px solid %2;border-radius:13px;}")
            .arg(wcss(wt.bgSurface), wcss(wt.divider)));
        auto *sl = new QHBoxLayout(strip);
        sl->setContentsMargins(16, 12, 16, 12);
        sl->setSpacing(13);
        auto *sk = new QLabel(QStringLiteral("SUBSYSTEMS"), strip);
        sk->setStyleSheet(QString(
            "color:%1;font-size:10px;font-weight:bold;letter-spacing:2px;")
            .arg(wcss(wt.textTime)));
        sl->addWidget(sk);
        static const char *pluginNames[] = {
            "webrtc", "opus", "vpx", "d3d11", "nvcodec",
            "srtp", "dtls", "nice", "wasapi2", "mediafoundation",
            "winscreencap", nullptr
        };
        for (int i = 0; pluginNames[i]; ++i) {
            GstPlugin *plugin = gst_registry_find_plugin(gst_registry_get(), pluginNames[i]);
            bool ok = (plugin != nullptr);
            if (plugin) gst_object_unref(plugin);
            auto *chip = new QLabel(QString::fromLatin1(pluginNames[i]), strip);
            chip->setStyleSheet(QString(
                "QLabel{color:%1;font-family:%2;font-size:10px;font-weight:bold;"
                "border:1px solid %3;border-radius:6px;padding:3px 8px;}")
                .arg(wcss(ok ? wt.online : wt.danger), wmono,
                     wcss(ok ? wt.online : wt.danger)));
            sl->addWidget(chip);
        }
        sl->addStretch();
        welcomeLayout->addWidget(strip);
    }

    // Flight log: the changelog, framed as the mission log (fills height).
    auto *flightPanel = new QFrame(root);
    flightPanel->setObjectName("mcPanel");
    flightPanel->setStyleSheet(QString(
        "QFrame#mcPanel{background:%1;border:1px solid %2;border-radius:14px;}")
        .arg(wcss(wt.bgSurface), wcss(wt.divider)));
    auto *flightLayout = new QVBoxLayout(flightPanel);
    flightLayout->setContentsMargins(0, 0, 0, 0);
    flightLayout->setSpacing(0);
    auto *flightHdr = new QLabel(QStringLiteral("●  FLIGHT LOG · WHAT'S NEW"), flightPanel);
    flightHdr->setStyleSheet(QString(
        "color:%1;font-size:10px;font-weight:bold;letter-spacing:2px;"
        "padding:13px 17px;border-bottom:1px solid %2;")
        .arg(wcss(wt.textTime), wcss(wt.divider)));
    flightLayout->addWidget(flightHdr);
    auto *changelog = new QTextBrowser(flightPanel);
    changelog->setFrameShape(QFrame::NoFrame);
    changelog->setStyleSheet(QString(
        "QTextBrowser{background:transparent;color:%1;border:none;"
        "padding:14px 18px;font-size:13px;}")
        .arg(wcss(wt.textPrimary)));
    {
        // Read the changelog resource once per process; rebuilds reuse it.
        static QString s_changelogMd;
        static bool s_changelogTried = false;
        if (!s_changelogTried) {
            s_changelogTried = true;
            QFile f(QStringLiteral(":/docs/CHANGELOG.md"));
            if (f.open(QIODevice::ReadOnly))
                s_changelogMd = QString::fromUtf8(f.readAll());
            else
                qWarning() << "welcome: :/docs/CHANGELOG.md missing:" << f.errorString();
        }
        // Give the markdown real hierarchy/spacing — Qt's built-in
        // markdown CSS is cramped and ignores the theme. Must precede
        // setMarkdown() to take effect.
        changelog->document()->setDefaultStyleSheet(QString(
            "h1{font-size:20px;font-weight:700;color:%1;margin:16px 0 8px;}"
            "h2{font-size:16px;font-weight:700;color:%1;margin:20px 0 8px;}"
            "h3{font-size:12px;font-weight:700;color:%2;margin:14px 0 4px;}"
            "p{color:%1;margin:6px 0;}"
            "li{color:%1;margin:5px 0;}"
            "strong,b{color:%3;font-weight:700;}"
            "code{color:%3;}"
            "a{color:%3;text-decoration:none;}")
            .arg(wcss(wt.textPrimary), wcss(wt.textSecondary), wcss(wt.accent)));
        if (!s_changelogMd.isEmpty())
            changelog->setMarkdown(s_changelogMd);
        else
            changelog->setPlainText(tr("Release notes unavailable."));
    }
    changelog->setReadOnly(true);
    changelog->setOpenExternalLinks(true);
    flightLayout->addWidget(changelog, 1);
    welcomeLayout->addWidget(flightPanel, 1);

    // Footer readout.
    {
        auto *foot = new QLabel(QString(
            "<span style='color:%1'>SESSION</span> %2    "
            "<span style='color:%1'>RENDERER</span> QPainter    "
            "<span style='color:%1'>TalQ</span> v%3")
            .arg(wcss(wt.textSecondary),
                 (m_auth ? m_auth->displayName() : QStringLiteral("local")),
                 QApplication::applicationVersion()), root);
        foot->setStyleSheet(QString(
            "color:%1;font-family:%2;font-size:11px;letter-spacing:.4px;")
            .arg(wcss(wt.textTime), wmono));
        welcomeLayout->addWidget(foot);
    }

    refreshWelcomeStatus();
}

// Make the welcome host visible. Rebuild its content only if a theme change
// happened while it was hidden (deferred from applyThemeId), so opening a
// chat and cycling themes mid-chat stay cheap.
void MainWindow::showWelcome()
{
    if (!m_welcomeWidget) return;
    if (m_welcomeDirty || !m_welcomeContent)
        buildWelcomeContent();
    m_welcomeWidget->show();
    refreshWelcomeStatus();
}

// Brief, theme-tinted "Theme: X" overlay, bottom-centre, auto-hiding. Honors
// reduced-motion by not animating (plain show/hide).
void MainWindow::showThemeToast(const QString &name)
{
    PainterTheme t(m_themeId, m_fontScale);
    auto hx = [](const QColor &c){ return c.name(QColor::HexRgb); };
    if (!m_themeToast) {
        m_themeToast = new QLabel(this);
        m_themeToast->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_themeToast->setAlignment(Qt::AlignCenter);
    }
    m_themeToast->setText(tr("  Theme: %1  ").arg(name));
    // High-contrast accent pill (controlInk on accent), top-centre so it's
    // unmistakable. Transient confirmation, so the One-Signal accent is fine.
    m_themeToast->setStyleSheet(QString(
        "background:%1;color:%2;border:none;border-radius:14px;"
        "padding:9px 22px;font-size:14px;font-weight:700;letter-spacing:0.3px;")
        .arg(hx(t.accent), hx(t.controlInk)));
    m_themeToast->adjustSize();
    m_themeToast->move((width() - m_themeToast->width()) / 2, 28);
    m_themeToast->raise();
    m_themeToast->show();
    QPointer<QLabel> tp(m_themeToast);
    QTimer::singleShot(1600, this, [tp]{ if (tp) tp->hide(); });
}

// Re-apply theme tokens to the QSS-styled sidebar chrome. These widgets are
// QWidgets (not QPainter) so they don't follow the painter theme; without
// this, the search field, sidebar icon buttons (incl. their hover/pressed
// states), profile name, avatar, and splitter handle keep their build-time
// colours after a theme switch.
void MainWindow::restyleChrome()
{
    PainterTheme t(m_themeId, m_fontScale);
    auto hx = [](const QColor &c){ return c.name(QColor::HexRgb); };

    // Single source of truth: the whole app's widget chrome is generated
    // from PainterTheme and applied app-wide here, so every dialog/button/
    // input/menu tracks all four themes from one place. The few specific
    // theme-driven sheets below (search field, update banner) are component
    // styles layered on top — widget-level QSS wins over the app sheet.
    AppStyle::installRepolishFilter();
    qApp->setStyleSheet(AppStyle::sheet(t));

    if (m_uploadBar)
        m_uploadBar->setStyleSheet(QString(
            "QWidget#uploadBar{background:%1;}"
            "QWidget#uploadProgress{background:%2;border-radius:1px;}")
            .arg(hx(t.bgSecondary), hx(t.accent)));

    if (m_searchField)
        m_searchField->setStyleSheet(QString(
            "QLineEdit{background:%1;border:1px solid %2;border-radius:16px;"
            "padding:4px 14px;font-size:13px;color:%3;}"
            "QLineEdit:focus{border-color:%4;background:%5;}")
            .arg(hx(t.bgSecondary), hx(t.divider), hx(t.textPrimary),
                 hx(t.accent), hx(t.bgHover)));

    const QString iconQSS = QString(
        "QPushButton{background:transparent;color:%1;border:none;"
        "border-radius:8px;font-size:14px;"
        "font-family:'Segoe Fluent Icons','Segoe MDL2 Assets','Segoe UI Symbol';}"
        "QPushButton:hover{background:%2;color:%3;}"
        "QPushButton:pressed{background:%4;}")
        .arg(hx(t.textSecondary), hx(t.bgHover), hx(t.textPrimary),
             hx(t.bgSelected));
    if (m_sidebarCol)
        for (auto *b : m_sidebarCol->findChildren<QPushButton*>(QStringLiteral("sbIcon")))
            b->setStyleSheet(iconQSS);

    // Filter trigger: tint accent while a non-default sort/filter is active so
    // the funnel reads as "engaged" without opening the menu.
    if (m_filterBtn && m_sidebar) {
        const bool active = m_sidebar->filterMode() != SidebarPainter::FilterAll
                         || m_sidebar->sortMode()   != SidebarPainter::SortRecent;
        if (active)
            m_filterBtn->setStyleSheet(QString(
                "QPushButton{background:transparent;color:%1;border:none;"
                "border-radius:8px;font-size:14px;font-family:'Segoe Fluent Icons',"
                "'Segoe MDL2 Assets','Segoe UI Symbol';}"
                "QPushButton:hover{background:%2;color:%1;}"
                "QPushButton:pressed{background:%3;}")
                .arg(hx(t.accent), hx(t.bgHover), hx(t.bgSelected)));
    }

    if (m_filterMenu)
        m_filterMenu->setStyleSheet(QString(
            "QMenu{background:%1;border:1px solid %2;border-radius:10px;"
            "padding:6px;color:%3;font-size:13px;}"
            "QMenu::item{padding:7px 28px 7px 24px;border-radius:6px;}"
            "QMenu::item:selected{background:%4;color:%5;}"
            "QMenu::item:checked{color:%6;font-weight:600;}"
            "QMenu::separator{height:1px;background:%2;margin:6px 8px;}")
            .arg(hx(t.bgSurface), hx(t.divider), hx(t.textPrimary),
                 hx(t.bgHover), hx(t.textPrimary), hx(t.accent)));

    if (m_profileNameLabel)
        m_profileNameLabel->setStyleSheet(QString(
            "color:%1;font-size:14px;font-weight:600;letter-spacing:0.1px;")
            .arg(hx(t.textPrimary)));

    if (m_sidebarCol) {
        if (auto *av = m_sidebarCol->findChild<QLabel*>(QStringLiteral("sbAvatar")))
            av->setStyleSheet(QString("border-radius:18px;background:%1;")
                                  .arg(hx(t.accent)));
    }

    if (m_splitter)
        m_splitter->setStyleSheet(QString("QSplitter::handle{background:%1;}")
                                      .arg(hx(t.divider)));

    if (m_offlineBanner) {
        // Quiet, neutral strip (Telegram-style): a muted secondary-surface fill
        // with a hairline divider and soft secondary text — informational, not
        // an alarm. No red: offline is a transient background state here, not an
        // error the user must act on.
        m_offlineBanner->setStyleSheet(QString(
            "QWidget#offlineRoot{background:%1;border-bottom:1px solid %2;}"
            "QLabel#offlineLabel{color:%3;font-size:12px;font-weight:500;"
            "letter-spacing:0.2px;background:transparent;}")
            .arg(hx(t.bgSecondary), hx(t.divider), hx(t.textSecondary)));
    }

    if (m_updateBanner) {
        const QString accentHi = t.accent.lighter(118).name(QColor::HexRgb);
        m_updateBanner->setStyleSheet(QString(
            "QWidget#ubRoot{background:%1;border-top:1px solid %2;"
            "border-bottom:1px solid %2;}"
            "QLabel#ubGlyph{color:%2;font-size:20px;"
            "font-family:'Segoe Fluent Icons','Segoe MDL2 Assets','Segoe UI Symbol';}"
            "QLabel#ubLabel{color:%3;font-size:14px;}"
            "QPushButton#ubInstall{background:%2;color:%4;border:none;"
            "border-radius:7px;padding:8px 20px;font-weight:700;}"
            "QPushButton#ubInstall:hover{background:%5;}"
            "QPushButton#ubGhost{color:%6;border:none;padding:7px 12px;"
            "font-weight:600;background:transparent;}"
            "QPushButton#ubGhost:hover{color:%3;}"
            "QPushButton#ubClose{color:%6;border:none;background:transparent;"
            "font-size:13px;}"
            "QPushButton#ubClose:hover{color:%3;}"
            "QProgressBar#ubProgress{background:%7;border:1px solid %8;"
            "border-radius:6px;color:%3;text-align:center;}"
            "QProgressBar#ubProgress::chunk{background:%2;border-radius:5px;}")
            .arg(hx(t.bgSurface), hx(t.accent), hx(t.textPrimary),
                 hx(t.controlInk), accentHi, hx(t.textTime),
                 hx(t.bgSecondary), hx(t.divider)));
    }
}

void MainWindow::switchToLogin()
{
    m_chatMode = false;
    showNormal();
    setMinimumWidth(380);
    resize(460, 520);
    if (auto *screen = QApplication::primaryScreen()) {
        QRect geo = screen->availableGeometry();
        move(geo.center() - QPoint(230, 260));
    }
    m_stack->setCurrentWidget(m_loginWidget);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveWindowState();  // always save, even when closing to tray
    if (m_chatMode && m_closeToTray) {
        event->ignore();
        // Capture fullscreen separately — isMaximized() returns false while
        // fullscreen, so without this we'd silently downgrade fullscreen
        // users to "normal" on restore from tray.
        m_wasFullScreen = isFullScreen();
        m_wasMaximized = !m_wasFullScreen && isMaximized();
        // Minimize to the taskbar instead of hide()-to-tray. A minimized window
        // KEEPS its taskbar button, so the unread-count overlay badge stays
        // visible there — hide() removed the button entirely, leaving only the
        // tray badge. The tray icon remains available for Show / Quit.
        showMinimized();
    } else {
        event->accept();
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    // The status popover is its own top-level window; minimizing TalQ
    // would otherwise leave it floating on the desktop.
    if (event->type() == QEvent::WindowStateChange
        && (windowState() & Qt::WindowMinimized)
        && m_statusPopover)
        m_statusPopover->hide();
    // Activation refresh: the 60 s user-status poll is fine for users
    // who keep TalQ in the foreground, but anyone returning from another
    // app or from a long sleep wants the sidebar dots to reflect "now",
    // not "up to a minute ago". Trigger one out-of-band refresh on
    // ActivationChange when we become the active window, but rate-limit
    // to once per 5 s so a busy desktop's repeated focus toggles don't
    // pile up redundant in-flight HTTP requests.
    if (event->type() == QEvent::ActivationChange && isActiveWindow()
        && m_conversations) {
        // bug 13 — TalQ came to the foreground: if we auto-flipped to Away
        // while idle, restore to Online immediately (no-op otherwise).
        if (m_userStatus)
            m_userStatus->tryRestoreFromAutoAway();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastActivationStatusRefreshMs >= 5000) {
            m_lastActivationStatusRefreshMs = now;
            m_conversations->refreshUserStatuses();
            // Multi-instance sync: pull OUR OWN status too, so a status set on
            // another TalQ/Talk instance of this account shows here the moment we
            // alt-tab back (not only on the 60 s heartbeat). keepAlive=false —
            // the tryRestoreFromAutoAway() above already handles "user is back",
            // this is purely a read. Shares the 5 s rate limit.
            if (m_userStatus)
                m_userStatus->refreshFromServer(false);
            // bug 1 — also re-sync the OPEN room on activation. The
            // open room's only live path is the long-poll; if it stalled while
            // we were away (sleep/wake, network blip) the room would show stale
            // messages until a full conversation switch. refreshLatest()
            // reconciles it and restarts the poller. Shares the 5 s limit.
            if (m_messages)
                m_messages->refresh();
            // Re-sync the CONVERSATION LIST on activation too. A new group
            // created on another device generates no push to its creator, and
            // a reply in a mention-only group may produce no push at all — so
            // the list previously only updated on the 30 s fallback timer (or a
            // restart). Refreshing the moment the user returns to TalQ makes a
            // newly-created/just-active conversation appear right away. Shares
            // the 5 s rate limit; ConversationListModel coalesces if a refresh
            // is already in flight.
            if (m_conversations)
                m_conversations->refresh();
            // 0.52.7 — refresh the OPEN conversation's TOPIC list too, so the
            // per-topic unread counters on the topic bar are current when the user
            // returns (they only recomputed on conversation-open before). Debounced
            // (shared with the live-message path) + gated on m_showTopics.
            if (m_showTopics)
                m_topicUnreadDebounce.start();
            // Avatars only ever lived in each painter's in-memory cache and
            // were never invalidated, so a group/user avatar changed elsewhere
            // stayed stale until restart. Mark them stale on focus; the next
            // paint re-fetches in the background (the current image stays up
            // until the new one loads). A 15-min TTL inside the painters covers
            // the always-focused case.
            if (m_sidebar)     m_sidebar->invalidateAvatars();
            if (m_header)      m_header->invalidateAvatars();
            if (m_chatPainter) m_chatPainter->invalidateAvatars();
            // Re-check the server the moment the user returns (laptop wake,
            // network change): an active /status.php probe confirms reachability
            // without waiting for one of the refreshes above to fail/succeed.
            if (m_api) m_api->probeReachability();
        }
    }
}

void MainWindow::hideEvent(QHideEvent *event)
{
    QMainWindow::hideEvent(event);
    // Covers close-to-tray and switchToLogin (both hide() the window).
    if (m_statusPopover) m_statusPopover->hide();
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    // Windows broadcasts the registered "TaskbarButtonCreated" message every
    // time our taskbar button appears — first show, restore from the tray, or
    // an Explorer restart. The unread overlay badge (ITaskbarList3::
    // SetOverlayIcon) only persists while that button exists, so we re-assert
    // it here. This is the documented requirement that was missing, and the
    // reason the badge never showed: badges pushed while the window was hidden
    // in the tray were dropped, and nothing re-applied them on restore.
    if (m_taskbarButtonCreatedMsg && m_notifications) {
        const MSG *msg = static_cast<const MSG *>(message);
        if (msg && msg->message == m_taskbarButtonCreatedMsg)
            m_notifications->reapplyTaskbarOverlay();
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
#ifdef Q_OS_WIN
    // Belt-and-suspenders alongside the TaskbarButtonCreated handler: re-assert
    // the overlay badge when the window is shown again after close-to-tray.
    // Deferred one event-loop turn so the freshly-created taskbar button exists
    // by the time we push the icon.
    if (m_notifications) {
        QPointer<NotificationManager> n(m_notifications);
        QTimer::singleShot(0, this, [n]() { if (n) n->reapplyTaskbarOverlay(); });
    }
#endif
}

void MainWindow::restoreFromTray()
{
    if (m_wasFullScreen)
        showFullScreen();
    else if (m_wasMaximized)
        showMaximized();
    else
        showNormal();
    raise();
    activateWindow();
}

void MainWindow::openConversation(const QString &token)
{
    // Bring window to front — Windows blocks focus stealing, so use SetForegroundWindow.
    // Restore path branches on three cases so fullscreen/maximized survive:
    //   1. Minimized: clear the WindowMinimized flag — Qt retains the
    //      prior fullscreen/maximized bit through the minimize cycle, so
    //      we resume in whatever state we left.
    //   2. Hidden (close-to-tray): consult the explicit m_wasFullScreen /
    //      m_wasMaximized snapshot captured in closeEvent.
    //   3. Already visible: just raise() — don't touch state at all,
    //      otherwise we'd drop fullscreen on a click-while-visible.
    if (isMinimized()) {
        setWindowState(windowState() & ~Qt::WindowMinimized);
    } else if (!isVisible()) {
        if (m_wasFullScreen)
            showFullScreen();
        else if (m_wasMaximized)
            showMaximized();
        else
            showNormal();
    }
    raise();
    activateWindow();
#ifdef Q_OS_WIN
    // Force foreground on Windows (bypasses focus stealing prevention)
    SetForegroundWindow(reinterpret_cast<HWND>(winId()));
#endif

    if (token.isEmpty()) return;

    // Find the conversation in the model and select it
    int count = m_conversations->rowCount();
    for (int i = 0; i < count; ++i) {
        QModelIndex idx = m_conversations->index(i, 0);
        QString t = idx.data(ConversationListModel::TokenRole).toString();
        if (t == token) {
            QString name    = idx.data(ConversationListModel::DisplayNameRole).toString();
            QString userId  = idx.data(ConversationListModel::ActorIdRole).toString();
            int convType    = idx.data(ConversationListModel::TypeRole).toInt();
            QString status  = idx.data(ConversationListModel::UserStatusRole).toString();
            QString statusMessage = m_conversations->userStatusMessageForToken(token);
            QString statusIcon    = m_conversations->userStatusIconForToken(token);
            m_sidebar->setSelectedIndex(i);
            onConversationSelected(token, name, userId, convType, status, statusMessage, statusIcon);
            return;
        }
    }
}

void MainWindow::saveWindowState()
{
    if (!m_chatMode || !m_geometrySaveEnabled) return;

    m_settings.beginGroup("WindowGeometry");
    if (isMaximized()) {
        m_settings.setValue("savedVisibility", 4);
    } else {
        m_settings.setValue("savedX", x());
        m_settings.setValue("savedY", y());
        m_settings.setValue("savedWidth", width());
        m_settings.setValue("savedHeight", height());
        m_settings.setValue("savedVisibility", 2);
    }
    // Final flush of the splitter widths. The splitterMoved debouncer
    // already writes during normal use; this catches the rare case where
    // the user resized < 250 ms before closing the window.
    if (m_splitter)
        m_settings.setValue("splitterState", m_splitter->saveState());
    m_settings.endGroup();
}

void MainWindow::restoreChatGeometry()
{
    m_geometrySaveEnabled = false;
    setMinimumWidth(500);

    m_settings.beginGroup("WindowGeometry");
    int w = qMax(m_settings.value("savedWidth", 1000).toInt(), 500);
    int h = qMax(m_settings.value("savedHeight", 700).toInt(), 400);
    int sx = m_settings.value("savedX", -1).toInt();
    int sy = m_settings.value("savedY", -1).toInt();
    int vis = m_settings.value("savedVisibility", 2).toInt();
    m_settings.endGroup();

    // Clamp to screen bounds
    if (auto *screen = QApplication::primaryScreen()) {
        QRect avail = screen->availableGeometry();
        w = qMin(w, avail.width());
        h = qMin(h, avail.height());
    }

    // Set windowed geometry first
    resize(w, h);
    if (sx != -1 || sy != -1) {
        move(sx, sy);
    } else {
        // First launch: center on screen
        if (auto *screen = QApplication::primaryScreen()) {
            QRect geo = screen->availableGeometry();
            move(geo.center() - QPoint(w / 2, h / 2));
        }
    }

    // Restore maximized state if that was the last state
    if (vis == 4)
        showMaximized();
    else
        show();

    // Delay enabling geometry save to avoid saving during restore
    QTimer::singleShot(500, this, [this]() {
        m_geometrySaveEnabled = true;
    });
}

void MainWindow::applyFontScale(qreal scale)
{
    m_fontScale = scale;
    m_chatPainter->setFontScale(m_fontScale);

    // Scale composer font to match chat zoom (base px is owned by
    // ComposerWidget so the two sides can't desync).
    QFont inputFont = m_composer->font();
    inputFont.setPixelSize(qRound(ComposerWidget::kBaseInputPx * m_fontScale));
    m_composer->setInputFont(inputFont);

    m_settings.beginGroup("Theme");
    m_settings.setValue("fontScale", m_fontScale);
    m_settings.endGroup();
}

void MainWindow::applyTheme(bool dark)
{
    applyThemeId(dark ? PainterTheme::Theme::Vivid : PainterTheme::Theme::Paper);
}

void MainWindow::applyThemeId(PainterTheme::Theme t)
{
    if (m_themeId == t) return;
    m_themeId = t;
    m_darkMode = (t != PainterTheme::Theme::Paper);
    m_sidebar->setTheme(t);
    m_header->setTheme(t);
    m_chatPainter->setTheme(t);
    m_threadsPainter->setTheme(t);
    if (m_callWindow) m_callWindow->setTheme(t);
    if (m_topicTabBar) m_topicTabBar->setTheme(t);   // bug 10
    applyDarkPalette();
    restyleChrome();          // search field, sidebar icons, profile, splitter
    // Re-tint the Mission Control home. Rebuilding it (delete + recreate ~30
    // widgets + re-parse the full CHANGELOG markdown) is expensive, so only do
    // it when the user is actually looking at it; otherwise defer to next show.
    if (m_welcomeWidget && m_welcomeWidget->isVisible())
        buildWelcomeContent();
    else
        m_welcomeDirty = true;
    if (m_themeBtn)
        m_themeBtn->setToolTip(tr("Theme: %1 (Ctrl+D to cycle)")
                                   .arg(PainterTheme::themeLabel(t)));
    m_settings.beginGroup("Theme");
    m_settings.setValue("theme", PainterTheme::themeId(t));
    m_settings.setValue("darkMode", m_darkMode);
    m_settings.endGroup();
    showThemeToast(PainterTheme::themeLabel(t));
}

void MainWindow::applyDarkPalette()
{
    PainterTheme theme(m_themeId, 1.0);
    QPalette pal;
    pal.setColor(QPalette::Window, theme.bgPrimary);
    pal.setColor(QPalette::WindowText, theme.textPrimary);
    pal.setColor(QPalette::Base, theme.bgSurface);
    pal.setColor(QPalette::AlternateBase, theme.bgSecondary);
    pal.setColor(QPalette::Text, theme.textPrimary);
    pal.setColor(QPalette::Button, theme.bgSurface);
    pal.setColor(QPalette::ButtonText, theme.textPrimary);
    pal.setColor(QPalette::Highlight, theme.accent);
    pal.setColor(QPalette::HighlightedText, theme.controlInk);
    pal.setColor(QPalette::PlaceholderText, theme.textMuted);
    pal.setColor(QPalette::Mid, theme.divider);
    QApplication::setPalette(pal);
}

void MainWindow::loadProfileAvatar(QLabel *avatarLabel)
{
    auto *reply = m_api->getAbsoluteUrl("/index.php/avatar/" + m_auth->userId() + "/64");
    connect(reply, &QNetworkReply::finished, this, [reply, avatarLabel]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            qWarning() << "loadProfileAvatar: HTTP" << status << reply->errorString();
            return;
        }
        QImage img;
        if (!img.loadFromData(reply->readAll())) {
            qWarning() << "loadProfileAvatar: failed to decode avatar payload";
            return;
        }
        QImage circle = PainterTheme::cropToCircle(img, 36);
        avatarLabel->setPixmap(QPixmap::fromImage(circle));
        avatarLabel->setStyleSheet("");
    });
}

void MainWindow::openStatusPopover()
{
    if (!m_statusPopover)
        m_statusPopover = new StatusPopover(m_userStatus, this);
    if (m_profileBar) {
        const QRect anchor(m_profileBar->mapToGlobal(QPoint(0, 0)),
                           m_profileBar->size());
        m_statusPopover->popupNear(anchor);
    } else {
        m_statusPopover->show();
    }
}

void MainWindow::refreshStatusIndicator()
{
    if (!m_statusDot || !m_statusPill) return;

    const auto s = m_userStatus->status();
    const QColor c = UserStatusManager::colorFor(s);
    m_statusDot->setColor(c);

    QString text = UserStatusManager::label(s);
    const QString msg = m_userStatus->message();
    const QString icon = m_userStatus->icon();
    if (!msg.isEmpty())
        text = (icon.isEmpty() ? QString() : icon + QLatin1Char(' ')) + msg;

    QFontMetrics fm(m_statusPill->font());
    const QString elided = fm.elidedText(text, Qt::ElideRight, 150);
    m_statusPill->setText(QStringLiteral("● ") + elided + QStringLiteral("  ▾"));
    PainterTheme th(m_themeId, m_fontScale);
    m_statusPill->setStyleSheet(QStringLiteral(
        "#sbStatusPill { color:%1; background:transparent; border:none;"
        " text-align:left; padding:0; font-size:11px; }"
        "#sbStatusPill:hover { color:%2; }")
        .arg(c.name(), th.textPrimary.name(QColor::HexRgb)));
}

void MainWindow::moveEvent(QMoveEvent *)
{
    m_saveGeometryTimer.start();
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    m_saveGeometryTimer.start();
}

void MainWindow::onUpdateReadyToLaunch(const QString &installerPath)
{
    m_pendingInstallerPath = installerPath;
    m_updateProgress->hide();

    // 0.52.14 \u2014 the user clicked "Update now": skip the idle countdown and install
    // immediately. maybeLaunchPendingInstaller still defers if a call is active
    // (and re-runs on callStateChanged), so this can't drop a live call.
    if (m_userWantsImmediateInstall) {
        m_userWantsImmediateInstall = false;
        m_updateNowChecking = false;
        m_updateLabel->setText(tr("Update downloaded \u2014 installing\u2026"));
        m_updateInstallBtn->hide();
        m_updateLaterBtn->hide();
        m_updateWhatsNewBtn->hide();
        maybeLaunchPendingInstaller();
        return;
    }

    // 0.41.0 \u2014 when auto-install-on-idle is enabled (default ON), stage
    // the install behind the idle gate instead of immediately quitting
    // the app. The user keeps a visible "Cancel auto-install" escape
    // hatch and an "Install now" override. Active calls / unsent text /
    // mid-upload are hard-gated below; user activity (mouse/keyboard)
    // resets the countdown via GetLastInputInfo.
    const bool autoInstallEnabled =
        QSettings().value(QStringLiteral("updates/autoInstall"), true).toBool();
    if (autoInstallEnabled && !m_autoInstallCancelledForSession) {
        m_autoInstallActive = true;
        m_countdownNotified = false;
        // Anchor the wait window to download-completion time. A user who
        // was passively watching the download for longer than the
        // configured idle threshold would otherwise see GetLastInputInfo
        // already past the gate on the first tick and the install would
        // fire with no visible countdown.
        m_autoInstallReadyAtMs = QDateTime::currentMSecsSinceEpoch();
        m_updateBannerActive = true;
        m_updateBanner->show();
        m_updateBanner->raise();
        m_updateInstallBtn->setText(tr("Install now"));
        m_updateInstallBtn->show();
        m_updateLaterBtn->setText(tr("Cancel auto-install"));
        m_updateLaterBtn->show();
        m_updateWhatsNewBtn->hide();
        m_autoInstallTick.setInterval(1000);   // 1 s so the visible MM:SS countdown ticks once a second
        m_autoInstallTick.setSingleShot(false);
        if (!m_autoInstallTick.isActive()) m_autoInstallTick.start();
        onUpdateAutoInstallTick();   // paint the banner immediately
        return;
    }

    // Auto-install disabled (or cancelled for this session) \u2014 original
    // immediate-relaunch path.
    m_updateLabel->setText(tr("Update downloaded \u2014 relaunching\u2026"));
    m_updateInstallBtn->hide();
    m_updateLaterBtn->hide();
    m_updateWhatsNewBtn->hide();
    maybeLaunchPendingInstaller();
}

void MainWindow::onUpdateAutoInstallTick()
{
    if (!m_autoInstallActive || m_pendingInstallerPath.isEmpty()) {
        m_autoInstallTick.stop();
        return;
    }

    // 0.40.8 \u2014 banner-visibility constants. While the user is actively
    // interacting with TalQ the banner is HIDDEN entirely (not just
    // relabeled "paused..."). It returns only after a short "preview"
    // idle window, or during the final-minute countdown. This keeps a
    // non-urgent update prompt from sitting above the chat all day.
    constexpr qint64 kPreviewIdleMs = 30 * 1000;   // 30 s of inactivity
    constexpr qint64 kCountdownMs   = 60 * 1000;   // last-60 s window

    // Any held mouse button counts as active input even though
    // GetLastInputInfo only tracks events (presses, releases,
    // movement) \u2014 a slow drag-resize / drag-select / drag-to-scroll
    // keeps the cursor still for minutes while the idle counter climbs.
    const bool blocked =
        (m_callManager
            && (m_callManager->state() != CallManager::Idle
                || m_callManager->isScreenSharing()))
        || (m_composer && !m_composer->currentText().isEmpty())
        || (m_messages && m_messages->uploadProgress() >= 0.0)
        || (QApplication::mouseButtons() != Qt::NoButton);

    const int idleWaitMin = qBound(1,
        QSettings().value(QStringLiteral("updates/autoInstallIdleMinutes"), 5).toInt(),
        60);
    const qint64 idleWaitMs = qint64(idleWaitMin) * 60 * 1000;

    if (blocked) {
        // User is mid-something \u2014 fully hide the banner; the next tick
        // will re-evaluate.
        // 0.52.12 \u2014 and RESET the idle anchor while blocked (above all, during
        // an active CALL / screen-share). A call generates no TalQ INPUT events,
        // so the idle counter would otherwise climb past idleWaitMs while you talk;
        // then the instant the call dropped to Idle the gate unblocked and the
        // install fired IMMEDIATELY, restarting the app out from under the user and
        // killing the call (Kalin, live). Resetting here forces the update to wait
        // the full idle window AFTER the call/activity ends, never the moment it drops.
        m_lastTalqInputMs = QDateTime::currentMSecsSinceEpoch();
        if (m_updateBanner) m_updateBanner->hide();
        return;
    }

    // 0.40.16 — idle is measured AGAINST TalQ INPUT, not system input.
    // The previous GetLastInputInfo gate reset the countdown any time
    // the user touched the keyboard or mouse anywhere on the desktop,
    // even in another app. Per user intent, only input that actually
    // reaches TalQ (m_lastTalqInputMs, set by eventFilter) should reset.
    const qint64 nowMs  = QDateTime::currentMSecsSinceEpoch();
    qint64       idleMs = qMax<qint64>(0, nowMs - m_lastTalqInputMs);

    // Clamp the effective idle to ms-since-download-ready so the
    // countdown always runs at least once. Without this, a user who
    // was already idle while the download streamed would skip the
    // visible "Installing in M:SS…" banner entirely.
    if (m_autoInstallReadyAtMs > 0) {
        const qint64 sinceReady =
            QDateTime::currentMSecsSinceEpoch() - m_autoInstallReadyAtMs;
        idleMs = qMin(idleMs, sinceReady);
    }

    const qint64 remainingMs = idleWaitMs - idleMs;
    const bool inCountdown   = (remainingMs <= kCountdownMs);

    // 0.40.16 \u2014 on FIRST entry into the final-minute countdown, fire a
    // tray/desktop notification too. If the user has TalQ minimised /
    // backgrounded the inline banner alone is invisible, so they'd
    // otherwise only ever see "Installing in\u2026" after they happened to
    // bring TalQ to focus. The notification gives them a real chance
    // to alt-tab over and cancel.
    if (inCountdown && !m_countdownNotified) {
        m_countdownNotified = true;
        if (m_notifications) {
            const int sec = int(qMax<qint64>(0, remainingMs) / 1000);
            const QString msg = sec >= 30
                ? tr("TalQ will install the new version in about a minute. "
                     "Open TalQ to cancel.")
                : tr("TalQ will install the new version in %1 s. "
                     "Open TalQ to cancel.").arg(sec);
            m_notifications->notify(tr("Update ready to install"),
                                    msg, /*alwaysSound*/ false, QString());
        }
    }

    // Recent activity: hide the banner unless we're already in the
    // last-minute countdown (which the user MUST see to cancel).
    if (idleMs < kPreviewIdleMs && !inCountdown) {
        if (m_updateBanner) m_updateBanner->hide();
        return;
    }

    // About to be visible \u2014 make sure the banner is back on-screen.
    if (m_updateBanner) {
        m_updateBanner->show();
        m_updateBanner->raise();
    }

    if (idleMs >= idleWaitMs) {
        // Idle window satisfied \u2014 kick the relaunch path. We don't
        // pre-set a "relaunching\u2026" label here: if the user has joined
        // a call between the gate check above and the inner call gate
        // inside maybeLaunchPendingInstaller, that path sets its own
        // "you're in a call" label and the connection wired in the
        // ctor will retry once the call ends.
        m_autoInstallActive = false;
        m_autoInstallTick.stop();
        m_updateInstallBtn->hide();
        m_updateLaterBtn->hide();
        m_updateWhatsNewBtn->hide();
        maybeLaunchPendingInstaller();
        return;
    }

    if (inCountdown) {
        // Last minute \u2014 visible countdown.
        const int m = int(remainingMs / 60000);
        const int s = int((remainingMs % 60000) / 1000);
        m_updateLabel->setText(
            tr("<b>Installing in %1:%2\u2026</b> Touch the mouse or "
               "keyboard to cancel.")
                .arg(m, 2, 10, QLatin1Char('0'))
                .arg(s, 2, 10, QLatin1Char('0')));
    } else {
        m_updateLabel->setText(
            tr("<b>Update ready.</b> Will auto-install when you've been "
               "idle for %n minute(s).", "", idleWaitMin));
    }
}

void MainWindow::maybeLaunchPendingInstaller()
{
    if (m_pendingInstallerPath.isEmpty()) return;

    if (m_callManager) {
        if (m_callManager->state() != CallManager::Idle
            || m_callManager->isScreenSharing()) {
            m_updateLabel->setText(
                tr("You\u2019re in a call \u2014 update will start when the call ends."));
            return;  // slot re-runs on callStateChanged
        }
    }

    const QStringList args{
        QStringLiteral("/VERYSILENT"),
        QStringLiteral("/SUPPRESSMSGBOXES"),
        QStringLiteral("/CLOSEAPPLICATIONS"),
        QStringLiteral("/RESTARTAPPLICATIONS"),
        QStringLiteral("/NORESTART"),
    };
    bool ok = QProcess::startDetached(m_pendingInstallerPath, args);
    if (!ok) {
        // Self-heal: the downloaded installer can't be launched. Most
        // common causes are AV quarantine, a stale file lock from an
        // interrupted prior run, or zero-byte from a truncated write.
        // Delete the cached file and re-download once - no user-visible
        // "manual install" path required. If THAT re-download also
        // produces an unlaunchable file, surface the failure (probably
        // an environment issue we can't paper over).
        QFile::remove(m_pendingInstallerPath);
        m_pendingInstallerPath.clear();
        if (m_updateChecker && !m_updateRelaunchAttempted) {
            m_updateRelaunchAttempted = true;
            m_updateLabel->setText(
                tr("Installer was unavailable - re-downloading…"));
            m_updateProgress->setValue(0);
            m_updateProgress->show();
            m_updateChecker->retryDownload();
            return;
        }
        m_updateLabel->setText(tr(
            "Auto-update failed twice. Try Retry, restart TalQ, or "
            "download the latest installer from the project's Releases "
            "page."));
        // Keep the install button visible as a manual retry escape -
        // a user who just whitelisted TalQ in AV (the most common cause
        // of repeated launch failures) can recover without restarting.
        m_updateInstallBtn->setText(tr("Retry"));
        m_updateInstallBtn->show();
        return;
    }
    // Arm the force-exit watchdog AT THE AUTO-UPDATE TRIGGER (not just the tray-
    // Quit one — the 0.51.15 fix covered only that path). The downloaded
    // installer is already running with /CLOSEAPPLICATIONS; we now quit ourselves
    // so it can replace our locked files and restart us. But if a teardown step
    // wedges (the same swallowed-quit that 0.51.15 found) OR closeEvent's
    // minimize-on-close (0.51.12) blocks the installer's Restart-Manager close,
    // the old process keeps the files locked and the update silently never
    // installs (field: a box stuck on 0.51.17 with 0.52.x already on disk). The
    // watchdog guarantees this process is GONE within the grace window, so the
    // installer can always complete + restart. Armed at the trigger so a quit()
    // that never propagates can't bypass it.
    talq::armShutdownWatchdog(6);
    QTimer::singleShot(500, qApp, &QApplication::quit);
}

void MainWindow::scheduleReminder(int messageId, const QDateTime &when)
{
    const QString token = m_messages ? m_messages->conversationToken() : QString();
    if (token.isEmpty() || messageId <= 0 || !when.isValid()) return;
    m_api->setMessageReminder(token, messageId, when, this,
        [this, when](bool ok, const QString &error) {
            if (ok) {
                m_notifications->notify(tr("Reminder set"),
                    tr("You'll be reminded at %1")
                        .arg(when.toString(QStringLiteral("ddd d MMM, HH:mm"))),
                    false, QString());
            } else {
                QMessageBox::warning(this, tr("Reminder not set"),
                    error.isEmpty() ? tr("Nextcloud refused the reminder.") : error);
            }
        });
}

QDateTime MainWindow::askReminderTime()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Remind me at\u2026"));
    PainterTheme th(m_themeId, m_fontScale);
    auto hx = [](const QColor &c){ return c.name(QColor::HexRgb); };
    dlg.setStyleSheet(QStringLiteral(
        "QDialog { background: %1; color: %2; }"
        "QLabel, QDateTimeEdit { color: %2; }"
        "QDateTimeEdit { background: %3; border: 1px solid %4;"
        " border-radius: 6px; padding: 6px 8px; font-size: 14px; }"
        "QPushButton { background: %5; color: %2; border: none;"
        " border-radius: 6px; padding: 6px 16px; }"
        "QPushButton:default { background: %6; color: %7; }"
    ).arg(hx(th.bgPrimary), hx(th.textPrimary), hx(th.bgSurface), hx(th.divider),
          hx(th.bgHover), hx(th.accent), hx(th.controlInk)));
    auto *lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(16, 16, 16, 16);
    lay->setSpacing(12);
    lay->addWidget(new QLabel(tr("Pick a date and time:"), &dlg));

    auto *edit = new QDateTimeEdit(QDateTime::currentDateTime().addSecs(60 * 60), &dlg);
    edit->setCalendarPopup(true);
    edit->setDisplayFormat(QStringLiteral("ddd d MMM yyyy  HH:mm"));
    edit->setMinimumDateTime(QDateTime::currentDateTime().addSecs(60));
    lay->addWidget(edit);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(box);

    if (dlg.exec() != QDialog::Accepted) return {};
    return edit->dateTime();
}

void MainWindow::openUpcomingReminders()
{
    auto *dlg = new UpcomingRemindersDialog(m_api, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &UpcomingRemindersDialog::openConversationAt,
            this, [this](const QString &token, int messageId) {
        openConversation(token);
        if (m_chatPainter) m_chatPainter->scrollToMessage(messageId);
    });
    dlg->show();
}

void MainWindow::openNewChatDialog()
{
    auto *dlg = new NewChatDialog(m_api, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QDialog::accepted, this, [this, dlg]() {
        const QString token = dlg->createdToken();
        if (token.isEmpty()) return;
        // Refresh the sidebar so the new room appears, then open it once the
        // conversation row is known to the model.
        m_conversations->refresh();
        QTimer::singleShot(300, this, [this, token]() { openConversation(token); });
    });
    dlg->exec();
}

void MainWindow::createNewTopic()
{
    if (m_activeConvToken.isEmpty() || !m_messages) return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("New topic"));
    dlg.setMinimumWidth(380);
    PainterTheme th(m_themeId, m_fontScale);
    auto hx = [](const QColor &c){ return c.name(QColor::HexRgb); };
    dlg.setStyleSheet(QStringLiteral(
        "QDialog { background: %1; color: %2; }"
        "QLabel { color: %2; }"
        "QLabel#eyebrow { color: %3; font-size: 10px; letter-spacing: 2px;"
        "  text-transform: uppercase; font-weight: 600; }"
        "QLineEdit { background: transparent; border: none;"
        "  border-bottom: 1px solid %4; padding: 8px 0; color: %2;"
        "  font-size: 18px; font-weight: 500; }"
        "QLineEdit:focus { border-bottom-color: %5; }"
        "QPushButton { background: transparent; color: %3; border: none;"
        "  padding: 8px 14px; font-size: 12px; letter-spacing: 1px;"
        "  text-transform: uppercase; font-weight: 600; }"
        "QPushButton:hover { color: %2; }"
        "QPushButton#primary { background: %5; color: %6; border-radius: 6px; }"
        "QPushButton#primary:hover { background: %5; }"
        "QPushButton#primary:disabled { background: %7; color: %8; }"
    ).arg(hx(th.bgPrimary), hx(th.textPrimary), hx(th.textSecondary), hx(th.divider),
          hx(th.accent), hx(th.controlInk), hx(th.bgSurface), hx(th.textMuted)));
    auto *lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(20, 18, 20, 16);
    lay->setSpacing(10);
    auto *eyebrow = new QLabel(tr("TOPIC NAME"), &dlg);
    eyebrow->setObjectName("eyebrow");
    lay->addWidget(eyebrow);
    auto *input = new QLineEdit(&dlg);
    input->setPlaceholderText(tr("e.g. Design review"));
    lay->addWidget(input);
    lay->addSpacing(6);
    auto *row = new QHBoxLayout();
    row->addStretch();
    auto *cancel = new QPushButton(tr("Cancel"), &dlg);
    auto *create = new QPushButton(tr("Create topic"), &dlg);
    create->setObjectName("primary");
    create->setEnabled(false);
    create->setDefault(true);
    row->addWidget(cancel);
    row->addWidget(create);
    lay->addLayout(row);

    connect(input, &QLineEdit::textChanged, &dlg, [input, create]() {
        create->setEnabled(!input->text().trimmed().isEmpty());
    });
    connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(create, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(input, &QLineEdit::returnPressed, &dlg, [&dlg, create]() {
        if (create->isEnabled()) dlg.accept();
    });

    if (dlg.exec() != QDialog::Accepted) return;

    const QString title = input->text().trimmed();
    const QString token = m_activeConvToken;
    // Seed message — shows up as the first visible message in the new topic.
    // Keep it short and recognizable so no one wonders where it came from.
    const QString seed = QStringLiteral("\U0001F4CC  ") + title;

    // Single-call thread creation: Talk's POST /chat/{token} accepts a
    // `threadTitle` form field; when non-empty (and replyTo == 0), the
    // server creates a new thread rooted at the message in the same call.
    // Earlier 0.40.x cuts split this into send + a separate setThreadTitle
    // PUT/POST that DOES NOT EXIST in Talk v23.0.4 (all 4 fallback shapes
    // return 998 Invalid query), so the seed-message-as-topic actually
    // shipped as a plain chat line with no thread metadata.
    m_api->sendChatMessage(token, seed, this,
        [this, token, messageId_title = title](bool ok, int messageId, const QString &err) {
            if (!ok || messageId <= 0) {
                QMessageBox::warning(this, tr("Couldn't create topic"),
                    err.isEmpty() ? tr("The server refused the seed message.")
                                  : err);
                return;
            }
            // User may have switched rooms while the create was in flight —
            // don't yank them into a topic on a different room.
            if (m_activeConvToken != token) return;
            m_threads->refresh();
            openThread(messageId, messageId_title);
        },
        title);
}

void MainWindow::openConversationInfo()
{
    if (m_activeConvToken.isEmpty()) return;
    // Pull roomType + my participant role from the conversation list model.
    int roomType = 0;
    int myType = RoomParticipant::User;
    QString name;
    for (int i = 0; i < m_conversations->rowCount(); ++i) {
        QModelIndex idx = m_conversations->index(i, 0);
        if (idx.data(ConversationListModel::TokenRole).toString() == m_activeConvToken) {
            roomType = idx.data(ConversationListModel::TypeRole).toInt();
            name     = idx.data(ConversationListModel::DisplayNameRole).toString();
            const auto v = idx.data(ConversationListModel::ParticipantTypeRole);
            if (v.isValid()) myType = v.toInt();
            break;
        }
    }
    auto *dlg = new ConversationInfoDialog(m_api, m_activeConvToken,
                                           name, QString(),
                                           roomType, myType, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &ConversationInfoDialog::roomChanged, this, [this]() {
        m_conversations->refresh();
    });
    connect(dlg, &ConversationInfoDialog::roomDeleted, this, [this]() {
        m_conversations->refresh();
        // Drop the user back to Home — the room they were viewing is gone.
        emit m_sidebar->homeRequested();
    });
    dlg->exec();
}

void MainWindow::ensureSettingsDialog()
{
    if (m_settingsDialog) return;
    m_settingsDialog = new SettingsDialog(
        m_deviceManager, m_notifications, m_appSettings, m_auth, this);
    // The Settings live background preview opens a SECOND camera consumer. During
    // a call the publisher already holds the (exclusive, Windows MF) camera, so
    // the preview must not open the device or it steals it and wedges the in-call
    // video (Ilko, 0.52.16). Seed the dialog's call-active flag for the current
    // state and keep it live — a call can start/end while Settings is open.
    m_settingsDialog->setCallActive(m_callManager->state() != CallManager::Idle);
    connect(m_callManager, &CallManager::stateChanged, m_settingsDialog, [this]() {
        if (m_settingsDialog)
            m_settingsDialog->setCallActive(m_callManager->state() != CallManager::Idle);
    });
    connect(m_settingsDialog, &SettingsDialog::closeToTrayChanged,
            this, [this](bool enabled) { m_closeToTray = enabled; });
    connect(m_settingsDialog, &SettingsDialog::themeIdChanged,
            this, [this](int id) {
                applyThemeId(static_cast<PainterTheme::Theme>(id));
            });
    connect(m_settingsDialog, &SettingsDialog::checkForUpdatesRequested,
            this, [this]() {
        if (m_updateChecker) m_updateChecker->checkNow();
    });
    // 0.52.14 — manual "Update now": check immediately AND install as soon as the
    // download lands (skip the idle countdown; the no-restart-during-call gate in
    // maybeLaunchPendingInstaller still applies). m_updateNowChecking gates the
    // "You're up to date" fallback if no newer version turns up.
    connect(m_settingsDialog, &SettingsDialog::updateNowRequested,
            this, [this]() {
        if (!m_updateChecker) {
            if (m_settingsDialog) m_settingsDialog->setUpdateNowStatus(tr("Updates are not available in this build"));
            return;
        }
        if (!m_pendingInstallerPath.isEmpty()) {
            // A download already completed and is just waiting — install it
            // directly (maybeLaunchPendingInstaller respects the no-restart-during-
            // call gate). No immediate-install flag needed: there's no second
            // readyToLaunch coming, so nothing to gate.
            if (m_settingsDialog) m_settingsDialog->setUpdateNowStatus(tr("Installing update…"));
            maybeLaunchPendingInstaller();
            return;
        }
        m_userWantsImmediateInstall = true;
        m_updateNowChecking = true;
        m_updateChecker->checkNow();
        // Fallback feedback: if no updateAvailable arrives, report up-to-date.
        QTimer::singleShot(9000, this, [this]() {
            if (!m_updateNowChecking) return;          // updateAvailable already handled it
            m_updateNowChecking = false;
            m_userWantsImmediateInstall = false;
            if (m_settingsDialog) m_settingsDialog->setUpdateNowStatus(tr("You’re up to date"));
        });
    });
    // #20 — live-apply Background section changes during a call.
    connect(m_settingsDialog, &SettingsDialog::backgroundSettingsChanged,
            m_callManager, &CallManager::applyBackgroundSettings);
}

void MainWindow::openSettingsToBackgrounds()
{
    ensureSettingsDialog();
    m_settingsDialog->refresh();
    m_settingsDialog->selectAudioVideoTab();
    m_settingsDialog->exec();
}
