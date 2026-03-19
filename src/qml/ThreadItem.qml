import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

ItemDelegate {
    id: threadItem
    height: 64
    padding: 0

    required property int index
    required property int threadId
    required property string title
    required property string lastMessage
    required property string lastAuthor
    required property real lastActivity
    required property int replyCount
    required property int iconColor

    property bool selected: false

    background: Rectangle {
        color: selected ? Theme.bgSelected
             : threadItem.hovered ? Theme.bgHover
             : "transparent"

        Behavior on color { ColorAnimation { duration: Theme.animFast } }

        // Selection indicator bar
        Rectangle {
            width: 3
            height: parent.height * 0.5
            radius: 2
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.accent
            visible: selected
            opacity: selected ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.animNormal } }
        }
    }

    leftPadding: Theme.spacingLarge
    rightPadding: Theme.spacingNormal
    topPadding: Theme.spacingSmall
    bottomPadding: Theme.spacingSmall

    contentItem: RowLayout {
        spacing: Theme.spacingNormal

        // Colored dot indicator
        Rectangle {
            width: 8
            height: 8
            radius: 4
            color: {
                var colors = ["#2ec4b6", "#e07060", "#f0a050", "#5ec76a", "#9b7cd4", "#e87aae"]
                return colors[Math.abs(iconColor) % colors.length]
            }
            Layout.alignment: Qt.AlignVCenter
        }

        // Text content
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall

                Label {
                    Layout.fillWidth: true
                    text: title
                    font.pixelSize: Theme.fontSizeNormal
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }

                Label {
                    text: {
                        if (lastActivity <= 0) return ""
                        var d = new Date(lastActivity * 1000)
                        var now = new Date()
                        if (d.toDateString() === now.toDateString())
                            return d.toLocaleTimeString(Qt.locale(), "HH:mm")
                        var yesterday = new Date(now)
                        yesterday.setDate(yesterday.getDate() - 1)
                        if (d.toDateString() === yesterday.toDateString())
                            return "Yesterday"
                        return d.toLocaleDateString(Qt.locale(), "dd MMM")
                    }
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.textTime
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall

                Label {
                    Layout.fillWidth: true
                    text: {
                        if (lastMessage.length === 0) return ""
                        var prefix = lastAuthor.length > 0 ? lastAuthor + ": " : ""
                        return prefix + lastMessage
                    }
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textMuted
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }

                // Reply count badge
                Rectangle {
                    visible: replyCount > 0
                    width: Math.max(20, replyLabel.implicitWidth + 10)
                    height: 20
                    radius: 10
                    color: Theme.bgInput

                    Label {
                        id: replyLabel
                        anchors.centerIn: parent
                        text: replyCount > 99 ? "99+" : replyCount
                        font.pixelSize: 10
                        font.weight: Font.Bold
                        color: Theme.textSecondary
                    }

                    scale: visible ? 1 : 0
                    Behavior on scale { NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutBack } }
                }
            }
        }
    }
}
