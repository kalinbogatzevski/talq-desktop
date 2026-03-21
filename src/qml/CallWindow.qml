import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import TalkQt

Window {
    id: callWindow
    width: 480
    height: 400
    minimumWidth: 400
    minimumHeight: 340
    color: "#16162a"
    title: {
        if (callManager.state === CallManager.Outgoing) return "Calling..."
        if (callManager.state === CallManager.Incoming) return "Incoming call"
        return "Call — " + callManager.remotePeerName
    }
    visible: false

    Connections {
        target: callManager
        function onStateChanged() {
            if (callManager.state !== CallManager.Idle && callManager.state !== CallManager.Ending
                    && callManager.state !== CallManager.Incoming)
                callWindow.visible = true
            else if (callManager.state === CallManager.Idle) {
                callWindow.visible = false
                waveCanvas.samples = []
            }
        }
        function onAudioLevelChanged() {
            // Push new sample into waveform buffer
            var s = waveCanvas.samples
            s.push(callManager.audioLevel)
            if (s.length > waveCanvas.maxSamples)
                s.shift()
            waveCanvas.samples = s
            waveCanvas.requestPaint()
        }
    }

    // Pulsing ring during outgoing/incoming
    Rectangle {
        id: pulseRing
        anchors.centerIn: avatarCircle
        width: 96; height: 96; radius: 48
        color: "transparent"
        border.color: "#2ecc71"; border.width: 2; opacity: 0
        visible: callManager.state === CallManager.Outgoing || callManager.state === CallManager.Incoming
        SequentialAnimation on opacity {
            running: pulseRing.visible; loops: Animation.Infinite
            NumberAnimation { to: 0.6; duration: 800 }
            NumberAnimation { to: 0; duration: 800 }
        }
        SequentialAnimation on scale {
            running: pulseRing.visible; loops: Animation.Infinite
            NumberAnimation { to: 1.3; duration: 1600 }
            NumberAnimation { to: 1.0; duration: 0 }
        }
    }

    // Main content
    ColumnLayout {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 24
        spacing: 8

        // Avatar
        Rectangle {
            id: avatarCircle
            Layout.alignment: Qt.AlignHCenter
            width: 72; height: 72; radius: 36
            color: Theme.accent

            Image {
                anchors.fill: parent
                source: callManager.remotePeerId.length > 0
                    ? "image://avatar/" + callManager.remotePeerId : ""
                sourceSize: Qt.size(72, 72)
                visible: status === Image.Ready
            }
            Label {
                anchors.centerIn: parent
                visible: callManager.remotePeerId.length === 0
                text: callManager.remotePeerName.length > 0 ? callManager.remotePeerName[0].toUpperCase() : "?"
                font.pixelSize: 26; font.weight: Font.Bold; color: "white"
            }
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: callManager.remotePeerName || "Unknown"
            font.pixelSize: 16; font.weight: Font.DemiBold; color: "white"
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: {
                switch (callManager.state) {
                case CallManager.Outgoing: return "Calling..."
                case CallManager.Incoming: return "Incoming call..."
                case CallManager.Connecting: return "Connecting..."
                case CallManager.Active: return formatDuration(callManager.callDuration)
                default: return ""
                }
            }
            font.pixelSize: 13
            color: callManager.state === CallManager.Active ? "#2ecc71" : "#aaaacc"
            function formatDuration(s) {
                var m = Math.floor(s / 60)
                var sec = s % 60
                return (m < 10 ? "0" : "") + m + ":" + (sec < 10 ? "0" : "") + sec
            }
        }

        // Audio waveform visualization
        Canvas {
            id: waveCanvas
            Layout.alignment: Qt.AlignHCenter
            width: 200; height: 40
            visible: callManager.state === CallManager.Active

            property var samples: []
            property int maxSamples: 60

            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)

                if (samples.length < 2) return

                var midY = height / 2
                var stepX = width / maxSamples

                // Draw filled waveform (mirrored)
                ctx.beginPath()
                ctx.moveTo(0, midY)
                for (var i = 0; i < samples.length; i++) {
                    var x = i * stepX
                    var amp = samples[i] * midY * 0.9
                    ctx.lineTo(x, midY - amp)
                }
                ctx.lineTo((samples.length - 1) * stepX, midY)
                for (var j = samples.length - 1; j >= 0; j--) {
                    var x2 = j * stepX
                    var amp2 = samples[j] * midY * 0.9
                    ctx.lineTo(x2, midY + amp2)
                }
                ctx.closePath()

                // Gradient fill
                var grad = ctx.createLinearGradient(0, 0, 0, height)
                grad.addColorStop(0, "#2ecc7180")
                grad.addColorStop(0.5, "#2ecc71")
                grad.addColorStop(1, "#2ecc7180")
                ctx.fillStyle = grad
                ctx.fill()

                // Center line
                ctx.strokeStyle = "#2ecc7140"
                ctx.lineWidth = 1
                ctx.beginPath()
                ctx.moveTo(0, midY)
                ctx.lineTo(width, midY)
                ctx.stroke()
            }
        }

        // Call stats (toggleable)
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            width: 220
            height: statsCol.implicitHeight + 12
            radius: 6
            color: "#1a1a3e"
            visible: statsToggle.checked && callManager.state === CallManager.Active

            ColumnLayout {
                id: statsCol
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 6 }
                spacing: 1

                Repeater {
                    model: callManager.callStats.length > 0 ? callManager.callStats.split("\n") : []
                    Label {
                        text: modelData
                        font.pixelSize: 9; font.family: "Consolas"
                        color: "#8888aa"
                    }
                }
            }
        }
    }

    // Control bar
    RowLayout {
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: 24
        spacing: 20

        // Stats toggle
        RoundButton {
            implicitWidth: 40; implicitHeight: 40
            id: statsToggle
            checkable: true
            checked: false
            visible: callManager.state === CallManager.Active
            contentItem: Label {
                text: "i"
                font.pixelSize: 16; font.weight: Font.Bold; font.italic: true
                color: statsToggle.checked ? "white" : "#8888aa"
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 20
                color: statsToggle.checked ? "#3a3a6e" : "#2a2a4e"
                border.color: "#4a4a7e"; border.width: 1
            }
            ToolTip.visible: hovered; ToolTip.text: "Call info"
        }

        // Mute
        RoundButton {
            implicitWidth: 50; implicitHeight: 50
            visible: callManager.state === CallManager.Connecting || callManager.state === CallManager.Active
            onClicked: callManager.toggleMute()
            contentItem: Image {
                source: callManager.isMuted ? "qrc:/icons/mic-off.svg" : "qrc:/icons/mic.svg"
                sourceSize: Qt.size(22, 22); anchors.centerIn: parent
            }
            background: Rectangle {
                radius: 25
                color: callManager.isMuted ? "#e74c3c" : "#3a3a5e"
                border.color: callManager.isMuted ? "#e74c3c" : "#5a5a8e"; border.width: 1
            }
            ToolTip.visible: hovered; ToolTip.text: callManager.isMuted ? "Unmute" : "Mute"
        }

        // Hang up
        RoundButton {
            implicitWidth: 56; implicitHeight: 56
            onClicked: callManager.state === CallManager.Incoming ? callManager.declineCall() : callManager.hangUp()
            contentItem: Image {
                source: "qrc:/icons/phone-off.svg"
                sourceSize: Qt.size(24, 24); anchors.centerIn: parent
            }
            background: Rectangle { radius: 28; color: parent.hovered ? "#ff4444" : "#e74c3c" }
            ToolTip.visible: hovered
            ToolTip.text: callManager.state === CallManager.Incoming ? "Decline" : "Hang up"
        }

        // Accept (incoming)
        RoundButton {
            implicitWidth: 56; implicitHeight: 56
            visible: callManager.state === CallManager.Incoming
            onClicked: callManager.acceptCall(false)
            contentItem: Image {
                source: "qrc:/icons/phone-call.svg"
                sourceSize: Qt.size(24, 24); anchors.centerIn: parent
            }
            background: Rectangle { radius: 28; color: parent.hovered ? "#33dd77" : "#2ecc71" }
            ToolTip.visible: hovered; ToolTip.text: "Accept"
        }
    }

    onClosing: function(close) { callManager.hangUp() }
}
