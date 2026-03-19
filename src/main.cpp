#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QIcon>
#include <QWindow>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

#include "core/ApiClient.h"
#include "core/AvatarProvider.h"
#include "core/AuthManager.h"
#include "core/MessageCache.h"
#include "core/NotificationManager.h"
#include "core/PushClient.h"
#include "models/ConversationListModel.h"
#include "models/MessageListModel.h"
#include "models/ThreadListModel.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("TalQ");
    app.setOrganizationName("TalQ");
#ifdef TALQ_BUILD_TS
    app.setApplicationVersion("0.4.0-" TALQ_BUILD_TS);
#else
    app.setApplicationVersion("0.4.0");
#endif
    app.setWindowIcon(QIcon(":/logo.png"));

    QQuickStyle::setStyle("Basic");

    // Core services
    ApiClient api;
    AuthManager auth(&api);
    ConversationListModel conversations(&api);
    MessageCache cache;
    MessageListModel messages(&api, &cache);
    ThreadListModel threads(&api);
    NotificationManager notifications;
    PushClient push(&api);

    // QML engine
    QQmlApplicationEngine engine;

    // Expose to QML
    engine.rootContext()->setContextProperty("api", &api);
    engine.rootContext()->setContextProperty("auth", &auth);
    engine.rootContext()->setContextProperty("conversationModel", &conversations);
    engine.rootContext()->setContextProperty("messageModel", &messages);
    engine.rootContext()->setContextProperty("threadModel", &threads);
    engine.rootContext()->setContextProperty("pushClient", &push);
    engine.rootContext()->setContextProperty("notifications", &notifications);

    engine.addImageProvider("avatar", new AvatarProvider(&api));

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
        notifications.notify(actorName, text);
    });

    // Notify on new messages in OTHER conversations
    QObject::connect(&conversations, &ConversationListModel::newUnreadMessage,
                     &notifications, [&notifications, &messages](const QString &name, const QString &lastMsg, const QString &token) {
        if (token == messages.conversationToken()) return;
        QString preview = lastMsg;
        preview.remove(QRegularExpression("<[^>]*>"));
        if (preview.length() > 80) preview = preview.left(80) + "...";
        notifications.notify(name, preview, true);
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
    QObject::connect(&auth, &AuthManager::loggedInChanged, &conversations, [&auth, &conversations, &push]() {
        if (auth.isLoggedIn()) {
            push.start();
            conversations.startAutoRefresh();  // 30s fallback for push
        } else {
            push.stop();
            conversations.stopAutoRefresh();
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

    return app.exec();
}
