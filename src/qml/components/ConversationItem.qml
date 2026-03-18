import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

/**
 * Single conversation row in the sidebar — Telegram style.
 * Shows avatar placeholder, name, last message preview, time, and unread badge.
 */
ItemDelegate {
    id: convItem
    height: visible ? 72 : 0
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

    property bool selected: false
    property string filterText: ""

    // Filter by search text
    visible: filterText.length === 0 || displayName.toLowerCase().includes(filterText.toLowerCase())

    background: Rectangle {
        color: selected ? Theme.bgSelected
             : convItem.hovered ? Theme.bgHover
             : "transparent"
    }

    contentItem: RowLayout {
        spacing: 12
        anchors.leftMargin: 12
        anchors.rightMargin: 12

        // Avatar circle
        Rectangle {
            width: 48
            height: 48
            radius: 24
            color: {
                // Generate consistent color from name
                var colors = ["#5eb5f7", "#e17076", "#faa05a", "#7bc862", "#a695e7", "#ee7aae", "#6ec9cb", "#65aadd"]
                var hash = 0
                for (var i = 0; i < displayName.length; i++) {
                    hash = ((hash << 5) - hash) + displayName.charCodeAt(i)
                    hash = hash & hash
                }
                return colors[Math.abs(hash) % colors.length]
            }

            Label {
                anchors.centerIn: parent
                text: {
                    if (conversationType === 6) return "📝" // Note to self
                    return displayName.length > 0 ? displayName[0].toUpperCase() : "?"
                }
                font.pixelSize: conversationType === 6 ? 22 : 20
                font.weight: Font.DemiBold
                color: "white"
            }

            // Group icon overlay
            Rectangle {
                visible: conversationType === 2 || conversationType === 3
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: 18
                height: 18
                radius: 9
                color: Theme.bgSidebar
                Label {
                    anchors.centerIn: parent
                    text: conversationType === 3 ? "🔓" : "👥"
                    font.pixelSize: 10
                }
            }
        }

        // Text content
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4

            RowLayout {
                Layout.fillWidth: true

                // Favorite star
                Label {
                    visible: isFavorite
                    text: "★"
                    font.pixelSize: 12
                    color: "#faa05a"
                }

                // Conversation name
                Label {
                    Layout.fillWidth: true
                    text: displayName
                    font.pixelSize: Theme.fontSizeNormal
                    font.weight: unreadCount > 0 ? Font.DemiBold : Font.Normal
                    color: Theme.textPrimary
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }

                // Time
                Label {
                    text: {
                        if (lastActivity <= 0) return ""
                        var d = new Date(lastActivity * 1000)
                        var now = new Date()
                        if (d.toDateString() === now.toDateString()) {
                            return d.toLocaleTimeString(Qt.locale(), "HH:mm")
                        }
                        var yesterday = new Date(now)
                        yesterday.setDate(yesterday.getDate() - 1)
                        if (d.toDateString() === yesterday.toDateString()) {
                            return "Yesterday"
                        }
                        return d.toLocaleDateString(Qt.locale(), "dd MMM")
                    }
                    font.pixelSize: Theme.fontSizeSmall
                    color: unreadCount > 0 ? Theme.accent : Theme.textTime
                }
            }

            RowLayout {
                Layout.fillWidth: true

                // Last message preview
                Label {
                    Layout.fillWidth: true
                    text: {
                        if (lastMessage.length === 0) return ""
                        var prefix = lastAuthor.length > 0 ? lastAuthor + ": " : ""
                        return prefix + lastMessage
                    }
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textSecondary
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }

                // Unread badge
                Rectangle {
                    visible: unreadCount > 0
                    width: Math.max(22, unreadLabel.implicitWidth + 12)
                    height: 22
                    radius: 11
                    color: unreadMention ? Theme.accent : Theme.unreadBadge

                    Label {
                        id: unreadLabel
                        anchors.centerIn: parent
                        text: unreadCount > 99 ? "99+" : unreadCount
                        font.pixelSize: 11
                        font.weight: Font.Bold
                        color: "white"
                    }
                }
            }
        }
    }
}
