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
        TqIconButton {
            iconName: "attach"
            size: Theme.buttonSizeMedium
            onClicked: fileDialog.open()
            ToolTip.visible: hovered; ToolTip.text: "Attach file"; ToolTip.delay: 300
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
        TqIconButton {
            id: sendButton
            iconName: "send"
            size: 40
            bgColor: enabled ? Theme.accent : "transparent"
            iconColor: enabled ? "#000" : Theme.textMuted
            enabled: inputField.text.trim().length > 0
            onClicked: sendAction()
            ToolTip.visible: hovered
            ToolTip.text: "Send message (Enter)"
            ToolTip.delay: 500
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
