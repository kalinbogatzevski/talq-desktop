import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: chatRoot
    property string conversationName: ""
    property string conversationUserId: ""
    property int conversationType: 0
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

            Label {
                text: chatRoot.activeThreadId > 0 ? chatRoot.activeThreadTitle
                    : (chatRoot.conversationName || "Select a conversation")
                font.pixelSize: Theme.fontSizeLarge
                font.weight: chatRoot.conversationName.length > 0 ? Font.DemiBold : Font.Normal
                color: chatRoot.conversationName.length > 0 ? Theme.textPrimary : Theme.textMuted
                elide: Text.ElideRight
                Layout.fillWidth: true
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
        boundsBehavior: Flickable.StopAtBounds

        // Scroll to newest message on any count change
        onCountChanged: {
            positionViewAtEnd()
            scrollEndTimer.restart()
        }

        Timer {
            id: scrollEndTimer
            interval: 100
            onTriggered: messageListView.positionViewAtEnd()
        }

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded; width: 5
            contentItem: Rectangle { radius: 2; color: Theme.textMuted; opacity: 0.3 }
        }

        // Welcome screen
        Column {
            anchors.centerIn: parent
            spacing: Theme.spacingLarge
            visible: messageModel.conversationToken.length === 0 && !messageModel.loading

            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                source: "qrc:/logo.png"; width: 96; height: 96
                sourceSize: Qt.size(192, 192); fillMode: Image.PreserveAspectFit; opacity: 0.8
            }
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Welcome, " + auth.displayName
                font.pixelSize: Theme.fontSizeLarge; font.weight: Font.DemiBold; color: Theme.textPrimary
            }
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Pick a chat from the sidebar"
                font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondary
            }
        }

        // Empty chat
        Column {
            anchors.centerIn: parent
            spacing: Theme.spacingNormal
            visible: messageModel.conversationToken.length > 0 && messageModel.count === 0 && !messageModel.loading
            Label { anchors.horizontalCenter: parent.horizontalCenter; text: "💬"; font.pixelSize: 40; opacity: 0.6 }
            Label { anchors.horizontalCenter: parent.horizontalCenter; text: "No messages yet — say hello!"; font.pixelSize: Theme.fontSizeNormal; color: Theme.textSecondary }
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

    // Thread list overlay for group chats
    ThreadListView {
        anchors.fill: parent
        visible: chatRoot.isGroupChat && chatRoot.activeThreadId === 0 && messageModel.conversationToken.length > 0
        onThreadSelected: function(threadId, title) {
            chatRoot.openThread(threadId, title)
        }
    }

    Connections {
        target: messageModel
        function onConversationTokenChanged() {
            chatRoot.closeThread()
            if (chatRoot.isGroupChat && messageModel.conversationToken.length > 0) {
                threadModel.conversationToken = messageModel.conversationToken
            }
        }
    }
}
