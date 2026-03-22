import QtQuick
import QtQuick.Controls
import TalkQt

ComboBox {
    id: root

    Layout.fillWidth: true

    background: Rectangle {
        radius: Theme.radiusSmall
        color: Theme.bgSurface
        border.width: Theme.borderWidthNormal
        border.color: Theme.border
    }

    contentItem: Text {
        leftPadding: Theme.spacingNormal
        rightPadding: 30
        text: root.displayText
        color: root.enabled ? Theme.textPrimary : Theme.textMuted
        font.pixelSize: Theme.fontSizeSmall
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Text {
        x: root.width - width - Theme.spacingNormal
        anchors.verticalCenter: parent.verticalCenter
        text: "\u25BE"
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSizeSmall
    }

    delegate: ItemDelegate {
        required property int index
        required property string modelData
        width: root.width
        highlighted: root.highlightedIndex === index
        contentItem: Text {
            text: modelData
            color: highlighted ? "#000000" : Theme.textPrimary
            font.pixelSize: Theme.fontSizeSmall
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: highlighted ? Theme.accent : Theme.bgSurface
        }
    }

    popup: Popup {
        y: root.height
        width: root.width
        implicitHeight: contentItem.implicitHeight + 2
        padding: 1
        background: Rectangle {
            color: Theme.bgSurface
            border.width: Theme.borderWidthNormal
            border.color: Theme.border
            radius: Theme.radiusSmall
        }
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.popup.visible ? root.delegateModel : null
            currentIndex: root.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
    }
}
