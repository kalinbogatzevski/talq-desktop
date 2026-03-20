import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: chatRoot
    property string conversationName: ""
    property string conversationUserId: ""
    property int conversationType: 0
    property string peerStatus: ""   // "online", "away", "dnd", "offline"
    property int replyToId: 0
    property string replyToAuthor: ""
    property string replyToText: ""

    property int activeThreadId: 0
    property string activeThreadTitle: ""
    property bool isGroupChat: conversationType === 2 || conversationType === 3
    property bool isInTopicMode: false
    property bool sidebarSqueezed: false
    signal expandSidebar()
    property int activeThreadColor: 0
    readonly property bool isViewingTopic: isInTopicMode && activeThreadId > 0
    // Only show typing when it's from the current conversation
    readonly property bool isTyping: signaling.typingUser.length > 0
        && signaling.typingRoom === messageModel.conversationToken

    function openThread(threadId, title) {
        activeThreadId = threadId
        activeThreadTitle = title
        messageModel.threadId = threadId
    }

    function closeThread() {
        activeThreadId = 0
        activeThreadTitle = ""
        messageModel.threadId = 0
    }

    function startReply(msgId, author, text) {
        replyToId = msgId
        replyToAuthor = author
        replyToText = text
        Qt.callLater(function() { messageListView.positionViewAtEnd() })
    }

    function cancelReply() {
        replyToId = 0
        replyToAuthor = ""
        replyToText = ""
    }

    function confirmPaste() {
        if (pasteBar.pendingPath.length > 0) {
            var path = pasteBar.pendingPath
            messageModel.sendFileWithCaption(path, pasteCaptionField.text.trim())
            // Clean up temp file after upload starts (file data already read into memory)
            messageModel.cleanupTempFile(path)
            cancelPaste()
        }
    }

    function cancelPaste() {
        if (pasteBar.pendingPath.length > 0)
            messageModel.cleanupTempFile(pasteBar.pendingPath)
        pasteBar.visible = false
        pasteBar.pendingPath = ""
        pastePreview.source = ""
        pasteCaptionField.text = ""
    }

    padding: 0
    background: Rectangle { color: Theme.bgSecondary }

    header: Rectangle {
        height: Theme.headerHeight
        color: Theme.bgPrimary

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spacingLarge
            anchors.rightMargin: Theme.spacingXLarge
            spacing: Theme.spacingSmall

            // Back to chat list (when sidebar is squeezed in topic mode)
            ToolButton {
                visible: chatRoot.sidebarSqueezed && chatRoot.conversationName.length > 0
                width: 30; height: 30
                onClicked: chatRoot.expandSidebar()
                contentItem: Label {
                    text: "\u2190"
                    font.pixelSize: 18
                    color: Theme.accent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { radius: 15; color: parent.hovered ? Theme.bgHover : "transparent" }
            }

            // Back button (when viewing a thread or topic)
            ToolButton {
                visible: chatRoot.activeThreadId > 0
                width: 30; height: 30
                onClicked: chatRoot.closeThread()
                contentItem: Label {
                    text: "\u2190"  // ← arrow
                    font.pixelSize: 18
                    color: Theme.accent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { radius: 15; color: parent.hovered ? Theme.bgHover : "transparent" }
            }

            Rectangle {
                width: 10; height: 10; radius: 5
                visible: chatRoot.isViewingTopic
                color: Theme.topicColor(chatRoot.activeThreadColor)
                Layout.alignment: Qt.AlignVCenter
            }

            Item {
                width: 30; height: 30
                visible: chatRoot.conversationName.length > 0
                Image {
                    id: headerChatAvatar
                    anchors.fill: parent
                    source: chatRoot.conversationType === 1 && chatRoot.conversationUserId.length > 0
                        ? "image://avatar/" + chatRoot.conversationUserId : ""
                    sourceSize: Qt.size(30, 30)
                    visible: status === Image.Ready
                }
                Rectangle {
                    anchors.fill: parent; radius: 15
                    visible: headerChatAvatar.status !== Image.Ready && chatRoot.conversationName.length > 0
                    color: Theme.topicColor(Theme.stringHash(chatRoot.conversationName))
                    Label { anchors.centerIn: parent; text: chatRoot.conversationName.length > 0 ? chatRoot.conversationName[0].toUpperCase() : ""; font.pixelSize: 13; font.weight: Font.DemiBold; color: "white" }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                Label {
                    text: chatRoot.activeThreadId > 0 ? chatRoot.activeThreadTitle
                        : (chatRoot.conversationName || "Select a conversation")
                    font.pixelSize: Theme.fontSizeLarge
                    font.weight: chatRoot.conversationName.length > 0 ? Font.DemiBold : Font.Normal
                    color: chatRoot.conversationName.length > 0 ? Theme.textPrimary : Theme.textMuted
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                // Status line: typing indicator OR user status (1:1 chats)
                Label {
                    visible: chatRoot.isTyping
                        || (chatRoot.conversationType === 1 && chatRoot.peerStatus.length > 0)
                    text: {
                        if (chatRoot.isTyping)
                            return signaling.typingUser + " is typing..."
                        if (chatRoot.conversationType === 1 && chatRoot.peerStatus.length > 0) {
                            switch (chatRoot.peerStatus) {
                            case "online": return "online"
                            case "away": return "away"
                            case "dnd": return "do not disturb"
                            default: return "offline"
                            }
                        }
                        return ""
                    }
                    font.pixelSize: Theme.fontSizeTiny
                    font.italic: chatRoot.isTyping
                    color: {
                        if (chatRoot.isTyping) return Theme.accent
                        switch (chatRoot.peerStatus) {
                        case "online": return Theme.online
                        case "away": return Theme.warning
                        case "dnd": return Theme.danger
                        default: return Theme.textMuted
                        }
                    }
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }

                Label {
                    visible: chatRoot.isViewingTopic && !chatRoot.isTyping
                    text: chatRoot.conversationName + " \u00B7 " + messageModel.count + " messages"
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.textSecondary
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }

            // Call button (1:1 chats only, hidden during active call)
            ToolButton {
                visible: chatRoot.conversationType === 1 && callManager.state === 0
                implicitWidth: 32; implicitHeight: 32
                onClicked: callManager.startCall(messageModel.conversationToken, false)
                contentItem: Label {
                    text: "\uD83D\uDCDE"  // phone emoji
                    font.pixelSize: 16
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { radius: 16; color: parent.hovered ? Theme.bgHover : "transparent" }
                ToolTip.visible: hovered; ToolTip.text: "Audio call"
            }

            // Active call indicator
            Label {
                visible: callManager.state > 0
                text: "\uD83D\uDCDE " + formatDuration(callManager.callDuration)
                font.pixelSize: Theme.fontSizeTiny
                color: Theme.online
                function formatDuration(s) {
                    var m = Math.floor(s / 60)
                    var sec = s % 60
                    return (m < 10 ? "0" : "") + m + ":" + (sec < 10 ? "0" : "") + sec
                }
            }

            Label {
                visible: messageModel.loading && messageModel.count > 0
                text: "\u21BB"; font.pixelSize: 14; color: Theme.textMuted
                RotationAnimation on rotation { from: 0; to: 360; duration: 1000; loops: Animation.Infinite; running: messageModel.loading && messageModel.count > 0 }
            }

            BusyIndicator {
                running: messageModel.loading && messageModel.count === 0
                visible: running; implicitWidth: 20; implicitHeight: 20; palette.dark: Theme.accent
            }
        }
    }

    footer: Pane {
        padding: 0
        visible: messageModel.conversationToken.length > 0
        background: Rectangle { color: Theme.bgPrimary }

        ColumnLayout {
            width: parent.width
            spacing: 0

            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }

            // Reply bar
            Rectangle {
                Layout.fillWidth: true
                height: visible ? replyPreviewCol.implicitHeight + 12 : 0
                color: Theme.bgSurface
                visible: chatRoot.replyToId > 0

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingLarge
                    anchors.rightMargin: Theme.spacingNormal
                    spacing: Theme.spacingSmall

                    Rectangle { width: 3; Layout.fillHeight: true; Layout.topMargin: 6; Layout.bottomMargin: 6; radius: 1.5; color: Theme.accent }
                    ColumnLayout {
                        id: replyPreviewCol
                        Layout.fillWidth: true; spacing: 1
                        Label { text: chatRoot.replyToAuthor; font.pixelSize: Theme.fontSizeTiny; font.weight: Font.DemiBold; color: Theme.accent }
                        Label { Layout.fillWidth: true; text: chatRoot.replyToText; font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondary; elide: Text.ElideRight; maximumLineCount: 1 }
                    }
                    ToolButton {
                        width: 28; height: 28; onClicked: chatRoot.cancelReply()
                        contentItem: Label { text: "\u2715"; font.pixelSize: 14; color: Theme.textSecondary; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { radius: 14; color: parent.hovered ? Theme.bgHover : "transparent" }
                    }
                }
            }

            // Upload progress bar
            Rectangle {
                Layout.fillWidth: true
                height: visible ? 36 : 0
                color: Theme.bgSurface
                visible: messageModel.uploadProgress >= 0

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingLarge
                    anchors.rightMargin: Theme.spacingLarge
                    spacing: Theme.spacingSmall

                    Label {
                        text: "\uD83D\uDCCE"  // 📎
                        font.pixelSize: 14
                    }
                    Label {
                        text: messageModel.uploadFileName
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.textSecondary
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                    Label {
                        text: Math.round(messageModel.uploadProgress * 100) + "%"
                        font.pixelSize: Theme.fontSizeTiny
                        font.weight: Font.DemiBold
                        color: Theme.accent
                    }
                }

                // Progress bar at bottom
                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    width: parent.width * Math.max(0, messageModel.uploadProgress)
                    height: 2
                    color: Theme.accent
                    Behavior on width { NumberAnimation { duration: 100 } }
                }
            }

            // Paste confirmation bar
            Rectangle {
                id: pasteBar
                Layout.fillWidth: true
                height: visible ? pasteContent.implicitHeight + 16 : 0
                color: Theme.bgSurface
                visible: false

                property string pendingPath: ""

                RowLayout {
                    id: pasteContent
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingLarge
                    anchors.rightMargin: Theme.spacingNormal
                    spacing: Theme.spacingSmall

                    Rectangle { width: 3; Layout.fillHeight: true; Layout.topMargin: 6; Layout.bottomMargin: 6; radius: 1.5; color: Theme.accent }

                    // Thumbnail preview
                    Image {
                        id: pastePreview
                        Layout.preferredWidth: 40
                        Layout.preferredHeight: 40
                        fillMode: Image.PreserveAspectFit
                        visible: status === Image.Ready
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Label { text: "Send image"; font.pixelSize: Theme.fontSizeTiny; font.weight: Font.DemiBold; color: Theme.accent }

                        TextField {
                            id: pasteCaptionField
                            Layout.fillWidth: true
                            placeholderText: "Add a caption..."
                            placeholderTextColor: Theme.textMuted
                            font.pixelSize: Theme.fontSizeTiny
                            color: Theme.textPrimary
                            background: Rectangle { color: "transparent" }
                            padding: 0
                            Keys.onReturnPressed: chatRoot.confirmPaste()
                            Keys.onEscapePressed: chatRoot.cancelPaste()
                        }
                    }

                    // Send button
                    ToolButton {
                        width: 28; height: 28
                        onClicked: chatRoot.confirmPaste()
                        contentItem: Label { text: "\u276F"; font.pixelSize: 14; font.weight: Font.Bold; color: Theme.accent; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { radius: 14; color: parent.hovered ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.15) : "transparent" }
                    }

                    // Cancel button
                    ToolButton {
                        width: 28; height: 28
                        onClicked: chatRoot.cancelPaste()
                        contentItem: Label { text: "\u2715"; font.pixelSize: 14; color: Theme.textSecondary; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { radius: 14; color: parent.hovered ? Theme.bgHover : "transparent" }
                    }
                }
            }

            MessageComposer {
                Layout.fillWidth: true
                topicName: chatRoot.isViewingTopic ? chatRoot.activeThreadTitle : ""
                onSendMessage: function(text) {
                    var replyId = chatRoot.replyToId > 0 ? chatRoot.replyToId : chatRoot.activeThreadId
                    messageModel.sendMessage(text, replyId)
                    chatRoot.cancelReply()
                }
            }
        }
    }

    // Messages list — fills the content area between header and footer
    ListView {
        id: messageListView
        anchors.fill: parent
        model: messageModel
        clip: true
        spacing: 2
        bottomMargin: Theme.spacingLarge
        boundsBehavior: Flickable.StopAtBounds

        property bool autoScrolling: true

        onContentHeightChanged: {
            if (autoScrolling && count > 0)
                positionViewAtIndex(count - 1, ListView.End)
        }

        onMovingChanged: {
            if (moving)
                autoScrolling = false
        }

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded; width: 5
            contentItem: Rectangle { radius: 2; color: Theme.textMuted; opacity: 0.3 }
        }

        // Welcome screen
        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - 60, 360)
            spacing: Theme.spacingXLarge
            visible: messageModel.conversationToken.length === 0 && !messageModel.loading

            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                source: isBranded ? brandLogo : "qrc:/logo.png"; width: 80; height: 80
                sourceSize: Qt.size(192, 192); fillMode: Image.PreserveAspectFit; opacity: 0.9
            }

            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.spacingSmall

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Welcome, " + auth.displayName
                    font.pixelSize: Theme.fontSizeTitle; font.weight: Font.DemiBold; color: Theme.textPrimary
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Pick a conversation from the sidebar"
                    font.pixelSize: Theme.fontSizeNormal; color: Theme.textSecondary
                }
            }

            // Server info card
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width
                height: serverInfoCol.implicitHeight + Theme.spacingXLarge
                radius: Theme.radiusNormal
                color: Theme.bgSurface
                border.color: Theme.divider
                border.width: 1
                visible: auth.serverUrl.length > 0

                ColumnLayout {
                    id: serverInfoCol
                    anchors {
                        left: parent.left; right: parent.right
                        verticalCenter: parent.verticalCenter
                        margins: Theme.spacingLarge
                    }
                    spacing: Theme.spacingSmall

                    // Card header
                    Label {
                        text: "Server"
                        font.pixelSize: Theme.fontSizeTiny
                        font.weight: Font.DemiBold
                        color: Theme.textMuted
                        font.letterSpacing: 0.8
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }

                    // Info rows — ColumnLayout with RowLayouts for clean alignment
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacingSmall

                        Repeater {
                            model: [
                                { icon: "\u2601", text: auth.serverUrl.replace(/^https?:\/\//, ""), bold: true, accent: true, vis: auth.serverUrl.length > 0 },
                                { icon: "\u24C3", text: "Nextcloud " + auth.nextcloudVersion, bold: false, accent: false, vis: auth.nextcloudVersion.length > 0 },
                                { icon: "\u260E", text: "Talk " + auth.talkVersion, bold: false, accent: false, vis: auth.talkVersion.length > 0 },
                                { icon: "\u26A1", text: auth.signalingMode, bold: false, accent: false, vis: auth.signalingMode.length > 0 }
                            ]

                            RowLayout {
                                visible: modelData.vis
                                spacing: Theme.spacingNormal
                                Item {
                                    width: 24; height: 20
                                    Label {
                                        anchors.centerIn: parent
                                        text: modelData.icon; font.pixelSize: 16
                                        color: modelData.accent ? Theme.accent : Theme.textSecondary
                                    }
                                }
                                Label {
                                    text: modelData.text
                                    font.pixelSize: modelData.bold ? Theme.fontSizeNormal : Theme.fontSizeSmall
                                    font.weight: modelData.bold ? Font.DemiBold : Font.Normal
                                    color: modelData.accent ? Theme.textPrimary : Theme.textSecondary
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                        }

                        // Push status
                        RowLayout {
                            spacing: Theme.spacingNormal
                            Item {
                                width: 24; height: 20
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 8; height: 8; radius: 4
                                    color: pushClient.connected ? Theme.online : Theme.warning
                                }
                            }
                            Label {
                                text: pushClient.connected ? "Push connected (real-time)" : "Push disconnected (polling)"
                                font.pixelSize: Theme.fontSizeSmall
                                color: pushClient.connected ? Theme.online : Theme.textSecondary
                                Layout.fillWidth: true
                            }
                        }

                        // Signaling status
                        RowLayout {
                            spacing: Theme.spacingNormal
                            Item {
                                width: 24; height: 20
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 8; height: 8; radius: 4
                                    color: signaling.connected ? Theme.online : Theme.textMuted
                                }
                            }
                            Label {
                                text: signaling.connected ? "Signaling connected" : "Signaling disconnected"
                                font.pixelSize: Theme.fontSizeSmall
                                color: signaling.connected ? Theme.online : Theme.textSecondary
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }
        }

        // Empty chat
        Column {
            anchors.centerIn: parent
            spacing: Theme.spacingLarge
            visible: messageModel.conversationToken.length > 0 && messageModel.count === 0 && !messageModel.loading

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 64; height: 64; radius: 32
                color: Theme.bgSurface
                Label {
                    anchors.centerIn: parent
                    text: "💬"; font.pixelSize: 28
                }
            }

            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.spacingTiny
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "No messages yet"
                    font.pixelSize: Theme.fontSizeLarge; font.weight: Font.DemiBold; color: Theme.textPrimary
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Be the first to say something!"
                    font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondary
                }
            }
        }

        Column {
            anchors.centerIn: parent
            spacing: Theme.spacingLarge
            visible: chatRoot.isInTopicMode && !chatRoot.isViewingTopic && messageModel.conversationToken.length > 0

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "\uD83D\uDCAC"
                font.pixelSize: 48
                opacity: 0.3
            }
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Select a topic"
                font.pixelSize: Theme.fontSizeNormal
                color: Theme.textMuted
            }
        }

        delegate: MessageBubble {
            width: messageListView.width
            isOwnMessage: actorId === auth.userId
            onReplyRequested: function(msgId, author, text) {
                chatRoot.startReply(msgId, author, text)
            }
            onThreadOpenRequested: function(threadId) {
                chatRoot.openThread(threadId, "Thread")
            }
        }
    }

    // Scroll-to-bottom floating button
    Rectangle {
        id: scrollToBottomBtn
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 16
        anchors.bottomMargin: 12
        width: 36; height: 36; radius: 18
        color: scrollBtnArea.containsMouse ? Theme.accent : Theme.bgSurface
        border.color: scrollBtnArea.containsMouse ? Theme.accent : Theme.divider
        border.width: 1
        visible: !messageListView.atYEnd && messageModel.count > 0
        opacity: visible ? 1 : 0
        z: 50

        Behavior on opacity { NumberAnimation { duration: Theme.animFast } }
        Behavior on color { ColorAnimation { duration: Theme.animFast } }

        Label {
            anchors.centerIn: parent
            text: "\u2193"  // ↓
            font.pixelSize: 16
            font.weight: Font.Bold
            color: scrollBtnArea.containsMouse ? "white" : Theme.textSecondary
            Behavior on color { ColorAnimation { duration: Theme.animFast } }
        }

        MouseArea {
            id: scrollBtnArea
            anchors.fill: parent
            anchors.margins: -4
            cursorShape: Qt.PointingHandCursor
            hoverEnabled: true
            onClicked: messageListView.positionViewAtEnd()
        }

        // Subtle scale on press
        scale: scrollBtnArea.pressed ? 0.9 : 1
        Behavior on scale { NumberAnimation { duration: Theme.animFast; easing.type: Easing.OutCubic } }
    }

    Connections {
        target: messageModel
        function onConversationTokenChanged() {
            chatRoot.closeThread()
            chatRoot.cancelPaste()
        }
        function onNewMessagesAtEnd() {
            messageListView.autoScrolling = true
            if (messageListView.count > 0)
                messageListView.positionViewAtIndex(messageListView.count - 1, ListView.End)
        }
        function onPasteReady(filePath, width, height) {
            pasteBar.pendingPath = filePath
            pastePreview.source = filePath
            pasteBar.visible = true
            pasteCaptionField.forceActiveFocus()
        }
    }

    // Drag and drop file upload
    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]

        onEntered: function(drag) {
            drag.accepted = drag.hasUrls
            dropOverlay.visible = drag.hasUrls
        }
        onExited: dropOverlay.visible = false
        onDropped: function(drop) {
            dropOverlay.visible = false
            if (drop.hasUrls) {
                for (var i = 0; i < drop.urls.length; i++) {
                    messageModel.sendFile(drop.urls[i].toString())
                }
            }
        }
    }

    // Drop overlay
    Rectangle {
        id: dropOverlay
        anchors.fill: parent
        visible: false
        color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.15)
        z: 100

        Rectangle {
            anchors.centerIn: parent
            width: dropLabel.implicitWidth + Theme.spacingXLarge * 2
            height: dropLabel.implicitHeight + Theme.spacingXLarge
            radius: Theme.radiusLarge
            color: Theme.bgSurface
            border.color: Theme.accent
            border.width: 2

            Label {
                id: dropLabel
                anchors.centerIn: parent
                text: "\uD83D\uDCCE  Drop files here to send"
                font.pixelSize: Theme.fontSizeLarge
                font.weight: Font.DemiBold
                color: Theme.accent
            }
        }
    }
}
