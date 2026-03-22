import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.settings
import TalkQt

Window {
    id: settingsDialog
    title: "Settings"
    width: 480
    height: 520
    minimumWidth: 400
    minimumHeight: 440
    color: Theme.bgPrimary
    modality: Qt.ApplicationModal
    visible: false

    // --- Inline components for repeated patterns ---

    // Uppercase section header (used 8 times across tabs)
    component SectionHeader: Label {
        font.pixelSize: 10
        font.weight: Font.DemiBold
        font.letterSpacing: 1
        color: Theme.textSecondary
        opacity: 0.7
    }

    // Toggle button for option groups (used 7 times across tabs)
    component OptionButton: Button {
        palette.button: checked ? Theme.accent : Theme.bgSurface
        palette.buttonText: checked ? "#000000" : Theme.textSecondary
        font.pixelSize: Theme.fontSizeSmall
    }

    // --- Persistence blocks ---
    Settings {
        id: videoSettings
        category: "Video"
        property int resolution: 0  // 0=1080p, 1=720p
    }

    Settings {
        id: notifSettings
        category: "Notifications"
        property bool enabled: true
        property string style: "popup"     // "popup" or "windows"
        property string soundMode: "internal"  // "internal", "system", "none"
    }

    // Push saved notification settings to NotificationManager on load
    Component.onCompleted: {
        deviceManager.refresh()
        notifications.notificationsEnabled = notifSettings.enabled
        notifications.notifStyle = notifSettings.style
        notifications.soundMode = notifSettings.soundMode
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TabBar {
            id: tabBar
            Layout.fillWidth: true
            background: Rectangle { color: Theme.bgSecondary }

            TabButton {
                text: "Audio && Video"
                palette.button: Theme.bgSecondary
                palette.buttonText: tabBar.currentIndex === 0 ? Theme.accent : Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
            }
            TabButton {
                text: "Notifications"
                palette.button: Theme.bgSecondary
                palette.buttonText: tabBar.currentIndex === 1 ? Theme.accent : Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
            }
            TabButton {
                text: "General"
                palette.button: Theme.bgSecondary
                palette.buttonText: tabBar.currentIndex === 2 ? Theme.accent : Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
            }
            TabButton {
                text: "Account"
                palette.button: Theme.bgSecondary
                palette.buttonText: tabBar.currentIndex === 3 ? Theme.accent : Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
            }
        }

        // Active tab indicator line
        Rectangle {
            Layout.fillWidth: true
            height: 2
            color: Theme.accent
        }

        StackLayout {
            currentIndex: tabBar.currentIndex
            Layout.fillWidth: true
            Layout.fillHeight: true

            // ============ Tab 1: Audio & Video ============
            ScrollView {
                ColumnLayout {
                    width: settingsDialog.width - 40
                    spacing: 12
                    anchors.margins: 20

                    Item { height: 20 }

                    SectionHeader { text: "MICROPHONE" }
                    ComboBox {
                        Layout.fillWidth: true
                        model: deviceManager.audioInputNames
                        currentIndex: deviceManager.selectedAudioInput >= 0 ? deviceManager.selectedAudioInput : 0
                        onActivated: (index) => deviceManager.selectedAudioInput = index
                        enabled: deviceManager.audioInputNames.length > 0
                        palette.window: Theme.bgSurface
                        palette.text: Theme.textPrimary
                        palette.buttonText: Theme.textPrimary
                    }
                    Label {
                        visible: deviceManager.audioInputNames.length === 0
                        text: "No microphones found"
                        color: Theme.textSecondary; font.pixelSize: 11
                    }

                    SectionHeader { text: "SPEAKER"; Layout.topMargin: 4 }
                    ComboBox {
                        Layout.fillWidth: true
                        model: deviceManager.audioOutputNames
                        currentIndex: deviceManager.selectedAudioOutput >= 0 ? deviceManager.selectedAudioOutput : 0
                        onActivated: (index) => deviceManager.selectedAudioOutput = index
                        enabled: deviceManager.audioOutputNames.length > 0
                        palette.window: Theme.bgSurface
                        palette.text: Theme.textPrimary
                        palette.buttonText: Theme.textPrimary
                    }
                    Label {
                        visible: deviceManager.audioOutputNames.length === 0
                        text: "No speakers found"
                        color: Theme.textSecondary; font.pixelSize: 11
                    }

                    SectionHeader { text: "CAMERA"; Layout.topMargin: 4 }
                    ComboBox {
                        Layout.fillWidth: true
                        model: deviceManager.videoInputNames
                        currentIndex: deviceManager.selectedVideoInput >= 0 ? deviceManager.selectedVideoInput : 0
                        onActivated: (index) => deviceManager.selectedVideoInput = index
                        enabled: deviceManager.videoInputNames.length > 0
                        palette.window: Theme.bgSurface
                        palette.text: Theme.textPrimary
                        palette.buttonText: Theme.textPrimary
                    }
                    Label {
                        visible: deviceManager.videoInputNames.length === 0
                        text: "No cameras found"
                        color: Theme.textSecondary; font.pixelSize: 11
                    }

                    SectionHeader { text: "VIDEO QUALITY"; Layout.topMargin: 4 }
                    Row {
                        spacing: 8
                        OptionButton {
                            text: "Full HD (1080p)"
                            checked: videoSettings.resolution === 0
                            onClicked: videoSettings.resolution = 0
                        }
                        OptionButton {
                            text: "HD (720p)"
                            checked: videoSettings.resolution === 1
                            onClicked: videoSettings.resolution = 1
                        }
                    }
                    Label {
                        text: "Changes apply to next call"
                        color: Theme.textSecondary; font.pixelSize: 11
                        visible: callManager.state !== CallManager.Idle
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider; Layout.topMargin: 8 }
                    RowLayout {
                        Layout.fillWidth: true
                        Item { Layout.fillWidth: true }
                        Button {
                            text: "Refresh Devices"
                            onClicked: deviceManager.refresh()
                            palette.button: Theme.bgSurface
                            palette.buttonText: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }

                    Item { height: 20 }
                }
            }

            // ============ Tab 2: Notifications ============
            ScrollView {
                ColumnLayout {
                    width: settingsDialog.width - 40
                    spacing: 12
                    anchors.margins: 20

                    Item { height: 20 }

                    SectionHeader { text: "DESKTOP NOTIFICATIONS" }
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: "Enable notifications"
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeNormal
                            Layout.fillWidth: true
                        }
                        Switch {
                            checked: notifSettings.enabled
                            onToggled: {
                                notifSettings.enabled = checked
                                notifications.notificationsEnabled = checked
                            }
                            palette.highlight: Theme.accent
                        }
                    }

                    SectionHeader { text: "NOTIFICATION STYLE"; Layout.topMargin: 4 }
                    Row {
                        spacing: 8
                        OptionButton {
                            text: "In-app popup"
                            checked: notifSettings.style === "popup"
                            onClicked: { notifSettings.style = "popup"; notifications.notifStyle = "popup" }
                        }
                        OptionButton {
                            text: "Windows toast"
                            checked: notifSettings.style === "windows"
                            onClicked: { notifSettings.style = "windows"; notifications.notifStyle = "windows" }
                        }
                    }

                    SectionHeader { text: "SOUND"; Layout.topMargin: 4 }
                    Row {
                        spacing: 8
                        OptionButton {
                            text: "TalQ chime"
                            checked: notifSettings.soundMode === "internal"
                            onClicked: { notifSettings.soundMode = "internal"; notifications.soundMode = "internal" }
                        }
                        OptionButton {
                            text: "System sound"
                            checked: notifSettings.soundMode === "system"
                            onClicked: { notifSettings.soundMode = "system"; notifications.soundMode = "system" }
                        }
                        OptionButton {
                            text: "None"
                            checked: notifSettings.soundMode === "none"
                            onClicked: { notifSettings.soundMode = "none"; notifications.soundMode = "none" }
                        }
                    }

                    Item { height: 8 }
                    Rectangle {
                        Layout.fillWidth: true
                        height: hintLabel.implicitHeight + 24
                        radius: 6
                        color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.1)
                        border.width: 0

                        Rectangle {
                            width: 3; height: parent.height
                            color: Theme.accent; radius: 2
                        }

                        Label {
                            id: hintLabel
                            anchors.fill: parent
                            anchors.leftMargin: 14; anchors.rightMargin: 12
                            anchors.topMargin: 12; anchors.bottomMargin: 12
                            text: "To mute individual conversations, right-click on them in the sidebar."
                            color: Theme.textSecondary
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }
                    }

                    Item { height: 20 }
                }
            }

            // ============ Tab 3: General ============
            ScrollView {
                ColumnLayout {
                    width: settingsDialog.width - 40
                    spacing: 12
                    anchors.margins: 20

                    Item { height: 20 }

                    SectionHeader { text: "STARTUP" }

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: "Start with Windows"
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeNormal
                            Layout.fillWidth: true
                        }
                        Switch {
                            checked: generalSettings.autoStart
                            onToggled: {
                                generalSettings.autoStart = checked
                                appSettings.setAutoStart(checked)
                            }
                            palette.highlight: Theme.accent
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: "Start minimized to tray"
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeNormal
                            Layout.fillWidth: true
                        }
                        Switch {
                            checked: generalSettings.startMinimized
                            onToggled: generalSettings.startMinimized = checked
                            palette.highlight: Theme.accent
                        }
                    }

                    SectionHeader { text: "BEHAVIOR"; Layout.topMargin: 8 }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout {
                            spacing: 2
                            Layout.fillWidth: true
                            Label {
                                text: "Close to tray"
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontSizeNormal
                            }
                            Label {
                                text: "Minimize to tray instead of quitting"
                                color: Theme.textSecondary
                                font.pixelSize: 11
                            }
                        }
                        Switch {
                            checked: generalSettings.closeToTray
                            onToggled: generalSettings.closeToTray = checked
                            palette.highlight: Theme.accent
                        }
                    }

                    Item { height: 20 }
                }
            }

            // ============ Tab 4: Account ============
            ScrollView {
                ColumnLayout {
                    width: settingsDialog.width - 40
                    spacing: 12
                    anchors.margins: 20

                    Item { height: 20 }

                    // Profile card
                    RowLayout {
                        spacing: 14
                        Rectangle {
                            width: 52; height: 52; radius: 26
                            color: Theme.accent
                            clip: true
                            Image {
                                id: avatarImage
                                anchors.fill: parent
                                source: auth.userId ? "image://avatar/" + auth.userId + "/52" : ""
                                fillMode: Image.PreserveAspectCrop
                                visible: status === Image.Ready
                            }
                            Label {
                                anchors.centerIn: parent
                                text: auth.displayName.length > 0 ? auth.displayName.charAt(0).toUpperCase() : "?"
                                font.pixelSize: 22; font.weight: Font.DemiBold
                                color: "#000000"
                                visible: !avatarImage.visible
                            }
                        }
                        ColumnLayout {
                            spacing: 2
                            Label {
                                text: auth.displayName
                                color: Theme.textPrimary
                                font.pixelSize: 14; font.weight: Font.Medium
                            }
                            Label {
                                text: auth.serverUrl.replace(/^https?:\/\//, "")
                                color: Theme.textSecondary
                                font.pixelSize: 12
                            }
                        }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider; Layout.topMargin: 4 }

                    SectionHeader { text: "SERVER" }
                    Rectangle {
                        Layout.fillWidth: true
                        height: serverUrlLabel.implicitHeight + 16
                        radius: 6; color: Theme.bgSurface
                        Label {
                            id: serverUrlLabel
                            anchors.fill: parent; anchors.margins: 8
                            text: auth.serverUrl
                            color: Theme.textSecondary
                            font.pixelSize: 12
                            elide: Text.ElideMiddle
                        }
                    }

                    SectionHeader { text: "NEXTCLOUD"; Layout.topMargin: 4 }
                    Row {
                        spacing: 20
                        Label {
                            text: "Version: " + auth.nextcloudVersion
                            color: Theme.textSecondary; font.pixelSize: 11
                        }
                        Label {
                            text: "Talk: " + auth.talkVersion
                            color: Theme.textSecondary; font.pixelSize: 11
                        }
                    }

                    Item { Layout.fillHeight: true }

                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: "TalQ " + Qt.application.version
                            color: Theme.textSecondary
                            font.pixelSize: 11
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            text: "Log out"
                            onClicked: {
                                auth.logout()
                                settingsDialog.visible = false
                            }
                            palette.button: "#e07060"
                            palette.buttonText: "white"
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }

                    Item { height: 10 }
                }
            }
        }
    }
}
