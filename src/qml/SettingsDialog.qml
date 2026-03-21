import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.settings
import TalkQt

Window {
    id: settingsDialog
    title: "Settings"
    width: 400
    height: 520
    minimumWidth: 350
    minimumHeight: 440
    color: Theme.bgPrimary
    modality: Qt.ApplicationModal
    visible: false

    Settings {
        id: videoSettings
        category: "Video"
        property int resolution: 0
    }

    Component.onCompleted: {
        deviceManager.refresh()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 16

        Label {
            text: "Audio Settings"
            font.pixelSize: 16; font.weight: Font.DemiBold
            color: Theme.textPrimary
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }

        // Microphone
        ColumnLayout {
            spacing: 4
            Label {
                text: "Microphone"
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondary
            }
            ComboBox {
                id: micCombo
                Layout.fillWidth: true
                model: deviceManager.audioInputNames
                currentIndex: deviceManager.selectedAudioInput >= 0 ? deviceManager.selectedAudioInput : 0
                onActivated: function(index) { deviceManager.selectedAudioInput = index }
                palette.window: Theme.bgSurface
                palette.text: Theme.textPrimary
                palette.buttonText: Theme.textPrimary
            }
        }

        // Speaker
        ColumnLayout {
            spacing: 4
            Label {
                text: "Speaker"
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondary
            }
            ComboBox {
                id: speakerCombo
                Layout.fillWidth: true
                model: deviceManager.audioOutputNames
                currentIndex: deviceManager.selectedAudioOutput >= 0 ? deviceManager.selectedAudioOutput : 0
                onActivated: function(index) { deviceManager.selectedAudioOutput = index }
                palette.window: Theme.bgSurface
                palette.text: Theme.textPrimary
                palette.buttonText: Theme.textPrimary
            }
        }

        // Camera
        Text {
            text: "Camera"
            color: Theme.textPrimary
            font.pixelSize: 14
            font.bold: true
            Layout.topMargin: 16
        }

        ComboBox {
            Layout.fillWidth: true
            model: deviceManager.videoInputNames
            currentIndex: deviceManager.selectedVideoInput
            onActivated: (index) => deviceManager.selectedVideoInput = index
            enabled: deviceManager.videoInputNames.length > 0
            palette.window: Theme.bgSurface
            palette.text: Theme.textPrimary
            palette.buttonText: Theme.textPrimary
        }

        Text {
            visible: deviceManager.videoInputNames.length === 0
            text: "No cameras found"
            color: Theme.textSecondary
            font.pixelSize: 12
        }

        // Video Quality
        Text {
            text: "Video Quality"
            color: Theme.textPrimary
            font.pixelSize: 14
            font.bold: true
            Layout.topMargin: 12
        }

        ComboBox {
            Layout.fillWidth: true
            model: ["Full HD (1080p)", "HD (720p)"]
            currentIndex: videoSettings.resolution
            onActivated: (index) => videoSettings.resolution = index
            palette.window: Theme.bgSurface
            palette.text: Theme.textPrimary
            palette.buttonText: Theme.textPrimary
        }

        Text {
            text: "Changes apply to next call"
            color: Theme.textSecondary
            font.pixelSize: 11
            Layout.alignment: Qt.AlignHCenter
            visible: callManager.state !== CallManager.Idle
        }

        // Refresh button
        Button {
            text: "Refresh devices"
            onClicked: deviceManager.refresh()
            palette.button: Theme.bgSurface
            palette.buttonText: Theme.textPrimary
        }

        Item { Layout.fillHeight: true }

        // Close
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Button {
                text: "Close"
                onClicked: settingsDialog.visible = false
                palette.button: Theme.accent
                palette.buttonText: "white"
            }
        }
    }
}
