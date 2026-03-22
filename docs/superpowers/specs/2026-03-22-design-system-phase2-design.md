# TalQ Design System — Phase 2: Reusable Components

## Overview

Extract 5 reusable QML components from duplicated patterns found across 14 QML files. Each component uses Theme tokens from Phase 1. After creation, update existing QML files to use them — eliminating copy-pasted avatar, button, badge, switch, and combo box code.

## Components

### 1. TqAvatar.qml

Circular avatar with image-from-provider fallback to colored initial.

**Props:**
- `userId: string` — for AvatarProvider image source
- `displayName: string` — first letter used as fallback
- `size: int = Theme.avatarSize` — diameter
- `showStatus: bool = false` — show online/offline dot
- `status: string = ""` — "online", "away", "dnd", "offline"

**Behavior:**
- `Image { source: "image://avatar/" + userId }` when userId is non-empty
- Fallback: colored circle (derived from `Theme.topicColor(Theme.stringHash(displayName))`) with uppercase first initial
- Optional status dot (bottom-right, `Theme.statusDotSize`)
- `clip: true` + `radius: size/2` for circular mask

**Replaces code in:** ConversationItem.qml (lines 72-100), ConversationList.qml (lines 44-101), ChatView.qml (lines 125-145), SettingsDialog.qml (lines 473-498), IncomingCallPopup.qml (avatar section), CallWindow.qml (avatar section), MessageBubble.qml (avatar in message header)

### 2. TqIconButton.qml

Circular button for toolbar/action icons with hover state.

**Props:**
- `icon: string` — emoji text or single character
- `size: int = Theme.buttonSizeMedium` — button diameter
- `iconSize: int = Theme.iconSizeMedium` — font size for icon text
- `bgColor: color = "transparent"` — default background
- `hoverColor: color = Theme.bgHover` — hover background
- `iconColor: color = Theme.textSecondary` — icon color

**Behavior:**
- Circular `Rectangle` background with hover animation (`Behavior on color`)
- Centered `Text` with the icon emoji
- `signal clicked()` — emitted on press
- `cursorShape: Qt.PointingHandCursor`
- Hover: `bgColor` → `hoverColor` transition

**Replaces code in:** ConversationList.qml (theme toggle ~36x36, refresh ~36x36, exit ~36x36), ChatView.qml (back button ~30x30, call button ~34x34, scroll-to-bottom ~36x36), ThreadListView.qml (back button ~28x28), MessageComposer.qml (attach ~36x36, send ~40x40), MessageBubble.qml (react button ~30x30)

### 3. TqBadge.qml

Pill-shaped unread count badge.

**Props:**
- `count: int = 0` — number to display
- `color: color = Theme.unreadBadge` — background color
- `textColor: color = "#000000"` — text color

**Behavior:**
- `visible: count > 0`
- Height: `Theme.badgeHeight`
- Width: `Math.max(Theme.badgeHeight, badgeText.implicitWidth + 10)` — pill shape for multi-digit
- Radius: `Theme.badgeHeight / 2`
- Text: `Theme.badgeFontSize`, centered, bold
- Displays `"99+"` when count > 99

**Replaces code in:** ConversationItem.qml (lines 200-218 expanded, lines 258-276 squeezed), ThreadItem.qml (unread badge)

### 4. TqSwitch.qml

Dark-mode toggle switch with animated thumb.

**Props:**
- Standard `Switch` API (`checked`, `onToggled`)

**Behavior:**
- Track: 40x22px rounded rectangle
- On: `Theme.accent` background, white thumb right
- Off: `Theme.bgSurface` background with `Theme.border` border, `Theme.textSecondary` thumb left
- Thumb: 16x16 circle with `Behavior on x` animation (`Theme.animFast`)

**Replaces code in:** SettingsDialog.qml (StyledSwitch inline component — 3 instances)

### 5. TqComboBox.qml

Dark-mode styled dropdown.

**Props:**
- Standard `ComboBox` API (`model`, `currentIndex`, `onActivated`)

**Behavior:**
- Background: `Theme.bgSurface` with `Theme.border` border, `Theme.radiusSmall` radius
- Content text: `Theme.textPrimary`, `Theme.fontSizeSmall`
- Dropdown indicator: "▾" in `Theme.textSecondary`
- Popup: `Theme.bgSurface` background with border
- Delegate items: `Theme.textPrimary` text, `Theme.accent` highlight with black text
- `Layout.fillWidth: true` by default

**Replaces code in:** SettingsDialog.qml (StyledComboBox inline component — 3 instances)

## File changes

### New files (5)
- `src/qml/TqAvatar.qml`
- `src/qml/TqIconButton.qml`
- `src/qml/TqBadge.qml`
- `src/qml/TqSwitch.qml`
- `src/qml/TqComboBox.qml`

### Modified files
- `CMakeLists.txt` — add 5 new QML files to `qt_add_qml_module`
- `src/qml/SettingsDialog.qml` — remove inline StyledSwitch, StyledComboBox, replace with TqSwitch, TqComboBox. Remove inline avatar code, replace with TqAvatar.
- `src/qml/ConversationItem.qml` — replace avatar code with TqAvatar, badge code with TqBadge
- `src/qml/ConversationList.qml` — replace header avatar with TqAvatar, toolbar buttons with TqIconButton
- `src/qml/ChatView.qml` — replace header avatar with TqAvatar, back/call/scroll buttons with TqIconButton
- `src/qml/ThreadListView.qml` — replace back button with TqIconButton
- `src/qml/MessageComposer.qml` — replace attach/send buttons with TqIconButton
- `src/qml/IncomingCallPopup.qml` — replace avatar with TqAvatar
- `src/qml/ThreadItem.qml` — replace badge with TqBadge

### NOT modified (keep as-is)
- `src/qml/MessageBubble.qml` — 975 lines, too large to safely refactor in this phase. Avatar and react button replacement deferred to Phase 3.
- `src/qml/CallWindow.qml` — entirely custom styling, deferred to Phase 3.
- `src/qml/Main.qml` — no component duplication to extract.
- `src/qml/LoginView.qml` — no component duplication to extract.
- `src/qml/DebugOverlay.qml` — debug tool, exempt.

## Architecture notes

- All components are uppercase QML files registered via `qt_add_qml_module` — automatically available as `TqAvatar {}` etc. in any file that imports `TalkQt`.
- `Tq` prefix avoids name collisions with Qt Quick Controls types.
- No C++ changes needed.
- Components use only Theme.qml tokens — no hardcoded colors or sizes.
