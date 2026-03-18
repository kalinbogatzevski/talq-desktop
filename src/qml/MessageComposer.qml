import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: composer
    height: Math.max(Theme.composerMinHeight, inputField.implicitHeight + 18)
    color: Theme.bgPrimary

    signal sendMessage(string text)

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spacingNormal
        anchors.rightMargin: Theme.spacingNormal
        anchors.topMargin: Theme.spacingTiny
        anchors.bottomMargin: Theme.spacingTiny
        spacing: Theme.spacingSmall

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            TextArea {
                id: inputField
                placeholderText: "Write a message..."
                placeholderTextColor: Theme.textMuted
                font.pixelSize: Theme.fontSizeNormal
                color: Theme.textPrimary
                wrapMode: TextEdit.Wrap
                background: Rectangle {
                    radius: Theme.radiusNormal
                    color: Theme.bgInput
                    border.color: inputField.activeFocus
                        ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.35)
                        : "transparent"
                    border.width: 1
                    Behavior on border.color { ColorAnimation { duration: Theme.animNormal } }
                }
                padding: Theme.spacingNormal
                leftPadding: Theme.spacingLarge

                Keys.onReturnPressed: function(event) {
                    if (event.modifiers & Qt.ShiftModifier) {
                        event.accepted = false
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
            width: 40
            height: 40
            flat: true
            enabled: inputField.text.trim().length > 0

            background: Rectangle {
                radius: 20
                color: sendButton.enabled
                    ? (sendButton.pressed ? Theme.accentPressed
                       : sendButton.hovered ? Theme.accentHover : Theme.accent)
                    : "transparent"
                Behavior on color { ColorAnimation { duration: Theme.animFast } }

                scale: sendButton.pressed ? 0.92 : 1
                Behavior on scale { NumberAnimation { duration: Theme.animFast; easing.type: Easing.OutCubic } }
            }

            contentItem: Label {
                text: "➤"
                font.pixelSize: 18
                color: sendButton.enabled ? "white" : Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                Behavior on color { ColorAnimation { duration: Theme.animNormal } }
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
