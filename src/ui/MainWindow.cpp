#include "MainWindow.h"
#include "CallDialog.h"
#include "SettingsDialog.h"
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
#include <QDesktopServices>
#include <QDialog>
#include <QClipboard>
#include <QRegularExpression>
#include <QWidgetAction>
#include <QMessageBox>
#include <QNetworkReply>
#include <QLabel>
#include <QUrl>

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
        m_darkMode = !m_darkMode;
        m_sidebar->setDarkMode(m_darkMode);
        m_header->setDarkMode(m_darkMode);
        m_chatPainter->setDarkMode(m_darkMode);
        m_threadsPainter->setDarkMode(m_darkMode);
        applyDarkPalette();
        m_settings.beginGroup("Theme");
        m_settings.setValue("darkMode", m_darkMode);
        m_settings.endGroup();
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

    m_searchField = new QLineEdit(sidebarCol);
    m_searchField->setPlaceholderText("Search conversations...");
    m_searchField->setMinimumHeight(32);
    QFont sf; sf.setPixelSize(12);
    m_searchField->setFont(sf);
    m_searchField->setContentsMargins(6, 4, 6, 4);

    // ── User profile header ──
    auto *profileBar = new QWidget(sidebarCol);
    profileBar->setFixedHeight(52);
    profileBar->installEventFilter(this);  // for paint

    auto *profileLayout = new QHBoxLayout(profileBar);
    profileLayout->setContentsMargins(12, 8, 12, 8);
    profileLayout->setSpacing(10);

    // Avatar label (will be painted by SidebarPainter's avatar cache)
    auto *profileAvatar = new QLabel(profileBar);
    profileAvatar->setFixedSize(36, 36);
    profileAvatar->setStyleSheet("border-radius: 18px; background: #2ec4b6;");
    profileLayout->addWidget(profileAvatar);

    // Display name
    m_profileNameLabel = new QLabel(profileBar);
    m_profileNameLabel->setStyleSheet("font-weight: bold; font-size: 14px;");
    profileLayout->addWidget(m_profileNameLabel, 1);

    // Settings button
    auto *settingsBtn = new QPushButton("\u2699", profileBar);
    settingsBtn->setFixedSize(28, 28);
    settingsBtn->setFlat(true);
    settingsBtn->setStyleSheet("font-size: 16px; border: none; border-radius: 14px;");
    settingsBtn->setCursor(Qt::PointingHandCursor);
    profileLayout->addWidget(settingsBtn);

    connect(settingsBtn, &QPushButton::clicked, this, [this]() {
        if (!m_settingsDialog) {
            m_settingsDialog = new SettingsDialog(
                m_deviceManager, m_notifications, m_appSettings, m_auth, this);
            connect(m_settingsDialog, &SettingsDialog::closeToTrayChanged,
                    this, [this](bool enabled) { m_closeToTray = enabled; });
        }
        m_settingsDialog->refresh();
        m_settingsDialog->exec();
    });

    sidebarLayout->addWidget(profileBar);
    sidebarLayout->addWidget(m_searchField);

    m_profileAvatarLabel = profileAvatar;

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
    m_sidebar->setDarkMode(m_darkMode);
    sidebarLayout->addWidget(m_sidebar, 1);

    connect(m_searchField, &QLineEdit::textChanged, m_sidebar, &SidebarPainter::setFilterText);
    connect(m_sidebar, &SidebarPainter::conversationClicked, this, &MainWindow::onConversationSelected);
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

    // Welcome screen (shown when no conversation selected)
    m_welcomeWidget = new QWidget(chatCol);
    auto *welcomeLayout = new QVBoxLayout(m_welcomeWidget);
    welcomeLayout->setAlignment(Qt::AlignCenter);
    welcomeLayout->setSpacing(16);

    auto *logoLabel = new QLabel(m_welcomeWidget);
    QPixmap logo(":/logo.png");
    if (!logo.isNull())
        logoLabel->setPixmap(logo.scaled(80, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logoLabel->setAlignment(Qt::AlignCenter);
    welcomeLayout->addWidget(logoLabel);

    m_welcomeNameLabel = new QLabel(m_welcomeWidget);
    m_welcomeNameLabel->setAlignment(Qt::AlignCenter);
    m_welcomeNameLabel->setStyleSheet("font-size: 22px; font-weight: bold;");
    welcomeLayout->addWidget(m_welcomeNameLabel);

    auto *pickLabel = new QLabel("Pick a conversation from the sidebar", m_welcomeWidget);
    pickLabel->setAlignment(Qt::AlignCenter);
    pickLabel->setStyleSheet("font-size: 14px; color: #8a8680;");
    welcomeLayout->addWidget(pickLabel);

    auto *versionLabel = new QLabel("v" + QApplication::applicationVersion(), m_welcomeWidget);
    versionLabel->setAlignment(Qt::AlignCenter);
    versionLabel->setStyleSheet("font-size: 11px; color: #5a5850;");
    welcomeLayout->addWidget(versionLabel);

    // Server info card
    auto *serverCard = new QWidget(m_welcomeWidget);
    serverCard->setMaximumWidth(420);
    serverCard->setStyleSheet("background: #1c1c1a; border-radius: 12px; border: 1px solid #3a3a36;");
    auto *serverLayout = new QVBoxLayout(serverCard);
    serverLayout->setContentsMargins(20, 16, 20, 16);
    serverLayout->setSpacing(10);

    auto addInfoRow = [&](const QString &icon, const QString &text, const QString &color = "#8a8680") {
        auto *row = new QHBoxLayout();
        row->setSpacing(12);
        auto *iconLbl = new QLabel(icon, serverCard);
        iconLbl->setFixedWidth(28);
        iconLbl->setStyleSheet(QString("font-size: 16px; color: %1;").arg(color));
        row->addWidget(iconLbl);
        auto *textLbl = new QLabel(text, serverCard);
        textLbl->setStyleSheet(QString("font-size: 14px; color: %1;").arg(color));
        row->addWidget(textLbl, 1);
        serverLayout->addLayout(row);
        return textLbl;
    };

    auto *sectionLbl = new QLabel("Server", serverCard);
    sectionLbl->setStyleSheet("font-size: 11px; font-weight: bold; color: #6a6660; letter-spacing: 1px;");
    serverLayout->addWidget(sectionLbl);

    m_welcomeServerLabel = addInfoRow("\u2601", "", "#2ec4b6");
    m_welcomeNcLabel = addInfoRow("\u24C3", "");
    m_welcomeTalkLabel = addInfoRow("\u260E", "");
    m_welcomeSignalingLabel = addInfoRow("\u26A1", "");
    m_welcomePushLabel = addInfoRow("\u25CF", "");

    // Live-update status labels when services connect/disconnect
    connect(m_signaling, &SignalingClient::connectedChanged, this, [this]() {
        if (!m_welcomeSignalingLabel) return;
        bool on = m_signaling->isConnected();
        m_welcomeSignalingLabel->setText(on ? "Signaling connected" : "Signaling disconnected");
        m_welcomeSignalingLabel->setStyleSheet(QString("font-size: 14px; color: %1;").arg(on ? "#5ec76a" : "#8a8680"));
    });
    connect(m_push, &PushClient::connectedChanged, this, [this]() {
        if (!m_welcomePushLabel) return;
        bool on = m_push->isConnected();
        m_welcomePushLabel->setText(on ? "Push connected (real-time)" : "Push disconnected (polling)");
        m_welcomePushLabel->setStyleSheet(QString("font-size: 14px; color: %1;").arg(on ? "#5ec76a" : "#8a8680"));
    });

    welcomeLayout->addWidget(serverCard, 0, Qt::AlignCenter);

    chatLayout->addWidget(m_welcomeWidget, 1);

    // Chat content (hidden until conversation selected)
    m_chatPainter = new ChatPainter(chatCol);
    m_chatPainter->setModel(m_messages);
    m_chatPainter->setMyUserId(m_auth->userId());
    m_chatPainter->setDarkMode(m_darkMode);
    m_chatPainter->setFontScale(m_fontScale);
    m_chatPainter->hide();
    chatLayout->addWidget(m_chatPainter, 1);

    m_composer = new ComposerWidget(chatCol);
    m_composer->setSignaling(m_signaling);
    m_composer->setMessageModel(m_messages);
    m_composer->hide();
    chatLayout->addWidget(m_composer);

    connect(m_composer, &ComposerWidget::sendMessage, this, [this](const QString &text) {
        int replyId = m_replyToId > 0 ? m_replyToId : m_activeThreadId;
        m_messages->sendMessage(text, replyId);
        m_replyToId = 0;
        m_replyToAuthor.clear();
        m_replyToText.clear();
        m_composer->hideReplyBar();
    });
    connect(m_composer, &ComposerWidget::replyBarCancelled, this, [this]() {
        m_replyToId = 0;
        m_replyToAuthor.clear();
        m_replyToText.clear();
    });

    // Drag-and-drop files onto chat → show confirmation in composer
    connect(m_chatPainter, &ChatPainter::fileDropped, m_composer, &ComposerWidget::showPendingFile);

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
        menu->addAction(QStringLiteral("\U0001F4CC  Pin"), this, [this, msgId]() {
            m_messages->pinMessage(msgId);
        });
        menu->addAction(QStringLiteral("\U0001F517  Copy link"), this, [this, msgId]() {
            QString link = m_messages->messageLink(msgId);
            QApplication::clipboard()->setText(link);
        });
        // Thread action only for group conversations (type 2, 3)
        if (m_header->conversationType() >= 2) {
            menu->addAction(QStringLiteral("\U0001F4AC  Thread"), this, [this, msgId]() {
                openThread(msgId, "Thread");
            });
        }

        if (isOwn) {
            menu->addSeparator();
            menu->addAction(QStringLiteral("\U0001F5D1\uFE0F  Delete"), this, [this, msgId]() {
                auto reply = QMessageBox::question(this, "Delete message",
                    "Are you sure you want to delete this message?",
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (reply == QMessageBox::Yes)
                    m_messages->deleteMessage(msgId);
            });
        }

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
            // Show image preview in a simple dialog
            QImage img = m_chatPainter->cachedPreview(fileId);
            if (!img.isNull()) {
                auto *viewer = new QDialog(this);
                viewer->setWindowTitle(fileName);
                viewer->setAttribute(Qt::WA_DeleteOnClose);
                viewer->setStyleSheet("background: #000000;");
                auto *label = new QLabel(viewer);
                QPixmap pix = QPixmap::fromImage(img);
                QSize screenSize = screen() ? screen()->availableSize() * 0.8 : QSize(800, 600);
                pix = pix.scaled(screenSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                label->setPixmap(pix);
                label->setAlignment(Qt::AlignCenter);
                auto *layout = new QVBoxLayout(viewer);
                layout->setContentsMargins(0, 0, 0, 0);
                layout->addWidget(label);
                viewer->resize(pix.size());
                viewer->show();
            }
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
    m_splitter->setStyleSheet("QSplitter::handle { background: #2a2a26; }");

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

    // Call dialog (shows/hides automatically via CallManager::stateChanged)
    m_callDialog = new CallDialog(m_callManager, this);

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

    m_stack->addWidget(m_chatPage);
}

void MainWindow::sidebarSqueezedChanged()
{
    m_sidebar->setSqueezed(m_sidebarSqueezed);
    m_header->setSidebarSqueezed(m_sidebarSqueezed);

    // Adjust sidebar constraints for squeeze mode
    m_sidebarCol->setMinimumWidth(m_sidebarSqueezed ? 56 : 200);
    m_sidebarCol->setMaximumWidth(m_sidebarSqueezed ? 56 : 500);

    int sideW = m_sidebarSqueezed ? 56 : 280;
    int topicsW = m_showTopics ? 240 : 0;
    m_splitter->setSizes({sideW, topicsW, m_splitter->width() - sideW - topicsW});
}

void MainWindow::onConversationSelected(const QString &token, const QString &name,
                                         const QString &userId, int convType,
                                         const QString &userStatus)
{
    m_activeConvToken = token;

    // Switch from welcome to chat
    m_welcomeWidget->hide();
    m_chatPainter->show();
    m_composer->show();

    m_header->setConversationName(name);
    m_header->setConversationUserId(userId);
    m_header->setConversationType(convType);
    m_header->setPeerStatus(userStatus);

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

    // Populate welcome screen
    m_welcomeNameLabel->setText("Welcome, " + m_auth->displayName());
    QString url = m_auth->serverUrl();
    url.remove(QRegularExpression("^https?://"));
    m_welcomeServerLabel->setText(url);
    m_welcomeNcLabel->setText("Nextcloud " + m_auth->nextcloudVersion());
    m_welcomeTalkLabel->setText("Talk " + m_auth->talkVersion());
    m_welcomeSignalingLabel->setText(m_signaling->isConnected() ? "Signaling connected" : "Signaling disconnected");
    m_welcomePushLabel->setText(m_push->isConnected() ? "Push connected (real-time)" : "Push disconnected (polling)");

    // Show welcome, hide chat content
    m_welcomeWidget->show();
    m_chatPainter->hide();
    m_composer->hide();
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
        m_wasMaximized = isMaximized();
        hide();
    } else {
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

void MainWindow::loadProfileAvatar(QLabel *avatarLabel)
{
    auto *reply = m_api->getAbsoluteUrl("/index.php/avatar/" + m_auth->userId() + "/64");
    connect(reply, &QNetworkReply::finished, this, [reply, avatarLabel]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        QImage img;
        img.loadFromData(reply->readAll());
        if (img.isNull()) return;
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
