#include <QApplication>
#include <QIcon>
#include <QRegularExpression>
#include <QSharedMemory>
#include <QScreen>
#include <QPainter>
#include <QSplashScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QTime>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

#include "core/ApiClient.h"
#include "core/AvatarProvider.h"
#include "core/AuthManager.h"
#include "core/MessageCache.h"
#include "core/NotificationManager.h"
#include "core/PushClient.h"
#include "core/SignalingClient.h"
#include "core/FilePreviewProvider.h"
#include "models/ConversationListModel.h"
#include "models/MessageListModel.h"
#include "models/ThreadListModel.h"
#include "core/MediaDeviceManager.h"
#include "core/CallManager.h"
#include "core/DebugMonitor.h"
#include "core/AppSettings.h"
#include "core/EmojiData.h"
#include "core/TalqLog.h"
#include "ui/MainWindow.h"
#include "ui/NotificationPopup.h"
#include <gst/gst.h>

int main(int argc, char *argv[])
{
    // Detect --debug flag before anything else so verbose logging is on
    // from the very first line executed.
    bool debugRequested = false;
    for (int i = 1; i < argc; ++i) {
        QByteArray a(argv[i]);
        if (a == "--debug" || a == "-d") { debugRequested = true; break; }
    }
    bool enableFileLogging = debugRequested;
#ifdef QT_DEBUG
    enableFileLogging = true;
#endif
    if (enableFileLogging) TalqLog::g_verbose = true;

    // Set GStreamer plugin path relative to the exe (before gst_init)
    {
        std::string exePath(argv[0]);
        auto lastSlash = exePath.find_last_of("\\/");
        std::string appDir = (lastSlash != std::string::npos) ? exePath.substr(0, lastSlash) : ".";
        std::string gstPath = appDir + "/gst-plugins";
        qputenv("GST_PLUGIN_PATH", QByteArray::fromStdString(gstPath));
    }

    QString logPath;
    if (enableFileLogging) {
        qputenv("QT_FORCE_STDERR_LOGGING", "1");
        qputenv("GST_DEBUG", "2");  // gst warnings into the log
        logPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/talq_debug.log";
        QDir().mkpath(QFileInfo(logPath).absolutePath());
        // Redirect C stderr to a log file (Release builds have no console,
        // so this is the only way to capture GStreamer / any fprintf output).
        freopen(logPath.toUtf8().constData(), "w", stderr);
        qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &msg) {
            QByteArray line = (QTime::currentTime().toString("HH:mm:ss.zzz") + " " + msg + "\n").toUtf8();
            fwrite(line.constData(), 1, line.size(), stderr);
            fflush(stderr);
        });
        qInfo().noquote() << "[TalQ] verbose logging enabled; log at" << logPath;
    }

    gst_init(&argc, &argv);

    QApplication app(argc, argv);

    // Single-instance guard
    QSharedMemory singleInstance("TalQ_SingleInstance_Lock");
    singleInstance.attach();
    singleInstance.detach();
    if (!singleInstance.create(1)) {
        qWarning() << "TalQ is already running!";
        return 0;
    }
#ifdef TALQ_BRAND_123NET
    app.setApplicationName("123NET TalQ");
    app.setOrganizationName("123NET");
#else
    app.setApplicationName("TalQ");
    app.setOrganizationName("TalQ");
#endif
    app.setWindowIcon(QIcon(":/logo.png"));

    // EmojiData reads/writes recents via QSettings — must run after
    // setApplicationName/setOrganizationName so the right storage is used.
    EmojiData::initialize();
#ifdef TALQ_BUILD_TS
    app.setApplicationVersion(TALQ_VERSION "-" TALQ_BUILD_TS);
#else
    app.setApplicationVersion(TALQ_VERSION);
#endif

    // Splash screen — dark themed, centered logo, smooth rendering
    {
#ifdef TALQ_BRAND_123NET
        QPixmap logoPix(":/123net-logo.png");
#else
        QPixmap logoPix(":/logo.png");
#endif
        // Create a dark splash canvas
        QPixmap splashCanvas(380, 280);
        splashCanvas.fill(QColor("#121210"));
        QPainter p(&splashCanvas);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setRenderHint(QPainter::Antialiasing);

        // Draw logo centered
        QPixmap logo = logoPix.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        p.drawPixmap((380 - logo.width()) / 2, 60, logo);

        // App name
        QFont nameFont;
        nameFont.setPixelSize(24);
        nameFont.setWeight(QFont::DemiBold);
        p.setFont(nameFont);
        p.setPen(QColor("#e4e0da"));
        p.drawText(QRect(0, 170, 380, 30), Qt::AlignCenter, app.applicationName());

        // Version
        QFont verFont;
        verFont.setPixelSize(12);
        p.setFont(verFont);
        p.setPen(QColor("#8a8680"));
        p.drawText(QRect(0, 200, 380, 20), Qt::AlignCenter, "v" + app.applicationVersion());

        // Connecting status
        p.drawText(QRect(0, 240, 380, 20), Qt::AlignCenter, "Connecting...");
        p.end();

        static QSplashScreen splash(splashCanvas);
        splash.show();
        app.processEvents();

        // Splash stays at least 1.5s for branding visibility
        QTimer::singleShot(1500, &splash, [&splash]() {
            splash.close();
        });
    }

    // Core services
    ApiClient api;
    AuthManager auth(&api);
    ConversationListModel conversations(&api);
    MessageCache cache;
    MessageListModel messages(&api, &cache);
    ThreadListModel threads(&api);
    threads.setCache(&cache);
    NotificationManager notifications;
    PushClient push(&api);
    SignalingClient signaling(&api);
    MediaDeviceManager deviceManager;
    CallManager callManager(&api, &signaling, &deviceManager);

    DebugMonitor debug;
    AppSettings appSettings;

    // Legacy image providers (used for debug monitor cache stats)
    AvatarProvider avatarProvider(&api);
    FilePreviewProvider previewProvider(&api);

    MainWindow window(
        &api, &auth, &conversations, &messages, &threads,
        &notifications, &push, &signaling, &deviceManager,
        &callManager, &debug, &appSettings,
        &avatarProvider, &previewProvider
    );

    // Custom notification popup
    NotificationPopup notifPopup;
    QObject::connect(&notifications, &NotificationManager::desktopPopupRequested,
                     &notifPopup, [&notifPopup, &window](const QString &title, const QString &message, const QString &token) {
        // Position at bottom-right of primary screen
        QScreen *screen = QApplication::primaryScreen();
        if (!screen) return;
        QRect screenGeom = screen->availableGeometry();
        QPoint pos(screenGeom.right() - notifPopup.width() - 16,
                   screenGeom.bottom() - notifPopup.height() - 16);
        notifPopup.showNotification(title, message, token, pos);
    });
    QObject::connect(&notifPopup, &NotificationPopup::clicked,
                     &window, [&window](const QString &token) {
        window.openConversation(token);
    });

    // Update conversation list preview when new messages arrive
    QObject::connect(&messages, &MessageListModel::newMessagesAtEnd, &conversations, [&messages, &conversations]() {
        int count = messages.rowCount();
        if (count == 0) return;
        auto idx = messages.index(0);
        QString author = messages.data(idx, MessageListModel::ActorNameRole).toString();
        QString text = messages.data(idx, MessageListModel::MessageTextRole).toString();
        text.remove(QRegularExpression("<[^>]*>"));
        if (text.length() > 80) text = text.left(80) + "...";
        conversations.updateLastMessage(messages.conversationToken(), author, text);
    });

    // Notify on new polled messages in active conversation
    QObject::connect(&messages, &MessageListModel::newMessagesAtEnd, &notifications, [&messages, &notifications, &auth]() {
        int count = messages.rowCount();
        if (count == 0) return;
        auto idx = messages.index(0);  // model is newest-first, index 0 = latest message
        QString actorId = messages.data(idx, MessageListModel::ActorIdRole).toString();
        if (actorId == auth.userId()) return;
        QString actorName = messages.data(idx, MessageListModel::ActorNameRole).toString();
        QString text = messages.data(idx, MessageListModel::MessageTextRole).toString();
        text.remove(QRegularExpression("<[^>]*>"));
        if (text.length() > 100) text = text.left(100) + "...";
        notifications.notify(actorName, text, false, messages.conversationToken());
    });

    // Notify on new messages in OTHER conversations
    QObject::connect(&conversations, &ConversationListModel::newUnreadMessage,
                     &notifications, [&notifications, &messages](const QString &name, const QString &lastMsg, const QString &token) {
        if (token == messages.conversationToken()) return;
        QString preview = lastMsg;
        preview.remove(QRegularExpression("<[^>]*>"));
        if (preview.length() > 80) preview = preview.left(80) + "...";
        notifications.notify(name, preview, true, token);
    });

    // Incoming call detection
    QObject::connect(&conversations, &ConversationListModel::incomingCallDetected,
                     &callManager, [&callManager](const QString &name, const QString &token, int callFlag) {
        callManager.onIncomingCallDetected(name, token, callFlag);
    });

    // Tray unread count
    QObject::connect(&conversations, &ConversationListModel::totalUnreadChanged,
                     &notifications, [&conversations, &notifications]() {
        notifications.updateUnreadCount(conversations.totalUnread());
    });

    // Push events -> refresh
    QObject::connect(&push, &PushClient::pushReceived, &conversations, [&conversations, &messages](const QString &type) {
        qDebug() << "Push event received:" << type << "-- refreshing conversations + messages";
        conversations.refresh();
        messages.refresh();  // instant read status + new message pickup
    });

    // Start push + signaling after login (both fresh login AND session restore)
    auto startServices = [&auth, &conversations, &push, &signaling]() {
        if (auth.isLoggedIn()) {
            push.start();
            signaling.start();
            conversations.startAutoRefresh();
        } else {
            push.stop();
            signaling.stop();
            conversations.stopAutoRefresh();
        }
    };
    QObject::connect(&auth, &AuthManager::loggedInChanged, &conversations, startServices);
    QObject::connect(&auth, &AuthManager::restoringChanged, &conversations, [&auth, startServices]() {
        if (!auth.isRestoringSession() && auth.isLoggedIn())
            startServices();
    });

    // User status heartbeat
    QTimer statusTimer;
    statusTimer.setInterval(120000);
    QObject::connect(&statusTimer, &QTimer::timeout, [&api]() {
        QJsonObject body;
        body["statusType"] = "online";
        api.put("apps/user_status/api/v1/user_status/status", body, [](bool ok, const QJsonObject &, int) {
            if (!ok) qWarning() << "Status heartbeat failed";
        });
    });
    QObject::connect(&auth, &AuthManager::loggedInChanged, &api, [&auth, &api, &statusTimer]() {
        if (auth.isLoggedIn()) {
            QJsonObject body;
            body["statusType"] = "online";
            api.put("apps/user_status/api/v1/user_status/status", body, [](bool ok, const QJsonObject &, int) {
                qDebug() << "User status set:" << (ok ? "online" : "failed");
            });
            statusTimer.start();
        } else {
            statusTimer.stop();
        }
    });

    // Restore session
    auth.tryRestore();

    // Debug monitor feed
    QObject::connect(&debug, &DebugMonitor::updated, [&]() {
        debug.setMessageCount(messages.rowCount());
        debug.setConversationCount(conversations.rowCount());
        debug.setAvatarCacheCount(avatarProvider.cacheCount());
        debug.setPreviewCacheCount(previewProvider.cacheCount());
        debug.setPreviewCacheBytes(previewProvider.cacheBytes());
        debug.setPendingRequests(api.pendingCount());
    });

    return app.exec();
}
