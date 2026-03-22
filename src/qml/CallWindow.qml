import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
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

    VideoOutput {
        id: remoteVideo
        anchors.fill: parent
        visible: callManager.remoteVideoProvider && callManager.remoteVideoProvider.hasVideo
        fillMode: VideoOutput.PreserveAspectCrop
    }

    // Bind the VideoOutput's sink to the VideoFrameProvider
    Connections {
        target: callManager
        function onRemoteVideoProviderChanged() {
            if (callManager.remoteVideoProvider) {
                callManager.remoteVideoProvider.videoSink = remoteVideo.videoSink
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        propagateComposedEvents: true
        onPositionChanged: (mouse) => {
            controlBar.controlBarVisible = true
            hideTimer.restart()
            mouse.accepted = false
        }
        onClicked: (mouse) => mouse.accepted = false
    }

    Timer {
        id: hideTimer
        interval: 3000
        onTriggered: {
            if (remoteVideo.visible)
                controlBar.controlBarVisible = false
        }
    }

    // Duration overlay for video mode
    Rectangle {
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: 16
        width: durationOverlayText.width + 24
        height: 32
        radius: 16
        color: "#80000000"
        visible: remoteVideo.visible && callManager.state === CallManager.Active
        z: 10

        Text {
            id: durationOverlayText
            anchors.centerIn: parent
            text: callManager.state === CallManager.Active ? formatDuration(callManager.callDuration) : ""
            color: "white"
            font.pixelSize: 14
        }
    }

    function formatDuration(s) {
        var m = Math.floor(s / 60)
        var sec = s % 60
        return (m < 10 ? "0" : "") + m + ":" + (sec < 10 ? "0" : "") + sec
    }

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
            waveCanvas.lastLevel = Math.max(waveCanvas.lastLevel, callManager.audioLevel)
        }
    }

    // Pulsing ring during outgoing/incoming
    Rectangle {
        id: pulseRing
        anchors.centerIn: avatarCircle
        width: 96; height: 96; radius: 48
        color: "transparent"
        border.color: "#2ecc71"; border.width: 2; opacity: 0
        visible: !remoteVideo.visible && (callManager.state === CallManager.Outgoing || callManager.state === CallManager.Incoming)
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
            visible: !remoteVideo.visible

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
            visible: !remoteVideo.visible
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            visible: !remoteVideo.visible
            text: {
                switch (callManager.state) {
                case CallManager.Outgoing: return "Calling..."
                case CallManager.Incoming: return "Incoming call..."
                case CallManager.Connecting: return "Connecting..."
                case CallManager.Active: return callWindow.formatDuration(callManager.callDuration)
                default: return ""
                }
            }
            font.pixelSize: 13
            color: callManager.state === CallManager.Active ? "#2ecc71" : "#aaaacc"
        }

        // Audio waveform — scrolling bars drawn on Canvas, last ~8 seconds
        Canvas {
            id: waveCanvas
            Layout.alignment: Qt.AlignHCenter
            width: 280; height: 54
            visible: !remoteVideo.visible && callManager.state === CallManager.Active

            property var samples: []
            property int maxBars: 80
            property double lastLevel: 0

            // Continuous scroll — push a sample every 100ms regardless
            Timer {
                id: waveTimer
                interval: 100
                repeat: true
                running: waveCanvas.visible
                onTriggered: {
                    var s = waveCanvas.samples
                    s.push(waveCanvas.lastLevel)
                    waveCanvas.lastLevel = waveCanvas.lastLevel * 0.7  // decay toward silence
                    if (s.length > waveCanvas.maxBars * 2)
                        s = s.slice(-waveCanvas.maxBars)
                    waveCanvas.samples = s
                    waveCanvas.requestPaint()
                }
            }

            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                var midY = height / 2
                var barW = 2
                var gap = 1
                var step = barW + gap

                for (var i = 0; i < maxBars; i++) {
                    var idx = samples.length - maxBars + i
                    var lvl = (idx >= 0 && idx < samples.length) ? samples[idx] : 0
                    var barH = Math.max(1, lvl * midY * 1.8)
                    var x = i * step

                    if (lvl > 0.5) ctx.fillStyle = "#e74c3c"
                    else if (lvl > 0.15) ctx.fillStyle = "#2ecc71"
                    else if (lvl > 0.02) ctx.fillStyle = "#3a6a8e"
                    else ctx.fillStyle = "#1e1e3e"

                    // Draw bar centered vertically
                    ctx.fillRect(x, midY - barH, barW, barH * 2)
                }
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

    // Control bar overlay
    Rectangle {
        id: controlBar
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 100
        color: remoteVideo.visible ? "#80000000" : "transparent"
        z: 5

        property bool controlBarVisible: true
        opacity: controlBarVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 300 } }

        RowLayout {
            anchors.centerIn: parent
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

            // Camera toggle
            RoundButton {
                implicitWidth: 50; implicitHeight: 50
                visible: callManager.state === CallManager.Connecting || callManager.state === CallManager.Active
                onClicked: callManager.toggleCamera()
                contentItem: Label {
                    text: callManager.isCameraOn ? "\uD83D\uDCF9" : "\uD83D\uDCF9"
                    font.pixelSize: 22
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                    opacity: callManager.isCameraOn ? 1.0 : 0.5
                }
                background: Rectangle {
                    radius: 25
                    color: callManager.isCameraOn ? "#2ecc71" : "#3a3a5e"
                    border.color: callManager.isCameraOn ? "#2ecc71" : "#5a5a8e"; border.width: 1
                }
                ToolTip.visible: hovered; ToolTip.text: callManager.isCameraOn ? "Camera off" : "Camera on"
            }

            // Mute (with mic level fill)
            RoundButton {
                implicitWidth: 50; implicitHeight: 50
                visible: callManager.state === CallManager.Connecting || callManager.state === CallManager.Active
                onClicked: callManager.toggleMute()
                contentItem: Image {
                    source: callManager.isMuted ? "qrc:/icons/mic-off.svg" : "qrc:/icons/mic.svg"
                    sourceSize: Qt.size(22, 22); anchors.centerIn: parent; z: 2
                }
                background: Rectangle {
                    radius: 25
                    color: callManager.isMuted ? "#e74c3c" : "#3a3a5e"
                    border.color: callManager.isMuted ? "#e74c3c" : "#5a5a8e"; border.width: 1
                    clip: true

                    // Green level fill from bottom (clipped to circle)
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: callManager.isMuted ? 0 : parent.height * callManager.audioLevel
                        color: "#2ecc71"
                        opacity: 0.4
                        Behavior on height { NumberAnimation { duration: 80 } }
                    }
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
    }

    onClosing: function(close) { callManager.hangUp() }
}
