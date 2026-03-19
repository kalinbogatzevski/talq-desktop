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

    function openThread(threadId, title) {
        activeThreadId = threadId
        activeThreadTitle = title
        messageModel.threadId = threadId
        threadModel.conversationToken = ""  // stop thread list loading
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
        // Delay scroll — footer needs time to resize with reply bar
        replyScrollTimer.restart()
    }

    Timer {
        id: replyScrollTimer
        interval: 100
        onTriggered: messageListView.positionViewAtEnd()
    }

    function cancelReply() {
        replyToId = 0
        replyToAuthor = ""
        replyToText = ""
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

            // Back button (when in thread)
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
                    color: {
                        var colors = ["#2ec4b6", "#e07060", "#f0a050", "#5ec76a", "#9b7cd4", "#e87aae"]
                        var hash = 0; var n = chatRoot.conversationName
                        for (var i = 0; i < n.length; i++) { hash = ((hash << 5) - hash) + n.charCodeAt(i); hash = hash & hash }
                        return colors[Math.abs(hash) % colors.length]
                    }
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
                    visible: signaling.typingUser.length > 0
                        || (chatRoot.conversationType === 1 && chatRoot.peerStatus.length > 0)
                    text: {
                        if (signaling.typingUser.length > 0)
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
                    font.italic: signaling.typingUser.length > 0
                    color: {
                        if (signaling.typingUser.length > 0) return Theme.accent
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

            MessageComposer {
                Layout.fillWidth: true
                onSendMessage: function(text) {
                    messageModel.sendMessage(text, chatRoot.replyToId)
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

        // Scroll to newest message on any count change
        onCountChanged: {
            positionViewAtEnd()
            scrollEndTimer.restart()
        }

        Timer {
            id: scrollEndTimer
            interval: 150
            onTriggered: messageListView.positionViewAtEnd()
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

    // Thread list overlay for group chats (DISABLED for debugging)
    ThreadListView {
        anchors.fill: parent
        visible: false // chatRoot.isGroupChat && chatRoot.activeThreadId === 0 && messageModel.conversationToken.length > 0
        onThreadSelected: function(threadId, title) {
            chatRoot.openThread(threadId, title)
        }
    }

    Connections {
        target: messageModel
        function onConversationTokenChanged() {
            chatRoot.closeThread()
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
