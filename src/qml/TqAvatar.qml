import QtQuick
import TalkQt

Rectangle {
    id: root

    property string userId: ""
    property string displayName: ""
    property int size: Theme.avatarSize
    property bool showStatus: false
    property string status: ""  // "online", "away", "dnd", "offline"

    width: size
    height: size
    radius: size / 2
    color: Theme.topicColor(Theme.stringHash(displayName || userId))
    clip: true

    Image {
        id: avatarImg
        anchors.fill: parent
        source: root.userId.length > 0 ? "image://avatar/" + root.userId : ""
        sourceSize: Qt.size(root.size, root.size)
        fillMode: Image.PreserveAspectCrop
        visible: status === Image.Ready
    }

    Text {
        anchors.centerIn: parent
        text: root.displayName.length > 0 ? root.displayName.charAt(0).toUpperCase() : "?"
        font.pixelSize: Math.round(root.size * 0.42)
        font.weight: Font.DemiBold
        color: "white"
        visible: avatarImg.status !== Image.Ready
    }

    // Status dot
    Rectangle {
        visible: root.showStatus && root.status.length > 0
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: Theme.statusDotSize
        height: Theme.statusDotSize
        radius: Theme.statusDotSize / 2
        color: root.status === "online" ? Theme.success
             : root.status === "away" ? Theme.warning
             : root.status === "dnd" ? Theme.danger
             : Theme.textMuted
        border.color: Theme.bgSecondary
        border.width: Theme.borderWidthThick
    }
}
