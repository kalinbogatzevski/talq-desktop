import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import TalkQt

Window {
    id: callWindow
    width: 380
    height: 280
    minimumWidth: 320
    minimumHeight: 220
    color: "#16162a"
    title: {
        if (callManager.state === 1) return "Calling..."
        if (callManager.state === 2) return "Incoming call"
        return "Call — " + callManager.remotePeerName
    }
    visible: false

    // Show when call is outgoing, incoming, connecting or active
    Connections {
        target: callManager
        function onStateChanged() {
            if (callManager.state >= 1 && callManager.state <= 4)
                callWindow.visible = true
            else if (callManager.state === 0)
                callWindow.visible = false
        }
    }

    // Subtle pulsing ring animation during outgoing/incoming
    Rectangle {
        id: pulseRing
        anchors.centerIn: avatarCircle
        width: 96; height: 96; radius: 48
        color: "transparent"
        border.color: "#2ecc71"
        border.width: 2
        opacity: 0
        visible: callManager.state === 1 || callManager.state === 2

        SequentialAnimation on opacity {
            running: pulseRing.visible
            loops: Animation.Infinite
            NumberAnimation { to: 0.6; duration: 800; easing.type: Easing.OutQuad }
            NumberAnimation { to: 0; duration: 800; easing.type: Easing.InQuad }
        }
        SequentialAnimation on scale {
            running: pulseRing.visible
            loops: Animation.Infinite
            NumberAnimation { to: 1.3; duration: 1600; easing.type: Easing.OutQuad }
            NumberAnimation { to: 1.0; duration: 0 }
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -20
        spacing: 12

        // Avatar
        Rectangle {
            id: avatarCircle
            Layout.alignment: Qt.AlignHCenter
            width: 80; height: 80; radius: 40
            color: Theme.accent

            Image {
                anchors.fill: parent
                source: callManager.remotePeerId.length > 0
                    ? "image://avatar/" + callManager.remotePeerId : ""
                sourceSize: Qt.size(80, 80)
                visible: status === Image.Ready
            }

            Label {
                anchors.centerIn: parent
                visible: callManager.remotePeerId.length === 0
                text: callManager.remotePeerName.length > 0 ? callManager.remotePeerName[0].toUpperCase() : "?"
                font.pixelSize: 30; font.weight: Font.Bold; color: "white"
            }
        }

        // Name
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: callManager.remotePeerName || "Unknown"
            font.pixelSize: 18; font.weight: Font.DemiBold
            color: "white"
        }

        // Status
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: {
                switch (callManager.state) {
                case 1: return "Calling..."
                case 2: return "Incoming call..."
                case 3: return "Connecting..."
                case 4: return formatDuration(callManager.callDuration)
                default: return ""
                }
            }
            font.pixelSize: 13
            color: callManager.state === 4 ? "#2ecc71" : "#aaaacc"

            function formatDuration(s) {
                var m = Math.floor(s / 60)
                var sec = s % 60
                return (m < 10 ? "0" : "") + m + ":" + (sec < 10 ? "0" : "") + sec
            }
        }
    }

    // Control bar
    RowLayout {
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: 28
        spacing: 24

        // Mute button
        RoundButton {
            implicitWidth: 50; implicitHeight: 50
            visible: callManager.state >= 3  // only during connecting/active
            onClicked: callManager.toggleMute()
            contentItem: Image {
                source: callManager.isMuted ? "qrc:/icons/mic-off.svg" : "qrc:/icons/mic.svg"
                sourceSize: Qt.size(22, 22)
                anchors.centerIn: parent
            }
            background: Rectangle {
                radius: 25
                color: callManager.isMuted ? "#e74c3c" : "#3a3a5e"
                border.color: callManager.isMuted ? "#e74c3c" : "#5a5a8e"
                border.width: 1
            }
            ToolTip.visible: hovered
            ToolTip.text: callManager.isMuted ? "Unmute" : "Mute"
        }

        // Hang up / Decline
        RoundButton {
            implicitWidth: 56; implicitHeight: 56
            onClicked: callManager.state === 2 ? callManager.declineCall() : callManager.hangUp()
            contentItem: Image {
                source: "qrc:/icons/phone-off.svg"
                sourceSize: Qt.size(24, 24)
                anchors.centerIn: parent
            }
            background: Rectangle {
                radius: 28
                color: parent.hovered ? "#ff4444" : "#e74c3c"
            }
            ToolTip.visible: hovered
            ToolTip.text: callManager.state === 2 ? "Decline" : "Hang up"
        }

        // Accept (incoming only)
        RoundButton {
            implicitWidth: 56; implicitHeight: 56
            visible: callManager.state === 2
            onClicked: callManager.acceptCall(false)
            contentItem: Image {
                source: "qrc:/icons/phone-call.svg"
                sourceSize: Qt.size(24, 24)
                anchors.centerIn: parent
            }
            background: Rectangle {
                radius: 28
                color: parent.hovered ? "#33dd77" : "#2ecc71"
            }
            ToolTip.visible: hovered
            ToolTip.text: "Accept"
        }
    }

    onClosing: function(close) {
        callManager.hangUp()
    }
}
