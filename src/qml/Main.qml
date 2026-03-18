import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 1000
    height: 700
    minimumWidth: 600
    minimumHeight: 400
    visible: true
    title: "Talk Qt"
    color: Theme.bgPrimary

    // Main layout: Login or Chat
    StackView {
        id: mainStack
        anchors.fill: parent
        initialItem: auth.loggedIn ? chatPage : loginPage
    }

    // React to login state changes
    Connections {
        target: auth
        function onLoggedInChanged() {
            if (auth.loggedIn) {
                mainStack.replace(chatPage)
                conversationModel.refresh()
            } else {
                mainStack.replace(loginPage)
            }
        }
    }

    Component {
        id: loginPage
        LoginView {}
    }

    Component {
        id: chatPage
        SplitView {
            orientation: Qt.Horizontal

            // Left panel — conversation list (Telegram sidebar)
            ConversationList {
                SplitView.preferredWidth: 320
                SplitView.minimumWidth: 260
                SplitView.maximumWidth: 450
                onConversationSelected: function(token, name) {
                    messageModel.conversationToken = token
                    chatView.conversationName = name
                }
            }

            // Right panel — chat
            ChatView {
                id: chatView
                SplitView.fillWidth: true
                SplitView.minimumWidth: 300
            }
        }
    }
}
