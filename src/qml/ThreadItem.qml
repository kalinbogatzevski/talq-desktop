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
    required property int unreadCount
    required property bool isAllMessages

    property bool selected: false

    readonly property color threadColor: Theme.topicColor(iconColor)

    background: Rectangle {
        color: {
            if (selected) {
                var c = Qt.color(threadItem.threadColor)
                return Qt.rgba(c.r, c.g, c.b, 0.1)
            }
            return threadItem.hovered ? Theme.bgHover : "transparent"
        }
        Behavior on color { ColorAnimation { duration: Theme.animFast } }

        // Selection indicator bar
        Rectangle {
            width: 3
            height: parent.height
            anchors.left: parent.left
            color: threadItem.threadColor
            visible: selected
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
            color: threadItem.threadColor
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

                // Unread count badge
                TqBadge {
                    count: unreadCount
                    visible: unreadCount > 0 && !selected
                    badgeColor: threadItem.threadColor

                    scale: visible ? 1 : 0
                    Behavior on scale { NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutBack } }
                }
            }
        }
    }
}
