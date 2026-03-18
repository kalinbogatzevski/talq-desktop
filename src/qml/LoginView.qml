import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: loginRoot

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 420)
        spacing: 20

        // Logo / Title
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: "Talk Qt"
            font.pixelSize: 32
            font.weight: Font.Light
            color: Theme.textPrimary
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: "Connect to your Nextcloud server"
            font.pixelSize: 14
            color: Theme.textSecondary
        }

        // Server URL input
        TextField {
            id: serverInput
            Layout.fillWidth: true
            placeholderText: "https://cloud.example.com"
            placeholderTextColor: "#506070"
            font.pixelSize: 15
            color: Theme.textPrimary
            enabled: !auth.waitingForBrowser
            background: Rectangle {
                radius: 8
                color: Theme.bgInput
                border.color: serverInput.activeFocus ? Theme.accent : Theme.border
                border.width: 1
            }
            padding: 14
            onAccepted: connectButton.clicked()
        }

        // Status message (connecting, waiting for browser, etc.)
        Label {
            Layout.fillWidth: true
            text: auth.statusMessage
            color: Theme.accent
            font.pixelSize: 13
            wrapMode: Text.Wrap
            visible: auth.statusMessage.length > 0
            horizontalAlignment: Text.AlignHCenter
        }

        // Error message
        Label {
            Layout.fillWidth: true
            text: auth.errorMessage
            color: "#e74c3c"
            font.pixelSize: 13
            wrapMode: Text.Wrap
            visible: auth.errorMessage.length > 0
        }

        // Waiting indicator
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 12
            visible: auth.waitingForBrowser

            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: auth.waitingForBrowser
                implicitWidth: 48
                implicitHeight: 48
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: "Complete the login in your browser,\nthen return here"
                font.pixelSize: 13
                color: Theme.textSecondary
                horizontalAlignment: Text.AlignHCenter
            }
        }

        // Connect / Cancel button
        Button {
            id: connectButton
            Layout.fillWidth: true
            text: auth.waitingForBrowser ? "Cancel" : "Connect"
            font.pixelSize: 15
            padding: 14
            background: Rectangle {
                radius: 8
                color: auth.waitingForBrowser
                    ? (connectButton.hovered ? "#c0392b" : "#e74c3c")
                    : (connectButton.hovered ? "#4da5e7" : Theme.accent)
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
