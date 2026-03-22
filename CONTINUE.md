# TalQ v0.8.1 → v0.9.0 Continue Prompt

## What was done (2026-03-22)

### v0.8.1 — Settings Dialog & Design System (PATCH RELEASE)

#### Settings Dialog (new)
- **Full tabbed settings** — 4 tabs: Audio & Video, Notifications, General, Account
- **Device persistence** — mic/speaker/camera selections saved via QSettings, restored on restart (name + device ID matching with fallback for duplicates)
- **Notification settings** — enable/disable, style (in-app popup vs Windows toast), sound mode (chime/system/none), all persisted via Qt.labs.settings
- **General settings** — start with Windows (registry auto-start), start minimized to tray, close to tray toggle
- **Account tab** — avatar from AvatarProvider, display name, server URL, NC/Talk versions, app version, logout button
- **Dark mode styled** — custom StyledComboBox, StyledSwitch, OptionButton, TabBar with teal indicator, window-level palette
- **Click avatar to open** — click avatar or display name in sidebar header to open settings
- **AppSettings C++ helper** — `setAutoStart()`/`isAutoStart()` for Windows Run registry key

#### Per-conversation mute
- **Right-click mute** — right-click any conversation → Mute/Unmute
- **Server-synced** — PUT `/apps/spreed/api/v4/room/{token}/notify`, notification level stored in ConversationListModel
- **Visual indicator** — "Muted" label on muted conversations
- **notificationLevel** — added to Conversation struct, ConversationListModel role, parsed from API JSON

#### Design System Phase 1: Warm Carbon Theme
- **Color identity** — warm/olive-tinted blacks (#121210 base) where teal (#2ec4b6) is the only vivid color
- **30+ new tokens** in Theme.qml: semantic colors (success, danger, warning, info), button sizes (small 28, medium 36, large 48), icon sizes (16, 20, 24), avatar sizes (tiny 24, small 32, normal 44, large 52), alpha helpers, border widths, scrollbar width, fontSizeXSmall (9), fontSizeXLarge (20), statusDotSize, badgeHeight
- **All existing colors migrated** to Warm Carbon palette (warm grays instead of blue-grays)

#### Design System Phase 2: Reusable Components
- **TqAvatar** — circular avatar with AvatarProvider image + colored initial fallback + optional status dot
- **TqIconButton** — circular button with emoji icon, hover state, configurable size/colors
- **TqBadge** — pill-shaped unread count badge, auto-hides at 0, caps at 99+
- **TqSwitch** — dark-mode toggle with animated thumb
- **TqComboBox** — dark-mode dropdown with themed popup and delegate
- **484 lines removed** from 8 QML files by replacing duplicated avatar/button/badge/switch/combobox code

#### Bug fixes
- **Scroll-to-bottom** — onContentHeightChanged re-scrolls when autoScrolling (catches file attachment and image height changes without timers)
- **Window position restore** — fix Qt.labs.settings unsigned int wrapping for negative X/Y on multi-monitor setups
- **Reply bubble width** — 260px minimum now applied to other-person reply bubbles too (was only on own messages)
- **Code review fixes** — deduplicated generalSettings (single source in Main.qml), m_restoring guard prevents redundant saveDevices() during restoreDevices(), token capture by value in setNotificationLevel callback

### Known issues
- **Office machine Qt6Multimedia ABI mismatch** — aqt-installed qtmultimedia module crashes with 0xC0000139 (STATUS_ENTRYPOINT_NOT_FOUND). Home machine works fine. Fix: reinstall full Qt via online installer on office machine, or build without multimedia temporarily.
- **Ctrl+, shortcut** — may not work on all keyboard layouts. Backup: Ctrl+P or click avatar.
- **Settings dialog combo boxes** — unequal height between mic/speaker/camera dropdowns (cosmetic)
- **SettingsDialog SectionHeader** — still uses hardcoded `font.pixelSize: 10` instead of `Theme.fontSizeXSmall` (Phase 3 task)

### Test user
- Username: `test-talq` / Password: `talQing123@`
- 1:1 conversation with kalin: token `u2f3gbu4`
- Can log in at `https://ncloud.123net.link` in browser for call testing

## What to do next (v0.9.0)

### Priority 1: Design System Phase 3 — Apply Theme tokens
- Sweep all 14 QML files replacing hardcoded colors, font sizes, dimensions with Theme references
- Target: MessageBubble.qml (975 lines, worst offender — 15+ hardcoded sizes, RGBA hover states)
- Target: CallWindow.qml (20+ hardcoded colors, no Theme reference at all)
- Target: Main.qml (notification styling not themed)
- Fix all hardcoded button colors (#27ae60, #e74c3c, #2ecc71) to use Theme.success/danger

### Priority 2: Group calls + screen sharing
- Multiple SubscribePipelines with video (grid layout)
- Screen sharing capture (dxgiscreencapsrc or similar)
- Self-preview PIP

### Priority 3: Call improvements
- Echo cancellation (shared pipeline or custom GStreamer build)
- Mid-call device switching
- qml6glsink for zero-copy video rendering (custom GStreamer build)

### Priority 4: Chat improvements
- Message search
- Emoji picker + emoji rendering in chat history (text emoji shortcodes → actual emoji)
- Custom GStreamer build (Meson, selective plugins)

### Priority 5: UX polish
- Use frontend-design skill for CallWindow, settings, chat UI
- Proper app icon and branding
- System tray improvements
- Light mode refinement (Warm Carbon light palette needs testing)

## Architecture reference

### Design System
- **Theme.qml** — Singleton, Warm Carbon palette. All colors, fonts, spacing, dimensions.
- **Tq* components** — TqAvatar, TqIconButton, TqBadge, TqSwitch, TqComboBox. In `src/qml/`, registered via qt_add_qml_module.
- **Specs** — `docs/superpowers/specs/2026-03-22-design-system-phase1-design.md`, `phase2-design.md`

### Settings
- **SettingsDialog.qml** — TabBar + StackLayout, 4 tabs. Notification settings in local Qt.labs.settings, General settings in Main.qml's generalSettings block.
- **AppSettings.h/cpp** — Q_INVOKABLE setAutoStart/isAutoStart for Windows Run registry key.
- **MediaDeviceManager** — saveDevices()/restoreDevices() with QSettings, name+ID matching.

### MCU Call Flow (working)
```
Outgoing:
1. startCall → POST /api/v4/call/{token} (join call)
2. Fetch STUN from /api/v3/signaling/settings
3. PublishPipeline starts → offer to own session via HPB
4. MCU answers → publisher ICE connects
5. participantJoinedCall event → requestOffer for remote peer
6. MCU sends subscriber offer → SubscribePipeline answers → ICE connects → audio flows

Incoming:
1. Push notification → conversation refresh → hasCall detected
2. IncomingCallPopup shown with ringtone
3. Accept → POST participants/active (join room)
4. Wait for HPB roomJoined signal (CRITICAL)
5. POST /api/v4/call/{token} (join call)
6. Same as outgoing steps 2-6
```

### Server config
- HPB: `wss://ncloud.123net.link/standalone-signaling/spreed`
- MCU: enabled (Janus) — confirmed via `server.features: ["mcu", ...]`
- STUN: `stun:turn-za.123net.link:3478`, `stun:turn-bg.123net.link:3478`
- TURN: `turn:turn-za.123net.link`, `turn:turn-bg.123net.link` (with time-limited credentials)

## Build

### Office machine
```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
cd C:/build/talq
cmake C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build . --target talq
```

**NOTE**: Office machine may need Qt6Multimedia reinstall. If build crashes with 0xC0000139, the aqt-installed multimedia module is ABI-incompatible. Workaround: use online Qt installer instead of aqt for the multimedia module.

### Home machine
```bash
export PATH="/c/msys64/mingw64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:/c/Qt/6.8.2/mingw_64/bin:$PATH"
cd C:/build/talq
cmake C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -Wno-dev
cmake --build . --target talq
```

### Run
```bash
export QT_FORCE_STDERR_LOGGING=1
C:/build/talq/talq.exe
```

### GStreamer plugins (copy to gst-plugins/ next to exe)
```bash
mkdir -p gst-plugins && cp /c/msys64/mingw64/lib/gstreamer-1.0/libgst{coreelements,audioconvert,audioresample,autodetect,dtls,nice,opus,rtp,rtpmanager,srtp,wasapi2,webrtc,app,level,vpx,openh264,videoconvertscale,winks,sctp,jpeg}.dll gst-plugins/
cp /c/msys64/mingw64/bin/libjpeg-8.dll dist/
```

### Packaging
- Inno Setup at `C:\Users\bogat\InnoSetup\ISCC.exe`
- `windeployqt6.exe --no-translations --qmldir src/qml talq.exe`
- Copy QtMultimedia QML module: `cp -r /c/Qt/6.8.2/mingw_64/qml/QtMultimedia dist/qml/`
- GitLab API token "Talk QT" (id: 17) with `api` scope for uploads

## NC Talk source reference
- Clone: `git clone --depth 1 https://github.com/nextcloud/spreed.git /tmp/spreed`
- Signaling: `src/utils/signaling.js`
- WebRTC: `src/utils/webrtc/webrtc.js`
