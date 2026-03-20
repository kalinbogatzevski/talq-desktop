import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import TalkQt

Window {
    id: callWindow
    width: 400
    height: 300
    minimumWidth: 320
    minimumHeight: 200
    color: "#1a1a2e"
    title: "Call — " + callManager.remotePeerName
    visible: false

    // Show when call is connecting or active
    Connections {
        target: callManager
        function onStateChanged() {
            if (callManager.state === 3 || callManager.state === 4)  // Connecting or Active
                callWindow.visible = true
            else if (callManager.state === 0)  // Idle
                callWindow.visible = false
        }
    }

    // Avatar display (no video yet — Phase 4)
    ColumnLayout {
        anchors.centerIn: parent
        spacing: Theme.spacingLarge

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            width: 80; height: 80; radius: 40
            color: Theme.accent

            Label {
                anchors.centerIn: parent
                text: callManager.remotePeerName.length > 0 ? callManager.remotePeerName[0].toUpperCase() : "?"
                font.pixelSize: 32; font.weight: Font.Bold; color: "white"
            }
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: callManager.remotePeerName || "Calling..."
            font.pixelSize: 18; font.weight: Font.DemiBold
            color: "white"
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: {
                switch (callManager.state) {
                case 1: return "Ringing..."       // Outgoing
                case 2: return "Incoming..."      // Incoming
                case 3: return "Connecting..."    // Connecting
                case 4: return formatDuration(callManager.callDuration)  // Active
                default: return ""
                }
            }
            font.pixelSize: 14
            color: "#cccccc"

            function formatDuration(s) {
                var m = Math.floor(s / 60)
                var sec = s % 60
                return (m < 10 ? "0" : "") + m + ":" + (sec < 10 ? "0" : "") + sec
            }
        }
    }

    // Control bar at bottom
    RowLayout {
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: 24
        spacing: 20

        // Mute
        RoundButton {
            implicitWidth: 48; implicitHeight: 48
            onClicked: callManager.toggleMute()
            contentItem: Label {
                text: callManager.isMuted ? "\uD83D\uDD07" : "\uD83C\uDFA4"  // muted / mic
                font.pixelSize: 20
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 24
                color: callManager.isMuted ? "#e74c3c" : "#4a4a6a"
            }
        }

        // Hang up
        RoundButton {
            implicitWidth: 56; implicitHeight: 56
            onClicked: callManager.hangUp()
            contentItem: Label {
                text: "\uD83D\uDCF5"  // end call
                font.pixelSize: 24
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle { radius: 28; color: "#e74c3c" }
        }
    }

    // Close window = hang up
    onClosing: function(close) {
        callManager.hangUp()
    }
}
