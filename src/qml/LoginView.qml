import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: loginRoot
    implicitWidth: 380
    implicitHeight: 420

    Rectangle {
        anchors.fill: parent
        color: Theme.bgPrimary

        // Subtle gradient overlay for depth
        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(0.36, 0.71, 0.97, 0.03) }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 320)
        spacing: 0

        // Logo with glow
        Item {
            Layout.alignment: Qt.AlignHCenter
            Layout.bottomMargin: Theme.spacingNormal
            width: 88
            height: 88
            opacity: 0
            Component.onCompleted: opacity = 1
            Behavior on opacity { NumberAnimation { duration: Theme.animSlow; easing.type: Easing.OutCubic } }

            // Pulsing glow behind the logo
            Rectangle {
                anchors.centerIn: parent
                width: 76
                height: 76
                radius: 18
                color: "transparent"
                border.color: Qt.rgba(0.37, 0.82, 0.67, 0.25)
                border.width: 3

                SequentialAnimation on border.width {
                    loops: Animation.Infinite
                    NumberAnimation { from: 3; to: 6; duration: 2000; easing.type: Easing.InOutSine }
                    NumberAnimation { from: 6; to: 3; duration: 2000; easing.type: Easing.InOutSine }
                }
                SequentialAnimation on opacity {
                    loops: Animation.Infinite
                    NumberAnimation { from: 1.0; to: 0.3; duration: 2000; easing.type: Easing.InOutSine }
                    NumberAnimation { from: 0.3; to: 1.0; duration: 2000; easing.type: Easing.InOutSine }
                }
            }

            Image {
                anchors.centerIn: parent
                source: "qrc:/logo.png"
                width: 72
                height: 72
                sourceSize: Qt.size(160, 160)
                fillMode: Image.PreserveAspectFit

                // Slight hover float
                SequentialAnimation on y {
                    loops: Animation.Infinite
                    NumberAnimation { from: 8; to: 5; duration: 3000; easing.type: Easing.InOutQuad }
                    NumberAnimation { from: 5; to: 8; duration: 3000; easing.type: Easing.InOutQuad }
                }
            }
        }

        // App name
        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.bottomMargin: 4
            text: "TalQ"
            font.pixelSize: 28
            font.weight: Font.DemiBold
            font.letterSpacing: -0.5
            color: Theme.textPrimary
            opacity: 0
            Component.onCompleted: opacity = 1
            Behavior on opacity { NumberAnimation { duration: Theme.animSlow; easing.type: Easing.OutCubic } }
        }

        // Tagline
        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.bottomMargin: 32
            text: "Nextcloud Talk for your desktop"
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textSecondary
            opacity: 0
            Component.onCompleted: opacity = 1
            Behavior on opacity { NumberAnimation { duration: Theme.animSlow; easing.type: Easing.OutCubic } }
        }

        // Server URL label
        Label {
            Layout.bottomMargin: 6
            text: "Server address"
            font.pixelSize: Theme.fontSizeTiny
            font.weight: Font.DemiBold
            color: Theme.textSecondary
            font.letterSpacing: 0.5
            visible: !auth.waitingForBrowser
        }

        // Server URL input
        TextField {
            id: serverInput
            Layout.fillWidth: true
            Layout.bottomMargin: Theme.spacingLarge
            text: "https://ncloud.123net.link"
            placeholderText: "https://cloud.example.com"
            placeholderTextColor: Theme.textMuted
            font.pixelSize: Theme.fontSizeNormal
            color: Theme.textPrimary
            enabled: !auth.waitingForBrowser
            visible: !auth.waitingForBrowser
            selectByMouse: true
            background: Rectangle {
                radius: Theme.radiusSmall
                color: Theme.bgInput
                border.color: serverInput.activeFocus ? Theme.accent : "transparent"
                border.width: 2
                Behavior on border.color { ColorAnimation { duration: Theme.animNormal } }
            }
            padding: 12
            leftPadding: 14
            onAccepted: connectButton.clicked()
        }

        // Status message
        Label {
            Layout.fillWidth: true
            Layout.bottomMargin: Theme.spacingSmall
            text: auth.statusMessage
            color: Theme.accent
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.Wrap
            visible: auth.statusMessage.length > 0 && !auth.waitingForBrowser
            horizontalAlignment: Text.AlignHCenter
        }

        // Error message
        Rectangle {
            Layout.fillWidth: true
            Layout.bottomMargin: Theme.spacingNormal
            height: errorLabel.implicitHeight + 16
            radius: Theme.radiusSmall
            color: Qt.rgba(0.91, 0.30, 0.24, 0.1)
            visible: auth.errorMessage.length > 0

            Label {
                id: errorLabel
                anchors.centerIn: parent
                width: parent.width - 20
                text: auth.errorMessage
                color: Theme.danger
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
            }
        }

        // Waiting for browser state
        ColumnLayout {
            Layout.alignment: Qt.AlignHCenter
            Layout.bottomMargin: Theme.spacingLarge
            spacing: Theme.spacingLarge
            visible: auth.waitingForBrowser

            // Pulsing ring indicator
            Item {
                Layout.alignment: Qt.AlignHCenter
                width: 56
                height: 56

                Rectangle {
                    anchors.centerIn: parent
                    width: 56
                    height: 56
                    radius: 28
                    color: "transparent"
                    border.color: Theme.accent
                    border.width: 2
                    opacity: 0.3

                    SequentialAnimation on scale {
                        loops: Animation.Infinite
                        NumberAnimation { from: 1.0; to: 1.3; duration: 1200; easing.type: Easing.OutQuad }
                        NumberAnimation { from: 1.3; to: 1.0; duration: 1200; easing.type: Easing.InQuad }
                    }
                    SequentialAnimation on opacity {
                        loops: Animation.Infinite
                        NumberAnimation { from: 0.3; to: 0.0; duration: 1200; easing.type: Easing.OutQuad }
                        NumberAnimation { from: 0.0; to: 0.3; duration: 1200; easing.type: Easing.InQuad }
                    }
                }

                Rectangle {
                    anchors.centerIn: parent
                    width: 44
                    height: 44
                    radius: 22
                    color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.1)

                    Label {
                        anchors.centerIn: parent
                        text: "🌐"
                        font.pixelSize: 20
                    }
                }
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: "Waiting for browser login..."
                font.pixelSize: Theme.fontSizeNormal
                font.weight: Font.DemiBold
                color: Theme.textPrimary
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: "Complete sign-in in your browser,\nthen return here"
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondary
                horizontalAlignment: Text.AlignHCenter
                lineHeight: 1.4
            }
        }

        // Connect / Cancel button
        Button {
            id: connectButton
            Layout.fillWidth: true
            font.pixelSize: Theme.fontSizeNormal
            font.weight: Font.DemiBold
            padding: 13
            background: Rectangle {
                radius: Theme.radiusSmall
                color: auth.waitingForBrowser
                    ? (connectButton.pressed ? Qt.darker(Theme.bgInput, 1.2)
                       : connectButton.hovered ? Qt.lighter(Theme.bgInput, 1.2) : Theme.bgInput)
                    : (connectButton.pressed ? Theme.accentPressed
                       : connectButton.hovered ? Theme.accentHover : Theme.accent)
                Behavior on color { ColorAnimation { duration: Theme.animFast } }

                scale: connectButton.pressed ? 0.98 : 1.0
                Behavior on scale { NumberAnimation { duration: 80; easing.type: Easing.OutCubic } }
            }
            contentItem: Text {
                text: auth.waitingForBrowser ? "Cancel" : "Connect"
                color: auth.waitingForBrowser ? Theme.textSecondary : "white"
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

        // Version
        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Theme.spacingXLarge
            text: "v" + Qt.application.version
            font.pixelSize: 10
            color: Theme.textMuted
        }
    }
}
