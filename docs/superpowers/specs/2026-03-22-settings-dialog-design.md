# TalQ Settings Dialog — Design Spec

## Overview

Replace the current minimal SettingsDialog (mic/speaker/camera + video resolution) with a full tabbed settings dialog covering Audio & Video, Notifications, General, and Account. All settings persisted via `Qt.labs.settings` so they survive app restarts.

## Layout

Tabbed modal dialog, opened via Ctrl+, (existing shortcut). Four tabs across the top. Size: 480×520px, minimum 400×440px. Window close (X) button dismisses the dialog (no separate Close button needed). `modality: Qt.ApplicationModal` preserved.

## Tab 1: Audio & Video

### Controls
- **Microphone** — ComboBox bound to `deviceManager.selectedAudioInput`, with live level meter bar underneath (reuse existing GStreamer `level` element)
- **Speaker** — ComboBox bound to `deviceManager.selectedAudioOutput`, with "Test" button that plays a short tone
- **Camera** — ComboBox bound to `deviceManager.selectedVideoInput`. "No cameras found" fallback when list is empty
- **Video Quality** — Row of two exclusive Buttons styled as a toggle group (no built-in SegmentedButton in Qt6, use `Row { Button {...} Button {...} }` with manual `checked` state): "Full HD (1080p)" / "HD (720p)". Show "Changes apply to next call" hint when `callManager.state !== CallManager.Idle`.
- **Refresh Devices** — Button at bottom, calls `deviceManager.refresh()`. Also auto-refresh on `Component.onCompleted` (preserve existing behavior).
- **"No devices found" fallback** — Show placeholder text for any empty device list (mic, speaker, camera), not just camera.

### Persistence
- Add QSettings to `MediaDeviceManager`: save device name + GStreamer device ID (path) under category `"Devices"`. Name is primary match key; device ID is fallback when multiple devices share the same name (e.g., two identical USB mics).
- On app start, `MediaDeviceManager::restore()` matches saved name against current enumeration, falls back to device ID if name is ambiguous, and selects the matching index. If no match found, stays on system default (-1).
- Video resolution already persisted (`Qt.labs.settings` category `"Video"`, property `resolution`)

## Tab 2: Notifications

### Controls
- **Enable notifications** — Toggle switch, bound to `notifications.notificationsEnabled`
- **Notification style** — Row of exclusive Buttons (same toggle group pattern as Video Quality): "In-app popup" / "Windows toast", bound to `notifications.notifStyle`
- **Sound** — Row of exclusive Buttons: "TalQ chime" / "System sound" / "None", bound to `notifications.soundMode`
- **Hint** — Info box at bottom: "To mute individual conversations, right-click on them in the sidebar."

### Persistence
- Add `Qt.labs.settings` block in SettingsDialog with category `"Notifications"`, properties: `notificationsEnabled`, `notifStyle`, `soundMode`
- **Startup sync**: QML `Settings` loads saved values on `Component.onCompleted`. These override `NotificationManager` C++ defaults. Binding direction: QML Settings → NotificationManager (one-way push on load), then UI changes write to both NotificationManager and Settings simultaneously.
- If no saved value exists (first launch), `NotificationManager` defaults apply (enabled=true, style="popup", sound="internal").

### Per-conversation mute (separate feature)
- Right-click context menu on ConversationItem: "Mute" / "Unmute"
- Calls `PUT /apps/spreed/api/v4/room/{token}/notify` (OCS path, ApiClient prepends base) with notification level (1=always, 2=mention-only, 3=never)
- Show muted icon on conversation item when level is 3
- Not part of the settings dialog — lives in sidebar context menu

## Tab 3: General

### Controls
- **Start with Windows** — Toggle switch. Manages `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` registry key with app exe path
- **Start minimized** — Toggle switch. When enabled, app starts hidden to tray
- **Close to tray** — Toggle switch with subtitle "Minimize to tray instead of quitting". Controls whether window close hides or quits. Currently hardcoded to always close-to-tray when logged in; this makes it configurable

### Persistence
- `Qt.labs.settings` category `"General"`, properties: `autoStart`, `startMinimized`, `closeToTray`
- `autoStart` changes also write/remove the Windows Run registry key via C++ helper

## Tab 4: Account

### Controls (all read-only except logout)
- **Profile card** — Avatar (circular, from AvatarProvider) + display name + server hostname
- **Server URL** — Read-only text field showing full server URL
- **Nextcloud version** — From capabilities (already fetched by AuthManager)
- **Talk version** — From capabilities
- **App version** — Via `Qt.application.version` (globally available in QML, already set from CMake `PROJECT_VERSION`)
- **Log out** — Red button, calls `auth.logout()`

### No persistence needed
- All data comes from AuthManager (already persisted via QSettings)

## Architecture

### Files to modify
- `src/qml/SettingsDialog.qml` — Rewrite with TabBar + StackLayout, four tab content panels
- `src/core/MediaDeviceManager.h/cpp` — Add QSettings for device name persistence, `saveDevices()`/`restoreDevices()` methods
- `src/core/NotificationManager.h/cpp` — Add Q_PROPERTY setters that are bindable from QML settings
- `src/qml/Main.qml` — Wire General tab settings: read `closeToTray` from settings to control close behavior, read `startMinimized` on startup to decide initial visibility
- `src/qml/ConversationItem.qml` — Add "Mute"/"Unmute" to right-click context menu (separate from dialog)
- `src/core/ApiClient.h/cpp` — Add `setNotificationLevel(token, level)` method for per-conversation mute

### New files
- None. All changes fit in existing files.

### C++ helpers needed
- Add `Q_INVOKABLE void setAutoStart(bool enabled)` and `Q_INVOKABLE bool isAutoStart()` to an existing context-property class (e.g., `AuthManager` or a new lightweight `AppSettings` registered as a context property). These methods read/write `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` with the app exe path. QML calls `appSettings.setAutoStart(checked)` from the toggle.

### QML structure
```
SettingsDialog (Window, modal)
├── TabBar
│   ├── TabButton "Audio & Video"
│   ├── TabButton "Notifications"
│   ├── TabButton "General"
│   └── TabButton "Account"
└── StackLayout (currentIndex bound to TabBar)
    ├── AudioVideoTab (ScrollView > ColumnLayout)
    ├── NotificationsTab (ScrollView > ColumnLayout)
    ├── GeneralTab (ScrollView > ColumnLayout)
    └── AccountTab (ScrollView > ColumnLayout)
```

### Settings storage map

| Setting | Mechanism | Registry path |
|---------|-----------|---------------|
| Mic device name | QSettings in MediaDeviceManager | `HKCU\...\Devices\audioInput` |
| Speaker device name | QSettings in MediaDeviceManager | `HKCU\...\Devices\audioOutput` |
| Camera device name | QSettings in MediaDeviceManager | `HKCU\...\Devices\videoInput` |
| Video resolution | Qt.labs.settings | `HKCU\...\Video\resolution` |
| Notifications enabled | Qt.labs.settings | `HKCU\...\Notifications\enabled` |
| Notification style | Qt.labs.settings | `HKCU\...\Notifications\style` |
| Sound mode | Qt.labs.settings | `HKCU\...\Notifications\soundMode` |
| Auto-start | QSettings + Registry Run key | `HKCU\...\General\autoStart` + Run key |
| Start minimized | Qt.labs.settings | `HKCU\...\General\startMinimized` |
| Close to tray | Qt.labs.settings | `HKCU\...\General\closeToTray` |

## Design decisions

- **Device names + IDs over indices**: Indices shift when devices are plugged/unplugged. Primary match by name, fallback to GStreamer device ID for disambiguation when multiple devices share the same name.
- **Qt.labs.settings for QML-side, QSettings for C++-side**: Existing pattern in the codebase. Notification and general settings live in QML, device persistence lives in C++ MediaDeviceManager.
- **No separate settings manager class**: The scope doesn't justify it. Each subsystem manages its own persistence.
- **Per-conversation mute via context menu**: Natural UX (right-click), uses existing NC Talk API, doesn't clutter the global settings dialog.
- **TabBar + StackLayout**: Qt's built-in tabbed container, minimal code, matches the approved mockup.
