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
    signal threadOpenRequested(int threadId)

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

        // Action bar (hover: right side for others, left of bubble for own)
        Row {
            visible: msgHover.hovered && messageId > 0 && sendStatus !== "sending" && sendStatus !== "failed"
            z: 10
            spacing: 2
            x: isOwnMessage
                ? (ownBubble.x - width - 6)
                : (otherMsg.x + otherMsg.width + 6)
            y: {
                var targetY = isOwnMessage
                    ? (ownBubble.y + (ownBubble.height - height) / 2)
                    : (otherMsg.y + (otherMsg.height - height) / 2)
                return Math.max(0, Math.min(targetY, parent.height - height))
            }

            ToolButton {
                width: 26; height: 26
                ToolTip.visible: hovered; ToolTip.text: "React"; ToolTip.delay: 300
                onClicked: {
                    var pos = mapToItem(msgContent, 0, height)
                    quickEmojisLoader.open()
                }
                contentItem: Label {
                    text: "\u263A"; font.pixelSize: 13
                    color: parent.hovered ? Theme.accent : Theme.textSecondary
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { radius: 13; color: parent.hovered ? Theme.bgHover : Theme.bgSurface }
            }

            ToolButton {
                width: 26; height: 26
                ToolTip.visible: hovered; ToolTip.text: "Reply"; ToolTip.delay: 300
                onClicked: bubble.replyRequested(messageId, actorName, messageText)
                contentItem: Label {
                    text: "\u21A9"; font.pixelSize: 13
                    color: parent.hovered ? Theme.accent : Theme.textSecondary
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { radius: 13; color: parent.hovered ? Theme.bgHover : Theme.bgSurface }
            }
        }

        // Right-click → unified context popup at cursor
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: function(mouse) {
                msgPopupLoader.openAt(mouse.x, mouse.y - 200)
            }
        }

        TextEdit { id: contextClipHelper; visible: false }

        // Lazy-loaded popups — only created when opened (saves 100s of window handles)
        Loader {
            id: quickEmojisLoader
            active: false
            sourceComponent: quickEmojisComp
            function open() { active = true; item.open() }
        }

        Loader {
            id: msgPopupLoader
            active: false
            sourceComponent: msgPopupComp
            function openAt(x, y) { active = true; item.x = x; item.y = y; item.open() }
        }

        Component {
            id: quickEmojisComp
        Popup {
            id: quickEmojis
            width: quickRow.width + 12
            height: 36
            padding: 4
            background: Rectangle {
                radius: 18
                color: Theme.bgSurface
                border.color: Theme.divider
                border.width: 1
            }

            Row {
                id: quickRow
                anchors.centerIn: parent
                spacing: 2

                Repeater {
                    model: ["\uD83D\uDC4D", "\u2764\uFE0F", "\uD83D\uDE02", "\uD83D\uDE2E", "\uD83D\uDE22", "\uD83C\uDF89"]

                    Rectangle {
                        width: 28; height: 28; radius: 14
                        color: qeMa.containsMouse ? Theme.bgHover : "transparent"

                        Label {
                            anchors.centerIn: parent
                            text: modelData; font.pixelSize: 15
                        }

                        MouseArea {
                            id: qeMa
                            anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                messageModel.addReaction(messageId, modelData)
                                quickEmojis.close()
                            }
                        }
                    }
                }
            }
        }
        } // end Component quickEmojisComp

        Component {
            id: msgPopupComp
        // Unified popup: emoji row + actions
        Popup {
            id: msgPopup
            x: isOwnMessage ? -width - 4 : parent.width + 4
            y: Math.max(0, Math.min(parent.height - height, 0))
            width: popupCol.width + 20
            height: popupCol.height + 16
            padding: 8
            background: Rectangle {
                radius: Theme.radiusNormal
                color: Theme.bgSurface
                border.color: Theme.divider
                border.width: 1
            }

            ColumnLayout {
                id: popupCol
                spacing: 6

                // Emoji quick-react row
                Row {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 2
                    visible: messageId > 0

                    Repeater {
                        model: ["\uD83D\uDC4D", "\u2764\uFE0F", "\uD83D\uDE02", "\uD83D\uDE2E", "\uD83D\uDE22", "\uD83C\uDF89", "\uD83D\uDC4F", "\uD83D\uDE4F"]

                        Rectangle {
                            width: 32; height: 32; radius: 8
                            color: emojiMa.containsMouse ? Theme.bgHover : "transparent"

                            Label {
                                anchors.centerIn: parent
                                text: modelData
                                font.pixelSize: 17
                            }

                            MouseArea {
                                id: emojiMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    messageModel.addReaction(messageId, modelData)
                                    msgPopup.close()
                                }
                            }
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider; visible: messageId > 0 }

                // Action buttons
                Repeater {
                    model: [
                        { icon: "\u21A9", label: "Reply", action: "reply", ownerOnly: false },
                        { icon: "\uD83D\uDCCB", label: "Copy", action: "copy", ownerOnly: false },
                        { icon: "\u21AA", label: "Forward", action: "forward", ownerOnly: false },
                        { icon: "\uD83D\uDCCC", label: "Pin", action: "pin", ownerOnly: false },
                        { icon: "\uD83D\uDD17", label: "Copy message link", action: "copylink", ownerOnly: false },
                        { icon: "\u2709", label: "Mark as unread", action: "unread", ownerOnly: false },
                        { icon: "\uD83D\uDCDD", label: "Note to self", action: "notetoself", ownerOnly: false },
                        { icon: "\u23F0", label: "Set reminder", action: "reminder", ownerOnly: false },
                        { icon: "\uD83D\uDCAC", label: "Reply in thread", action: "thread", ownerOnly: false },
                        { icon: "\uD83D\uDDD1", label: "Delete", action: "delete", ownerOnly: true }
                    ]

                    Rectangle {
                        Layout.fillWidth: true
                        width: 200; height: 32; radius: Theme.radiusSmall
                        color: actionMa.containsMouse ? Theme.bgHover : "transparent"
                        visible: !modelData.ownerOnly || isOwnMessage

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            spacing: 8

                            Label {
                                text: modelData.icon
                                font.pixelSize: 14
                                color: modelData.action === "delete" ? Theme.danger : Theme.textSecondary
                            }
                            Label {
                                text: modelData.label
                                font.pixelSize: Theme.fontSizeSmall
                                color: modelData.action === "delete" ? Theme.danger : Theme.textPrimary
                                Layout.fillWidth: true
                            }
                        }

                        MouseArea {
                            id: actionMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                msgPopup.close()
                                if (modelData.action === "reply") {
                                    bubble.replyRequested(messageId, actorName, messageText)
                                } else if (modelData.action === "copy") {
                                    var plain = messageText.replace(/<[^>]*>/g, "")
                                    contextClipHelper.text = plain
                                    contextClipHelper.selectAll()
                                    contextClipHelper.copy()
                                } else if (modelData.action === "thread") {
                                    // Signal to ChatView to open this message as a thread
                                    bubble.threadOpenRequested(messageId)
                                } else if (modelData.action === "delete") {
                                    messageModel.deleteMessage(messageId)
                                } else if (modelData.action === "pin") {
                                    messageModel.pinMessage(messageId)
                                } else if (modelData.action === "copylink") {
                                    var link = messageModel.messageLink(messageId)
                                    contextClipHelper.text = link
                                    contextClipHelper.selectAll()
                                    contextClipHelper.copy()
                                }
                            }
                        }
                    }
                }
            }
        }
        } // end Component msgPopupComp

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

            // Message content (flat — subtle card when reply)
            ColumnLayout {
                id: otherMsgCol
                property real maxWidth: bubble.width * 0.75 - Theme.avatarSizeSmall - 20
                width: replyToText.length > 0
                    ? Math.min(Math.max(implicitWidth, 280), maxWidth)
                    : Math.min(implicitWidth, maxWidth)
                spacing: 2

                // Background card for messages with replies
                Rectangle {
                    visible: replyToText.length > 0
                    anchors.fill: parent
                    anchors.margins: -8
                    radius: Theme.radiusNormal
                    color: Theme.darkMode ? Qt.rgba(1, 1, 1, 0.04) : Qt.rgba(0, 0, 0, 0.04)
                    z: -1
                }

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
                    Layout.fillWidth: true
                    text: messageText
                    font.pixelSize: Theme.fontSizeNormal
                    color: Theme.textPrimary
                    wrapMode: Text.Wrap
                    textFormat: messageText.indexOf("<b") >= 0 ? Text.RichText : Text.PlainText
                }

                // Reactions
                Flow {
                    visible: reactions.length > 0
                    Layout.fillWidth: true
                    spacing: 4

                    Repeater {
                        model: reactions.length > 0 ? reactions.split("  ") : []

                        Rectangle {
                            width: rxLabel.implicitWidth + 14
                            height: 22
                            radius: 11
                            color: rxMa.containsMouse
                                ? (Theme.darkMode ? Qt.rgba(1,1,1,0.14) : Qt.rgba(0,0,0,0.10))
                                : (Theme.darkMode ? Qt.rgba(1,1,1,0.07) : Qt.rgba(0,0,0,0.05))

                            Label {
                                id: rxLabel
                                anchors.centerIn: parent
                                text: modelData
                                font.pixelSize: Theme.fontSizeTiny
                            }

                            MouseArea {
                                id: rxMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    var emoji = modelData.split(" ")[0]
                                    messageModel.addReaction(messageId, emoji)
                                }
                            }
                        }
                    }
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

                // Message text
                Label {
                    Layout.fillWidth: true
                    text: messageText
                    font.pixelSize: Theme.fontSizeNormal
                    color: Theme.textPrimary
                    wrapMode: Text.Wrap
                    textFormat: messageText.indexOf("<b") >= 0 ? Text.RichText : Text.PlainText
                }

                // Reactions
                Flow {
                    visible: reactions.length > 0
                    Layout.fillWidth: true
                    spacing: 4

                    Repeater {
                        model: reactions.length > 0 ? reactions.split("  ") : []

                        Rectangle {
                            width: ownRxLabel.implicitWidth + 14
                            height: 22
                            radius: 11
                            color: ownRxMa.containsMouse
                                ? (Theme.darkMode ? Qt.rgba(1,1,1,0.14) : Qt.rgba(0,0,0,0.10))
                                : (Theme.darkMode ? Qt.rgba(1,1,1,0.07) : Qt.rgba(0,0,0,0.05))

                            Label {
                                id: ownRxLabel
                                anchors.centerIn: parent
                                text: modelData
                                font.pixelSize: Theme.fontSizeTiny
                            }

                            MouseArea {
                                id: ownRxMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    var emoji = modelData.split(" ")[0]
                                    messageModel.addReaction(messageId, emoji)
                                }
                            }
                        }
                    }
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
