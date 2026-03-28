#include "MainWindow.h"
#include "LoginWidget.h"
#include "ComposerWidget.h"
#include "painter/ChatPainter.h"
#include "painter/SidebarPainter.h"
#include "painter/HeaderPainter.h"
#include "painter/ThreadsPainter.h"
#include "painter/PainterTheme.h"
#include "core/ApiClient.h"
#include "core/AuthManager.h"
#include "core/NotificationManager.h"
#include "core/PushClient.h"
#include "core/SignalingClient.h"
#include "core/CallManager.h"
#include "core/DebugMonitor.h"
#include "core/AppSettings.h"
#include "core/AvatarProvider.h"
#include "core/FilePreviewProvider.h"
#include "core/MediaDeviceManager.h"
#include "models/ConversationListModel.h"
#include "models/MessageListModel.h"
#include "models/ThreadListModel.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCloseEvent>
#include <QScreen>
#include <QMenu>
#include <QFileDialog>
#include <QDesktopServices>
#include <QUrl>
#include <QClipboard>
#include <QRegularExpression>
#include <QScrollBar>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

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
    AvatarProvider *avatarProvider,
    FilePreviewProvider *previewProvider,
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
    , m_avatarProvider(avatarProvider)
    , m_previewProvider(previewProvider)
    , m_windowSettings("TalQ", "TalQ")
    , m_themeSettings("TalQ", "TalQ")
    , m_generalSettings("TalQ", "TalQ")
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
    m_themeSettings.beginGroup("Theme");
    m_darkMode = m_themeSettings.value("darkMode", true).toBool();
    m_fontScale = m_themeSettings.value("fontScale", 1.0).toReal();
    m_themeSettings.endGroup();

    m_generalSettings.beginGroup("General");
    m_closeToTray = m_generalSettings.value("closeToTray", true).toBool();
    m_generalSettings.endGroup();

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
        m_fontScale = qMin(m_fontScale + 0.1, 2.0);
        m_chatPainter->setFontScale(m_fontScale);
        m_themeSettings.beginGroup("Theme");
        m_themeSettings.setValue("fontScale", m_fontScale);
        m_themeSettings.endGroup();
    });

    auto *zoomOut = new QShortcut(QKeySequence("Ctrl+-"), this);
    connect(zoomOut, &QShortcut::activated, this, [this]() {
        m_fontScale = qMax(m_fontScale - 0.1, 0.7);
        m_chatPainter->setFontScale(m_fontScale);
        m_themeSettings.beginGroup("Theme");
        m_themeSettings.setValue("fontScale", m_fontScale);
        m_themeSettings.endGroup();
    });

    auto *zoomReset = new QShortcut(QKeySequence("Ctrl+0"), this);
    connect(zoomReset, &QShortcut::activated, this, [this]() {
        m_fontScale = 1.0;
        m_chatPainter->setFontScale(m_fontScale);
        m_themeSettings.beginGroup("Theme");
        m_themeSettings.setValue("fontScale", m_fontScale);
        m_themeSettings.endGroup();
    });

    auto *toggleDark = new QShortcut(QKeySequence("Ctrl+D"), this);
    connect(toggleDark, &QShortcut::activated, this, [this]() {
        m_darkMode = !m_darkMode;
        m_sidebar->setDarkMode(m_darkMode);
        m_header->setDarkMode(m_darkMode);
        m_chatPainter->setDarkMode(m_darkMode);
        m_threadsPainter->setDarkMode(m_darkMode);
        applyDarkPalette();
        m_themeSettings.beginGroup("Theme");
        m_themeSettings.setValue("darkMode", m_darkMode);
        m_themeSettings.endGroup();
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
    auto *sidebarCol = new QWidget(m_chatPage);
    auto *sidebarLayout = new QVBoxLayout(sidebarCol);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    sidebarLayout->setSpacing(0);

    m_searchField = new QLineEdit(sidebarCol);
    m_searchField->setPlaceholderText("Search conversations...");
    m_searchField->setMinimumHeight(32);
    QFont sf; sf.setPixelSize(12);
    m_searchField->setFont(sf);
    m_searchField->setContentsMargins(6, 4, 6, 4);
    sidebarLayout->addWidget(m_searchField);

    m_sidebar = new SidebarPainter(sidebarCol);
    m_sidebar->setModel(m_conversations);
    m_sidebar->setApi(m_api);
    m_sidebar->setDarkMode(m_darkMode);
    sidebarLayout->addWidget(m_sidebar, 1);

    connect(m_searchField, &QLineEdit::textChanged, m_sidebar, &SidebarPainter::setFilterText);
    connect(m_sidebar, &SidebarPainter::conversationClicked, this, &MainWindow::onConversationSelected);
    connect(m_sidebar, &SidebarPainter::contextMenuRequested, this, [this](int modelIndex, int notifLevel, qreal, qreal) {
        auto *menu = new QMenu(this);
        auto *action = menu->addAction(notifLevel == 3 ? "Unmute" : "Mute");
        connect(action, &QAction::triggered, this, [this, modelIndex, notifLevel]() {
            int newLevel = (notifLevel == 3) ? 0 : 3;
            m_conversations->setNotificationLevel(modelIndex, newLevel);
        });
        menu->popup(QCursor::pos());
    });

    sidebarCol->setMinimumWidth(56);
    sidebarCol->setMaximumWidth(500);

    // ── Topics panel ──
    m_threadsPanel = new QWidget(m_chatPage);
    auto *threadsLayout = new QVBoxLayout(m_threadsPanel);
    threadsLayout->setContentsMargins(0, 0, 0, 0);
    threadsLayout->setSpacing(0);

    m_threadsPainter = new ThreadsPainter(m_threadsPanel);
    m_threadsPainter->setModel(m_threads);
    m_threadsPainter->setDarkMode(m_darkMode);
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

    connect(m_threadsPainter, &ThreadsPainter::newTopicClicked, this, [this]() {
        // TODO: inline topic creation input
    });

    // ── Chat area ──
    auto *chatCol = new QWidget(m_chatPage);
    auto *chatLayout = new QVBoxLayout(chatCol);
    chatLayout->setContentsMargins(0, 0, 0, 0);
    chatLayout->setSpacing(0);

    m_header = new HeaderPainter(chatCol);
    m_header->setDarkMode(m_darkMode);
    m_header->setApi(m_api);
    chatLayout->addWidget(m_header);

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

    m_chatPainter = new ChatPainter(chatCol);
    m_chatPainter->setModel(m_messages);
    m_chatPainter->setMyUserId(m_auth->userId());
    m_chatPainter->setDarkMode(m_darkMode);
    m_chatPainter->setFontScale(m_fontScale);
    chatLayout->addWidget(m_chatPainter, 1);

    m_composer = new ComposerWidget(chatCol);
    m_composer->setSignaling(m_signaling);
    m_composer->setMessageModel(m_messages);
    chatLayout->addWidget(m_composer);

    connect(m_composer, &ComposerWidget::sendMessage, this, [this](const QString &text) {
        int replyId = m_activeThreadId;
        m_messages->sendMessage(text, replyId);
    });

    // Chat mouse interaction — wheel and click are handled by ChatPainter directly
    // Link/file clicks from ChatPainter signals
    connect(m_chatPainter, &ChatPainter::linkActivated, this, [](const QString &url) {
        QDesktopServices::openUrl(QUrl(url));
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
    m_splitter->setHandleWidth(5);

    mainLayout->addWidget(m_splitter);

    // ── Model signals ──
    connect(m_messages, &MessageListModel::conversationTokenChanged, this, [this]() {
        closeThread();
        m_chatPainter->scrollToBottom();
    });

    connect(m_messages, &MessageListModel::newMessagesAtEnd, this, [this]() {
        if (m_chatPainter->atBottom())
            m_chatPainter->scrollToBottom();
    });

    connect(m_messages, &MessageListModel::loadingChanged, this, [this]() {
        m_header->setLoading(m_messages->isLoading());
    });

    // Topic mode detection
    connect(m_threads, &ThreadListModel::hasTopicsChanged, this, [this]() {
        bool active = m_threads->hasTopics() && m_auth->hasThreadsSupport();
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
    connect(m_callManager, &CallManager::durationChanged, this, [this]() {
        m_header->setCallDuration(m_callManager->callDuration());
    });

    // Update userId when logged in
    connect(m_auth, &AuthManager::userInfoChanged, this, [this]() {
        m_chatPainter->setMyUserId(m_auth->userId());
    });

    m_stack->addWidget(m_chatPage);
}

void MainWindow::sidebarSqueezedChanged()
{
    m_sidebar->setSqueezed(m_sidebarSqueezed);
    m_header->setSidebarSqueezed(m_sidebarSqueezed);
    if (m_sidebarSqueezed) {
        m_splitter->setSizes({56, m_showTopics ? 240 : 0, m_splitter->width() - 56 - (m_showTopics ? 240 : 0)});
    } else {
        m_splitter->setSizes({280, m_showTopics ? 240 : 0, m_splitter->width() - 280 - (m_showTopics ? 240 : 0)});
    }
}

void MainWindow::onConversationSelected(const QString &token, const QString &name,
                                         const QString &userId, int convType,
                                         const QString &userStatus)
{
    m_activeConvToken = token;
    m_header->setConversationName(name);
    m_header->setConversationUserId(userId);
    m_header->setConversationType(convType);
    m_header->setPeerStatus(userStatus);
    m_header->setActiveThreadId(0);
    m_header->setActiveThreadTitle("");
    m_header->setIsInTopicMode(false);
    m_isInTopicMode = false;

    m_conversations->clearUnreadForToken(token);
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
    m_threadsPanel->setVisible(active);

    if (active) {
        m_sidebarSqueezed = true;
        sidebarSqueezedChanged();
        m_conversations->setHasTopics(m_activeConvToken, true);
        setMinimumWidth(600);
    } else {
        setMinimumWidth(500);
    }
}

void MainWindow::switchToChat()
{
    m_chatMode = true;
    m_stack->setCurrentWidget(m_chatPage);
    restoreChatGeometry();
    m_conversations->refresh();
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
    if (m_chatMode && m_closeToTray) {
        event->ignore();
        m_wasMaximized = isMaximized();
        hide();
    } else {
        saveWindowState();
        event->accept();
    }
}

void MainWindow::restoreFromTray()
{
    if (m_wasMaximized)
        showMaximized();
    else
        showNormal();
    raise();
    activateWindow();
}

void MainWindow::saveWindowState()
{
    if (!m_chatMode || !m_geometrySaveEnabled) return;

    m_windowSettings.beginGroup("WindowGeometry");
    if (isMaximized()) {
        m_windowSettings.setValue("savedVisibility", 4);
    } else {
        m_windowSettings.setValue("savedX", x());
        m_windowSettings.setValue("savedY", y());
        m_windowSettings.setValue("savedWidth", width());
        m_windowSettings.setValue("savedHeight", height());
        m_windowSettings.setValue("savedVisibility", 2);
    }
    m_windowSettings.endGroup();
}

void MainWindow::restoreChatGeometry()
{
    m_geometrySaveEnabled = false;
    setMinimumWidth(500);

    m_windowSettings.beginGroup("WindowGeometry");
    int w = qMax(m_windowSettings.value("savedWidth", 1000).toInt(), 500);
    int h = qMax(m_windowSettings.value("savedHeight", 700).toInt(), 400);
    int sx = m_windowSettings.value("savedX", -1).toInt();
    int sy = m_windowSettings.value("savedY", -1).toInt();
    int vis = m_windowSettings.value("savedVisibility", 2).toInt();
    m_windowSettings.endGroup();

    resize(w, h);
    if (sx != -1 || sy != -1)
        move(sx, sy);

    if (vis == 4)
        showMaximized();
    else
        show();

    // Delay enabling geometry save
    QTimer::singleShot(500, this, [this]() {
        m_geometrySaveEnabled = true;
    });
}

void MainWindow::applyDarkPalette()
{
    PainterTheme theme(m_darkMode, 1.0);
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

void MainWindow::moveEvent(QMoveEvent *)
{
    m_saveGeometryTimer.start();
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    m_saveGeometryTimer.start();
}
