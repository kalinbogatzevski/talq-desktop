import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

/**
 * Telegram-style message bubble.
 * Own messages on the right (blue), others on the left (dark).
 */
Item {
    id: bubble
    height: bubbleContent.height + (isGrouped ? 2 : 8)

    required property int messageId
    required property string actorName
    required property string actorId
    required property string messageText
    required property real timestamp
    required property bool isSystem
    required property string messageType
    required property bool isGrouped
    required property string replyToText
    required property string replyToAuthor
    required property string reactions
    required property string timeString

    property bool isOwnMessage: false

    // System messages (user joined, etc.)
    Rectangle {
        visible: isSystem
        anchors.horizontalCenter: parent.horizontalCenter
        width: systemLabel.implicitWidth + 24
        height: systemLabel.implicitHeight + 12
        radius: Theme.radiusSmall
        color: "transparent"

        Label {
            id: systemLabel
            anchors.centerIn: parent
            text: messageText
            font.pixelSize: Theme.fontSizeSmall
            font.italic: true
            color: Theme.systemMsg
        }
    }

    // Regular message bubble
    Item {
        id: bubbleContent
        visible: !isSystem
        width: parent.width
        height: bubbleRect.height

        Rectangle {
            id: bubbleRect
            width: Math.min(messageColumn.implicitWidth + 28, bubble.width * 0.75)
            height: messageColumn.implicitHeight + 16
            radius: Theme.radiusNormal
            color: isOwnMessage ? Theme.bgMessageOwn : Theme.bgMessage

            // Position: own messages right-aligned, others left-aligned
            anchors {
                left: isOwnMessage ? undefined : parent.left
                right: isOwnMessage ? parent.right : undefined
                leftMargin: isOwnMessage ? 0 : 12
                rightMargin: isOwnMessage ? 12 : 0
            }

            ColumnLayout {
                id: messageColumn
                anchors {
                    fill: parent
                    margins: 8
                    leftMargin: 12
                    rightMargin: 12
                }
                spacing: 4

                // Author name (not shown for own messages or grouped messages)
                Label {
                    visible: !isOwnMessage && !isGrouped
                    text: actorName
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Font.DemiBold
                    color: {
                        var colors = ["#5eb5f7", "#e17076", "#faa05a", "#7bc862", "#a695e7", "#ee7aae"]
                        var hash = 0
                        for (var i = 0; i < actorId.length; i++) {
                            hash = ((hash << 5) - hash) + actorId.charCodeAt(i)
                            hash = hash & hash
                        }
                        return colors[Math.abs(hash) % colors.length]
                    }
                }

                // Reply preview
                Rectangle {
                    visible: replyToText.length > 0
                    Layout.fillWidth: true
                    height: replyColumn.implicitHeight + 8
                    radius: 4
                    color: Qt.rgba(255, 255, 255, 0.06)

                    Rectangle {
                        width: 3
                        height: parent.height
                        radius: 1.5
                        color: Theme.accent
                    }

                    ColumnLayout {
                        id: replyColumn
                        anchors {
                            left: parent.left
                            right: parent.right
                            top: parent.top
                            leftMargin: 10
                            rightMargin: 8
                            topMargin: 4
                        }
                        spacing: 2

                        Label {
                            text: replyToAuthor
                            font.pixelSize: Theme.fontSizeSmall
                            font.weight: Font.DemiBold
                            color: Theme.accent
                        }
                        Label {
                            Layout.fillWidth: true
                            text: replyToText
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.textSecondary
                            elide: Text.ElideRight
                            maximumLineCount: 1
                        }
                    }
                }

                // Message text
                Label {
                    Layout.fillWidth: true
                    text: messageText
                    font.pixelSize: Theme.fontSizeNormal
                    color: Theme.textPrimary
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                }

                // Reactions row
                Label {
                    visible: reactions.length > 0
                    text: reactions
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textSecondary
                }

                // Timestamp (bottom-right, Telegram style)
                Label {
                    Layout.alignment: Qt.AlignRight
                    text: timeString
                    font.pixelSize: 11
                    color: Theme.textTime
                }
            }
        }
    }
}
