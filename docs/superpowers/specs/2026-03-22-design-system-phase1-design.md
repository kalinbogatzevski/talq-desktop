# TalQ Design System — Phase 1: Theme Foundation

## Overview

Expand Theme.qml into a complete design system foundation using the "Warm Carbon" color palette — warm-tinted blacks with teal (#2ec4b6) accent. Add standardized dimensions for buttons, icons, avatars, status indicators, borders, and typography. No QML file refactoring — this phase only changes Theme.qml.

## Design Direction

- **Personality:** Signal-like minimal, understated, content-first
- **Color identity:** "Warm Carbon" — warm/olive-tinted blacks where teal is the only vivid color
- **Icons:** Emoji for now, SVG migration planned separately
- **Scope:** Theme.qml only — Phases 2 (components) and 3 (apply) are separate specs

## Color System: Warm Carbon

### Backgrounds (warm-tinted blacks stepping up in lightness)

| Token | Dark | Light | Use |
|-------|------|-------|-----|
| `bgPrimary` | `#121210` | `#ffffff` | Window/page background |
| `bgSecondary` | `#1a1a16` | `#f5f5f2` | Sidebar, panels |
| `bgSidebar` | `#181814` | `#f0f0ed` | Sidebar specific |
| `bgSurface` | `#222220` | `#ffffff` | Cards, inputs, dropdowns |
| `bgHover` | `#2c2c28` | `#eeeee8` | Hover states |
| `bgSelected` | `#33332e` | `#e4e4de` | Active/selected items |
| `bgInput` | `#1e1e1c` | `#f0f0ed` | Text inputs |
| `bgOverlay` | `#0a0a08` | `#00000040` | Modal backdrops |
| `bgMessage` | `transparent` | `transparent` | Other people's messages (flat) |
| `bgMessageOwn` | `#1a302e` | `#d4f0ed` | Own messages (teal-tinted) |

### Text (warm grays)

| Token | Dark | Light |
|-------|------|-------|
| `textPrimary` | `#e4e0da` | `#1a1a16` |
| `textSecondary` | `#8a8680` | `#6b6860` |
| `textMuted` | `#5a5850` | `#b0aca5` |
| `textTime` | `#6a665e` | `#9a968e` |

### Accent (teal — unchanged)

| Token | Dark | Light |
|-------|------|-------|
| `accent` | `#2ec4b6` | `#1aab9d` |
| `accentHover` | `#3dd4c6` | `#22bfb0` |
| `accentPressed` | `#25a99d` | `#159488` |
| `unreadBadge` | `#2ec4b6` | `#1aab9d` |

### Semantic colors

| Token | Value | Use |
|-------|-------|-----|
| `success` | `#5ec76a` | Online, call connected |
| `danger` | `#e06060` | Errors, hang up, logout |
| `dangerHover` | `#c94545` | Danger button hover |
| `warning` | `#f0a050` | Polling/degraded |
| `info` | `#5b9bd5` | Informational hints |
| `online` | `#5ec76a` | (alias for success) |
| `systemMsg` | dark: `#6a665e` / light: `#9a968e` | System messages |

### Borders & dividers (warm)

| Token | Dark | Light |
|-------|------|-------|
| `border` | `#2a2a26` | `#e5e2dc` |
| `divider` | `#222220` | `#eeeee8` |
| `borderFocused` | `#2ec4b6` | `#1aab9d` |
| `selectionBar` | (alias for accent) | |

### Alpha helpers (new)

| Token | Value | Use |
|-------|-------|-----|
| `alphaHover` | `0.06` | Hover overlay opacity |
| `alphaActive` | `0.10` | Active/pressed overlay |
| `alphaDisabled` | `0.35` | Disabled element opacity |

## Standardized Dimensions

### Button sizes (3 tiers)

| Token | Value | Use |
|-------|-------|-----|
| `buttonSizeSmall` | `28` | Inline actions, compact buttons |
| `buttonSizeMedium` | `36` | Toolbar, attach, theme toggle |
| `buttonSizeLarge` | `48` | Call accept/decline, primary actions |

### Icon sizes

| Token | Value | Use |
|-------|-------|-----|
| `iconSizeSmall` | `16` | Inline indicators, badges |
| `iconSizeMedium` | `20` | Standard icons |
| `iconSizeLarge` | `24` | Header/toolbar icons |

### Avatar sizes

| Token | Value | Use |
|-------|-------|-----|
| `avatarSizeTiny` | `24` | Inline mentions |
| `avatarSizeSmall` | `32` | Header, compact views (existing) |
| `avatarSize` | `44` | Conversation list (existing) |
| `avatarSizeLarge` | `52` | Profile cards, settings |

### Status indicators

| Token | Value | Use |
|-------|-------|-----|
| `statusDotSize` | `10` | Online/offline LED |
| `badgeHeight` | `18` | Unread count badges |
| `badgeFontSize` | `10` | Badge text |

### Border widths

| Token | Value |
|-------|-------|
| `borderWidthThin` | `0.5` |
| `borderWidthNormal` | `1` |
| `borderWidthThick` | `2` |

### Scrollbar

| Token | Value |
|-------|-------|
| `scrollbarWidth` | `4` |

## Typography

Keep existing 6 sizes, add 2:

| Token | Formula | Base px | Use |
|-------|---------|---------|-----|
| `fontSizeXSmall` | `9 * fontScale` | 9 | Badge text, debug, fine print |
| `fontSizeTiny` | `11 * fontScale` | 11 | (existing) Timestamps, hints |
| `fontSizeSmall` | `12 * fontScale` | 12 | (existing) Secondary text |
| `fontSizeNormal` | `14 * fontScale` | 14 | (existing) Body text |
| `fontSizeLarge` | `16 * fontScale` | 16 | (existing) Headers, names |
| `fontSizeXLarge` | `20 * fontScale` | 20 | Call window, popup emoji |
| `fontSizeTitle` | `22 * fontScale` | 22 | (existing) Page titles |
| `fontSizeHero` | `36 * fontScale` | 36 | (existing) Login branding |

## What stays unchanged

- `spacing*` (4, 8, 12, 16, 24)
- `radius*` (6, 10, 14, 100)
- `anim*` (120, 200, 350)
- `topicPalette` (6-color array)
- `topicColor()` and `stringHash()` functions
- `fontScale` zoom system (0.7–2.0)
- `headerHeight`, `composerMinHeight`, `conversationHeight`

## Implementation

### File to modify
- `src/qml/Theme.qml` — update existing color values to Warm Carbon palette, add new tokens

### What changes
- All existing background colors updated to Warm Carbon values
- All existing text colors updated to warm gray values
- Border/divider colors updated to warm values
- Add ~30 new tokens (semantic colors, dimensions, alphas, typography)
- Preserve all existing property names — no breaking changes

### What does NOT change
- No QML files other than Theme.qml
- No new files created
- No component extraction (that's Phase 2)
- No hardcoded value replacement in other files (that's Phase 3)
