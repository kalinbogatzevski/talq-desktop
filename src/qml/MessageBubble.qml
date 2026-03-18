import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: bubble
    implicitHeight: dateSep.height + (isSystem ? systemMsg.height + 6
            : msgContent.height + (isGrouped ? 2 : 8))
    height: implicitHeight

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
    required property string sendStatus

    property bool isOwnMessage: false

    signal replyRequested(int msgId, string author, string text)

    // Author color based on actorId hash
    function authorColor() {
        var colors = ["#2ec4b6", "#e07060", "#f0a050", "#5ec76a", "#9b7cd4", "#e87aae", "#50b8c8", "#5a9ecf"]
        var hash = 0
        for (var i = 0; i < actorId.length; i++) {
            hash = ((hash << 5) - hash) + actorId.charCodeAt(i)
            hash = hash & hash
        }
        return colors[Math.abs(hash) % colors.length]
    }

    // ── Date separator ──
    Rectangle {
        id: dateSep
        visible: showDateSeparator
        width: parent.width
        height: visible ? 40 : 0
        color: "transparent"

        Rectangle {
            anchors.centerIn: parent
            width: dateLabel.implicitWidth + Theme.spacingXLarge
            height: 26
            radius: 13
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

    // ── System message ──
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
            font.italic: true
            color: Theme.systemMsg
        }
    }

    // ── Regular message ──
    Item {
        id: msgContent
        visible: !isSystem
        anchors.top: dateSep.bottom
        width: parent.width
        height: isOwnMessage ? ownBubble.height : otherMsg.height

        HoverHandler {
            id: msgHover
        }

        // Reply button (appears on hover)
        ToolButton {
            visible: msgHover.hovered && messageId > 0 && sendStatus !== "sending" && sendStatus !== "failed"
            z: 10
            width: 28; height: 28
            anchors.right: isOwnMessage ? ownBubble.left : undefined
            anchors.left: isOwnMessage ? undefined : (otherMsg.visible ? otherMsgCol.right : undefined)
            anchors.top: parent.top
            anchors.margins: 2
            ToolTip.visible: hovered
            ToolTip.text: "Reply"
            ToolTip.delay: 300

            onClicked: bubble.replyRequested(messageId, actorName, messageText)

            contentItem: Label {
                text: "\u21A9"  // ↩ reply arrow
                font.pixelSize: 14
                color: parent.hovered ? Theme.accent : Theme.textSecondary
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 14
                color: parent.hovered ? Theme.bgHover : Theme.bgSurface
                border.color: Theme.divider
                border.width: 1
            }
        }

        // ═══════════════════════════════════════
        // OTHER PEOPLE'S MESSAGES — flat, no bubble
        // ═══════════════════════════════════════
        Row {
            id: otherMsg
            visible: !isOwnMessage
            spacing: Theme.spacingSmall
            anchors.left: parent.left
            anchors.leftMargin: Theme.spacingNormal

            // Avatar
            Item {
                width: Theme.avatarSizeSmall
                height: Theme.avatarSizeSmall
                anchors.top: parent.top
                anchors.topMargin: isGrouped ? 0 : 2
                opacity: isGrouped ? 0 : 1

                Image {
                    id: otherAvatarImg
                    anchors.fill: parent
                    source: !isGrouped && actorId.length > 0
                        ? "image://avatar/" + actorId : ""
                    sourceSize: Qt.size(Theme.avatarSizeSmall, Theme.avatarSizeSmall)
                    visible: status === Image.Ready
                    fillMode: Image.PreserveAspectFit
                }

                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    visible: !isGrouped && otherAvatarImg.status !== Image.Ready
                    color: authorColor()

                    Label {
                        anchors.centerIn: parent
                        text: actorName.length > 0 ? actorName[0].toUpperCase() : "?"
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        color: "white"
                    }
                }
            }

            // Message content (flat — no bubble)
            ColumnLayout {
                id: otherMsgCol
                width: Math.min(implicitWidth, bubble.width * 0.75 - Theme.avatarSizeSmall - 20)
                spacing: 2

                // Name + time row
                RowLayout {
                    visible: !isGrouped
                    spacing: Theme.spacingSmall

                    Label {
                        text: actorName
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: Font.DemiBold
                        color: authorColor()
                    }

                    Label {
                        text: timeString
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.textTime
                    }
                }

                // Reply quote
                Rectangle {
                    visible: replyToText.length > 0
                    Layout.fillWidth: true
                    height: otherReplyCol.implicitHeight + 8
                    radius: Theme.radiusSmall
                    color: Theme.darkMode ? Qt.rgba(1, 1, 1, 0.04) : Qt.rgba(0, 0, 0, 0.04)

                    Rectangle {
                        width: 3; height: parent.height
                        radius: 1.5; color: Theme.accent
                    }

                    ColumnLayout {
                        id: otherReplyCol
                        anchors { left: parent.left; right: parent.right; top: parent.top
                                  leftMargin: 10; rightMargin: 8; topMargin: 4 }
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

                // Message text with right-click copy
                Label {
                    id: msgLabel
                    Layout.fillWidth: true
                    text: messageText
                    font.pixelSize: Theme.fontSizeNormal
                    color: Theme.textPrimary
                    wrapMode: Text.Wrap
                    textFormat: messageText.indexOf("<b") >= 0 ? Text.RichText : Text.PlainText

                    // Hidden helper for clipboard
                    TextEdit {
                        id: clipHelper
                        visible: false
                    }

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.RightButton
                        onClicked: msgContextMenu.popup()
                    }

                    Menu {
                        id: msgContextMenu
                        MenuItem {
                            text: "Copy message"
                            onTriggered: {
                                var plain = messageText.replace(/<[^>]*>/g, "")
                                clipHelper.text = plain
                                clipHelper.selectAll()
                                clipHelper.copy()
                            }
                        }
                    }
                }

                // Reactions
                Label {
                    visible: reactions.length > 0
                    text: reactions
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textSecondary
                }

                // Timestamp for grouped messages (no name row)
                Label {
                    visible: isGrouped
                    text: timeString
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.textTime
                }
            }
        }

        // ═══════════════════════════════════════
        // OWN MESSAGES — teal-tinted bubble, right-aligned
        // ═══════════════════════════════════════
        Rectangle {
            id: ownBubble
            visible: isOwnMessage
            anchors.right: parent.right
            anchors.rightMargin: Theme.spacingNormal

            width: Math.min(Math.max(ownCol.implicitWidth + 28, 80), bubble.width * 0.75)
            height: ownCol.implicitHeight + 14
            radius: Theme.radiusNormal
            color: Theme.bgMessageOwn

            ColumnLayout {
                id: ownCol
                anchors {
                    left: parent.left; right: parent.right; top: parent.top
                    margins: 7
                    leftMargin: Theme.spacingNormal
                    rightMargin: Theme.spacingNormal
                }
                spacing: 2

                // Reply quote
                Rectangle {
                    visible: replyToText.length > 0
                    Layout.fillWidth: true
                    height: ownReplyCol.implicitHeight + 8
                    radius: Theme.radiusSmall
                    color: Qt.rgba(1, 1, 1, 0.06)

                    Rectangle {
                        width: 3; height: parent.height
                        radius: 1.5; color: Theme.accent
                    }

                    ColumnLayout {
                        id: ownReplyCol
                        anchors { left: parent.left; right: parent.right; top: parent.top
                                  leftMargin: 10; rightMargin: 8; topMargin: 4 }
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
                            color: Theme.darkMode ? Qt.rgba(1, 1, 1, 0.6) : Theme.textSecondary
                            elide: Text.ElideRight
                            maximumLineCount: 1
                        }
                    }
                }

                // Message text with right-click copy
                Label {
                    id: ownMsgLabel
                    Layout.fillWidth: true
                    text: messageText
                    font.pixelSize: Theme.fontSizeNormal
                    color: Theme.textPrimary
                    wrapMode: Text.Wrap
                    textFormat: messageText.indexOf("<b") >= 0 ? Text.RichText : Text.PlainText

                    TextEdit {
                        id: ownClipHelper
                        visible: false
                    }

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.RightButton
                        onClicked: ownContextMenu.popup()
                    }

                    Menu {
                        id: ownContextMenu
                        MenuItem {
                            text: "Copy message"
                            onTriggered: {
                                var plain = messageText.replace(/<[^>]*>/g, "")
                                ownClipHelper.text = plain
                                ownClipHelper.selectAll()
                                ownClipHelper.copy()
                            }
                        }
                    }
                }

                // Reactions
                Label {
                    visible: reactions.length > 0
                    text: reactions
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textSecondary
                }

                // Time + send/read status (inline, right-aligned)
                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    spacing: 4

                    Label {
                        text: sendStatus === "sending" ? "Sending..." : timeString
                        font.pixelSize: 10
                        font.italic: sendStatus === "sending"
                        color: sendStatus === "failed" ? Theme.danger
                             : (Theme.darkMode ? Qt.rgba(1, 1, 1, 0.45) : Theme.textTime)
                    }

                    // Send status indicator
                    Label {
                        visible: isOwnMessage && sendStatus !== "sending"
                        text: sendStatus === "failed" ? "\u26A0"  // ⚠ failed
                            : isRead ? "\u25C9" : "\u25CB"        // ◉ read / ○ sent
                        font.pixelSize: sendStatus === "failed" ? 12 : 9
                        color: sendStatus === "failed" ? Theme.danger
                             : isRead ? Theme.accent
                             : (Theme.darkMode ? Qt.rgba(1, 1, 1, 0.45) : Theme.textTime)

                        MouseArea {
                            anchors.fill: parent
                            visible: sendStatus === "failed"
                            cursorShape: Qt.PointingHandCursor
                            onClicked: messageModel.retryMessage(messageId)
                        }
                    }
                }
            }

            // Sending opacity
            opacity: sendStatus === "sending" ? 0.6 : 1.0
            Behavior on opacity { NumberAnimation { duration: Theme.animFast } }
        }
    }
}
