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

    Settings {
        id: generalSettings
        category: "General"
        property bool autoStart: false
        property bool startMinimized: false
        property bool closeToTray: true
    }

    // Push saved notification settings to NotificationManager on load
    Component.onCompleted: {
        deviceManager.refresh()
        notifications.notificationsEnabled = notifSettings.enabled
        notifications.notifStyle = notifSettings.style
        notifications.soundMode = notifSettings.soundMode
        // Sync auto-start toggle with registry
        generalSettings.autoStart = appSettings.isAutoStart()
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

                    // --- Microphone ---
                    Label {
                        text: "MICROPHONE"
                        font.pixelSize: 10; font.weight: Font.DemiBold
                        color: Theme.textSecondary; opacity: 0.7
                        font.letterSpacing: 1
                    }
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

                    // --- Speaker ---
                    Label {
                        text: "SPEAKER"
                        font.pixelSize: 10; font.weight: Font.DemiBold
                        color: Theme.textSecondary; opacity: 0.7
                        font.letterSpacing: 1
                        Layout.topMargin: 4
                    }
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

                    // --- Camera ---
                    Label {
                        text: "CAMERA"
                        font.pixelSize: 10; font.weight: Font.DemiBold
                        color: Theme.textSecondary; opacity: 0.7
                        font.letterSpacing: 1
                        Layout.topMargin: 4
                    }
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

                    // --- Video Quality ---
                    Label {
                        text: "VIDEO QUALITY"
                        font.pixelSize: 10; font.weight: Font.DemiBold
                        color: Theme.textSecondary; opacity: 0.7
                        font.letterSpacing: 1
                        Layout.topMargin: 4
                    }
                    Row {
                        spacing: 8
                        Button {
                            text: "Full HD (1080p)"
                            checked: videoSettings.resolution === 0
                            onClicked: videoSettings.resolution = 0
                            palette.button: checked ? Theme.accent : Theme.bgSurface
                            palette.buttonText: checked ? "#000000" : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                        Button {
                            text: "HD (720p)"
                            checked: videoSettings.resolution === 1
                            onClicked: videoSettings.resolution = 1
                            palette.button: checked ? Theme.accent : Theme.bgSurface
                            palette.buttonText: checked ? "#000000" : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }
                    Label {
                        text: "Changes apply to next call"
                        color: Theme.textSecondary; font.pixelSize: 11
                        visible: callManager.state !== CallManager.Idle
                    }

                    // --- Refresh ---
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

                    // --- Enable ---
                    Label {
                        text: "DESKTOP NOTIFICATIONS"
                        font.pixelSize: 10; font.weight: Font.DemiBold
                        color: Theme.textSecondary; opacity: 0.7
                        font.letterSpacing: 1
                    }
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

                    // --- Style ---
                    Label {
                        text: "NOTIFICATION STYLE"
                        font.pixelSize: 10; font.weight: Font.DemiBold
                        color: Theme.textSecondary; opacity: 0.7
                        font.letterSpacing: 1
                        Layout.topMargin: 4
                    }
                    Row {
                        spacing: 8
                        Button {
                            text: "In-app popup"
                            checked: notifSettings.style === "popup"
                            onClicked: { notifSettings.style = "popup"; notifications.notifStyle = "popup" }
                            palette.button: checked ? Theme.accent : Theme.bgSurface
                            palette.buttonText: checked ? "#000000" : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                        Button {
                            text: "Windows toast"
                            checked: notifSettings.style === "windows"
                            onClicked: { notifSettings.style = "windows"; notifications.notifStyle = "windows" }
                            palette.button: checked ? Theme.accent : Theme.bgSurface
                            palette.buttonText: checked ? "#000000" : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }

                    // --- Sound ---
                    Label {
                        text: "SOUND"
                        font.pixelSize: 10; font.weight: Font.DemiBold
                        color: Theme.textSecondary; opacity: 0.7
                        font.letterSpacing: 1
                        Layout.topMargin: 4
                    }
                    Row {
                        spacing: 8
                        Button {
                            text: "TalQ chime"
                            checked: notifSettings.soundMode === "internal"
                            onClicked: { notifSettings.soundMode = "internal"; notifications.soundMode = "internal" }
                            palette.button: checked ? Theme.accent : Theme.bgSurface
                            palette.buttonText: checked ? "#000000" : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                        Button {
                            text: "System sound"
                            checked: notifSettings.soundMode === "system"
                            onClicked: { notifSettings.soundMode = "system"; notifications.soundMode = "system" }
                            palette.button: checked ? Theme.accent : Theme.bgSurface
                            palette.buttonText: checked ? "#000000" : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                        Button {
                            text: "None"
                            checked: notifSettings.soundMode === "none"
                            onClicked: { notifSettings.soundMode = "none"; notifications.soundMode = "none" }
                            palette.button: checked ? Theme.accent : Theme.bgSurface
                            palette.buttonText: checked ? "#000000" : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }

                    // --- Hint ---
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

                    Label {
                        text: "STARTUP"
                        font.pixelSize: 10; font.weight: Font.DemiBold
                        color: Theme.textSecondary; opacity: 0.7
                        font.letterSpacing: 1
                    }

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

                    // --- Behavior ---
                    Label {
                        text: "BEHAVIOR"
                        font.pixelSize: 10; font.weight: Font.DemiBold
                        color: Theme.textSecondary; opacity: 0.7
                        font.letterSpacing: 1
                        Layout.topMargin: 8
                    }

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

                    // --- Profile card ---
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

                    // --- Server info ---
                    Label {
                        text: "SERVER"
                        font.pixelSize: 10; font.weight: Font.DemiBold
                        color: Theme.textSecondary; opacity: 0.7
                        font.letterSpacing: 1
                    }
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

                    Label {
                        text: "NEXTCLOUD"
                        font.pixelSize: 10; font.weight: Font.DemiBold
                        color: Theme.textSecondary; opacity: 0.7
                        font.letterSpacing: 1
                        Layout.topMargin: 4
                    }
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

                    // --- Footer: version + logout ---
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
