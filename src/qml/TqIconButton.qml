import QtQuick
import QtQuick.Controls
import TalkQt

AbstractButton {
    id: root

    property string icon: ""
    property int size: Theme.buttonSizeMedium
    property int iconSize: Theme.iconSizeMedium
    property color bgColor: "transparent"
    property color hoverColor: Theme.bgHover
    property color iconColor: Theme.textSecondary

    implicitWidth: size
    implicitHeight: size
    cursorShape: Qt.PointingHandCursor

    background: Rectangle {
        radius: root.size / 2
        color: root.hovered ? root.hoverColor : root.bgColor
        Behavior on color { ColorAnimation { duration: Theme.animFast } }
    }

    contentItem: Text {
        text: root.icon
        font.pixelSize: root.iconSize
        color: root.iconColor
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
