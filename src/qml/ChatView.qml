import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: chatRoot
    property string conversationName: ""

    Rectangle {
        anchors.fill: parent
        color: Theme.bgSecondary

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Chat header
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.headerHeight
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
                }
            }

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.divider
            }

            // Messages
            ListView {
                id: messageListView
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: messageModel
                clip: true
                verticalLayoutDirection: ListView.TopToBottom
                spacing: 2
                topMargin: Theme.spacingSmall
                bottomMargin: Theme.spacingSmall
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

                // Scroll to bottom on new messages / initial load
                onCountChanged: {
                    scrollTimer.restart()
                }

                Timer {
                    id: scrollTimer
                    interval: 50
                    onTriggered: messageListView.positionViewAtEnd()
                }

                // Empty state
                Column {
                    anchors.centerIn: parent
                    spacing: Theme.spacingNormal
                    visible: messageModel.count === 0 && !messageModel.loading

                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: messageModel.conversationToken.length > 0 ? "💬" : "👈"
                        font.pixelSize: 40
                        opacity: 0.6
                    }
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: messageModel.conversationToken.length > 0
                            ? "No messages yet — say hello!"
                            : "Select a conversation to start chatting"
                        font.pixelSize: Theme.fontSizeNormal
                        color: Theme.textSecondary
                    }
                }

                BusyIndicator {
                    anchors.centerIn: parent
                    running: messageModel.loading
                    visible: running
                    palette.dark: Theme.accent
                }

                delegate: MessageBubble {
                    width: messageListView.width
                    isOwnMessage: actorId === auth.userId
                }
            }

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.divider
            }

            // Composer
            MessageComposer {
                Layout.fillWidth: true
                visible: messageModel.conversationToken.length > 0
                onSendMessage: function(text) {
                    messageModel.sendMessage(text)
                }
            }
        }
    }
}
