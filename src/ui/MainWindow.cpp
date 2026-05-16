#include "MainWindow.h"
#include "CallDialog.h"
#include "SettingsDialog.h"
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
#include "core/NotificationManager.h"
#include "core/PushClient.h"
#include "core/SignalingClient.h"
#include "core/CallManager.h"
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
    , m_debug(debug)
    , m_appSettings(appSettings)
    , m_settings("TalQ", "TalQ")
{
    // Window setup
#ifdef TALQ_BRAND_123NET
    setWindowTitle("123NET TalQ " + QApplication::applicationVersion());
#else
    setWindowTitle("TalQ " + QApplication::applicationVersion());
#endif
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

    // Dark title bar on Windows
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(winId());
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode));
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
    profileLayout->addWidget(m_profileNameLabel, 1);

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
        if (!m_settingsDialog) {
            m_settingsDialog = new SettingsDialog(
                m_deviceManager, m_notifications, m_appSettings, m_auth, this);
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
        }
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
    m_uploadBar->setFixedHeight(36);
    m_uploadBar->setStyleSheet("background: #1a1a16;");
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
    m_uploadLabel->setStyleSheet("font-size: 12px; color: #b0aca5; background: transparent;");
    uploadLayout->addWidget(m_uploadLabel, 1);
    auto *percentLabel = new QLabel(uploadRow);
    percentLabel->setStyleSheet("font-size: 12px; font-weight: 600; color: #14b8a6; background: transparent;");
    uploadLayout->addWidget(percentLabel);
    uploadOuterLayout->addWidget(uploadRow, 1);

    // Teal progress line (positioned absolutely at bottom of m_uploadBar)
    m_uploadProgress = new QWidget(m_uploadBar);
    m_uploadProgress->setStyleSheet("background: #14b8a6;");
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

    m_selectionBar = new SelectionBarWidget(chatCol);
    m_selectionBar->hide();
    chatLayout->addWidget(m_selectionBar);

    connect(m_composer, &ComposerWidget::sendMessage, this, [this](const QString &text, bool silent) {
        int replyId = m_replyToId > 0 ? m_replyToId : m_activeThreadId;
        m_messages->sendMessage(text, replyId, silent);
        m_replyToId = 0;
        m_replyToAuthor.clear();
        m_replyToText.clear();
        m_composer->hideReplyBar();
    });
    connect(m_composer, &ComposerWidget::scheduleRequested, this,
            [this](const QString &text, qint64 sendAt, bool silent) {
        // Reply context behaves the same as sendMessage — picking a future
        // delivery time shouldn't drop the in-flight reply target.
        int replyId = m_replyToId > 0 ? m_replyToId : m_activeThreadId;
        m_messages->scheduleMessage(text, sendAt, replyId, silent);
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

        auto *menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->setStyleSheet(
            "QMenu {"
            "  background: #262b34;"
            "  border: 1px solid #363c48;"
            "  border-radius: 10px;"
            "  padding: 6px 0;"
            "}"
            "QMenu::item {"
            "  padding: 6px 16px 6px 12px;"
            "  color: #b0aca5;"
            "  font-size: 13px;"
            "}"
            "QMenu::item:selected {"
            "  background: rgba(255,255,255,0.08);"
            "  color: #e4e0da;"
            "}"
            "QMenu::separator {"
            "  height: 1px;"
            "  background: rgba(255,255,255,0.08);"
            "  margin: 4px 8px;"
            "}"
        );

        // Emoji quick-react row
        auto *emojiRow = new QWidgetAction(menu);
        auto *emojiWidget = new QWidget(menu);
        emojiWidget->setStyleSheet("background: rgba(255,255,255,0.04); border-radius: 8px; margin: 2px;");
        auto *emojiLayout = new QHBoxLayout(emojiWidget);
        emojiLayout->setContentsMargins(6, 4, 6, 4);
        emojiLayout->setSpacing(2);
        QStringList emojis = {"\U0001F44D", "\u2764\uFE0F", "\U0001F602", "\U0001F62E", "\U0001F622", "\U0001F389"};
        for (const auto &emoji : emojis) {
            auto *btn = new QPushButton(emoji, emojiWidget);
            btn->setFixedSize(34, 34);
            btn->setFlat(true);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setStyleSheet(
                "QPushButton { font-size: 18px; border: none; border-radius: 8px; background: transparent; }"
                "QPushButton:hover { background: rgba(255,255,255,0.12); }"
            );
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
            // Focus composer for reply
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
        // Thread action only for group conversations (type 2, 3)
        if (m_header->conversationType() >= 2) {
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
        auto *menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->setStyleSheet(
            "QMenu { background: #262b34; border: 1px solid #363c48; border-radius: 20px; padding: 4px; }"
        );
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
            btn->setStyleSheet(
                "QPushButton { font-size: 18px; border: none; border-radius: 8px; }"
                "QPushButton:hover { background: rgba(255,255,255,0.12); }"
            );
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

    mainLayout->addWidget(m_splitter);

    // Initial chrome styling from the active theme (re-applied on theme change
    // by applyThemeId → restyleChrome).
    restyleChrome();

    // ── Model signals ──
    connect(m_messages, &MessageListModel::conversationTokenChanged, this, [this]() {
        closeThread();
        m_chatPainter->scrollToBottom();
    });

    connect(m_messages, &MessageListModel::newMessagesAtEnd, this, [this]() {
        if (m_chatPainter->atBottom())
            m_chatPainter->scrollToBottom();
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

    // Topics panel visibility: show in any group/public room the server allows
    // threads in, regardless of whether topics exist yet — otherwise users
    // can't discover the "+ New topic" button to create the first one.
    connect(m_threads, &ThreadListModel::hasTopicsChanged, this, [this]() {
        const bool isGroup = m_header->conversationType() >= 2;
        const bool active  = isGroup && m_auth->hasThreadsSupport();
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

    // Call dialog (shows/hides automatically via CallManager::stateChanged)
    m_callDialog = new CallDialog(m_callManager, m_api, this);

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
        m_pendingUpdateNotes = m.notes;
        m_updateLabel->setText(tr("<b>Update available.</b> TalQ v%1 is ready "
                                   "to install.").arg(m.version));
        m_updateProgress->hide();
        m_updateInstallBtn->setText(tr("Install now"));
        m_updateInstallBtn->show();
        m_updateLaterBtn->show();
        m_updateWhatsNewBtn->show();
        m_updateBannerActive = true;
        m_updateBanner->show();
        m_updateBanner->raise();    // ensure it's above sibling painters on Z-order
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
    });
    connect(m_updateChecker, &UpdateChecker::readyToLaunch,
            this, &MainWindow::onUpdateReadyToLaunch);

    connect(m_updateInstallBtn, &QPushButton::clicked, this, [this]() {
        m_updateChecker->acceptUpdate();
    });
    connect(m_updateLaterBtn, &QPushButton::clicked, this, [this]() {
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
    m_searchResults->setStyleSheet(
        "QListWidget { background: #222230; color: #eee; border: 1px solid #333; border-radius: 6px; }"
        "QListWidget::item { padding: 6px 10px; }"
        "QListWidget::item:selected { background: #3a3a55; }"
    );

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
    // Escape in search input — close search bar
    if (obj == m_searchInput && event->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent*>(event);
        if (k->key() == Qt::Key_Escape) {
            m_searchBar->hide();
            if (m_searchResults) m_searchResults->hide();
            return true;
        }
    }
    // Avatar click -> open settings (in any mode)
    if (obj == m_profileAvatarLabel && event->type() == QEvent::MouseButtonRelease) {
        if (!m_settingsDialog) {
            m_settingsDialog = new SettingsDialog(
                m_deviceManager, m_notifications, m_appSettings, m_auth, this);
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
        }
        m_settingsDialog->refresh();
        m_settingsDialog->exec();
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

    m_activeConvToken = token;

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

    // Show the topics panel for any group/public room once threads are
    // supported. The panel hosts the "+ New topic" button — if we wait
    // for m_threads to signal hasTopicsChanged, empty rooms never get it.
    const bool topicsVisible = (convType >= 2) && m_auth->hasThreadsSupport();
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
}

void MainWindow::closeThread()
{
    m_activeThreadId = 0;
    m_activeThreadTitle = "";
    m_header->setActiveThreadId(0);
    m_header->setActiveThreadTitle("");
    m_messages->setThreadId(0);
    m_composer->setTopicName("");
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

    QString url = m_auth ? m_auth->serverUrl() : QString();
    url.remove(QRegularExpression(QStringLiteral("^https?://")));

    auto val = [&](const QString &big, const QString &sub) {
        return QString(
            "<span style='font-size:18px;font-weight:600;color:%1'>%2</span>"
            "<br><span style='font-family:%3;font-size:11px;color:%4'>%5</span>")
            .arg(hx(th.textPrimary), big.toHtmlEscaped(), mono,
                 hx(th.textSecondary), sub.toHtmlEscaped());
    };

    m_welcomeNameLabel->setText(QStringLiteral("Welcome back, ")
        + (m_auth ? m_auth->displayName() : QString()));
    m_welcomeServerLabel->setText(val(url.isEmpty() ? QStringLiteral("offline") : url,
                                      url.isEmpty() ? QStringLiteral("not connected")
                                                    : QStringLiteral("reachable")));
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
    m_welcomeGpuLabel->setText(val(gpu,
                                   gpuOn ? QStringLiteral("hardware accelerated")
                                         : QStringLiteral("software only")));

    auto setLed = [&](QLabel *led, bool ok) {
        if (led) led->setStyleSheet(QString("color:%1;font-size:9px;")
                                    .arg(hx(ok ? th.online : th.amber)));
    };
    setLed(m_wcSignalLed, sigOn);
    setLed(m_wcPushLed, pushOn);
    setLed(m_wcGpuLed, gpuOn);

    if (m_wcStatusPill) {
        const bool nominal = sigOn && pushOn && gpuOn;
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
        hide();
    } else {
        event->accept();
    }
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

    // Scale composer font to match chat zoom
    QFont inputFont = m_composer->font();
    inputFont.setPixelSize(qRound(14 * m_fontScale));
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
    pal.setColor(QPalette::HighlightedText, QColor("#000000"));
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
    m_updateLabel->setText(tr("Update downloaded \u2014 relaunching\u2026"));
    m_updateProgress->hide();
    m_updateInstallBtn->hide();
    m_updateLaterBtn->hide();
    m_updateWhatsNewBtn->hide();
    maybeLaunchPendingInstaller();
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
        m_updateLabel->setText(tr("Could not launch installer."));
        m_updateInstallBtn->setText(tr("Retry"));
        m_updateInstallBtn->show();
        m_pendingInstallerPath.clear();
        return;
    }
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
    dlg.setStyleSheet(
        "QDialog { background: #1a1a18; color: #e4e0da; }"
        "QLabel, QDateTimeEdit { color: #e4e0da; }"
        "QDateTimeEdit { background: #222220; border: 1px solid #2a2a26;"
        " border-radius: 6px; padding: 6px 8px; font-size: 14px; }"
        "QPushButton { background: #2a2a26; color: #e4e0da; border: none;"
        " border-radius: 6px; padding: 6px 16px; }"
        "QPushButton:default { background: #14b8a6; color: white; }"
    );
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
    dlg.setStyleSheet(
        "QDialog { background: #161614; color: #e4e0da; }"
        "QLabel { color: #e4e0da; }"
        "QLabel#eyebrow { color: #6f6a62; font-size: 10px; letter-spacing: 2px;"
        "  text-transform: uppercase; font-weight: 600; }"
        "QLineEdit { background: transparent; border: none;"
        "  border-bottom: 1px solid #2a2a26; padding: 8px 0; color: #f4f0ea;"
        "  font-size: 18px; font-weight: 500; }"
        "QLineEdit:focus { border-bottom-color: #14b8a6; }"
        "QPushButton { background: transparent; color: #8a8680; border: none;"
        "  padding: 8px 14px; font-size: 12px; letter-spacing: 1px;"
        "  text-transform: uppercase; font-weight: 600; }"
        "QPushButton:hover { color: #e4e0da; }"
        "QPushButton#primary { background: #14b8a6; color: #0e1817; border-radius: 6px; }"
        "QPushButton#primary:hover { background: #2dd4bf; }"
        "QPushButton#primary:disabled { background: #1c2b2a; color: #546361; }"
    );
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

    m_api->sendChatMessage(token, seed, this,
        [this, token, title](bool ok, int messageId, const QString &err) {
            if (!ok || messageId <= 0) {
                QMessageBox::warning(this, tr("Couldn't create topic"),
                    err.isEmpty() ? tr("The server refused the seed message.")
                                  : err);
                return;
            }
            // Best-effort: try to set a named thread title. Different NC Talk
            // versions accept different endpoint shapes — if all of them 404,
            // we still have a working thread rooted at the seed message (the
            // topic will display its seed-message text as the label).
            m_api->setChatThreadTitle(token, messageId, title, this,
                [this, token, messageId, title](bool /*ok2*/, const QString & /*err2*/) {
                    // User may have switched rooms while the two-step create
                    // was in flight — don't yank them into a topic on a
                    // different room.
                    if (m_activeConvToken != token) return;
                    m_threads->refresh();
                    openThread(messageId, title);
                });
        });
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
