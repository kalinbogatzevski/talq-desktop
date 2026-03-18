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

                    Label {
                        text: "💬"
                        font.pixelSize: 18
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
                        onClicked: conversationModel.refresh()
                        contentItem: Label {
                            text: "🔄"
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
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
                        contentItem: Label {
                            text: "🚪"
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
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
