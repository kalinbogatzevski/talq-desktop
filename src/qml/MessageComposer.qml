import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Rectangle {
    id: composer
    implicitHeight: Math.max(Theme.composerMinHeight, inputField.implicitHeight + 18)
    height: implicitHeight
    color: Theme.bgPrimary

    signal sendMessage(string text)
    property string topicName: ""

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spacingNormal
        anchors.rightMargin: Theme.spacingNormal
        anchors.topMargin: Theme.spacingTiny
        anchors.bottomMargin: Theme.spacingTiny
        spacing: Theme.spacingSmall

        // Attach file button
        RoundButton {
            width: 36; height: 36; flat: true
            ToolTip.visible: hovered; ToolTip.text: "Attach file"; ToolTip.delay: 300
            contentItem: Label {
                text: "\uD83D\uDCCE"  // 📎
                font.pixelSize: 18
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 18
                color: parent.hovered ? Theme.bgHover : "transparent"
                Behavior on color { ColorAnimation { duration: Theme.animFast } }
            }
            onClicked: fileDialog.open()
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            TextArea {
                id: inputField
                placeholderText: composer.topicName.length > 0 ? "Reply in " + composer.topicName + "..." : "Message..."
                placeholderTextColor: Theme.textMuted
                font.pixelSize: Theme.fontSizeNormal
                color: Theme.textPrimary
                wrapMode: TextEdit.Wrap
                background: Rectangle {
                    radius: Theme.radiusRound
                    color: Theme.bgInput
                    border.color: inputField.activeFocus
                        ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.35)
                        : "transparent"
                    border.width: 1
                    Behavior on border.color { ColorAnimation { duration: Theme.animNormal } }
                }
                padding: Theme.spacingNormal
                leftPadding: Theme.spacingLarge

                // Send typing indicator (debounced)
                onTextChanged: {
                    if (text.length > 0) {
                        signaling.sendStartedTyping()
                        typingStopTimer.restart()
                    }
                }

                Timer {
                    id: typingStopTimer
                    interval: 3000
                    onTriggered: signaling.sendStoppedTyping()
                }

                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_V && (event.modifiers & Qt.ControlModifier)) {
                        if (messageModel.pasteClipboardImage()) {
                            event.accepted = true
                            return
                        }
                    }
                }

                Keys.onReturnPressed: function(event) {
                    if (event.modifiers & Qt.ShiftModifier) {
                        event.accepted = false
                    } else {
                        event.accepted = true
                        typingStopTimer.stop()
                        signaling.sendStoppedTyping()
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
            ToolTip.visible: hovered
            ToolTip.text: "Send message (Enter)"
            ToolTip.delay: 500

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
                text: "\u276F"  // ❯ send arrow
                font.pixelSize: 18
                font.weight: Font.Bold
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

    FileDialog {
        id: fileDialog
        title: "Send file"
        onAccepted: {
            messageModel.promptFileSend(selectedFile.toString())
        }
    }
}
