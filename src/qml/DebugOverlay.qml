import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import TalkQt

// Toggle with Ctrl+D — shows real-time memory & cache stats
Rectangle {
    id: overlay
    visible: debugMonitor.visible
    anchors.right: parent.right
    anchors.top: parent.top
    anchors.margins: 8
    width: 260
    height: col.implicitHeight + 16
    radius: 6
    color: Qt.rgba(0, 0, 0, 0.85)
    z: 9999

    ColumnLayout {
        id: col
        anchors.fill: parent
        anchors.margins: 8
        spacing: 2

        Label {
            text: "DEBUG MONITOR"
            font.pixelSize: 11
            font.weight: Font.Bold
            color: "#ff6b6b"
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: "#444" }

        Label {
            text: "Memory: " + debugMonitor.memoryMB + " MB"
            font.pixelSize: 11; font.family: "Consolas"
            color: debugMonitor.memoryMB > 500 ? "#ff6b6b" : "#6bff6b"
        }
        Label {
            text: "Messages: " + debugMonitor.messageCount
            font.pixelSize: 11; font.family: "Consolas"; color: "#ccc"
        }
        Label {
            text: "Avatars cached: " + debugMonitor.avatarCacheCount
            font.pixelSize: 11; font.family: "Consolas"; color: "#ccc"
        }
        Label {
            text: "Previews cached: " + debugMonitor.previewCacheCount +
                  " (" + Math.round(debugMonitor.previewCacheBytes / 1024) + " KB)"
            font.pixelSize: 11; font.family: "Consolas"; color: "#ccc"
        }
        Label {
            text: "Pending requests: " + debugMonitor.pendingRequests
            font.pixelSize: 11; font.family: "Consolas"
            color: debugMonitor.pendingRequests > 5 ? "#ffaa6b" : "#ccc"
        }
        Label {
            text: "Conversations: " + debugMonitor.conversationCount
            font.pixelSize: 11; font.family: "Consolas"; color: "#ccc"
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: "#444" }

        // Scrollable log
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 120

            TextArea {
                readOnly: true
                text: debugMonitor.log
                font.pixelSize: 9
                font.family: "Consolas"
                color: "#999"
                background: null
                wrapMode: TextEdit.Wrap
            }
        }
    }

    // Close button
    Label {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 6
        text: "\u2715"
        font.pixelSize: 12
        color: "#888"
        MouseArea {
            anchors.fill: parent
            anchors.margins: -4
            cursorShape: Qt.PointingHandCursor
            onClicked: debugMonitor.visible = false
        }
    }
}
