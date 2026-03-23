import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: sidebar

    signal conversationSelected(string token, string name, string odataUserId, int convType, string userStatus)

    property int selectedIndex: -1
    property bool squeezed: false
    property real sidebarWidth: squeezed ? 56 : Math.max(200, Math.min(500, _userWidth))
    property real _userWidth: 320

    Behavior on sidebarWidth {
        enabled: !_resizing
        NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic }
    }
    property bool _resizing: false

    Rectangle {
        anchors.fill: parent
        color: Theme.bgSidebar

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Header
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.headerHeight
                color: Theme.bgSecondary

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: sidebar.squeezed ? 0 : Theme.spacingLarge
                    anchors.rightMargin: sidebar.squeezed ? 0 : Theme.spacingNormal

                    Behavior on anchors.leftMargin { NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic } }
                    Behavior on anchors.rightMargin { NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic } }

                    Item {
                        width: Theme.avatarSizeSmall
                        height: Theme.avatarSizeSmall
                        Layout.alignment: sidebar.squeezed ? Qt.AlignHCenter : Qt.AlignVCenter
                        Layout.leftMargin: sidebar.squeezed ? (parent.width - Theme.avatarSizeSmall) / 2 : 0

                        TqAvatar {
                            anchors.fill: parent
                            userId: auth.userId
                            displayName: auth.displayName
                            size: Theme.avatarSizeSmall
                            showStatus: true
                            status: pushClient.connected ? "online" : messageModel.connected ? "away" : "offline"
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: settingsDialog.visible = true
                        }
                    }

                    Label {
                        text: auth.displayName || "Talk"
                        font.pixelSize: Theme.fontSizeLarge
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        visible: !sidebar.squeezed
                        opacity: sidebar.squeezed ? 0 : 1
                        Behavior on opacity { NumberAnimation { duration: Theme.animFast } }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: settingsDialog.visible = true
                        }
                    }

                    TqIconButton {
                        iconName: Theme.darkMode ? "sun" : "moon"
                        size: Theme.buttonSizeMedium
                        visible: !sidebar.squeezed
                        opacity: sidebar.squeezed ? 0 : 1
                        Behavior on opacity { NumberAnimation { duration: Theme.animFast } }
                        onClicked: Theme.darkMode = !Theme.darkMode
                        ToolTip.visible: hovered
                        ToolTip.text: Theme.darkMode ? "Switch to light mode" : "Switch to dark mode"
                        ToolTip.delay: 500
                    }

                    TqIconButton {
                        iconName: "refresh"
                        size: Theme.buttonSizeMedium
                        visible: !sidebar.squeezed
                        opacity: sidebar.squeezed ? 0 : 1
                        Behavior on opacity { NumberAnimation { duration: Theme.animFast } }
                        onClicked: conversationModel.refresh()
                        ToolTip.visible: hovered
                        ToolTip.text: "Refresh conversations"
                        ToolTip.delay: 500
                    }

                    TqIconButton {
                        iconName: "power"
                        size: Theme.buttonSizeMedium
                        visible: !sidebar.squeezed
                        opacity: sidebar.squeezed ? 0 : 1
                        Behavior on opacity { NumberAnimation { duration: Theme.animFast } }
                        onClicked: exitDialog.open()
                        ToolTip.visible: hovered
                        ToolTip.text: "Exit"
                        ToolTip.delay: 500
                    }

                    // Exit confirmation dialog
                    Dialog {
                        id: exitDialog
                        title: "Exit TalQ"
                        modal: true
                        anchors.centerIn: Overlay.overlay
                        width: 300
                        standardButtons: Dialog.NoButton

                        background: Rectangle {
                            radius: Theme.radiusNormal
                            color: Theme.bgSurface
                            border.color: Theme.divider
                            border.width: 1
                        }

                        ColumnLayout {
                            width: parent.width
                            spacing: Theme.spacingLarge

                            Label {
                                text: "What would you like to do?"
                                font.pixelSize: Theme.fontSizeNormal
                                color: Theme.textPrimary
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                            }

                            Button {
                                Layout.fillWidth: true
                                text: "Minimize to tray"
                                font.pixelSize: Theme.fontSizeNormal
                                onClicked: { exitDialog.close(); root.saveWindowState(); root.hide() }
                                background: Rectangle {
                                    radius: Theme.radiusSmall
                                    color: parent.hovered ? Theme.bgHover : Theme.bgInput
                                }
                                contentItem: Label {
                                    text: parent.text; font: parent.font
                                    color: Theme.textPrimary
                                    horizontalAlignment: Text.AlignHCenter
                                }
                            }

                            Button {
                                Layout.fillWidth: true
                                text: "Log out"
                                font.pixelSize: Theme.fontSizeNormal
                                onClicked: { exitDialog.close(); auth.logout() }
                                background: Rectangle {
                                    radius: Theme.radiusSmall
                                    color: parent.hovered ? Theme.bgHover : Theme.bgInput
                                }
                                contentItem: Label {
                                    text: parent.text; font: parent.font
                                    color: Theme.warning
                                    horizontalAlignment: Text.AlignHCenter
                                }
                            }

                            Button {
                                Layout.fillWidth: true
                                text: "Quit TalQ"
                                font.pixelSize: Theme.fontSizeNormal
                                onClicked: { root.saveWindowState(); Qt.quit() }
                                background: Rectangle {
                                    radius: Theme.radiusSmall
                                    color: parent.hovered ? Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.15) : Theme.bgInput
                                }
                                contentItem: Label {
                                    text: parent.text; font: parent.font
                                    color: Theme.danger
                                    horizontalAlignment: Text.AlignHCenter
                                }
                            }

                            Button {
                                Layout.fillWidth: true
                                text: "Cancel"
                                font.pixelSize: Theme.fontSizeSmall
                                onClicked: exitDialog.close()
                                background: Rectangle { color: "transparent" }
                                contentItem: Label {
                                    text: parent.text; font: parent.font
                                    color: Theme.textMuted
                                    horizontalAlignment: Text.AlignHCenter
                                }
                            }
                        }
                    }
                }
            }

            // Divider
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.divider
            }

            // Search
            TextField {
                id: searchField
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacingSmall
                Layout.rightMargin: Theme.spacingSmall
                Layout.topMargin: Theme.spacingSmall
                Layout.bottomMargin: Theme.spacingSmall
                placeholderText: "🔍  Search conversations..."
                placeholderTextColor: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textPrimary
                visible: !sidebar.squeezed
                background: Rectangle {
                    radius: Theme.radiusSmall
                    color: Theme.bgInput
                    border.color: searchField.activeFocus ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.4) : "transparent"
                    border.width: 1
                    Behavior on border.color { ColorAnimation { duration: Theme.animNormal } }
                }
                padding: 10
            }

            // Conversation list
            ListView {
                id: convListView
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: conversationModel
                clip: true
                currentIndex: sidebar.selectedIndex
                boundsBehavior: Flickable.StopAtBounds

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                    width: 4
                    contentItem: Rectangle {
                        radius: 2
                        color: Theme.textMuted
                        opacity: 0.3
                    }
                }

                delegate: ConversationItem {
                    width: convListView.width
                    selected: index === sidebar.selectedIndex
                    filterText: searchField.text
                    squeezed: sidebar.squeezed
                    notificationLevel: model.notificationLevel

                    onClicked: {
                        sidebar.selectedIndex = index
                        sidebar.conversationSelected(token, displayName, participantUserId, conversationType, userStatus)
                    }
                }

                // Loading — only show on initial load when list is empty
                BusyIndicator {
                    anchors.centerIn: parent
                    running: conversationModel.loading && convListView.count === 0
                    visible: running
                    palette.dark: Theme.accent
                }

                // Empty state
                Column {
                    anchors.centerIn: parent
                    spacing: Theme.spacingSmall
                    visible: !conversationModel.loading && convListView.count === 0

                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "📭"
                        font.pixelSize: 32
                    }
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "No conversations"
                        font.pixelSize: Theme.fontSizeNormal
                        color: Theme.textSecondary
                    }
                }
            }

            // Manual squeeze toggle
            Rectangle {
                Layout.fillWidth: true
                height: 28
                color: "transparent"

                Label {
                    anchors.centerIn: parent
                    text: sidebar.squeezed ? "\u276F" : "\u276E"  // ❯ / ❮
                    font.pixelSize: 11
                    color: toggleArea.containsMouse ? Theme.textSecondary : Theme.textMuted
                    Behavior on color { ColorAnimation { duration: Theme.animFast } }
                }

                MouseArea {
                    id: toggleArea
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    hoverEnabled: true
                    onClicked: sidebar.squeezed = !sidebar.squeezed
                }
            }
        }
    }
}
