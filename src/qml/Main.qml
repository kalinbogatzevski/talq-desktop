import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.settings

ApplicationWindow {
    id: root
    width: 380
    height: 420
    minimumWidth: 380
    minimumHeight: 400
    visible: true
    title: "TalQ"
    color: Theme.bgPrimary

    Settings {
        id: windowSettings
        category: "Window"
        property alias x: root.x
        property alias y: root.y
        property alias width: root.width
        property alias height: root.height
    }

    Settings {
        id: themeSettings
        category: "Theme"
        property bool darkMode: true
        Component.onCompleted: Theme.darkMode = darkMode
    }

    Connections {
        target: Theme
        function onDarkModeChanged() {
            themeSettings.darkMode = Theme.darkMode
        }
    }

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
                    root.minimumWidth = 600
                    root.width = 1000
                    root.height = 700
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
                root.minimumWidth = 600
                root.width = 1000
                root.height = 700
                mainStack.replace(chatPage)
                conversationModel.refresh()
            } else {
                root.minimumWidth = 400
                root.width = 460
                root.height = 520
                mainStack.replace(loginPage)
            }
        }
    }

    Component {
        id: splashPage
        Rectangle {
            implicitWidth: 380
            implicitHeight: 420
            color: Theme.bgPrimary

            Column {
                anchors.centerIn: parent
                spacing: Theme.spacingLarge

                // App logo
                Image {
                    anchors.horizontalCenter: parent.horizontalCenter
                    source: "qrc:/logo.png"
                    width: 96
                    height: 96
                    sourceSize: Qt.size(192, 192)
                    fillMode: Image.PreserveAspectFit

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
            implicitWidth: 1000
            implicitHeight: 700
            orientation: Qt.Horizontal

            handle: Rectangle {
                implicitWidth: 1
                color: Theme.divider
            }

            ConversationList {
                SplitView.preferredWidth: 320
                SplitView.minimumWidth: 260
                SplitView.maximumWidth: 450
                onConversationSelected: function(token, name, userId, convType) {
                    messageModel.threadId = 0  // reset thread filter before switching
                    messageModel.conversationToken = token
                    chatView.conversationName = name
                    chatView.conversationUserId = userId
                    chatView.conversationType = convType
                    chatView.activeThreadId = 0
                    chatView.activeThreadTitle = ""
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
