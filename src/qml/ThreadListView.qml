import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: threadListRoot
    signal threadSelected(int threadId, string title)

    Rectangle {
        anchors.fill: parent
        color: Theme.bgSecondary

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Header
            Rectangle {
                Layout.fillWidth: true
                height: Theme.headerHeight
                color: Theme.bgSurface

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingLarge
                    anchors.rightMargin: Theme.spacingNormal

                    Label {
                        text: "Topics"
                        font.pixelSize: Theme.fontSizeLarge
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                        Layout.fillWidth: true
                    }

                    Button {
                        text: "+"
                        flat: true
                        font.pixelSize: Theme.fontSizeLarge
                        font.weight: Font.Bold
                        implicitWidth: 36
                        implicitHeight: 36

                        contentItem: Label {
                            text: parent.text
                            font: parent.font
                            color: Theme.accent
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        background: Rectangle {
                            radius: Theme.radiusSmall
                            color: parent.hovered ? Theme.bgHover : "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.animFast } }
                        }

                        onClicked: {
                            // Placeholder for new topic creation
                        }
                    }
                }

                // Bottom divider
                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 1
                    color: Theme.divider
                }
            }

            // Thread list
            ListView {
                id: threadList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: threadModel

                delegate: ThreadItem {
                    width: threadList.width

                    onClicked: {
                        threadListRoot.threadSelected(threadId, title)
                    }
                }

                // Empty state
                Column {
                    anchors.centerIn: parent
                    spacing: Theme.spacingNormal
                    visible: !threadModel.loading && threadList.count === 0

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 56; height: 56; radius: 28
                        color: Theme.bgSurface
                        Label {
                            anchors.centerIn: parent
                            text: "\uD83D\uDCAC"  // 💬
                            font.pixelSize: 24
                        }
                    }
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "No topics yet"
                        font.pixelSize: Theme.fontSizeNormal; font.weight: Font.DemiBold
                        color: Theme.textPrimary
                    }
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "Reply to a message to start a thread"
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.textSecondary
                    }
                }
            }

            // Loading indicator
            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Theme.spacingLarge
                Layout.bottomMargin: Theme.spacingLarge
                running: threadModel.loading
                visible: running
            }
        }
    }
}
