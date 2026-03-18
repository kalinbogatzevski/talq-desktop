import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: loginRoot

    Rectangle {
        anchors.fill: parent
        color: Theme.bgPrimary
    }

    // Login card
    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 60, 400)
        height: loginColumn.implicitHeight + Theme.spacingXLarge * 2
        radius: Theme.radiusLarge
        color: Theme.bgSurface
        border.color: Theme.divider
        border.width: 1

        // Subtle top accent line
        Rectangle {
            width: parent.width - 2
            height: 2
            radius: 1
            anchors.top: parent.top
            anchors.topMargin: 1
            anchors.horizontalCenter: parent.horizontalCenter
            color: Theme.accent
            opacity: 0.6
        }

        ColumnLayout {
            id: loginColumn
            anchors {
                left: parent.left; right: parent.right
                verticalCenter: parent.verticalCenter
                margins: Theme.spacingXLarge
            }
            spacing: Theme.spacingLarge

            // Icon
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: "💬"
                font.pixelSize: 40
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: "TalQ"
                font.pixelSize: Theme.fontSizeTitle
                font.weight: Font.DemiBold
                font.letterSpacing: -0.3
                color: Theme.textPrimary
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: "Connect to your Nextcloud server"
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondary
            }

            Item { height: Theme.spacingSmall }

            // Server URL
            TextField {
                id: serverInput
                Layout.fillWidth: true
                placeholderText: "https://cloud.example.com"
                placeholderTextColor: Theme.textMuted
                font.pixelSize: Theme.fontSizeNormal
                color: Theme.textPrimary
                enabled: !auth.waitingForBrowser
                background: Rectangle {
                    radius: Theme.radiusSmall
                    color: Theme.bgInput
                    border.color: serverInput.activeFocus ? Theme.accent : Theme.border
                    border.width: 1
                    Behavior on border.color { ColorAnimation { duration: Theme.animNormal } }
                }
                padding: 14
                onAccepted: connectButton.clicked()
            }

            // Status
            Label {
                Layout.fillWidth: true
                text: auth.statusMessage
                color: Theme.accent
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.Wrap
                visible: auth.statusMessage.length > 0
                horizontalAlignment: Text.AlignHCenter
            }

            // Error
            Label {
                Layout.fillWidth: true
                text: "⚠️ " + auth.errorMessage
                color: Theme.danger
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.Wrap
                visible: auth.errorMessage.length > 0
            }

            // Waiting
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingNormal
                visible: auth.waitingForBrowser

                BusyIndicator {
                    Layout.alignment: Qt.AlignHCenter
                    running: auth.waitingForBrowser
                    implicitWidth: 44
                    implicitHeight: 44
                    palette.dark: Theme.accent
                }

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Complete login in your browser\nthen return here ↩"
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textSecondary
                    horizontalAlignment: Text.AlignHCenter
                    lineHeight: 1.3
                }
            }

            // Button
            Button {
                id: connectButton
                Layout.fillWidth: true
                text: auth.waitingForBrowser ? "✕  Cancel" : "→  Connect"
                font.pixelSize: Theme.fontSizeNormal
                font.weight: Font.DemiBold
                padding: 14
                background: Rectangle {
                    radius: Theme.radiusSmall
                    color: auth.waitingForBrowser
                        ? (connectButton.pressed ? Theme.dangerHover
                           : connectButton.hovered ? "#d63328" : Theme.danger)
                        : (connectButton.pressed ? Theme.accentPressed
                           : connectButton.hovered ? Theme.accentHover : Theme.accent)
                    Behavior on color { ColorAnimation { duration: Theme.animFast } }
                }
                contentItem: Text {
                    text: connectButton.text
                    color: "white"
                    font: connectButton.font
                    horizontalAlignment: Text.AlignHCenter
                }
                onClicked: {
                    if (auth.waitingForBrowser) {
                        auth.cancelLogin()
                    } else if (serverInput.text.trim().length > 0) {
                        auth.startLogin(serverInput.text.trim())
                    }
                }
            }
        }
    }
}
