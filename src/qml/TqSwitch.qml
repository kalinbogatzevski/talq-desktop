import QtQuick
import QtQuick.Controls
import TalkQt

Switch {
    id: root

    indicator: Rectangle {
        x: root.leftPadding
        anchors.verticalCenter: parent.verticalCenter
        width: 40
        height: 22
        radius: 11
        color: root.checked ? Theme.accent : Theme.bgSurface
        border.width: Theme.borderWidthNormal
        border.color: root.checked ? Theme.accent : Theme.border

        Rectangle {
            x: root.checked ? parent.width - width - 3 : 3
            anchors.verticalCenter: parent.verticalCenter
            width: 16
            height: 16
            radius: 8
            color: root.checked ? "#ffffff" : Theme.textSecondary
            Behavior on x { NumberAnimation { duration: Theme.animFast } }
        }
    }
}
