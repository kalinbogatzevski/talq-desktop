#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QIcon>
#include <QWindow>
#include <QRegularExpression>
#include <QSharedMemory>

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
#include "core/CallPipeline.h"
#include "core/MediaDeviceManager.h"
#include "core/CallManager.h"
#include <gst/gst.h>

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    QApplication app(argc, argv);

    // Single-instance guard — attach first to clean stale segments
    QSharedMemory singleInstance("TalQ_SingleInstance_Lock");
    singleInstance.attach();  // clears stale segment from previous crash
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
    app.setWindowIcon(QIcon(":/logo.png"));  // TalQ icon always
#ifdef TALQ_BUILD_TS
    app.setApplicationVersion("0.6.1-" TALQ_BUILD_TS);
#else
    app.setApplicationVersion("0.6.1");
#endif

    QQuickStyle::setStyle("Basic");

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
    deviceManager.refresh();
    CallPipeline callPipeline;
    CallManager callManager(&api, &signaling);

    // QML engine
    QQmlApplicationEngine engine;

    // Expose to QML
    engine.rootContext()->setContextProperty("api", &api);
    engine.rootContext()->setContextProperty("auth", &auth);
    engine.rootContext()->setContextProperty("conversationModel", &conversations);
    engine.rootContext()->setContextProperty("messageModel", &messages);
    engine.rootContext()->setContextProperty("threadModel", &threads);
    engine.rootContext()->setContextProperty("pushClient", &push);
    engine.rootContext()->setContextProperty("signaling", &signaling);
    engine.rootContext()->setContextProperty("notifications", &notifications);
    engine.rootContext()->setContextProperty("deviceManager", &deviceManager);
    engine.rootContext()->setContextProperty("callManager", &callManager);

    // Branding context
#ifdef TALQ_BRAND_123NET
    engine.rootContext()->setContextProperty("brandName", QString("123NET TalQ"));
    engine.rootContext()->setContextProperty("brandServer", QString("https://ncloud.123net.link"));
    engine.rootContext()->setContextProperty("brandLogo", QString("qrc:/123net-logo.png"));
    engine.rootContext()->setContextProperty("isBranded", true);
#else
    engine.rootContext()->setContextProperty("brandName", QString("TalQ"));
    engine.rootContext()->setContextProperty("brandServer", QString(""));
    engine.rootContext()->setContextProperty("brandLogo", QString("qrc:/logo.png"));
    engine.rootContext()->setContextProperty("isBranded", false);
#endif

    engine.addImageProvider("avatar", new AvatarProvider(&api));
    engine.addImageProvider("preview", new FilePreviewProvider(&api));

    engine.loadFromModule("TalkQt", "Main");

    if (engine.rootObjects().isEmpty())
        return -1;

    // Wire notifications
    if (auto *window = qobject_cast<QQuickWindow*>(engine.rootObjects().first())) {
        notifications.setWindow(window);
        QObject::connect(&notifications, &NotificationManager::showRequested, window, [window]() {
            window->show();
            window->raise();
            window->requestActivate();
        });
    }

    // Notify on new polled messages in active conversation
    QObject::connect(&messages, &MessageListModel::newMessagesAtEnd, &notifications, [&messages, &notifications, &auth]() {
        int count = messages.rowCount();
        if (count == 0) return;
        auto idx = messages.index(count - 1);
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

    // Tray unread count
    QObject::connect(&conversations, &ConversationListModel::totalUnreadChanged,
                     &notifications, [&conversations, &notifications]() {
        notifications.updateUnreadCount(conversations.totalUnread());
    });

    // Push events → refresh conversation list
    QObject::connect(&push, &PushClient::pushReceived, &conversations, [&conversations](const QString &type) {
        if (type == "notify_notification" || type == "notify_activities") {
            conversations.refresh();
        }
    });

    // Start push + conversation polling after login
    QObject::connect(&auth, &AuthManager::loggedInChanged, &conversations, [&auth, &conversations, &push, &signaling]() {
        if (auth.isLoggedIn()) {
            push.start();
            signaling.start();
        } else {
            push.stop();
            signaling.stop();
            conversations.stopAutoRefresh();
        }
    });

    // User status heartbeat — keep "online" while app is running
    QTimer statusTimer;
    statusTimer.setInterval(120000); // 2 minutes
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

#ifdef Q_OS_WIN
    // Force dark title bar on Windows
    for (auto *obj : engine.rootObjects()) {
        if (auto *window = qobject_cast<QWindow*>(obj)) {
            HWND hwnd = reinterpret_cast<HWND>(window->winId());
            BOOL darkMode = TRUE;
            DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));
        }
    }
#endif

    // Restore session after QML is loaded so loading screen is visible
    auth.tryRestore();



#ifdef Q_OS_WIN
    // Memory monitor — every 30s (debug)
    QTimer memTimer;
    memTimer.setInterval(30000);
    QObject::connect(&memTimer, &QTimer::timeout, [&]() {
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            qDebug() << "MEM:" << (pmc.WorkingSetSize / 1024 / 1024) << "MB"
                     << "msgs:" << messages.rowCount()
                     << "convs:" << conversations.rowCount();
        }
    });
    memTimer.start();
#endif

    return app.exec();
}
