#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include "core/ApiClient.h"
#include "core/AuthManager.h"
#include "models/ConversationListModel.h"
#include "models/MessageListModel.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName("TalQ");
    app.setOrganizationName("TalQ");
    app.setApplicationVersion("0.2.0");

    QQuickStyle::setStyle("Basic");

    // Core services
    ApiClient api;
    AuthManager auth(&api);
    ConversationListModel conversations(&api);
    MessageListModel messages(&api);

    // QML engine
    QQmlApplicationEngine engine;

    // Expose to QML
    engine.rootContext()->setContextProperty("api", &api);
    engine.rootContext()->setContextProperty("auth", &auth);
    engine.rootContext()->setContextProperty("conversationModel", &conversations);
    engine.rootContext()->setContextProperty("messageModel", &messages);

    engine.loadFromModule("TalkQt", "Main");

    if (engine.rootObjects().isEmpty())
        return -1;

    // Restore session after QML is loaded so loading screen is visible
    auth.tryRestore();

    return app.exec();
}
