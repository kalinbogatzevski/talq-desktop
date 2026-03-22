import QtQuick
import TalkQt
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: threadListRoot
    signal threadSelected(int threadId, string title)
    signal backToChats()

    property string groupName: ""
    property bool creating: false
    property int selectedThreadId: -1

    property string createToken: ""

    Timer {
        id: refreshAfterCreate
        interval: 2000
        onTriggered: {
            if (threadModel.conversationToken === threadListRoot.createToken)
                threadModel.refresh()
        }
    }

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
                    anchors.leftMargin: Theme.spacingSmall
                    anchors.rightMargin: Theme.spacingNormal
                    spacing: 2

                    TqIconButton {
                        iconText: "\u2190"
                        size: 28
                        iconColor: Theme.accent
                        onClicked: threadListRoot.backToChats()
                    }

                    Label {
                        text: threadListRoot.groupName || "Topics"
                        font.pixelSize: Theme.fontSizeLarge
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }

                    // + button moved to bottom of list
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

            // Inline topic creation input
            TextField {
                id: createInput
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacingSmall
                Layout.rightMargin: Theme.spacingSmall
                Layout.topMargin: Theme.spacingSmall
                visible: threadListRoot.creating
                placeholderText: "Topic name..."
                placeholderTextColor: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textPrimary
                background: Rectangle {
                    radius: Theme.radiusSmall
                    color: Theme.bgInput
                    border.color: createInput.activeFocus ? Theme.accent : "transparent"
                    border.width: 1
                    Behavior on border.color { ColorAnimation { duration: Theme.animFast } }
                }
                padding: 8

                onVisibleChanged: if (visible) forceActiveFocus()

                Keys.onReturnPressed: {
                    var name = text.trim()
                    if (name.length > 0 && name.length <= 128) {
                        threadListRoot.createToken = threadModel.conversationToken
                        messageModel.createTopic(name)
                        text = ""
                        threadListRoot.creating = false
                        refreshAfterCreate.restart()
                    }
                }
                Keys.onEscapePressed: {
                    text = ""
                    threadListRoot.creating = false
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
                    selected: threadId === threadListRoot.selectedThreadId

                    onClicked: {
                        threadListRoot.selectedThreadId = threadId
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

            // New Topic button at the bottom
            Rectangle {
                Layout.fillWidth: true
                height: 44
                color: newTopicArea.containsMouse ? Theme.bgHover : "transparent"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingLarge
                    anchors.rightMargin: Theme.spacingLarge
                    spacing: Theme.spacingSmall

                    Label {
                        text: "+"
                        font.pixelSize: 18
                        font.weight: Font.Bold
                        color: Theme.accent
                    }
                    Label {
                        text: "New Topic"
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.accent
                        Layout.fillWidth: true
                    }
                }

                MouseArea {
                    id: newTopicArea
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    hoverEnabled: true
                    onClicked: threadListRoot.creating = true
                }

                Rectangle {
                    anchors.top: parent.top
                    width: parent.width; height: 1
                    color: Theme.divider
                }
            }
        }
    }
}
