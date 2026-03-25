import QtQuick
import TalkQt

Rectangle {
    id: root

    property string userId: ""
    property string displayName: ""
    property string token: ""     // conversation token — for group chat avatars
    property int size: Theme.avatarSize
    property bool showStatus: false
    property string status: ""  // "online", "away", "dnd", "offline"

    width: size
    height: size
    radius: size / 2
    color: Theme.topicColor(Theme.stringHash(displayName || userId || token))
    clip: true

    Image {
        id: avatarImg
        anchors.fill: parent
        source: {
            if (root.userId.length > 0)
                return "image://avatar/" + root.userId
            if (root.token.length > 0)
                return "image://avatar/room/" + root.token
            return ""
        }
        sourceSize: Qt.size(root.size, root.size)
        fillMode: Image.PreserveAspectCrop
        visible: status === Image.Ready
    }

    Text {
        anchors.centerIn: parent
        text: root.displayName.length > 0 ? root.displayName.charAt(0).toUpperCase() : "?"
        font.pixelSize: Math.round(root.size * 0.45)
        font.weight: Font.Bold
        color: "#ffffff"
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
