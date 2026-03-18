import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

/**
 * Message input area — Telegram-style with text field and send button.
 */
Rectangle {
    id: composer
    height: Math.max(56, inputField.implicitHeight + 20)
    color: Theme.bgPrimary

    signal sendMessage(string text)

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        anchors.topMargin: 6
        anchors.bottomMargin: 6
        spacing: 10

        // Text input
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            TextArea {
                id: inputField
                placeholderText: "Write a message..."
                placeholderTextColor: Theme.textSecondary
                font.pixelSize: Theme.fontSizeNormal
                color: Theme.textPrimary
                wrapMode: TextEdit.Wrap
                background: Rectangle {
                    radius: Theme.radiusNormal
                    color: Theme.bgInput
                }
                padding: 12

                // Enter to send, Shift+Enter for newline
                Keys.onReturnPressed: function(event) {
                    if (event.modifiers & Qt.ShiftModifier) {
                        event.accepted = false // allow newline
                    } else {
                        event.accepted = true
                        sendAction()
                    }
                }
            }
        }

        // Send button
        RoundButton {
            id: sendButton
            width: 42
            height: 42
            flat: true
            enabled: inputField.text.trim().length > 0

            background: Rectangle {
                radius: 21
                color: sendButton.enabled
                    ? (sendButton.hovered ? "#4da5e7" : Theme.accent)
                    : "transparent"
            }

            contentItem: Text {
                text: "➤"
                font.pixelSize: 20
                color: sendButton.enabled ? "white" : Theme.textSecondary
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            onClicked: sendAction()
        }
    }

    function sendAction() {
        var text = inputField.text.trim()
        if (text.length > 0) {
            composer.sendMessage(text)
            inputField.text = ""
        }
    }
}
