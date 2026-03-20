import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import TalkQt

Window {
    id: callPopup
    width: 320
    height: 180
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    transientParent: null
    color: Theme.bgSurface
    visible: false

    property string callerName: ""
    property bool withVideo: false

    signal accepted(bool withVideo)
    signal declined()

    // Center on screen
    x: (Screen.width - width) / 2
    y: (Screen.height - height) / 3

    Rectangle {
        anchors.fill: parent
        color: Theme.bgSurface
        radius: 12
        border.color: Theme.accent
        border.width: 2

        ColumnLayout {
            anchors.centerIn: parent
            spacing: Theme.spacingNormal

            // Caller avatar with pulse
            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                width: 56; height: 56; radius: 28
                color: Theme.accent

                Label {
                    anchors.centerIn: parent
                    text: callPopup.callerName.length > 0 ? callPopup.callerName[0].toUpperCase() : "?"
                    font.pixelSize: 22; font.weight: Font.Bold; color: "white"
                }

                SequentialAnimation on scale {
                    loops: Animation.Infinite
                    NumberAnimation { to: 1.08; duration: 600; easing.type: Easing.InOutQuad }
                    NumberAnimation { to: 1.0; duration: 600; easing.type: Easing.InOutQuad }
                }
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: callPopup.callerName
                font.pixelSize: Theme.fontSizeLarge
                font.weight: Font.DemiBold
                color: Theme.textPrimary
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: callPopup.withVideo ? "Incoming video call..." : "Incoming audio call..."
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondary
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: Theme.spacingLarge

                // Accept audio
                RoundButton {
                    implicitWidth: 48; implicitHeight: 48
                    onClicked: { callPopup.accepted(false); callPopup.visible = false }
                    contentItem: Label {
                        text: "\uD83D\uDCDE"; font.pixelSize: 20
                        horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle { radius: 24; color: "#27ae60" }
                }

                // Decline
                RoundButton {
                    implicitWidth: 48; implicitHeight: 48
                    onClicked: { callPopup.declined(); callPopup.visible = false }
                    contentItem: Label {
                        text: "\u2716"; font.pixelSize: 18; color: "white"
                        horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle { radius: 24; color: "#e74c3c" }
                }
            }
        }
    }
}
