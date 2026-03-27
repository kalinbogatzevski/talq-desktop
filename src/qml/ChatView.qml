import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: chatRoot
    property string conversationName: ""
    property string conversationUserId: ""
    property int conversationType: 0
    property string peerStatus: ""   // "online", "away", "dnd", "offline"
    property int replyToId: 0
    property string replyToAuthor: ""
    property string replyToText: ""

    property int activeThreadId: 0
    property string activeThreadTitle: ""
    property bool isGroupChat: conversationType === 2 || conversationType === 3
    property bool isInTopicMode: false
    property bool sidebarSqueezed: false
    signal expandSidebar()
    property int activeThreadColor: 0
    property bool usePainter: true
    property var ctxMenuData: ({})
    readonly property bool isViewingTopic: isInTopicMode && activeThreadId > 0
    // Only show typing when it's from the current conversation
    readonly property bool isTyping: signaling.typingUser.length > 0
        && signaling.typingRoom === messageModel.conversationToken

    function openThread(threadId, title) {
        activeThreadId = threadId
        activeThreadTitle = title
        messageModel.threadId = threadId
    }

    function closeThread() {
        activeThreadId = 0
        activeThreadTitle = ""
        messageModel.threadId = 0
    }

    function startReply(msgId, author, text) {
        replyToId = msgId
        replyToAuthor = author
        replyToText = text
        Qt.callLater(messageListView.scrollToBottom)
    }

    function cancelReply() {
        replyToId = 0
        replyToAuthor = ""
        replyToText = ""
    }

    function confirmPaste() {
        if (pasteBar.pendingPath.length > 0) {
            var path = pasteBar.pendingPath
            messageModel.sendFileWithCaption(path, pasteCaptionField.text.trim())
            // Clean up temp file after upload starts (file data already read into memory)
            messageModel.cleanupTempFile(path)
            cancelPaste()
        }
    }

    function cancelPaste() {
        if (pasteBar.pendingPath.length > 0)
            messageModel.cleanupTempFile(pasteBar.pendingPath)
        pasteBar.visible = false
        pasteBar.pendingPath = ""
        pasteBar.isImage = false
        pastePreview.source = ""
        pasteCaptionField.text = ""
    }

    padding: 0
    background: Rectangle { color: Theme.bgSecondary }

    header: Rectangle {
        height: Theme.headerHeight
        color: Theme.bgPrimary

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spacingLarge
            anchors.rightMargin: Theme.spacingXLarge
            spacing: Theme.spacingSmall

            // Back to chat list (when sidebar is squeezed in topic mode)
            TqIconButton {
                iconName: "arrow-left"
                size: 30
                iconColor: Theme.accent
                visible: chatRoot.sidebarSqueezed && chatRoot.conversationName.length > 0
                onClicked: chatRoot.expandSidebar()
            }

            // Back button (when viewing a thread or topic)
            TqIconButton {
                iconName: "arrow-left"
                size: 30
                iconColor: Theme.accent
                visible: chatRoot.activeThreadId > 0
                onClicked: chatRoot.closeThread()
            }

            Rectangle {
                width: 10; height: 10; radius: 5
                visible: chatRoot.isViewingTopic
                color: Theme.topicColor(chatRoot.activeThreadColor)
                Layout.alignment: Qt.AlignVCenter
            }

            TqAvatar {
                userId: chatRoot.conversationUserId
                displayName: chatRoot.conversationName
                size: 30
                visible: chatRoot.conversationName.length > 0
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                Label {
                    text: chatRoot.activeThreadId > 0 ? chatRoot.activeThreadTitle
                        : (chatRoot.conversationName || "Select a conversation")
                    font.pixelSize: Theme.fontSizeLarge
                    font.weight: chatRoot.conversationName.length > 0 ? Font.DemiBold : Font.Normal
                    color: chatRoot.conversationName.length > 0 ? Theme.textPrimary : Theme.textMuted
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                // Status line: typing indicator OR user status (1:1 chats)
                Label {
                    visible: chatRoot.isTyping
                        || (chatRoot.conversationType === 1 && chatRoot.peerStatus.length > 0)
                    text: {
                        if (chatRoot.isTyping)
                            return signaling.typingUser + " is typing..."
                        if (chatRoot.conversationType === 1 && chatRoot.peerStatus.length > 0) {
                            switch (chatRoot.peerStatus) {
                            case "online": return "online"
                            case "away": return "away"
                            case "dnd": return "do not disturb"
                            default: return "offline"
                            }
                        }
                        return ""
                    }
                    font.pixelSize: Theme.fontSizeTiny
                    font.italic: chatRoot.isTyping
                    color: {
                        if (chatRoot.isTyping) return Theme.accent
                        switch (chatRoot.peerStatus) {
                        case "online": return Theme.online
                        case "away": return Theme.warning
                        case "dnd": return Theme.danger
                        default: return Theme.textMuted
                        }
                    }
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }

                Label {
                    visible: chatRoot.isViewingTopic && !chatRoot.isTyping
                    text: chatRoot.conversationName + " \u00B7 " + messageModel.count + " messages"
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.textSecondary
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }

            // Audio call button
            TqIconButton {
                iconName: "phone"
                size: 34
                bgColor: Theme.success
                iconColor: "white"
                visible: chatRoot.conversationType === 1 && callManager.state === 0 && callManager.callsAvailable
                onClicked: { callManager.setRemotePeerInfo(chatRoot.conversationName, chatRoot.conversationUserId); callManager.startCall(messageModel.conversationToken, false) }
                ToolTip.visible: hovered; ToolTip.text: "Audio call"; ToolTip.delay: 300
            }

            // Video call button
            TqIconButton {
                iconName: "video"
                size: 34
                bgColor: Theme.accent
                iconColor: "white"
                visible: chatRoot.conversationType === 1 && callManager.state === 0 && callManager.callsAvailable
                onClicked: { callManager.setRemotePeerInfo(chatRoot.conversationName, chatRoot.conversationUserId); callManager.startCall(messageModel.conversationToken, true) }
                ToolTip.visible: hovered; ToolTip.text: "Video call"; ToolTip.delay: 300
            }

            // Calls unavailable indicator
            Label {
                visible: chatRoot.conversationType === 1 && !callManager.callsAvailable
                text: "Calls unavailable"
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeTiny
                ToolTip.visible: callsUnavailableHover.hovered
                ToolTip.text: callManager.callsUnavailableReason
                ToolTip.delay: 300
                HoverHandler { id: callsUnavailableHover }
            }

            // Active call indicator
            Label {
                visible: callManager.state > 0
                text: "\uD83D\uDCDE " + formatDuration(callManager.callDuration)
                font.pixelSize: Theme.fontSizeTiny
                color: Theme.online
                function formatDuration(s) {
                    var m = Math.floor(s / 60)
                    var sec = s % 60
                    return (m < 10 ? "0" : "") + m + ":" + (sec < 10 ? "0" : "") + sec
                }
            }

            Label {
                visible: messageModel.loading && messageModel.count > 0
                text: "\u21BB"; font.pixelSize: 14; color: Theme.textMuted
                RotationAnimation on rotation { from: 0; to: 360; duration: 1000; loops: Animation.Infinite; running: messageModel.loading && messageModel.count > 0 }
            }

            BusyIndicator {
                running: messageModel.loading && messageModel.count === 0
                visible: running; implicitWidth: 20; implicitHeight: 20; palette.dark: Theme.accent
            }
        }
    }

    footer: Pane {
        padding: 0
        visible: messageModel.conversationToken.length > 0
        background: Rectangle { color: Theme.bgPrimary }

        ColumnLayout {
            width: parent.width
            spacing: 0

            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }

            // Reply bar
            Rectangle {
                Layout.fillWidth: true
                height: visible ? replyPreviewCol.implicitHeight + 12 : 0
                color: Theme.bgSurface
                visible: chatRoot.replyToId > 0

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingLarge
                    anchors.rightMargin: Theme.spacingNormal
                    spacing: Theme.spacingSmall

                    Rectangle { width: 3; Layout.fillHeight: true; Layout.topMargin: 6; Layout.bottomMargin: 6; radius: 1.5; color: Theme.accent }
                    ColumnLayout {
                        id: replyPreviewCol
                        Layout.fillWidth: true; spacing: 1
                        Label { text: chatRoot.replyToAuthor; font.pixelSize: Theme.fontSizeTiny; font.weight: Font.DemiBold; color: Theme.accent }
                        Label { Layout.fillWidth: true; text: chatRoot.replyToText; font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondary; elide: Text.ElideRight; maximumLineCount: 1 }
                    }
                    ToolButton {
                        width: 28; height: 28; onClicked: chatRoot.cancelReply()
                        contentItem: Label { text: "\u2715"; font.pixelSize: 14; color: Theme.textSecondary; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { radius: 14; color: parent.hovered ? Theme.bgHover : "transparent" }
                    }
                }
            }

            // Upload progress bar
            Rectangle {
                Layout.fillWidth: true
                height: visible ? 36 : 0
                color: Theme.bgSurface
                visible: messageModel.uploadProgress >= 0

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingLarge
                    anchors.rightMargin: Theme.spacingLarge
                    spacing: Theme.spacingSmall

                    Label {
                        text: "\uD83D\uDCCE"  // 📎
                        font.pixelSize: 14
                    }
                    Label {
                        text: messageModel.uploadFileName
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.textSecondary
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                    Label {
                        text: Math.round(messageModel.uploadProgress * 100) + "%"
                        font.pixelSize: Theme.fontSizeTiny
                        font.weight: Font.DemiBold
                        color: Theme.accent
                    }
                }

                // Progress bar at bottom
                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    width: parent.width * Math.max(0, messageModel.uploadProgress)
                    height: 2
                    color: Theme.accent
                    Behavior on width { NumberAnimation { duration: 100 } }
                }
            }

            // Paste confirmation bar
            Rectangle {
                id: pasteBar
                Layout.fillWidth: true
                height: visible ? pasteContent.implicitHeight + 16 : 0
                color: Theme.bgSurface
                visible: false

                property string pendingPath: ""
                property bool isImage: false

                RowLayout {
                    id: pasteContent
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingLarge
                    anchors.rightMargin: Theme.spacingNormal
                    spacing: Theme.spacingSmall

                    Rectangle { width: 3; Layout.fillHeight: true; Layout.topMargin: 6; Layout.bottomMargin: 6; radius: 1.5; color: Theme.accent }

                    // Thumbnail preview (images only)
                    Image {
                        id: pastePreview
                        Layout.preferredWidth: 40
                        Layout.preferredHeight: 40
                        fillMode: Image.PreserveAspectFit
                        visible: pasteBar.isImage && status === Image.Ready
                    }

                    // File icon (non-images)
                    Label {
                        text: "\uD83D\uDCCE"
                        font.pixelSize: 22
                        visible: !pasteBar.isImage
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Label {
                            text: pasteBar.isImage ? "Send image"
                                : "Send file: " + pasteBar.pendingPath.split("/").pop()
                            font.pixelSize: Theme.fontSizeTiny
                            font.weight: Font.DemiBold
                            color: Theme.accent
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }

                        TextField {
                            id: pasteCaptionField
                            Layout.fillWidth: true
                            placeholderText: "Add a caption..."
                            placeholderTextColor: Theme.textMuted
                            font.pixelSize: Theme.fontSizeTiny
                            color: Theme.textPrimary
                            background: Rectangle { color: "transparent" }
                            padding: 0
                            Keys.onReturnPressed: chatRoot.confirmPaste()
                            Keys.onEscapePressed: chatRoot.cancelPaste()
                        }
                    }

                    // Send button
                    ToolButton {
                        width: 28; height: 28
                        onClicked: chatRoot.confirmPaste()
                        contentItem: Label { text: "\u276F"; font.pixelSize: 14; font.weight: Font.Bold; color: Theme.accent; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { radius: 14; color: parent.hovered ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.15) : "transparent" }
                    }

                    // Cancel button
                    ToolButton {
                        width: 28; height: 28
                        onClicked: chatRoot.cancelPaste()
                        contentItem: Label { text: "\u2715"; font.pixelSize: 14; color: Theme.textSecondary; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { radius: 14; color: parent.hovered ? Theme.bgHover : "transparent" }
                    }
                }
            }

            MessageComposer {
                Layout.fillWidth: true
                topicName: chatRoot.isViewingTopic ? chatRoot.activeThreadTitle : ""
                onSendMessage: function(text) {
                    var replyId = chatRoot.replyToId > 0 ? chatRoot.replyToId : chatRoot.activeThreadId
                    messageModel.sendMessage(text, replyId)
                    chatRoot.cancelReply()
                }
            }
        }
    }

    // ═══ Painted chat renderer (toggle with usePainter) ═══
    ChatPainter {
        id: chatPainter
        anchors.fill: parent
        visible: chatRoot.usePainter && messageModel.conversationToken.length > 0
        model: messageModel
        myUserId: auth.userId
        darkMode: Theme.darkMode
        fontScale: Theme.fontScale
        clip: true
        focus: true  // ensure it receives input events

        MouseArea {
            id: chatMouseArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            hoverEnabled: true
            property real dragStartY: 0
            property real dragStartScroll: 0
            property bool dragMoved: false
            cursorShape: {
                var hit = chatPainter.hitTestAt(mouseX, mouseY)
                return hit.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
            }
            onPressed: function(mouse) {
                dragStartY = mouse.y
                dragStartScroll = chatPainter.scrollY
                dragMoved = false
            }
            onPositionChanged: function(mouse) {
                if (pressed) {
                    var dy = dragStartY - mouse.y
                    if (Math.abs(dy) > 4) dragMoved = true
                    if (dragMoved) chatPainter.scrollY = dragStartScroll + dy
                }
            }
            onReleased: function(mouse) {
                if (!dragMoved && mouse.button === Qt.LeftButton) {
                    var hit = chatPainter.hitTestAt(mouse.x, mouse.y)
                    if (hit.startsWith("link:")) {
                        var url = hit.substring(5)
                        if (url.startsWith("http://") || url.startsWith("https://"))
                            Qt.openUrlExternally(url)
                    } else if (hit.startsWith("file:")) {
                        var parts = hit.substring(5).split(":")
                        messageModel.downloadFile(parseInt(parts[0]), parts.slice(1).join(":"))
                    } else if (hit.startsWith("reaction:")) {
                        var rparts = hit.substring(9).split(":")
                        messageModel.addReaction(parseInt(rparts[0]), rparts.slice(1).join(":"))
                    }
                }
                // Right-click → context menu
                if (!dragMoved && mouse.button === Qt.RightButton) {
                    var msg = chatPainter.messageAt(mouse.x, mouse.y)
                    if (msg.messageId > 0) {
                        ctxMenuData = msg
                        var popW = 220
                        var popH = msg.isOwn ? 260 : 230
                        // Popup parent = chatPainter, so x/y relative to chatPainter
                        // mouse.x/y already relative to chatMouseArea which fills chatPainter
                        ctxMenu.x = mouse.x
                        ctxMenu.y = mouse.y
                        ctxMenu.open()
                        // After open, clamp if overflowing
                        if (ctxMenu.x + ctxMenu.width > chatPainter.width)
                            ctxMenu.x = mouse.x - ctxMenu.width
                        if (ctxMenu.y + ctxMenu.height > chatPainter.height)
                            ctxMenu.y = mouse.y - ctxMenu.height
                        ctxMenu.x = Math.max(4, ctxMenu.x)
                        ctxMenu.y = Math.max(4, ctxMenu.y)
                    }
                }
            }
            onWheel: function(wheel) {
                chatPainter.scrollY = chatPainter.scrollY - wheel.angleDelta.y / 120.0 * 40.0
            }
        }

        ScrollBar {
            id: painterScrollBar
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 6
            policy: ScrollBar.AlwaysOn
            size: chatPainter.visibleHeight / Math.max(1, chatPainter.contentHeight)
            position: chatPainter.contentHeight > chatPainter.visibleHeight
                ? chatPainter.scrollY / chatPainter.contentHeight
                : 0
            onPositionChanged: {
                if (active)
                    chatPainter.scrollY = position * chatPainter.contentHeight
            }
            contentItem: Rectangle {
                implicitWidth: 6
                radius: 3
                color: Theme.textMuted
                opacity: painterScrollBar.active ? 0.5 : 0
                Behavior on opacity { NumberAnimation { duration: 400 } }
            }
            background: Item {}
        }
    }

    // ═══ Context menu for ChatPainter messages ═══
    Popup {
        id: ctxMenu
        parent: chatPainter
        width: ctxCol.width + 14
        height: ctxCol.height + 10
        padding: 5
        modal: false
        closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape
        Overlay.modal: Rectangle { color: "transparent" }
        Overlay.modeless: Rectangle { color: "transparent" }
        background: Rectangle {
            radius: 10
            color: Theme.darkMode ? "#262b34" : "#fafbfc"
            border.color: Theme.darkMode ? "#363c48" : "#dde0e4"
            border.width: 1
            Rectangle { anchors.fill: parent; anchors.margins: -3; radius: 13; color: "transparent"; border.color: Qt.rgba(0,0,0,0.04); border.width: 2; z: -2 }
            Rectangle { anchors.fill: parent; anchors.margins: -1; radius: 11; color: "transparent"; border.color: Qt.rgba(0,0,0,0.12); border.width: 1; z: -1 }
        }
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 100; easing.type: Easing.OutCubic } }
        exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 60 } }

        TextEdit { id: ctxClipHelper; visible: false }

        ColumnLayout {
            id: ctxCol
            spacing: 0

            // Emoji quick-react row
            Rectangle {
                Layout.fillWidth: true; Layout.margins: 2
                implicitWidth: ctxEmojiRow.width + 12; height: 38; radius: 8
                color: Theme.darkMode ? Qt.rgba(1,1,1,0.06) : Qt.rgba(0,0,0,0.035)
                Row {
                    id: ctxEmojiRow; anchors.centerIn: parent; spacing: 2
                    Repeater {
                        model: ["\uD83D\uDC4D", "\u2764\uFE0F", "\uD83D\uDE02", "\uD83D\uDE2E", "\uD83D\uDE22", "\uD83C\uDF89"]
                        Rectangle {
                            width: 30; height: 30; radius: 8
                            color: ctxEmoMa.containsMouse ? (Theme.darkMode ? Qt.rgba(1,1,1,0.16) : Qt.rgba(0,0,0,0.08)) : "transparent"
                            Label { anchors.centerIn: parent; text: modelData; font.pixelSize: 17; scale: ctxEmoMa.containsMouse ? 1.25 : 1.0; Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutBack } } }
                            MouseArea { id: ctxEmoMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { messageModel.addReaction(ctxMenuData.messageId, modelData); ctxMenu.close() } }
                        }
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; Layout.leftMargin: 6; Layout.rightMargin: 6; Layout.topMargin: 4; Layout.bottomMargin: 4; height: 1; color: Theme.darkMode ? Qt.rgba(1,1,1,0.10) : Qt.rgba(0,0,0,0.08) }

            // Action items
            Repeater {
                model: [
                    { icon: "\u2B07", label: "Download", action: "download", ownerOnly: false, fileOnly: true },
                    { icon: "\u2601", label: "Open in Nextcloud", action: "openincloud", ownerOnly: false, fileOnly: true },
                    { icon: "\uD83D\uDCCB", label: "Copy", action: "copy", ownerOnly: false, fileOnly: false },
                    { icon: "\u21A9", label: "Reply", action: "reply", ownerOnly: false, fileOnly: false },
                    { icon: "\uD83D\uDDD1", label: "Delete", action: "delete", ownerOnly: true, fileOnly: false }
                ]
                Rectangle {
                    Layout.fillWidth: true; Layout.leftMargin: 2; Layout.rightMargin: 2
                    width: 195; height: 32; radius: 6
                    color: ctxActMa.containsMouse ? (modelData.action === "delete" ? Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.15) : (Theme.darkMode ? Qt.rgba(1,1,1,0.12) : Qt.rgba(0,0,0,0.07))) : "transparent"
                    visible: (!modelData.ownerOnly || ctxMenuData.isOwn) && (!modelData.fileOnly || ctxMenuData.hasFile)
                    Row {
                        anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left; anchors.leftMargin: 10; spacing: 10
                        Label { text: modelData.icon; font.pixelSize: 14; width: 20; horizontalAlignment: Text.AlignHCenter; anchors.verticalCenter: parent.verticalCenter; color: modelData.action === "delete" ? Theme.danger : (ctxActMa.containsMouse ? Theme.textPrimary : Theme.textSecondary) }
                        Label { text: modelData.label; font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter; color: modelData.action === "delete" ? Theme.danger : (ctxActMa.containsMouse ? Theme.textPrimary : Theme.textSecondary) }
                    }
                    MouseArea {
                        id: ctxActMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            ctxMenu.close()
                            var d = ctxMenuData
                            if (modelData.action === "download") messageModel.downloadFile(d.fileId, d.fileName)
                            else if (modelData.action === "openincloud") Qt.openUrlExternally(messageModel.fileLink(d.fileId))
                            else if (modelData.action === "copy") { ctxClipHelper.text = d.messageText.replace(/<[^>]*>/g, ""); ctxClipHelper.selectAll(); ctxClipHelper.copy() }
                            else if (modelData.action === "reply") chatRoot.startReply(d.messageId, d.actorName, d.messageText)
                            else if (modelData.action === "delete") messageModel.deleteMessage(d.messageId)
                        }
                    }
                }
            }
        }
    }

    // Messages list — fills the content area between header and footer
    ListView {
        id: messageListView
        anchors.fill: parent
        visible: !chatRoot.usePainter
        model: messageModel
        clip: true
        spacing: 2
        topMargin: Theme.spacingNormal
        bottomMargin: Theme.spacingLarge
        boundsBehavior: Flickable.StopAtBounds
        cacheBuffer: 200
        verticalLayoutDirection: ListView.BottomToTop
        interactive: !chatRoot.usePainter  // disable when ChatPainter is active
        flickableDirection: Flickable.VerticalFlick

        property bool autoScrolling: true
        property int previousCount: 0
        property bool programmaticScroll: false
        property bool userHasScrolled: false  // true after first user interaction

        function scrollToBottom() {
            // BottomToTop: view naturally starts at bottom. No-op for initial load.
            // For poller/send: view is already at bottom if autoScrolling is true.
        }

        Timer {
            id: historyDebounce
            interval: 500  // cooldown after each history load
        }

        onDraggingChanged: {
            if (dragging) {
                autoScrolling = false
                userHasScrolled = true
            }
        }

        onFlickStarted: {
            if (!programmaticScroll) {
                autoScrolling = false
                userHasScrolled = true
            }
        }

        // Removed onContentHeightChanged auto-scroll — it can cause infinite
        // loops when positionViewAtEnd() triggers delegate loading which changes
        // contentHeight again. Scrolling is handled by onNewMessagesAtEnd instead.

        onMovingChanged: {
            if (!moving) {
                // BottomToTop: "at bottom" means contentY is near originY
                var atBottom = Math.abs(contentY - originY) < 40
                if (atBottom) autoScrolling = true
            }
        }

        onContentYChanged: {
            // BottomToTop: scrolling UP = contentY increasing toward contentHeight
            // Load history when near the top (oldest messages)
            var distFromTop = contentHeight - (contentY + height)
            if (userHasScrolled && distFromTop < 200 && !autoScrolling
                    && !messageModel.loading && messageModel.hasMoreHistory && count > 0
                    && !historyDebounce.running) {
                messageModel.loadHistory()
                historyDebounce.start()
            }
        }

        onCountChanged: {
            // BottomToTop: view naturally shows newest (index 0) at bottom.
            // No scroll needed on initial load or history append.
            previousCount = count
        }

        ScrollBar.vertical: ScrollBar {
            id: chatScrollBar
            policy: ScrollBar.AlwaysOn
            width: 6
            contentItem: Rectangle {
                implicitWidth: 6
                radius: 3
                color: Theme.textMuted
                opacity: chatScrollBar.active ? 0.5 : 0
                Behavior on opacity { NumberAnimation { duration: 400 } }
            }
            background: Item {}  // transparent track
        }

        // Welcome screen
        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - 60, 360)
            spacing: Theme.spacingXLarge
            visible: messageModel.conversationToken.length === 0 && !messageModel.loading

            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                source: isBranded ? brandLogo : "qrc:/logo.png"; width: 80; height: 80
                sourceSize: Qt.size(192, 192); fillMode: Image.PreserveAspectFit; opacity: 0.9
            }

            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.spacingSmall

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Welcome, " + auth.displayName
                    font.pixelSize: Theme.fontSizeTitle; font.weight: Font.DemiBold; color: Theme.textPrimary
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Pick a conversation from the sidebar"
                    font.pixelSize: Theme.fontSizeNormal; color: Theme.textSecondary
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "v" + Qt.application.version
                    font.pixelSize: Theme.fontSizeTiny; color: Theme.textMuted
                }
            }

            // Server info card
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width
                height: serverInfoCol.implicitHeight + Theme.spacingXLarge
                radius: Theme.radiusNormal
                color: Theme.bgSurface
                border.color: Theme.divider
                border.width: 1
                visible: auth.serverUrl.length > 0

                ColumnLayout {
                    id: serverInfoCol
                    anchors {
                        left: parent.left; right: parent.right
                        verticalCenter: parent.verticalCenter
                        margins: Theme.spacingLarge
                    }
                    spacing: Theme.spacingSmall

                    // Card header
                    Label {
                        text: "Server"
                        font.pixelSize: Theme.fontSizeTiny
                        font.weight: Font.DemiBold
                        color: Theme.textMuted
                        font.letterSpacing: 0.8
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }

                    // Info rows — ColumnLayout with RowLayouts for clean alignment
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacingSmall

                        Repeater {
                            model: [
                                { icon: "\u2601", text: auth.serverUrl.replace(/^https?:\/\//, ""), bold: true, accent: true, vis: auth.serverUrl.length > 0 },
                                { icon: "\u24C3", text: "Nextcloud " + auth.nextcloudVersion, bold: false, accent: false, vis: auth.nextcloudVersion.length > 0 },
                                { icon: "\u260E", text: "Talk " + auth.talkVersion, bold: false, accent: false, vis: auth.talkVersion.length > 0 },
                                { icon: "\u26A1", text: auth.signalingMode, bold: false, accent: false, vis: auth.signalingMode.length > 0 }
                            ]

                            RowLayout {
                                visible: modelData.vis
                                spacing: Theme.spacingNormal
                                Item {
                                    width: 24; height: 20
                                    Label {
                                        anchors.centerIn: parent
                                        text: modelData.icon; font.pixelSize: 16
                                        color: modelData.accent ? Theme.accent : Theme.textSecondary
                                    }
                                }
                                Label {
                                    text: modelData.text
                                    font.pixelSize: modelData.bold ? Theme.fontSizeNormal : Theme.fontSizeSmall
                                    font.weight: modelData.bold ? Font.DemiBold : Font.Normal
                                    color: modelData.accent ? Theme.textPrimary : Theme.textSecondary
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                        }

                        // Push status
                        RowLayout {
                            spacing: Theme.spacingNormal
                            Item {
                                width: 24; height: 20
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 8; height: 8; radius: 4
                                    color: pushClient.connected ? Theme.online : Theme.warning
                                }
                            }
                            Label {
                                text: pushClient.connected ? "Push connected (real-time)" : "Push disconnected (polling)"
                                font.pixelSize: Theme.fontSizeSmall
                                color: pushClient.connected ? Theme.online : Theme.textSecondary
                                Layout.fillWidth: true
                            }
                        }

                        // Signaling status
                        RowLayout {
                            spacing: Theme.spacingNormal
                            Item {
                                width: 24; height: 20
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 8; height: 8; radius: 4
                                    color: signaling.connected ? Theme.online : Theme.textMuted
                                }
                            }
                            Label {
                                text: signaling.connected ? "Signaling connected" : "Signaling disconnected"
                                font.pixelSize: Theme.fontSizeSmall
                                color: signaling.connected ? Theme.online : Theme.textSecondary
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }
        }

        // Empty chat
        Column {
            anchors.centerIn: parent
            spacing: Theme.spacingLarge
            visible: messageModel.conversationToken.length > 0 && messageModel.count === 0 && !messageModel.loading

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 64; height: 64; radius: 32
                color: Theme.bgSurface
                Label {
                    anchors.centerIn: parent
                    text: "💬"; font.pixelSize: 28
                }
            }

            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.spacingTiny
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "No messages yet"
                    font.pixelSize: Theme.fontSizeLarge; font.weight: Font.DemiBold; color: Theme.textPrimary
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Be the first to say something!"
                    font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondary
                }
            }
        }

        Column {
            anchors.centerIn: parent
            spacing: Theme.spacingLarge
            visible: chatRoot.isInTopicMode && !chatRoot.isViewingTopic && messageModel.conversationToken.length > 0

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "\uD83D\uDCAC"
                font.pixelSize: 48
                opacity: 0.3
            }
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Select a topic"
                font.pixelSize: Theme.fontSizeNormal
                color: Theme.textMuted
            }
        }

        delegate: MessageBubble {
            width: messageListView.width
            isOwnMessage: actorId === auth.userId
            onReplyRequested: function(msgId, author, text) {
                chatRoot.startReply(msgId, author, text)
            }
            onThreadOpenRequested: function(threadId) {
                chatRoot.openThread(threadId, "Thread")
            }
        }
    }

    // Scroll-to-bottom floating button
    TqIconButton {
        iconName: "arrow-down"
        size: Theme.buttonSizeMedium
        bgColor: Theme.bgSurface
        anchors.right: chatRoot.contentItem.right
        anchors.bottom: chatRoot.contentItem.bottom
        anchors.rightMargin: 20
        anchors.bottomMargin: 16
        visible: chatRoot.usePainter
            ? (!chatPainter.atBottom && messageModel.count > 0)
            : (!messageListView.autoScrolling && messageModel.count > 0)
        opacity: visible ? 1 : 0
        z: 50
        Behavior on opacity { NumberAnimation { duration: Theme.animFast } }
        onClicked: {
            if (chatRoot.usePainter) {
                chatPainter.scrollToBottom()
            } else {
                messageListView.autoScrolling = true
                messageListView.scrollToBottom()
            }
        }
    }

    Connections {
        target: messageModel
        function onConversationTokenChanged() {
            chatRoot.closeThread()
            chatRoot.cancelPaste()
            if (chatRoot.usePainter) {
                chatPainter.scrollToBottom()
            } else {
                messageListView.autoScrolling = true
                messageListView.previousCount = 0
                messageListView.userHasScrolled = false
            }
        }
        function onNewMessagesAtEnd() {
            if (chatRoot.usePainter) {
                if (chatPainter.atBottom)
                    chatPainter.scrollToBottom()
            } else {
                if (messageListView.count > 0 && messageListView.autoScrolling)
                    messageListView.scrollToBottom()
            }
        }
        function onPasteReady(filePath, width, height) {
            pasteBar.pendingPath = filePath
            pasteBar.isImage = (width > 0 && height > 0)
            pastePreview.source = pasteBar.isImage ? filePath : ""
            pasteBar.visible = true
            pasteCaptionField.forceActiveFocus()
        }
    }

    // Drag and drop file upload
    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]

        onEntered: function(drag) {
            drag.accepted = drag.hasUrls
            dropOverlay.visible = drag.hasUrls
        }
        onExited: dropOverlay.visible = false
        onDropped: function(drop) {
            dropOverlay.visible = false
            if (drop.hasUrls && drop.urls.length > 0) {
                // Show confirmation for first file (multi-file: send rest directly)
                messageModel.promptFileSend(drop.urls[0].toString())
                for (var i = 1; i < drop.urls.length; i++) {
                    messageModel.sendFile(drop.urls[i].toString())
                }
            }
        }
    }

    // Drop overlay
    Rectangle {
        id: dropOverlay
        anchors.fill: parent
        visible: false
        color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.15)
        z: 100

        Rectangle {
            anchors.centerIn: parent
            width: dropLabel.implicitWidth + Theme.spacingXLarge * 2
            height: dropLabel.implicitHeight + Theme.spacingXLarge
            radius: Theme.radiusLarge
            color: Theme.bgSurface
            border.color: Theme.accent
            border.width: 2

            Label {
                id: dropLabel
                anchors.centerIn: parent
                text: "\uD83D\uDCCE  Drop files here to send"
                font.pixelSize: Theme.fontSizeLarge
                font.weight: Font.DemiBold
                color: Theme.accent
            }
        }
    }
}
