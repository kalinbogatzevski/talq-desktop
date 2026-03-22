import QtQuick
import TalkQt

Rectangle {
    id: root

    property int count: 0
    property color badgeColor: Theme.unreadBadge
    property color textColor: "#000000"

    visible: count > 0
    height: Theme.badgeHeight
    width: Math.max(Theme.badgeHeight, badgeText.implicitWidth + 10)
    radius: Theme.badgeHeight / 2
    color: badgeColor

    Text {
        id: badgeText
        anchors.centerIn: parent
        text: root.count > 99 ? "99+" : root.count.toString()
        font.pixelSize: Theme.badgeFontSize
        font.weight: Font.Bold
        color: root.textColor
    }
}
