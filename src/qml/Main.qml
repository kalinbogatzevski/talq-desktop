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
    title: "TalQ"
    color: Theme.bgPrimary

    StackView {
        id: mainStack
        anchors.fill: parent
        initialItem: splashPage

        pushEnter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animNormal; easing.type: Easing.OutCubic }
        }
        pushExit: Transition {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.animFast }
        }
        replaceEnter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animSlow; easing.type: Easing.OutCubic }
        }
        replaceExit: Transition {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.animFast }
        }
    }

    Connections {
        target: auth
        function onRestoringChanged() {
            if (!auth.restoringSession) {
                if (auth.loggedIn) {
                    mainStack.replace(chatPage)
                    conversationModel.refresh()
                } else {
                    mainStack.replace(loginPage)
                }
            }
        }
        function onLoggedInChanged() {
            if (auth.restoringSession) return
            if (auth.loggedIn) {
                mainStack.replace(chatPage)
                conversationModel.refresh()
            } else {
                mainStack.replace(loginPage)
            }
        }
    }

    Component {
        id: splashPage
        Rectangle {
            color: Theme.bgPrimary

            Column {
                anchors.centerIn: parent
                spacing: Theme.spacingLarge

                // App icon placeholder
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 72
                    height: 72
                    radius: Theme.radiusLarge
                    color: Theme.bgSurface
                    border.color: Theme.divider
                    border.width: 1

                    Label {
                        anchors.centerIn: parent
                        text: "💬"
                        font.pixelSize: 32
                    }

                    // Fade in
                    opacity: 0
                    Component.onCompleted: opacity = 1
                    Behavior on opacity { NumberAnimation { duration: Theme.animSlow; easing.type: Easing.OutCubic } }
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "TalQ"
                    font.pixelSize: Theme.fontSizeHero
                    font.weight: Font.DemiBold
                    font.letterSpacing: -0.5
                    color: Theme.textPrimary

                    opacity: 0
                    Component.onCompleted: opacity = 1
                    Behavior on opacity { NumberAnimation { duration: Theme.animSlow; easing.type: Easing.OutCubic } }
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Connecting to your server..."
                    font.pixelSize: Theme.fontSizeNormal
                    color: Theme.textSecondary

                    opacity: 0
                    Component.onCompleted: opacity = 1
                    Behavior on opacity { NumberAnimation { duration: Theme.animSlow; easing.type: Easing.OutCubic } }
                }

                // Subtle loading bar instead of spinner
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 120
                    height: 3
                    radius: 2
                    color: Theme.bgSurface

                    Rectangle {
                        id: loadingBar
                        height: parent.height
                        radius: 2
                        color: Theme.accent
                        width: parent.width * 0.3

                        SequentialAnimation on x {
                            loops: Animation.Infinite
                            NumberAnimation { from: 0; to: 84; duration: 800; easing.type: Easing.InOutQuad }
                            NumberAnimation { from: 84; to: 0; duration: 800; easing.type: Easing.InOutQuad }
                        }
                    }
                }
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

            ConversationList {
                SplitView.preferredWidth: 320
                SplitView.minimumWidth: 260
                SplitView.maximumWidth: 450
                onConversationSelected: function(token, name) {
                    messageModel.conversationToken = token
                    chatView.conversationName = name
                }
            }

            ChatView {
                id: chatView
                SplitView.fillWidth: true
                SplitView.minimumWidth: 300
            }
        }
    }
}
