# Settings Dialog Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the minimal SettingsDialog with a full tabbed settings dialog (Audio/Video, Notifications, General, Account) with all settings persisted across restarts.

**Architecture:** Rewrite `SettingsDialog.qml` with `TabBar` + `StackLayout`. Add QSettings persistence to `MediaDeviceManager` for device names. Add `Qt.labs.settings` blocks for notification and general preferences. Add `AppSettings` C++ helper for Windows auto-start registry key. Per-conversation mute via context menu on `ConversationItem`.

**Tech Stack:** Qt6/QML, Qt.labs.settings, QSettings, GStreamer device monitor, Windows Registry API

**Spec:** `docs/superpowers/specs/2026-03-22-settings-dialog-design.md`

---

### Task 1: Add device persistence to MediaDeviceManager

**Files:**
- Modify: `src/core/MediaDeviceManager.h`
- Modify: `src/core/MediaDeviceManager.cpp`

- [ ] **Step 1: Add QSettings include and persistence methods to header**

In `src/core/MediaDeviceManager.h`, add `#include <QSettings>` after line 5, and add two public methods and a QSettings member:

```cpp
// After line 5:
#include <QSettings>

// After line 25 (after Q_INVOKABLE void refresh()):
    Q_INVOKABLE void saveDevices();
    void restoreDevices();

// After line 64 (new private member):
    QSettings m_settings;
```

- [ ] **Step 2: Implement saveDevices() and restoreDevices()**

In `src/core/MediaDeviceManager.cpp`, add after the `selectedOutputDeviceId()` method (after line 113):

```cpp
void MediaDeviceManager::saveDevices()
{
    m_settings.beginGroup("Devices");
    if (m_selectedInput >= 0 && m_selectedInput < m_audioInputs.size()) {
        m_settings.setValue("audioInputName", m_audioInputs[m_selectedInput].name);
        m_settings.setValue("audioInputId", m_audioInputs[m_selectedInput].id);
    }
    if (m_selectedOutput >= 0 && m_selectedOutput < m_audioOutputs.size()) {
        m_settings.setValue("audioOutputName", m_audioOutputs[m_selectedOutput].name);
        m_settings.setValue("audioOutputId", m_audioOutputs[m_selectedOutput].id);
    }
    if (m_selectedVideo >= 0 && m_selectedVideo < m_videoInputs.size()) {
        m_settings.setValue("videoInputName", m_videoInputs[m_selectedVideo].name);
        m_settings.setValue("videoInputId", m_videoInputs[m_selectedVideo].id);
    }
    m_settings.endGroup();
}

void MediaDeviceManager::restoreDevices()
{
    m_settings.beginGroup("Devices");
    auto matchDevice = [](const QVector<MediaDevice> &list, const QString &name, const QString &id) -> int {
        // Primary: match by name
        QVector<int> nameMatches;
        for (int i = 0; i < list.size(); ++i) {
            if (list[i].name == name)
                nameMatches.append(i);
        }
        if (nameMatches.size() == 1)
            return nameMatches.first();
        // Fallback: disambiguate by device ID
        if (nameMatches.size() > 1) {
            for (int idx : nameMatches) {
                if (list[idx].id == id)
                    return idx;
            }
            return nameMatches.first(); // multiple same-name, no ID match — pick first
        }
        return -1; // not found — stay on system default
    };

    QString inName = m_settings.value("audioInputName").toString();
    QString inId = m_settings.value("audioInputId").toString();
    if (!inName.isEmpty()) {
        int idx = matchDevice(m_audioInputs, inName, inId);
        if (idx >= 0) setSelectedAudioInput(idx);
    }

    QString outName = m_settings.value("audioOutputName").toString();
    QString outId = m_settings.value("audioOutputId").toString();
    if (!outName.isEmpty()) {
        int idx = matchDevice(m_audioOutputs, outName, outId);
        if (idx >= 0) setSelectedAudioOutput(idx);
    }

    QString vidName = m_settings.value("videoInputName").toString();
    QString vidId = m_settings.value("videoInputId").toString();
    if (!vidName.isEmpty()) {
        int idx = matchDevice(m_videoInputs, vidName, vidId);
        if (idx >= 0) setSelectedVideoInput(idx);
    }

    m_settings.endGroup();
    qDebug() << "MediaDeviceManager: restored devices — mic:" << m_selectedInput
             << "speaker:" << m_selectedOutput << "camera:" << m_selectedVideo;
}
```

- [ ] **Step 3: Call restoreDevices() after refresh(), and saveDevices() on selection change**

In `src/core/MediaDeviceManager.cpp`, at the end of `refresh()` (after `emit devicesChanged();` on line 74), add:

```cpp
    restoreDevices();
```

In each setter in `MediaDeviceManager.h` (lines 34-36), add `saveDevices()` call after emit:

```cpp
    void setSelectedAudioInput(int idx) { if (m_selectedInput != idx) { m_selectedInput = idx; emit selectedChanged(); saveDevices(); } }
    void setSelectedAudioOutput(int idx) { if (m_selectedOutput != idx) { m_selectedOutput = idx; emit selectedChanged(); saveDevices(); } }
    void setSelectedVideoInput(int idx) { if (m_selectedVideo != idx) { m_selectedVideo = idx; emit selectedChanged(); saveDevices(); } }
```

- [ ] **Step 4: Build and verify**

```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
cmake --build C:/build/talk-qt --target talq 2>&1 | tail -5
```

Expected: Builds with no errors.

- [ ] **Step 5: Commit**

```bash
cd "C:/Users/bogat/Desktop/My Projects/talk-desktop-qt"
git add src/core/MediaDeviceManager.h src/core/MediaDeviceManager.cpp
git commit -m "feat: persist device selection across restarts via QSettings"
```

---

### Task 2: Create AppSettings helper for auto-start registry

**Files:**
- Create: `src/core/AppSettings.h`
- Create: `src/core/AppSettings.cpp`
- Modify: `CMakeLists.txt:33-58` (add source file)
- Modify: `src/main.cpp:96-107` (register context property)

- [ ] **Step 1: Create AppSettings header**

Create `src/core/AppSettings.h`:

```cpp
#pragma once

#include <QObject>

class AppSettings : public QObject
{
    Q_OBJECT

public:
    explicit AppSettings(QObject *parent = nullptr);

    Q_INVOKABLE bool isAutoStart() const;
    Q_INVOKABLE void setAutoStart(bool enabled);
};
```

- [ ] **Step 2: Create AppSettings implementation**

Create `src/core/AppSettings.cpp`:

```cpp
#include "core/AppSettings.h"
#include <QCoreApplication>
#include <QSettings>
#include <QDebug>

AppSettings::AppSettings(QObject *parent)
    : QObject(parent)
{
}

bool AppSettings::isAutoStart() const
{
    QSettings reg("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                  QSettings::NativeFormat);
    return reg.contains(QCoreApplication::applicationName());
}

void AppSettings::setAutoStart(bool enabled)
{
    QSettings reg("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                  QSettings::NativeFormat);
    if (enabled) {
        QString path = QCoreApplication::applicationFilePath().replace('/', '\\');
        reg.setValue(QCoreApplication::applicationName(), "\"" + path + "\"");
        qDebug() << "AppSettings: auto-start enabled:" << path;
    } else {
        reg.remove(QCoreApplication::applicationName());
        qDebug() << "AppSettings: auto-start disabled";
    }
}
```

- [ ] **Step 3: Register in CMakeLists.txt and main.cpp**

In `CMakeLists.txt`, add after line 56 (`src/core/DebugMonitor.cpp`):

```cmake
    src/core/AppSettings.cpp
```

In `src/main.cpp`, add include after line 31 (`#include "core/DebugMonitor.h"`):

```cpp
#include "core/AppSettings.h"
```

Add instantiation after line 87 (`DebugMonitor debug;`):

```cpp
    AppSettings appSettings;
```

Add context property after line 107 (`setContextProperty("debugMonitor", ...)`):

```cpp
    engine.rootContext()->setContextProperty("appSettings", &appSettings);
```

- [ ] **Step 4: Build and verify**

```bash
cmake --build C:/build/talk-qt --target talq 2>&1 | tail -5
```

Expected: Builds with no errors.

- [ ] **Step 5: Commit**

```bash
git add src/core/AppSettings.h src/core/AppSettings.cpp CMakeLists.txt src/main.cpp
git commit -m "feat: add AppSettings helper for Windows auto-start registry"
```

---

### Task 3: Rewrite SettingsDialog with TabBar + StackLayout

**Files:**
- Modify: `src/qml/SettingsDialog.qml` (full rewrite)

- [ ] **Step 1: Rewrite SettingsDialog.qml**

Replace the entire contents of `src/qml/SettingsDialog.qml` with the tabbed dialog. This is the largest step — the complete QML file:

```qml
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

                    Item { height: 20 }  // top padding

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

                    Item { height: 20 }  // bottom padding
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
                                visible: !parent.children[0].visible  // fallback when avatar not loaded
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
```

- [ ] **Step 2: Build and verify**

```bash
rm -rf /c/build/talk-qt && cmake -B C:/build/talk-qt -S C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev && cmake --build C:/build/talk-qt --target talq 2>&1 | tail -5
```

Clean build needed because QML files changed (junction can cause stale detection — per CLAUDE.md pitfall).

Expected: Builds with no errors.

- [ ] **Step 3: Commit**

```bash
git add src/qml/SettingsDialog.qml
git commit -m "feat: rewrite SettingsDialog with tabbed layout (4 tabs)"
```

---

### Task 4: Wire General settings into Main.qml

**Files:**
- Modify: `src/qml/Main.qml:29-35` (close-to-tray behavior)
- Modify: `src/qml/Main.qml:37-42` (start-minimized on load)

- [ ] **Step 1: Add General settings block and wire close-to-tray**

In `src/qml/Main.qml`, add a `Settings` block after line 64 (after the `themeSettings` Connections block):

```qml
    Settings {
        id: generalSettings
        category: "General"
        property bool closeToTray: true
        property bool startMinimized: false
    }
```

- [ ] **Step 2: Update onClosing to respect closeToTray setting**

Replace the `onClosing` handler (lines 30-35):

```qml
    onClosing: function(close) {
        if (chatMode && generalSettings.closeToTray) {
            close.accepted = false
            root.hide()
        }
    }
```

- [ ] **Step 3: Wire startMinimized into Component.onCompleted**

In the existing `Component.onCompleted` block (lines 38-42), add a check after `visible = true`:

```qml
    Component.onCompleted: {
        x = (Screen.width - width) / 2
        y = (Screen.height - height) / 2
        // Only start minimized if already logged in (has a session to restore).
        // If not logged in, always show — user needs to see the login screen.
        visible = !(generalSettings.startMinimized && auth.isRestoringSession)
    }
```

Guard: `startMinimized` only takes effect when there's a session to restore. Without this, a first-time user with `startMinimized` enabled would see no window and no tray icon (tray is set up after auth).

- [ ] **Step 4: Build and verify**

```bash
cmake --build C:/build/talk-qt --target talq 2>&1 | tail -5
```

- [ ] **Step 5: Commit**

```bash
git add src/qml/Main.qml
git commit -m "feat: wire General settings (close-to-tray, start-minimized)"
```

---

### Task 5: Add per-conversation mute context menu

**Files:**
- Modify: `src/qml/ConversationItem.qml` (add right-click menu)
- Modify: `src/core/ApiClient.h` (add setNotificationLevel method)
- Modify: `src/core/ApiClient.cpp` (implement it)

- [ ] **Step 1: Add setNotificationLevel to ApiClient**

In `src/core/ApiClient.h`, add a new public method declaration (after line 44, after existing `del()` methods):

```cpp
    void setNotificationLevel(const QString &token, int level, Callback callback = nullptr);
```

Note: NOT `Q_INVOKABLE` — QML cannot call methods with `std::function` params. This method is called from C++ only; QML will call it indirectly via ConversationListModel (see Step 3).

In `src/core/ApiClient.cpp`, add the implementation:

```cpp
void ApiClient::setNotificationLevel(const QString &token, int level, Callback callback)
{
    QJsonObject body;
    body["level"] = level;
    put("/apps/spreed/api/v4/room/" + token + "/notify", body,
        callback ? callback : Callback([](bool, const QJsonObject &, int) {}));
}
```

Uses `put()` (not `post()`) per NC Talk API. The `Callback` typedef is `std::function<void(bool success, const QJsonObject &data, int statusCode)>` — matches the existing `put()` signature.

- [ ] **Step 2: Add notificationLevel property and context menu to ConversationItem**

In `src/qml/ConversationItem.qml`, add a `notificationLevel` property (after the existing required properties around line 22):

```qml
    property int notificationLevel: 0  // 0=default, 1=always, 2=mention, 3=never
```

Add a `Menu` component and `TapHandler` for right-click. Place this inside the ItemDelegate, after the `background` block:

```qml
    Menu {
        id: contextMenu
        palette.window: Theme.bgSurface
        palette.text: Theme.textPrimary

        MenuItem {
            text: convItem.notificationLevel === 3 ? "Unmute" : "Mute"
            onTriggered: {
                let newLevel = convItem.notificationLevel === 3 ? 0 : 3
                conversationModel.setNotificationLevel(convItem.index, newLevel)
            }
        }
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: contextMenu.popup()
    }
```

Note: the mute action calls `conversationModel.setNotificationLevel()` (a `Q_INVOKABLE` on the model) which internally calls `ApiClient::setNotificationLevel()` and updates the model data on success. This avoids the QML-cannot-call-std::function problem.

Add a muted icon indicator to the conversation item (in the expanded view, near the unread badge area). A small "Muted" label with reduced opacity when `notificationLevel === 3`:

```qml
    Label {
        visible: convItem.notificationLevel === 3
        text: "Muted"
        color: Theme.textSecondary
        font.pixelSize: 9
        opacity: 0.6
    }
```

- [ ] **Step 3: Add notificationLevel to Conversation struct, model, and delegate**

The `Conversation` struct (`src/models/Conversation.h`) does NOT have `notificationLevel`. The `ConversationListModel` does NOT expose it. All three layers need changes:

**a) `src/models/Conversation.h`** — Add field after line 54 (`int callFlag = 0;`):

```cpp
    int notificationLevel = 0;  // 0=default, 1=always, 2=mention-only, 3=never
```

**b) `src/models/Conversation.cpp`** — In `Conversation::fromJson()`, parse the field:

```cpp
    c.notificationLevel = json["notificationLevel"].toInt(0);
```

**c) `src/models/ConversationListModel.h`** — Add role to enum (after `HasTopicsRole` on line 34):

```cpp
        NotificationLevelRole,
```

**d) `src/models/ConversationListModel.cpp`** — Add to `roleNames()`:

```cpp
    { NotificationLevelRole, "notificationLevel" },
```

Add to `data()` switch:

```cpp
    case NotificationLevelRole: return m_conversations[index.row()].notificationLevel;
```

Add `Q_INVOKABLE` method for QML to call mute:

In `ConversationListModel.h`, add public method:

```cpp
    Q_INVOKABLE void setNotificationLevel(int index, int level);
```

In `ConversationListModel.cpp`, implement:

```cpp
void ConversationListModel::setNotificationLevel(int index, int level)
{
    if (index < 0 || index >= m_conversations.size()) return;
    const QString &token = m_conversations[index].token;
    m_api->setNotificationLevel(token, level,
        [this, index, level](bool success, const QJsonObject &, int) {
            if (!success || index >= m_conversations.size()) return;
            m_conversations[index].notificationLevel = level;
            QModelIndex mi = this->index(index);
            emit dataChanged(mi, mi, {NotificationLevelRole});
        });
}
```

**e) `src/qml/ConversationList.qml`** — In the delegate section, add `notificationLevel` to the delegate property bindings (where `token`, `displayName`, etc. are bound from the model):

```qml
    notificationLevel: model.notificationLevel
```

**f) `src/qml/ConversationItem.qml`** — The `required property int notificationLevel` should be changed to just `property int notificationLevel: 0` (not required, since it's a new role and older cached data won't have it).

- [ ] **Step 4: Build and verify**

```bash
rm -rf /c/build/talk-qt && cmake -B C:/build/talk-qt -S C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev && cmake --build C:/build/talk-qt --target talq 2>&1 | tail -5
```

- [ ] **Step 5: Commit**

```bash
git add src/core/ApiClient.h src/core/ApiClient.cpp src/qml/ConversationItem.qml src/models/ConversationListModel.h src/models/ConversationListModel.cpp src/qml/ConversationList.qml
git commit -m "feat: add per-conversation mute via right-click context menu"
```

---

### Task 6: Manual testing and polish

**Files:**
- Possibly: any file that needs fixes found during testing

- [ ] **Step 1: Launch the app and test each tab**

```bash
export PATH="/c/Qt/6.8.2/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH"
QT_FORCE_STDERR_LOGGING=1 /c/build/talk-qt/talq.exe
```

Test checklist:
1. Press Ctrl+, — settings dialog opens with 4 tabs
2. **Audio & Video tab**: select a different mic/speaker, close dialog, reopen — selection preserved. Close app, relaunch — selection restored.
3. **Notifications tab**: toggle notifications off, switch sound to "None", close dialog — verify no sounds on new message. Restart app — settings preserved.
4. **General tab**: toggle "Start with Windows" on — verify `HKCU\...\Run` registry key created. Toggle off — key removed. Toggle "Close to tray" off — verify closing the window actually quits the app.
5. **Account tab**: verify server URL, NC version, Talk version are correct. Verify logout button works.
6. **Per-conversation mute**: right-click a conversation → Mute. Verify muted indicator appears. Right-click → Unmute. Verify indicator disappears.

- [ ] **Step 2: Fix any issues found**

Address visual alignment, spacing, or functional issues discovered during testing.

- [ ] **Step 3: Final commit**

```bash
git add -u
git commit -m "fix: settings dialog polish from manual testing"
```
