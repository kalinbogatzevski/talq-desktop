import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: chatRoot
    property string conversationName: ""
    property int replyToId: 0
    property string replyToAuthor: ""
    property string replyToText: ""

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

    Rectangle {
        anchors.fill: parent
        color: Theme.bgSecondary

        // ── Header (top) ──
        Rectangle {
            id: chatHeader
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: Theme.headerHeight
            color: Theme.bgPrimary

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingXLarge
                anchors.rightMargin: Theme.spacingXLarge
                spacing: Theme.spacingNormal

                Label {
                    text: chatRoot.conversationName || "Select a conversation"
                    font.pixelSize: Theme.fontSizeLarge
                    font.weight: chatRoot.conversationName.length > 0 ? Font.DemiBold : Font.Normal
                    color: chatRoot.conversationName.length > 0 ? Theme.textPrimary : Theme.textMuted
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                BusyIndicator {
                    running: messageModel.loading && messageModel.count === 0
                    visible: running
                    implicitWidth: 20
                    implicitHeight: 20
                    palette.dark: Theme.accent
                }

                Label {
                    visible: messageModel.loading && messageModel.count > 0
                    text: "\u21BB"
                    font.pixelSize: 14
                    color: Theme.textMuted
                    RotationAnimation on rotation {
                        from: 0; to: 360
                        duration: 1000
                        loops: Animation.Infinite
                        running: messageModel.loading && messageModel.count > 0
                    }
                }
            }
        }

        Rectangle {
            id: headerDivider
            anchors.top: chatHeader.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: Theme.divider
        }

        // ── Composer (bottom, anchored) ──
        MessageComposer {
            id: composerArea
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            visible: messageModel.conversationToken.length > 0
            onSendMessage: function(text) {
                messageModel.sendMessage(text, chatRoot.replyToId)
                chatRoot.cancelReply()
            }
        }

        Rectangle {
            id: composerDivider
            anchors.bottom: replyBar.visible ? replyBar.top : composerArea.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: composerArea.visible ? 1 : 0
            color: Theme.divider
        }

        // ── Reply preview bar (above composer) ──
        Rectangle {
            id: replyBar
            anchors.bottom: composerArea.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: visible ? replyPreviewCol.implicitHeight + 12 : 0
            color: Theme.bgSurface
            visible: chatRoot.replyToId > 0

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingLarge
                anchors.rightMargin: Theme.spacingNormal
                spacing: Theme.spacingSmall

                Rectangle {
                    width: 3
                    height: parent.height - 8
                    radius: 1.5
                    color: Theme.accent
                    Layout.alignment: Qt.AlignVCenter
                }

                ColumnLayout {
                    id: replyPreviewCol
                    Layout.fillWidth: true
                    spacing: 1

                    Label {
                        text: chatRoot.replyToAuthor
                        font.pixelSize: Theme.fontSizeTiny
                        font.weight: Font.DemiBold
                        color: Theme.accent
                    }
                    Label {
                        Layout.fillWidth: true
                        text: chatRoot.replyToText
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.textSecondary
                        elide: Text.ElideRight
                        maximumLineCount: 1
                    }
                }

                ToolButton {
                    width: 28; height: 28
                    onClicked: chatRoot.cancelReply()
                    contentItem: Label {
                        text: "\u2715"
                        font.pixelSize: 14
                        color: Theme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 14
                        color: parent.hovered ? Theme.bgHover : "transparent"
                    }
                }
            }
        }

        // ── Messages (fills between header and composer) ──
        ListView {
            id: messageListView
            anchors.top: headerDivider.bottom
            anchors.bottom: composerDivider.top
            anchors.left: parent.left
            anchors.right: parent.right
            model: messageModel
            clip: true
            verticalLayoutDirection: ListView.BottomToTop
            spacing: 2
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
                width: 5
                contentItem: Rectangle {
                    radius: 2
                    color: Theme.textMuted
                    opacity: 0.3
                }
            }

            // Empty state — no conversation selected
            Column {
                anchors.centerIn: parent
                spacing: Theme.spacingLarge
                visible: messageModel.conversationToken.length === 0 && !messageModel.loading

                Image {
                    anchors.horizontalCenter: parent.horizontalCenter
                    source: "qrc:/logo.png"
                    width: 64; height: 64
                    sourceSize: Qt.size(128, 128)
                    fillMode: Image.PreserveAspectFit
                    opacity: 0.8
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Welcome, " + auth.displayName
                    font.pixelSize: Theme.fontSizeLarge
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Pick a chat from the sidebar to get started"
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textSecondary
                }

                Item { height: 4; width: 1 }

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: infoGrid.width + 40
                    height: infoGrid.height + 24
                    radius: Theme.radiusNormal
                    color: Theme.bgSurface

                    GridLayout {
                        id: infoGrid
                        anchors.centerIn: parent
                        columns: 2
                        columnSpacing: Theme.spacingLarge
                        rowSpacing: 8

                        Label { text: "Server"; font.pixelSize: Theme.fontSizeTiny; font.weight: Font.DemiBold; color: Theme.textMuted; Layout.alignment: Qt.AlignRight }
                        Label { text: auth.serverUrl.replace(/^https?:\/\//, ""); font.pixelSize: Theme.fontSizeTiny; color: Theme.accent }

                        Label { visible: auth.nextcloudVersion.length > 0; text: "Nextcloud"; font.pixelSize: Theme.fontSizeTiny; font.weight: Font.DemiBold; color: Theme.textMuted; Layout.alignment: Qt.AlignRight }
                        Label { visible: auth.nextcloudVersion.length > 0; text: "v" + auth.nextcloudVersion; font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondary }

                        Label { visible: auth.talkVersion.length > 0; text: "Talk"; font.pixelSize: Theme.fontSizeTiny; font.weight: Font.DemiBold; color: Theme.textMuted; Layout.alignment: Qt.AlignRight }
                        Label { visible: auth.talkVersion.length > 0; text: "v" + auth.talkVersion; font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondary }

                        Label { visible: auth.signalingMode.length > 0; text: "Signaling"; font.pixelSize: Theme.fontSizeTiny; font.weight: Font.DemiBold; color: Theme.textMuted; Layout.alignment: Qt.AlignRight }
                        Label { visible: auth.signalingMode.length > 0; text: auth.signalingMode; font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondary }

                        Label { text: "Chats"; font.pixelSize: Theme.fontSizeTiny; font.weight: Font.DemiBold; color: Theme.textMuted; Layout.alignment: Qt.AlignRight }
                        Label { text: conversationModel.count > 0 ? conversationModel.count : "..."; font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondary }
                    }
                }
            }

            // Empty state — conversation selected but no messages
            Column {
                anchors.centerIn: parent
                spacing: Theme.spacingNormal
                visible: messageModel.conversationToken.length > 0 && messageModel.count === 0 && !messageModel.loading

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "💬"
                    font.pixelSize: 40
                    opacity: 0.6
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "No messages yet — say hello!"
                    font.pixelSize: Theme.fontSizeNormal
                    color: Theme.textSecondary
                }
            }

            delegate: MessageBubble {
                width: messageListView.width
                isOwnMessage: actorId === auth.userId
                onReplyRequested: function(msgId, author, text) {
                    chatRoot.startReply(msgId, author, text)
                }
            }
        }
    }
}
