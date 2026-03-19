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
    title: brandName + " " + Qt.application.version
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

    property bool geometrySaveEnabled: false

    function restoreChatWindow() {
        geometrySaveEnabled = false  // block saving during restore
        root.minimumWidth = 500
        root.chatMode = true

        var w = Math.max(windowSettings.savedWidth, 500)
        var h = Math.max(windowSettings.savedHeight, 400)
        var sx = windowSettings.savedX
        var sy = windowSettings.savedY

        // Always set the windowed geometry first
        root.width = w
        root.height = h

        // Restore position — check if it's a valid saved value (not default -1)
        // Note: negative X is valid for multi-monitor setups (left monitor)
        if (sx !== -1 || sy !== -1) {
            root.x = sx
            root.y = sy
        }

        // Then apply maximized if that was the last state
        if (windowSettings.savedVisibility === 4) {
            root.showMaximized()
        }

        // Enable saving after geometry settles (avoids frame-inflation)
        restoreGuardTimer.start()
    }

    Timer {
        id: restoreGuardTimer
        interval: 500
        onTriggered: root.geometrySaveEnabled = true
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
        if (!chatMode || !geometrySaveEnabled) return

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

    // ── Desktop notification popup (separate window, bottom-right of screen) ──
    Connections {
        target: notifications
        function onDesktopPopupRequested(title, message, token) {
            desktopNotifTitle.text = title
            desktopNotifMessage.text = message
            // Position bottom-right of screen
            desktopNotif.x = Screen.width - desktopNotif.width - 16
            desktopNotif.y = Screen.height - desktopNotif.height - 80
            desktopNotif.show()
            desktopNotif.raise()
            desktopDismissTimer.interval = 5000
            desktopDismissTimer.restart()
        }
    }

    Window {
        id: desktopNotif
        width: 340; height: 80
        flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
        color: "transparent"
        visible: false
        transientParent: null

        Timer {
            id: desktopDismissTimer
            interval: 5000
            onTriggered: desktopNotif.visible = false
        }

        HoverHandler {
            id: desktopHover
            onHoveredChanged: {
                if (hovered) {
                    desktopDismissTimer.stop()
                } else {
                    desktopDismissTimer.restart()
                }
            }
        }

        Rectangle {
            anchors.fill: parent
            anchors.margins: 4
            radius: 12
            color: Theme.darkMode ? "#2a2f3a" : "#ffffff"
            border.color: Theme.darkMode ? "#3a4050" : "#e0e0e0"
            border.width: 1

            RowLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 10

                Image {
                    source: "qrc:/logo.png"
                    Layout.preferredWidth: 36; Layout.preferredHeight: 36
                    sourceSize: Qt.size(36, 36)
                    fillMode: Image.PreserveAspectFit
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label { id: desktopNotifTitle; font.pixelSize: 14; font.weight: Font.DemiBold; color: Theme.darkMode ? "#f0f2f5" : "#111111"; elide: Text.ElideRight; Layout.fillWidth: true }
                    Label { id: desktopNotifMessage; font.pixelSize: 12; color: Theme.darkMode ? "#c0c4cc" : "#444444"; elide: Text.ElideRight; maximumLineCount: 2; wrapMode: Text.Wrap; Layout.fillWidth: true }
                }

                Label {
                    text: "\u2715"; font.pixelSize: 12; color: Theme.textMuted
                    MouseArea { anchors.fill: parent; anchors.margins: -6; cursorShape: Qt.PointingHandCursor; onClicked: desktopNotif.visible = false }
                }
            }

            MouseArea {
                anchors.fill: parent; z: -1; cursorShape: Qt.PointingHandCursor
                onClicked: { desktopNotif.visible = false; root.show(); root.raise(); root.requestActivate() }
            }
        }
    }

    // ── In-app notification popup (top-right corner inside window) ──
    Connections {
        target: notifications
        function onPopupRequested(title, message, token) {
            notifTitle.text = title
            notifMessage.text = message
            notifPopup.notifToken = token
            notifPopup.open()
            notifDismissTimer.restart()
        }
    }

    Popup {
        id: notifPopup
        parent: Overlay.overlay
        x: root.width - width - 12
        y: 12
        width: 330
        padding: 0
        modal: false
        closePolicy: Popup.CloseOnPressOutside

        property string notifToken: ""

        enter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 200; easing.type: Easing.OutCubic }
                NumberAnimation { property: "x"; from: root.width; to: root.width - notifPopup.width - 12; duration: 200; easing.type: Easing.OutCubic }
            }
        }
        exit: Transition {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 150 }
        }

        Timer {
            id: notifDismissTimer
            interval: 5000
            onTriggered: notifPopup.close()
        }

        background: Rectangle {
            radius: 12
            color: Theme.darkMode ? "#2a2f3a" : "#ffffff"
            border.color: Theme.darkMode ? "#3a4050" : "#e0e0e0"
            border.width: 1

            // Subtle shadow
            Rectangle {
                anchors.fill: parent
                anchors.margins: -1
                radius: 13
                color: "transparent"
                border.color: Qt.rgba(0, 0, 0, 0.1)
                border.width: 1
                z: -1
            }
        }

        contentItem: RowLayout {
            spacing: 10

            Item { width: 12 }

            // Avatar
            Image {
                source: "qrc:/logo.png"
                Layout.preferredWidth: 36; Layout.preferredHeight: 36
                sourceSize: Qt.size(36, 36)
                fillMode: Image.PreserveAspectFit
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: 10
                Layout.bottomMargin: 10
                spacing: 2

                Label {
                    id: notifTitle
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Label {
                    id: notifMessage
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.textSecondary
                    elide: Text.ElideRight
                    maximumLineCount: 2
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }

            // Close button
            Label {
                text: "\u2715"
                font.pixelSize: 12
                color: Theme.textMuted
                Layout.rightMargin: 10
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 8

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    onClicked: notifPopup.close()
                }
            }
        }

        MouseArea {
            anchors.fill: parent
            z: -1
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                notifPopup.close()
                root.show()
                root.raise()
                root.requestActivate()
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

                // Brand logo (only for branded builds)
                Image {
                    anchors.horizontalCenter: parent.horizontalCenter
                    source: isBranded ? brandLogo : ""
                    visible: isBranded
                    width: 80
                    height: 80
                    sourceSize: Qt.size(160, 160)
                    fillMode: Image.PreserveAspectFit

                    opacity: 0
                    Component.onCompleted: opacity = 1
                    Behavior on opacity { NumberAnimation { duration: Theme.animSlow; easing.type: Easing.OutCubic } }
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: isBranded ? "Connecting..." : "Connecting to your server..."
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
        Item {
            id: chatLayout
            implicitWidth: 1000
            implicitHeight: 700

            property bool showTopics: false
            property string activeConvToken: ""

            ConversationList {
                id: convList
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: convList.sidebarWidth
                onConversationSelected: function(token, name, userId, convType, status) {
                    chatLayout.activeConvToken = token
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
                    chatView.isInTopicMode = false
                    messageModel.hideThreadMessages = false

                    // Load topics for this conversation (skip 1:1 chats)
                    threadModel.setConversationType(convType)
                    threadModel.conversationToken = token
                    topicList.groupName = name
                }
            }

            Rectangle {
                id: divider1
                anchors.left: convList.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 5
                color: Theme.divider

                MouseArea {
                    id: divider1Mouse
                    anchors.fill: parent
                    anchors.margins: -3
                    cursorShape: Qt.SplitHCursor
                    hoverEnabled: true
                    property real startX: 0
                    property real startWidth: 0
                    onPressed: function(mouse) {
                        startX = mouse.x + divider1.x
                        startWidth = convList._userWidth
                        convList._resizing = true
                    }
                    onPositionChanged: function(mouse) {
                        if (pressed) {
                            var delta = (mouse.x + divider1.x) - startX
                            convList._userWidth = Math.max(200, Math.min(500, startWidth + delta))
                            if (convList.squeezed && convList._userWidth > 150)
                                convList.squeezed = false
                        }
                    }
                    onReleased: {
                        convList._resizing = false
                        if (convList._userWidth < 100)
                            convList.squeezed = true
                    }
                }
            }

            ThreadListView {
                id: topicList
                anchors.left: divider1.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                property real userWidth: 240
                width: chatLayout.showTopics ? Math.max(160, Math.min(400, userWidth)) : 0
                clip: true
                visible: width > 0

                Behavior on width {
                    NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic }
                }

                onBackToChats: convList.squeezed = false

                onThreadSelected: function(threadId, title) {
                    if (threadId === 0) {
                        chatView.closeThread()
                        messageModel.hideThreadMessages = true
                    } else {
                        chatView.openThread(threadId, title)
                        messageModel.hideThreadMessages = false
                    }
                    threadModel.selectTopic(threadId)
                    chatView.activeThreadColor = threadModel.colorForThread(threadId)
                }
            }

            Rectangle {
                id: divider2
                anchors.left: topicList.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: chatLayout.showTopics ? 5 : 0
                color: divider2Mouse.pressed ? Theme.textMuted : Theme.divider

                MouseArea {
                    id: divider2Mouse
                    anchors.fill: parent
                    anchors.margins: -3
                    cursorShape: chatLayout.showTopics ? Qt.SplitHCursor : Qt.ArrowCursor
                    hoverEnabled: true
                    enabled: chatLayout.showTopics
                    property real startX: 0
                    property real startWidth: 0
                    onPressed: function(mouse) {
                        startX = mouse.x + divider2.x
                        startWidth = topicList.userWidth
                    }
                    onPositionChanged: function(mouse) {
                        if (pressed) {
                            var delta = (mouse.x + divider2.x) - startX
                            topicList.userWidth = Math.max(160, Math.min(400, startWidth + delta))
                        }
                    }
                }
            }

            ChatView {
                id: chatView
                sidebarSqueezed: convList.squeezed
                onExpandSidebar: convList.squeezed = false
                anchors.left: divider2.right
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
            }

            Connections {
                target: threadModel
                function onHasTopicsChanged() {
                    var active = threadModel.hasTopics && auth.hasThreadsSupport
                    chatLayout.showTopics = active
                    chatView.isInTopicMode = active
                    messageModel.hideThreadMessages = active
                    convList.squeezed = active
                    conversationModel.setHasTopics(chatLayout.activeConvToken, active)
                    root.minimumWidth = active ? 600 : 500
                }
            }
        }
    }
}
