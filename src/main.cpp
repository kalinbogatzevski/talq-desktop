#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>
#include <QWindow>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

#include "core/ApiClient.h"
#include "core/AvatarProvider.h"
#include "core/AuthManager.h"
#include "core/MessageCache.h"
#include "models/ConversationListModel.h"
#include "models/MessageListModel.h"
#include "models/ThreadListModel.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
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

    // QML engine
    QQmlApplicationEngine engine;

    // Expose to QML
    engine.rootContext()->setContextProperty("api", &api);
    engine.rootContext()->setContextProperty("auth", &auth);
    engine.rootContext()->setContextProperty("conversationModel", &conversations);
    engine.rootContext()->setContextProperty("messageModel", &messages);
    engine.rootContext()->setContextProperty("threadModel", &threads);

    engine.addImageProvider("avatar", new AvatarProvider(&api));

    engine.loadFromModule("TalkQt", "Main");

    if (engine.rootObjects().isEmpty())
        return -1;

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
