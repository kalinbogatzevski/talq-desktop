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
    required property string fileName
    required property string fileMime
    required property real fileSize
    required property string fileLink
    required property string filePreview
    required property bool hasFile
    required property int fileId

    property bool isOwnMessage: false
    property bool isImage: hasFile && fileMime.startsWith("image/")

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

        // Action bar (hover: right of others, left of own bubble)
        Row {
            id: hoverBar
            visible: msgHover.hovered && messageId > 0 && sendStatus !== "sending" && sendStatus !== "failed"
            z: 10
            spacing: 4
            x: isOwnMessage
                ? (ownBubble.x - width - 8)
                : (otherMsg.x + otherMsg.width + 8)
            y: {
                var targetY = isOwnMessage
                    ? (ownBubble.y + (ownBubble.height - height) / 2)
                    : (otherMsg.y + (otherMsg.height - height) / 2)
                return Math.max(0, Math.min(targetY, parent.height - height))
            }

            // Shared hover button style
            Repeater {
                model: [
                    { icon: "\u263A", tip: "React", isReact: true },
                    { icon: "\u21A9", tip: "Reply", isReact: false }
                ]

                Rectangle {
                    width: 34; height: 34; radius: 17
                    color: btnMa.containsMouse
                        ? (Theme.darkMode ? Qt.rgba(1,1,1,0.15) : Qt.rgba(0,0,0,0.08))
                        : Theme.bgSurface
                    border.color: Theme.divider
                    border.width: 0.5
                    scale: btnMa.containsMouse ? 1.1 : 1.0
                    Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.fill: parent
                        text: modelData.icon
                        font.pixelSize: 18
                        font.bold: true
                        color: btnMa.containsMouse ? Theme.accent : Theme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        Behavior on color { ColorAnimation { duration: 100 } }
                    }

                    ToolTip.visible: btnMa.containsMouse
                    ToolTip.text: modelData.tip
                    ToolTip.delay: 400

                    MouseArea {
                        id: btnMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (modelData.isReact) {
                                var pos = parent.mapToItem(msgContent, 0, 0)
                                var barW = 210
                                var ex = isOwnMessage ? (pos.x - barW - 4) : (pos.x + parent.width + 4)
                                quickEmojisLoader.openAt(ex, pos.y - 5)
                            } else {
                                bubble.replyRequested(messageId, actorName, messageText)
                            }
                        }
                    }
                }
            }
        }

        // Right-click → context popup at cursor, clamped to window bounds
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: function(mouse) {
                // Map click to the ListView's coordinate space (which fills the window content)
                var listView = msgContent.parent  // MessageBubble's parent = ListView delegate → ListView
                var globalPos = mapToItem(null, mouse.x, mouse.y)  // null = window coordinates

                var popupW = 260
                var popupH = 420  // approximate max height
                var winW = bubble.Window.width || 800
                var winH = bubble.Window.height || 600

                // Clamp X: prefer right of cursor, flip left if clipped
                var px = globalPos.x
                if (px + popupW > winW) px = globalPos.x - popupW

                // Clamp Y: prefer below cursor, flip above if clipped
                var py = globalPos.y
                if (py + popupH > winH) py = globalPos.y - popupH

                // Safety: never go off-screen
                px = Math.max(4, Math.min(px, winW - popupW - 4))
                py = Math.max(4, Math.min(py, winH - popupH - 4))

                // Convert back to msgContent local coordinates
                var localPos = mapFromItem(null, px, py)
                msgPopupLoader.openAt(localPos.x, localPos.y)
            }
        }

        TextEdit { id: contextClipHelper; visible: false }

        // Lazy-loaded popups — only created when opened (saves 100s of window handles)
        Loader {
            id: quickEmojisLoader
            active: false
            sourceComponent: quickEmojisComp
            onLoaded: item.closed.connect(function() { quickEmojisLoader.active = false })
            function openAt(globalX, globalY) {
                active = true
                item.x = globalX
                item.y = globalY
                item.open()
            }
        }

        Loader {
            id: msgPopupLoader
            active: false
            sourceComponent: msgPopupComp
            onLoaded: item.closed.connect(function() { msgPopupLoader.active = false })
            function openAt(x, y) { active = true; item.x = x; item.y = y; item.open() }
        }

        Component {
            id: quickEmojisComp
        Popup {
            id: quickEmojis
            width: quickRow.width + 14
            height: 40
            padding: 5
            background: Rectangle {
                radius: 20
                color: Theme.darkMode ? "#262b34" : "#fafbfc"
                border.color: Theme.darkMode ? "#363c48" : "#dde0e4"
                border.width: 1
            }

            enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 100 } }
            exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 60 } }

            Row {
                id: quickRow
                anchors.centerIn: parent
                spacing: 2

                Repeater {
                    model: ["\uD83D\uDC4D", "\u2764\uFE0F", "\uD83D\uDE02", "\uD83D\uDE2E", "\uD83D\uDE22", "\uD83C\uDF89"]

                    Rectangle {
                        width: 30; height: 30; radius: 8
                        color: qeMa.containsMouse
                            ? (Theme.darkMode ? Qt.rgba(1,1,1,0.16) : Qt.rgba(0,0,0,0.08))
                            : "transparent"
                        Behavior on color { ColorAnimation { duration: 80 } }

                        Label {
                            anchors.centerIn: parent
                            text: modelData; font.pixelSize: 17
                            scale: qeMa.containsMouse ? 1.25 : 1.0
                            Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutBack } }
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
        // Unified popup: emoji row + actions — polished Telegram-style
        Popup {
            id: msgPopup
            width: popupCol.width + 14
            height: popupCol.height + 10
            padding: 5
            background: Rectangle {
                radius: 10
                color: Theme.darkMode ? "#262b34" : "#fafbfc"
                border.color: Theme.darkMode ? "#363c48" : "#dde0e4"
                border.width: 1

                // Subtle shadow
                Rectangle {
                    anchors.fill: parent; anchors.margins: -1; radius: 11
                    color: "transparent"; border.color: Qt.rgba(0,0,0,0.08); border.width: 1; z: -1
                }
            }

            enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 120 } }
            exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 80 } }

            ColumnLayout {
                id: popupCol
                spacing: 0

                // ── Emoji quick-react row ──
                Rectangle {
                    Layout.fillWidth: true
                    Layout.margins: 2
                    height: 40
                    radius: 8
                    color: Theme.darkMode ? Qt.rgba(1,1,1,0.03) : Qt.rgba(0,0,0,0.02)
                    visible: messageId > 0

                    Row {
                        anchors.centerIn: parent
                        spacing: 2

                        Repeater {
                            model: ["\uD83D\uDC4D", "\u2764\uFE0F", "\uD83D\uDE02", "\uD83D\uDE2E", "\uD83D\uDE22", "\uD83C\uDF89", "\uD83D\uDC4F", "\uD83D\uDE4F"]

                            Rectangle {
                                width: 32; height: 32; radius: 8
                                color: emojiMa.containsMouse
                                    ? (Theme.darkMode ? Qt.rgba(1,1,1,0.16) : Qt.rgba(0,0,0,0.08))
                                    : "transparent"
                                Behavior on color { ColorAnimation { duration: 80 } }

                                Label {
                                    anchors.centerIn: parent
                                    text: modelData
                                    font.pixelSize: 17
                                    scale: emojiMa.containsMouse ? 1.25 : 1.0
                                    Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutBack } }
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
                }

                // Divider
                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: 8; Layout.rightMargin: 8
                    Layout.topMargin: 3; Layout.bottomMargin: 3
                    height: 1
                    color: Theme.darkMode ? Qt.rgba(1,1,1,0.06) : Qt.rgba(0,0,0,0.06)
                    visible: messageId > 0
                }

                // ── Action items ──
                Repeater {
                    model: [
                        { icon: "\u21A9", label: "Reply", action: "reply", ownerOnly: false },
                        { icon: "\uD83D\uDCCB", label: "Copy", action: "copy", ownerOnly: false },
                        { icon: "\u21AA", label: "Forward", action: "forward", ownerOnly: false },
                        { icon: "\uD83D\uDCCC", label: "Pin", action: "pin", ownerOnly: false },
                        { icon: "\uD83D\uDD17", label: "Copy link", action: "copylink", ownerOnly: false },
                        { icon: "\uD83D\uDCAC", label: "Thread", action: "thread", ownerOnly: false },
                        { icon: "\uD83D\uDDD1", label: "Delete", action: "delete", ownerOnly: true }
                    ]

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.leftMargin: 2; Layout.rightMargin: 2
                        width: 195; height: 32; radius: 6
                        color: actionMa.containsMouse
                            ? (modelData.action === "delete"
                                ? Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.12)
                                : (Theme.darkMode ? Qt.rgba(1,1,1,0.10) : Qt.rgba(0,0,0,0.06)))
                            : "transparent"
                        visible: !modelData.ownerOnly || isOwnMessage
                        Behavior on color { ColorAnimation { duration: 80 } }

                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            spacing: 10

                            Label {
                                text: modelData.icon
                                font.pixelSize: 14
                                width: 20
                                horizontalAlignment: Text.AlignHCenter
                                anchors.verticalCenter: parent.verticalCenter
                                color: modelData.action === "delete" ? Theme.danger : Theme.textSecondary
                            }
                            Label {
                                text: modelData.label
                                font.pixelSize: 13
                                anchors.verticalCenter: parent.verticalCenter
                                color: modelData.action === "delete" ? Theme.danger
                                    : (actionMa.containsMouse ? Theme.textPrimary : Theme.textSecondary)
                                Behavior on color { ColorAnimation { duration: 80 } }
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

                // Image preview (authenticated via image://preview/)
                Image {
                    visible: isImage && fileId > 0
                    Layout.fillWidth: true
                    Layout.maximumWidth: 300
                    Layout.maximumHeight: 300
                    source: isImage && fileId > 0 ? "image://preview/" + fileId : ""
                    fillMode: Image.PreserveAspectFit
                    sourceSize: Qt.size(300, 300)

                    // Click opens in NC
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: Qt.openUrlExternally(fileLink)
                    }

                    // Loading placeholder
                    Rectangle {
                        anchors.fill: parent
                        visible: parent.status === Image.Loading
                        color: Theme.bgSurface
                        radius: Theme.radiusSmall
                        Label { anchors.centerIn: parent; text: "Loading preview..."; color: Theme.textMuted; font.pixelSize: Theme.fontSizeTiny }
                    }
                }

                // File attachment (non-image)
                Rectangle {
                    visible: hasFile && !isImage
                    Layout.fillWidth: true
                    height: 44
                    radius: Theme.radiusSmall
                    color: Theme.darkMode ? Qt.rgba(1,1,1,0.06) : Qt.rgba(0,0,0,0.05)

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacingNormal
                        anchors.rightMargin: Theme.spacingNormal
                        spacing: Theme.spacingSmall

                        Label {
                            text: fileMime.startsWith("image/") ? "\uD83D\uDDBC"
                                : fileMime.startsWith("video/") ? "\uD83C\uDFA5"
                                : fileMime.startsWith("audio/") ? "\uD83C\uDFB5"
                                : fileMime.indexOf("pdf") >= 0 ? "\uD83D\uDCC4"
                                : fileMime.indexOf("spreadsheet") >= 0 || fileMime.indexOf("excel") >= 0 ? "\uD83D\uDCCA"
                                : fileMime.indexOf("document") >= 0 || fileMime.indexOf("word") >= 0 ? "\uD83D\uDCC3"
                                : "\uD83D\uDCCE"
                            font.pixelSize: 20
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                text: fileName
                                font.pixelSize: Theme.fontSizeSmall
                                font.weight: Font.DemiBold
                                color: Theme.accent
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                            Label {
                                text: {
                                    var kb = fileSize / 1024
                                    return kb > 1024 ? (kb/1024).toFixed(1) + " MB" : Math.round(kb) + " KB"
                                }
                                font.pixelSize: Theme.fontSizeTiny
                                color: Theme.textMuted
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: Qt.openUrlExternally(fileLink)
                    }
                }

                // Message text with right-click copy
                Label {
                    Layout.fillWidth: true
                    text: messageText
                    visible: messageText.length > 0
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

                // Image preview (own)
                Image {
                    visible: isImage && fileId > 0
                    Layout.fillWidth: true
                    Layout.maximumWidth: 280
                    Layout.maximumHeight: 280
                    source: isImage && fileId > 0 ? "image://preview/" + fileId : ""
                    fillMode: Image.PreserveAspectFit
                    sourceSize: Qt.size(280, 280)
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: Qt.openUrlExternally(fileLink) }
                }

                // File attachment (own — non-image)
                Rectangle {
                    visible: hasFile && !isImage
                    Layout.fillWidth: true
                    height: 40
                    radius: Theme.radiusSmall
                    color: Qt.rgba(1,1,1,0.08)
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 8
                        Label { text: "\uD83D\uDCCE"; font.pixelSize: 16 }
                        Label { text: fileName; font.pixelSize: Theme.fontSizeSmall; color: "white"; elide: Text.ElideMiddle; Layout.fillWidth: true }
                    }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: Qt.openUrlExternally(fileLink) }
                }

                // Message text
                Label {
                    Layout.fillWidth: true
                    text: messageText
                    visible: messageText.length > 0
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
