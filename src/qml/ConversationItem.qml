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

    leftPadding: squeezed ? 0 : Theme.spacingLarge
    rightPadding: squeezed ? 0 : Theme.spacingNormal
    topPadding: squeezed ? 0 : Theme.spacingSmall
    bottomPadding: squeezed ? 0 : Theme.spacingSmall

    // Shared avatar source — single image load for both expanded and squeezed views
    readonly property string avatarSource: (conversationType === 1 && participantUserId.length > 0)
        ? "image://avatar/" + participantUserId : ""
    readonly property color avatarFallbackColor: {
        var colors = ["#5eb5f7", "#e17076", "#faa05a", "#7bc862", "#a695e7", "#ee7aae", "#6ec9cb", "#65aadd"]
        var hash = 0
        for (var i = 0; i < displayName.length; i++) {
            hash = ((hash << 5) - hash) + displayName.charCodeAt(i)
            hash = hash & hash
        }
        return colors[Math.abs(hash) % colors.length]
    }
    readonly property string avatarLetter: {
        if (conversationType === 6) return "\uD83D\uDCDD"
        return displayName.length > 0 ? displayName[0].toUpperCase() : "?"
    }

    contentItem: Item {
        // Normal (expanded) view
        RowLayout {
            anchors.fill: parent
            spacing: Theme.spacingNormal
            visible: !convItem.squeezed
            opacity: convItem.squeezed ? 0 : 1
            Behavior on opacity { NumberAnimation { duration: Theme.animFast } }

            // Avatar
            Item {
                width: Theme.avatarSize
                height: Theme.avatarSize

                // Real avatar (pre-cropped to circle by AvatarProvider)
                Image {
                    id: avatarImg
                    anchors.fill: parent
                    source: convItem.avatarSource
                    sourceSize: Qt.size(Theme.avatarSize, Theme.avatarSize)
                    visible: status === Image.Ready
                    fillMode: Image.PreserveAspectFit
                }

                // Fallback colored circle
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.avatarSize / 2
                    visible: avatarImg.status !== Image.Ready
                    color: convItem.avatarFallbackColor

                    Label {
                        anchors.centerIn: parent
                        text: convItem.avatarLetter
                        font.pixelSize: conversationType === 6 ? 20 : 18
                        font.weight: Font.DemiBold
                        color: "white"
                    }
                }

                // Online status dot (1:1 chats only)
                Rectangle {
                    visible: conversationType === 1 && userStatus.length > 0 && userStatus !== "offline"
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    width: 14; height: 14; radius: 7
                    color: userStatus === "online" ? Theme.online
                         : userStatus === "away" ? Theme.warning
                         : userStatus === "dnd" ? Theme.danger
                         : "transparent"
                    border.color: Theme.bgSidebar
                    border.width: 2
                }

                // Group badge
                Rectangle {
                    visible: conversationType === 2 || conversationType === 3
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    width: 16
                    height: 16
                    radius: 8
                    color: Theme.bgSidebar
                    border.color: Theme.bgSidebar
                    border.width: 2
                    Label {
                        anchors.centerIn: parent
                        text: conversationType === 3 ? "\uD83C\uDF10" : "\uD83D\uDC65"
                        font.pixelSize: 8
                    }
                }
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
                    Rectangle {
                        visible: unreadCount > 0
                        width: Math.max(20, unreadLabel.implicitWidth + 10)
                        height: 20
                        radius: 10
                        color: unreadMention ? Theme.accent : Theme.unreadBadge

                        Label {
                            id: unreadLabel
                            anchors.centerIn: parent
                            text: unreadCount > 99 ? "99+" : unreadCount
                            font.pixelSize: 10
                            font.weight: Font.Bold
                            color: "white"
                        }

                        scale: visible ? 1 : 0
                        Behavior on scale { NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutBack } }
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

            // Reuse the expanded avatar (same source, scaled down)
            Image {
                anchors.fill: parent
                source: convItem.avatarSource
                sourceSize: Qt.size(Theme.avatarSize, Theme.avatarSize)  // same sourceSize = same cache slot
                visible: status === Image.Ready
                fillMode: Image.PreserveAspectFit
            }

            // Fallback colored circle
            Rectangle {
                anchors.fill: parent
                radius: 20
                visible: convItem.avatarSource.length === 0 || avatarImg.status !== Image.Ready
                color: convItem.avatarFallbackColor

                Label {
                    anchors.centerIn: parent
                    text: convItem.avatarLetter
                    font.pixelSize: conversationType === 6 ? 18 : 16
                    font.weight: Font.DemiBold
                    color: "white"
                }
            }

            // Unread badge overlay
            Rectangle {
                visible: unreadCount > 0
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: -2
                anchors.rightMargin: -2
                width: 16
                height: 16
                radius: 8
                color: unreadMention ? Theme.danger : Theme.accent

                Label {
                    anchors.centerIn: parent
                    text: unreadCount > 9 ? "9+" : unreadCount
                    font.pixelSize: 8
                    font.weight: Font.Bold
                    color: "white"
                }
            }
        }
    }
}
