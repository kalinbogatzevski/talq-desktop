import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import TalkQt

Window {
    id: settingsDialog
    title: "Settings"
    width: 400
    height: 320
    minimumWidth: 350
    minimumHeight: 280
    color: Theme.bgPrimary
    modality: Qt.ApplicationModal
    visible: false

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
