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
#include "models/ConversationListModel.h"
#include "models/MessageListModel.h"
#include "models/ThreadListModel.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("TalQ");
    app.setOrganizationName("TalQ");
    app.setApplicationVersion("0.2.0");
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

    // QML engine
    QQmlApplicationEngine engine;

    // Expose to QML
    engine.rootContext()->setContextProperty("api", &api);
    engine.rootContext()->setContextProperty("auth", &auth);
    engine.rootContext()->setContextProperty("conversationModel", &conversations);
    engine.rootContext()->setContextProperty("messageModel", &messages);
    engine.rootContext()->setContextProperty("threadModel", &threads);
    engine.rootContext()->setContextProperty("notifications", &notifications);

    engine.addImageProvider("avatar", new AvatarProvider(&api));

    engine.loadFromModule("TalkQt", "Main");

    if (engine.rootObjects().isEmpty())
        return -1;

    // Wire notification manager to window and message model
    if (auto *window = qobject_cast<QQuickWindow*>(engine.rootObjects().first())) {
        notifications.setWindow(window);

        // Show window when tray icon clicked
        QObject::connect(&notifications, &NotificationManager::showRequested, window, [window]() {
            window->show();
            window->raise();
            window->requestActivate();
        });
    }

    // Notify on new polled messages (not sent by us)
    QObject::connect(&messages, &MessageListModel::newMessagesAtEnd, &notifications, [&messages, &notifications, &auth]() {
        // Get the last message to show in notification
        int count = messages.rowCount();
        if (count == 0) return;

        auto idx = messages.index(count - 1);
        QString actorId = messages.data(idx, MessageListModel::ActorIdRole).toString();

        // Don't notify for our own messages
        if (actorId == auth.userId()) return;

        QString actorName = messages.data(idx, MessageListModel::ActorNameRole).toString();
        QString text = messages.data(idx, MessageListModel::MessageTextRole).toString();

        // Strip HTML tags for notification
        text.remove(QRegularExpression("<[^>]*>"));
        if (text.length() > 100) text = text.left(100) + "...";

        notifications.notify(actorName, text);
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
