import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: sidebar

    signal conversationSelected(string token, string name)

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

                    Image {
                        source: "qrc:/logo.png"
                        width: 24
                        height: 24
                        sourceSize: Qt.size(48, 48)
                        fillMode: Image.PreserveAspectFit
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
                        onClicked: auth.logout()
                        ToolTip.visible: hovered
                        ToolTip.text: "Log out"
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
                        sidebar.conversationSelected(token, displayName)
                    }
                }

                // Loading
                BusyIndicator {
                    anchors.centerIn: parent
                    running: conversationModel.loading
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
