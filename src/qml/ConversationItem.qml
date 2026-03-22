import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

ItemDelegate {
    id: convItem
    height: visible ? (squeezed ? 52 : Theme.conversationHeight) : 0
    padding: 0

    required property int index
    required property string token
    required property string displayName
    required property int conversationType
    required property int unreadCount
    required property bool unreadMention
    required property bool isFavorite
    required property string lastMessage
    required property string lastAuthor
    required property real lastActivity
    required property string participantUserId
    required property string userStatus

    property int notificationLevel: 0  // 0=default, 1=always, 2=mention, 3=never
    property bool selected: false
    property string filterText: ""
    property bool squeezed: false

    visible: filterText.length === 0 || displayName.toLowerCase().includes(filterText.toLowerCase())

    background: Rectangle {
        color: selected ? Theme.bgSelected
             : convItem.hovered ? Theme.bgHover
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

    Menu {
        id: contextMenu
        palette.window: Theme.bgSurface
        palette.text: Theme.textPrimary

        MenuItem {
            text: convItem.notificationLevel === 3 ? "Unmute" : "Mute"
            onTriggered: {
                let newLevel = convItem.notificationLevel === 3 ? 0 : 3
                conversationModel.setNotificationLevel(convItem.index, newLevel)
            }
        }
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: contextMenu.popup()
    }

    leftPadding: squeezed ? 0 : Theme.spacingLarge
    rightPadding: squeezed ? 0 : Theme.spacingNormal
    topPadding: squeezed ? 0 : Theme.spacingSmall
    bottomPadding: squeezed ? 0 : Theme.spacingSmall


    contentItem: Item {
        // Normal (expanded) view
        RowLayout {
            anchors.fill: parent
            spacing: Theme.spacingNormal
            visible: !convItem.squeezed
            opacity: convItem.squeezed ? 0 : 1
            Behavior on opacity { NumberAnimation { duration: Theme.animFast } }

            // Avatar
            TqAvatar {
                userId: participantUserId
                displayName: convItem.displayName
                size: Theme.avatarSize
                showStatus: conversationType === 1
                status: userStatus
            }

            // Text content
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacingSmall

                    Rectangle {
                        visible: isFavorite
                        width: 6; height: 6; radius: 3
                        color: Theme.accent
                    }

                    Label {
                        Layout.fillWidth: true
                        text: displayName
                        font.pixelSize: Theme.fontSizeNormal
                        font.weight: unreadCount > 0 ? Font.DemiBold : Font.Normal
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
                        color: unreadCount > 0 ? Theme.accent : Theme.textTime
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

                    // Unread badge
                    TqBadge {
                        count: unreadCount
                        badgeColor: unreadMention ? Theme.accent : Theme.unreadBadge

                        scale: visible ? 1 : 0
                        Behavior on scale { NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutBack } }
                    }

                    Label {
                        visible: convItem.notificationLevel === 3
                        text: "Muted"
                        color: Theme.textSecondary
                        font.pixelSize: 9
                        opacity: 0.6
                    }
                }
            }
        }

        // Squeezed (icon-only) view
        Item {
            anchors.centerIn: parent
            width: 40
            height: 40
            visible: convItem.squeezed
            opacity: convItem.squeezed ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.animFast } }

            TqAvatar {
                anchors.fill: parent
                userId: participantUserId
                displayName: convItem.displayName
                size: 40
            }

            // Unread badge overlay
            TqBadge {
                count: unreadCount
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: -2
                anchors.rightMargin: -2
                badgeColor: unreadMention ? Theme.danger : Theme.accent
            }
        }
    }
}
