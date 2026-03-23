import QtQuick
import QtQuick.Controls
import TalkQt

AbstractButton {
    id: root

    property string iconText: ""     // emoji fallback
    property string iconName: ""     // SVG icon name (takes precedence)
    property int size: Theme.buttonSizeMedium
    property int iconSize: Theme.iconSizeMedium
    property color bgColor: "transparent"
    property color hoverColor: Theme.bgHover
    property color iconColor: Theme.textSecondary

    implicitWidth: size
    implicitHeight: size

    HoverHandler { cursorShape: Qt.PointingHandCursor }

    background: Rectangle {
        radius: root.size / 2
        color: root.hovered ? root.hoverColor : root.bgColor
        Behavior on color { ColorAnimation { duration: Theme.animFast } }
    }

    contentItem: Item {
        // SVG icon (preferred)
        TqIcon {
            visible: root.iconName.length > 0
            anchors.centerIn: parent
            name: root.iconName
            size: root.iconSize
            color: root.iconColor
        }

        // Emoji text fallback
        Text {
            visible: root.iconName.length === 0
            anchors.centerIn: parent
            text: root.iconText
            font.pixelSize: root.iconSize
            color: root.iconColor
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
}
