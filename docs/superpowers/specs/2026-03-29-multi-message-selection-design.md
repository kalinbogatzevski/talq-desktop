# Multi-Message Selection (Telegram-style)

## Overview

Telegram-style multi-message selection for TalQ. Users can select multiple messages and perform bulk actions: Copy, Forward, Delete. Selection mode replaces the composer with an action bar.

## Entry and Exit

### Entering selection mode
- **Context menu "Select"**: Right-click a message → click "Select" → enters selection mode with that message pre-selected
- **Ctrl+Click**: Ctrl+Click on any message row toggles its selection. If not already in selection mode, enters it.

### Exiting selection mode
- **Esc** key clears selection and exits
- **Cancel** button in the action bar
- Switching conversations exits selection mode and clears selection
- Completing an action (Copy, Forward, Delete) exits selection mode

## Selection Mode Behavior

### Click targets
- The entire message **row** is clickable (full widget width), not just the bubble. Matches Telegram desktop behavior.
- Clicking anywhere on the row toggles that message's selection state.
- System messages are not selectable.

### Visual treatment
- **Checkbox**: Circular, 18px diameter. Appears on the left margin for other people's messages, right margin for own messages.
  - Unselected (while in selection mode): empty circle outline (`#555` border)
  - Selected: filled teal circle (`#2ec4b6`) with white checkmark
- **Row highlight**: Selected rows get a teal tint background (`rgba(46,196,182,0.12)`) spanning full row width.
- **Suppressed interactions**: Hover bar and right-click context menu are disabled while in selection mode. Only selection clicks and scroll are active.

### State tracking
- Selection tracked by `QSet<int>` of message IDs (not layout indices), so selection survives scroll and model updates.
- `m_selectionMode` bool on ChatPainter controls mode.

## Action Bar

Replaces the composer widget at the bottom of the chat area when selection mode is active.

### Layout
- **Left**: "N messages selected" label in teal (`#2ec4b6`), bold
- **Right**: Row of labeled buttons:
  - **Forward** (`↗ Forward`) — dark background (`#2a2a3e`), white text
  - **Copy** (`📋 Copy`) — dark background, white text
  - **Delete** (`🗑 Delete`) — red tint background (`rgba(248,81,73,0.15)`), red text. **Only visible when all selected messages are own.**
  - **Cancel** (`✕ Cancel`) — dark background, gray text

### Sizing
- Same height as the composer (~44px content area)
- Buttons: 7px vertical padding, 14px horizontal, 8px border radius, 12px font

## Actions

### Copy
1. Collect selected messages sorted chronologically (oldest first).
2. Format each as: `[AuthorName, HH:MM]\nMessageText\n\n`
3. For file-only messages: `[AuthorName, HH:MM]\n[File: filename.ext]\n\n`
4. Strip HTML tags from message text.
5. Copy concatenated result to clipboard via `QApplication::clipboard()->setText()`.
6. Exit selection mode.

### Forward
1. Open a **conversation picker dialog** (modal).
2. Dialog shows the existing conversation list from `ConversationListModel`:
   - Search/filter bar at top
   - Conversation rows with avatar, name, type indicator
   - Click a conversation to select as target
   - Esc or close button cancels
3. On target selection, for each selected message (chronological order):
   - **Text messages**: `POST /ocs/v2.php/apps/spreed/api/v1/chat/{targetToken}` with `message` = original text (mentions replaced with readable names)
   - **File messages**: Re-share via `POST /ocs/v2.php/apps/files_sharing/api/v1/shares` with `shareType=10`, `shareWith={targetToken}`, `path` resolved from `fileId` (may need a lookup via files API if path isn't cached). Alternatively, forward the file as a text message with a link if path resolution is too complex for v1.
4. Show brief confirmation (e.g. status bar text or toast).
5. Exit selection mode.

### Delete
1. Only available when all selected messages are from the current user.
2. Show confirmation dialog: "Delete N messages?"
3. On confirm, call `DELETE /ocs/v2.php/apps/spreed/api/v1/chat/{token}/{messageId}` for each selected message.
4. Remove deleted messages from the model immediately (optimistic).
5. Exit selection mode.

## Conversation Picker Dialog

- **QDialog**, modal, ~400x500px
- **Search bar** at top: filters conversations by display name
- **Conversation list**: reuses rendering logic from SidebarPainter (avatar, name, type icon) in a simpler list form
- **Click to select**: single click on a conversation triggers forward and closes dialog
- **Cancel**: Esc or X button closes without forwarding

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| Esc | Exit selection mode |
| Ctrl+Click | Toggle message selection |
| Delete / Backspace | Delete selected (only if all own, shows confirmation) |
| Ctrl+C | Copy selected (when in selection mode) |

## Signals and State

### ChatPainter additions
- `bool m_selectionMode` — whether selection mode is active
- `QSet<int> m_selectedIds` — set of selected message IDs
- `void selectionChanged(int count)` — emitted when selection changes
- `void selectionModeChanged(bool active)` — emitted on mode enter/exit
- `void forwardRequested(QVector<Message> messages)` — emitted when Forward clicked
- `void copyRequested(QVector<Message> messages)` — emitted when Copy clicked
- `void deleteRequested(QVector<int> messageIds)` — emitted when Delete clicked

### MainWindow connections
- `selectionModeChanged` → show/hide action bar, hide/show composer
- `forwardRequested` → open conversation picker dialog
- `copyRequested` → format and copy to clipboard
- `deleteRequested` → show confirmation, bulk delete

## Paint Changes

### ChatPainter::paintEvent
- When `m_selectionMode`:
  - Paint teal tint background on selected rows (full width)
  - Paint checkbox circle on each message row (left for others, right for own)
  - Skip hover bar painting
  - Skip hover index tracking

### LayoutEngine
- No changes needed — checkbox is painted as an overlay, not part of the layout computation. The 18px checkbox occupies the existing margin space.

### Mouse handling changes
- When `m_selectionMode`:
  - Left-click toggles selection for the row under cursor (hit test uses full row rect, not bubble)
  - Right-click does nothing (context menu suppressed)
  - Ctrl+Click always toggles selection (even outside selection mode)
- `mousePressEvent` checks `m_selectionMode` first, before existing hit-test logic

## Edge Cases

- **Empty selection**: If user deselects all messages, exit selection mode automatically.
- **Message deleted by server**: If a selected message disappears from the model (deleted by other party, or conversation refresh), remove it from `m_selectedIds`. If selection becomes empty, exit mode.
- **Conversation switch**: Clears selection and exits mode.
- **Forward failure**: If POST fails for a message, show error but continue with remaining messages. Report "Forwarded N of M messages" if partial failure.
