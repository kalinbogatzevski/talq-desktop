import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

/**
 * Telegram-style chat view with message list and composer.
 */
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
                Layout.preferredHeight: 56
                color: Theme.bgPrimary

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 20
                    anchors.rightMargin: 20

                    Label {
                        text: chatRoot.conversationName || "Select a conversation"
                        font.pixelSize: Theme.fontSizeLarge
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }
            }

            // Separator
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.border
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

                // Auto-scroll to bottom on new messages
                onCountChanged: {
                    Qt.callLater(positionViewAtEnd)
                }

                // Empty state
                Label {
                    anchors.centerIn: parent
                    visible: messageModel.count === 0 && !messageModel.loading
                    text: messageModel.conversationToken.length > 0
                        ? "No messages yet"
                        : "Select a conversation to start chatting"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeNormal
                }

                // Loading
                BusyIndicator {
                    anchors.centerIn: parent
                    running: messageModel.loading
                    visible: running
                }

                delegate: MessageBubble {
                    width: messageListView.width
                    isOwnMessage: actorId === auth.userId
                }
            }

            // Separator
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.border
            }

            // Message composer
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
