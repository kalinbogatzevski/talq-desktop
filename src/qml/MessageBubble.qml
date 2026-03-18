import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: bubble
    height: dateSep.height + (isSystem ? systemMsg.height + 6 : bubbleContent.height + (isGrouped ? 1 : 6))

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
    required property bool showDateSeparator
    required property string dateString
    required property bool isRead

    property bool isOwnMessage: false

    // Date separator
    Rectangle {
        id: dateSep
        visible: showDateSeparator
        width: parent.width
        height: visible ? 36 : 0

        color: "transparent"

        Rectangle {
            anchors.centerIn: parent
            width: dateLabel.implicitWidth + Theme.spacingXLarge
            height: 24
            radius: 12
            color: Theme.bgSurface

            Label {
                id: dateLabel
                anchors.centerIn: parent
                text: dateString
                font.pixelSize: Theme.fontSizeTiny
                font.weight: Font.DemiBold
                color: Theme.textSecondary
            }
        }
    }

    // System messages
    Item {
        id: systemMsg
        visible: isSystem
        anchors.top: dateSep.bottom
        width: parent.width
        height: visible ? systemLabel.implicitHeight + 16 : 0

        Label {
            id: systemLabel
            anchors.centerIn: parent
            text: messageText
            font.pixelSize: Theme.fontSizeTiny
            color: Theme.systemMsg
        }
    }

    // Regular message
    Item {
        id: bubbleContent
        visible: !isSystem
        anchors.top: dateSep.bottom
        width: parent.width
        height: bubbleRect.height

        Rectangle {
            id: bubbleRect
            width: Math.max(msgCol.implicitWidth + 28, timeLabel.implicitWidth + 28)
            height: msgCol.implicitHeight + 14

            // Clamp between reasonable min/max
            Component.onCompleted: {
                width = Qt.binding(function() {
                    var natural = Math.max(msgCol.implicitWidth + 28, timeLabel.implicitWidth + 40)
                    return Math.min(Math.max(natural, 80), bubble.width * 0.85)
                })
            }

            radius: Theme.radiusNormal
            color: isOwnMessage ? Theme.bgMessageOwn : Theme.bgMessage

            anchors {
                left: isOwnMessage ? undefined : parent.left
                right: isOwnMessage ? parent.right : undefined
                leftMargin: isOwnMessage ? 0 : Theme.spacingNormal
                rightMargin: isOwnMessage ? Theme.spacingNormal : 0
            }

            ColumnLayout {
                id: msgCol
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: 7
                    leftMargin: Theme.spacingNormal
                    rightMargin: Theme.spacingNormal
                }
                spacing: 2

                // Author
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

                // Reply
                Rectangle {
                    visible: replyToText.length > 0
                    Layout.fillWidth: true
                    height: replyCol.implicitHeight + 8
                    radius: Theme.radiusSmall
                    color: Qt.rgba(1, 1, 1, 0.05)

                    Rectangle {
                        width: 3
                        height: parent.height
                        radius: 1.5
                        color: Theme.accent
                    }

                    ColumnLayout {
                        id: replyCol
                        anchors {
                            left: parent.left; right: parent.right; top: parent.top
                            leftMargin: 10; rightMargin: 8; topMargin: 4
                        }
                        spacing: 1

                        Label {
                            text: replyToAuthor
                            font.pixelSize: Theme.fontSizeTiny
                            font.weight: Font.DemiBold
                            color: Theme.accent
                        }
                        Label {
                            Layout.fillWidth: true
                            text: replyToText
                            font.pixelSize: Theme.fontSizeTiny
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

                // Reactions
                Label {
                    visible: reactions.length > 0
                    text: reactions
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textSecondary
                }

                // Timestamp + sent indicator
                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    spacing: 4

                    Label {
                        id: timeLabel
                        text: timeString
                        font.pixelSize: 10
                        color: isOwnMessage ? Qt.rgba(1, 1, 1, 0.45) : Theme.textTime
                    }

                    // Delivery status for own messages
                    Label {
                        visible: isOwnMessage
                        text: isRead ? "✓✓" : "✓"
                        font.pixelSize: 10
                        font.letterSpacing: -2
                        color: isRead ? Theme.accent : Qt.rgba(1, 1, 1, 0.45)
                    }
                }
            }
        }
    }
}
