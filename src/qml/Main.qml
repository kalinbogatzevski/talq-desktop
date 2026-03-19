import QtQuick
import QtQuick.Window
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
    visible: false  // start hidden, show after centering
    title: "TalQ " + Qt.application.version
    color: Theme.bgPrimary

    property bool chatMode: false  // true after login — enables geometry saving

    // Font zoom: Ctrl+Plus / Ctrl+Minus / Ctrl+0
    Shortcut { sequence: "Ctrl+=" ; onActivated: Theme.fontScale = Math.min(Theme.fontScale + 0.1, 2.0) }
    Shortcut { sequence: "Ctrl+-" ; onActivated: Theme.fontScale = Math.max(Theme.fontScale - 0.1, 0.7) }
    Shortcut { sequence: "Ctrl+0" ; onActivated: Theme.fontScale = 1.0 }

    // Minimize to tray instead of closing (like Telegram/Discord)
    onClosing: function(close) {
        if (chatMode) {
            close.accepted = false
            root.hide()
        }
    }

    // Center splash and show
    Component.onCompleted: {
        x = (Screen.width - width) / 2
        y = (Screen.height - height) / 2
        visible = true
    }

    // Saved geometry — only used after login
    Settings {
        id: windowSettings
        category: "WindowGeometry"
        property int savedX: -1
        property int savedY: -1
        property int savedWidth: 1000
        property int savedHeight: 700
        property int savedVisibility: 2  // Window.Windowed = 2, Window.Maximized = 4
    }

    Settings {
        id: themeSettings
        category: "Theme"
        property bool darkMode: true
        property real fontScale: 1.0
        Component.onCompleted: {
            Theme.darkMode = darkMode
            Theme.fontScale = fontScale
        }
    }

    Connections {
        target: Theme
        function onDarkModeChanged() { themeSettings.darkMode = Theme.darkMode }
        function onFontScaleChanged() { themeSettings.fontScale = Theme.fontScale }
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

    function restoreChatWindow() {
        root.minimumWidth = 500
        root.chatMode = true

        var w = Math.max(windowSettings.savedWidth, 500)
        var h = Math.max(windowSettings.savedHeight, 400)
        var sx = windowSettings.savedX
        var sy = windowSettings.savedY

        // Always set the windowed geometry first
        root.width = w
        root.height = h
        if (sx >= 0 && sy >= 0) { root.x = sx; root.y = sy }

        // Then apply maximized if that was the last state
        if (windowSettings.savedVisibility === 4) {
            root.showMaximized()
        }
    }

    Connections {
        target: auth
        function onRestoringChanged() {
            if (!auth.restoringSession) {
                if (auth.loggedIn) {
                    root.restoreChatWindow()
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
                root.restoreChatWindow()
                mainStack.replace(chatPage)
                conversationModel.refresh()
            } else {
                root.saveWindowState()  // save before switching to login
                root.chatMode = false
                root.showNormal()
                root.minimumWidth = 400
                root.width = 460
                root.height = 520
                root.x = (Screen.width - 460) / 2
                root.y = (Screen.height - 520) / 2
                mainStack.replace(loginPage)
            }
        }
    }

    // Debounced window state save — proven pattern from QGroundControl
    Timer {
        id: saveGeometryTimer
        interval: 300
        onTriggered: root.saveWindowState()
    }

    function saveWindowState() {
        if (!chatMode) return

        switch (root.visibility) {
        case ApplicationWindow.Windowed:
            windowSettings.savedX = root.x
            windowSettings.savedY = root.y
            windowSettings.savedWidth = root.width
            windowSettings.savedHeight = root.height
            windowSettings.savedVisibility = 2  // Windowed
            break
        case ApplicationWindow.Maximized:
            windowSettings.savedVisibility = 4  // Maximized
            break
        // Ignore Hidden, Minimized, FullScreen
        }
    }

    onXChanged: saveGeometryTimer.restart()
    onYChanged: saveGeometryTimer.restart()
    onWidthChanged: saveGeometryTimer.restart()
    onHeightChanged: saveGeometryTimer.restart()
    onVisibilityChanged: saveGeometryTimer.restart()

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
                onConversationSelected: function(token, name, userId, convType, status) {
                    messageModel.threadId = 0
                    messageModel.conversationToken = token
                    chatView.conversationName = name
                    chatView.conversationUserId = userId
                    chatView.conversationType = convType
                    chatView.peerStatus = status
                    conversationModel.clearUnreadForToken(token)
                    signaling.joinRoom(token)
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
