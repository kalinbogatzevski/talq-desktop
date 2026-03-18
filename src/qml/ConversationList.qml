import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

/**
 * Telegram-style conversation sidebar.
 * Shows conversations sorted by favorites + activity, with unread badges.
 */
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

            // Header bar
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 56
                color: Theme.bgSecondary

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16

                    Label {
                        text: auth.displayName || "Talk"
                        font.pixelSize: Theme.fontSizeLarge
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    // Refresh button
                    ToolButton {
                        text: "↻"
                        font.pixelSize: 18
                        onClicked: conversationModel.refresh()
                        contentItem: Text {
                            text: parent.text
                            color: Theme.textSecondary
                            font: parent.font
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: 20
                            color: parent.hovered ? Theme.bgHover : "transparent"
                        }
                    }
                }
            }

            // Search bar
            TextField {
                id: searchField
                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                Layout.topMargin: 4
                Layout.bottomMargin: 4
                placeholderText: "Search..."
                placeholderTextColor: Theme.textSecondary
                font.pixelSize: Theme.fontSizeNormal
                color: Theme.textPrimary
                background: Rectangle {
                    radius: Theme.radiusNormal
                    color: Theme.bgInput
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

                delegate: ConversationItem {
                    width: convListView.width
                    selected: index === sidebar.selectedIndex
                    filterText: searchField.text

                    onClicked: {
                        sidebar.selectedIndex = index
                        sidebar.conversationSelected(token, displayName)
                    }
                }

                // Loading indicator
                BusyIndicator {
                    anchors.centerIn: parent
                    running: conversationModel.loading
                    visible: running
                }
            }
        }
    }
}
