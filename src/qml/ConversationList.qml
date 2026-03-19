import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: sidebar

    signal conversationSelected(string token, string name, string odataUserId, int convType, string userStatus)

    property int selectedIndex: -1

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
                    anchors.leftMargin: Theme.spacingLarge
                    anchors.rightMargin: Theme.spacingNormal

                    Item {
                        width: 32
                        height: 32

                        // User avatar
                        Image {
                            id: headerAvatar
                            anchors.fill: parent
                            source: auth.userId.length > 0 ? "image://avatar/" + auth.userId : ""
                            sourceSize: Qt.size(32, 32)
                            visible: status === Image.Ready
                            fillMode: Image.PreserveAspectFit
                        }

                        // Fallback
                        Rectangle {
                            anchors.fill: parent
                            radius: 16
                            visible: headerAvatar.status !== Image.Ready
                            color: Theme.accent

                            Label {
                                anchors.centerIn: parent
                                text: auth.displayName.length > 0 ? auth.displayName[0].toUpperCase() : "?"
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                color: "white"
                            }
                        }

                        // Connection status LED (green = push live, amber = polling, red = disconnected)
                        Rectangle {
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            width: 10; height: 10; radius: 5
                            color: pushClient.connected ? Theme.online
                                 : messageModel.connected ? Theme.warning
                                 : Theme.danger
                            border.color: Theme.bgSecondary
                            border.width: 2

                            SequentialAnimation on opacity {
                                loops: (!pushClient.connected && !messageModel.connected) ? Animation.Infinite : 0
                                running: !pushClient.connected && !messageModel.connected
                                NumberAnimation { from: 1.0; to: 0.3; duration: 800 }
                                NumberAnimation { from: 0.3; to: 1.0; duration: 800 }
                            }

                            ToolTip.visible: ledHover.hovered
                            ToolTip.text: pushClient.connected ? "Push connected (real-time)"
                                        : messageModel.connected ? "Polling (delayed)"
                                        : "Disconnected"
                            ToolTip.delay: 300

                            HoverHandler { id: ledHover }
                        }
                    }

                    Label {
                        text: auth.displayName || "Talk"
                        font.pixelSize: Theme.fontSizeLarge
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    ToolButton {
                        width: 36
                        height: 36
                        onClicked: Theme.darkMode = !Theme.darkMode
                        ToolTip.visible: hovered
                        ToolTip.text: Theme.darkMode ? "Switch to light mode" : "Switch to dark mode"
                        ToolTip.delay: 500
                        contentItem: Label {
                            text: Theme.darkMode ? "\u2600" : "\u263E"  // ☀ / ☾
                            font.pixelSize: 16
                            color: parent.hovered ? Theme.accent : Theme.textSecondary
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            Behavior on color { ColorAnimation { duration: Theme.animFast } }
                        }
                        background: Rectangle {
                            radius: 18
                            color: parent.hovered ? Theme.bgHover : "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.animFast } }
                        }
                    }

                    ToolButton {
                        width: 36
                        height: 36
                        onClicked: conversationModel.refresh()
                        ToolTip.visible: hovered
                        ToolTip.text: "Refresh conversations"
                        ToolTip.delay: 500
                        contentItem: Label {
                            text: "\u21BB"  // ↻ refresh arrow
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                            color: parent.hovered ? Theme.textPrimary : Theme.textSecondary
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            Behavior on color { ColorAnimation { duration: Theme.animFast } }
                        }
                        background: Rectangle {
                            radius: 18
                            color: parent.hovered ? Theme.bgHover : "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.animFast } }
                        }
                    }

                    ToolButton {
                        width: 36
                        height: 36
                        onClicked: exitDialog.open()
                        ToolTip.visible: hovered
                        ToolTip.text: "Exit"
                        ToolTip.delay: 500
                        contentItem: Label {
                            text: "\u23FB"  // ⏻ power symbol
                            font.pixelSize: 16
                            color: parent.hovered ? Theme.danger : Theme.textSecondary
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            Behavior on color { ColorAnimation { duration: Theme.animFast } }
                        }
                        background: Rectangle {
                            radius: 18
                            color: parent.hovered ? Theme.bgHover : "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.animFast } }
                        }
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
        }
    }
}
