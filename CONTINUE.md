# TalQ v0.14.4 Continue Prompt

## Current status
Full QWidget app with working audio + video calls via HPB/Janus MCU. Released v0.14.4.
All rendering via QPainter — no QML engine. Unified left-aligned chat layout.

## What was done (v0.14.0–v0.14.4, 2026-03-29)

### Multi-message selection (Telegram-style)
- Drag-to-select: click and drag sweeps over messages to select them
- Right-click → "Select" or Ctrl+Click to enter selection mode, Esc to exit
- Teal row highlight + circular checkboxes on the right
- Action bar replaces composer: Forward, Copy, Delete, Cancel
- Copy formats as `[Author, HH:MM]\nMessage` per message
- Forward: conversation picker dialog, posts messages as text to target
- Delete: bulk delete with confirmation (only when all selected are own)
- Ctrl+C shortcut copies in selection mode
- Drag-to-scroll removed — scroll via mouse wheel + scrollbar only

### Chat layout redesign
- Unified left-aligned layout — all messages (own + others) left-aligned with avatar column
- Own message avatars shown for non-grouped messages
- Author name above bubble (not inside)
- Bubbles computed in LayoutEngine with proper padding (10px horiz, 6px vert)
- Other-person messages get subtle transparent bubble background
- Timestamp right-aligned inside bubble, with read status icon for own
- Hover reply/react buttons right of bubble with proper gap
- Own messages: reply button only (no react). Others: react + reply

### Notifications
- Custom NotificationPopup — frameless dark rounded popup at bottom-right
- Click notification opens the conversation (restores window + selects chat)
- Shows for cross-chat messages even when app is focused
- Fixed: was showing oldest message instead of newest (model is newest-first, index 0)

### File upload
- Upload progress bar above composer (filename + percentage + teal progress line)
- Caption typed in main composer input (removed separate caption field)
- Enter key sends pending file with caption
- Pending bar: file preview + name + cancel only (send via composer)
- Scroll to bottom on messageSent
- Junction resolution: Qt6 blocks NTFS junction traversal; resolves each path component via isJunction() + junctionTarget()
- Temp-copy fallback if direct open still fails
- Error dialog for upload failures (errorOccurred signal wired)

### Other fixes
- Per-user install path (AppData\Local\Programs, no admin needed)
- Instant read status — push events trigger messages.refresh()
- Chat scrollbar — thin scrollbar thumb painted in ChatPainter
- Reaction counts — fixed showing 0 (was toInt on array, now uses array.size())
- Composer focus proxy for reply-to-focus
- HTML stripping uses QTextDocument::toPlainText() instead of regex
- Online status live updates — header refreshes when user statuses change
- File size displayed in attachment pills (KB/MB)
- Sidebar last message preview updates when new messages arrive
- build-release.sh script for reliable installer builds

### Code quality
- allSelectedOwn() helper avoids O(n) QVariantMap build on every toggle
- variantMapFromLayout() shared between messageAt() and selectedMessages()
- plainBodyText() helper for HTML stripping (copy + forward)
- Dead code removed: clearSelection(), popupRequested signal, unused includes

## Known bugs
- Duplicate message flash on send (optimistic send + poller overlap race)
- Notification stacking — single popup replaces previous, doesn't queue
- Notification always appears on primary screen, not the app's screen

## Next steps
- Camera doesn't work on this laptop (mfvideosrc COM/STA issue) — test on work laptop
- **Screen sharing** — d3d11screencapturesrc for full display or window-handle for specific app
- **Background blur** — Windows Studio Effects API (Win11) or MediaPipe segmentation + GStreamer
- In-bubble text selection (click-drag to select words within a message)
- Notification stacking (queue multiple popups)
- Notification on correct monitor
- Hardcoded dark theme colors in SelectionBarWidget/ConversationPickerDialog — use PainterTheme
- Cancel upload button on progress bar
- File attachment size not showing (fileSize parsed but might not reach paint for all cases)

## Architecture notes

### Message rendering
- ChatPainter: QPainter-based, all messages left-aligned
- LayoutEngine: computes MessageLayout with bubbleRect, contentX, contentW
- bubblePadX = 10, bubblePadTop/Bottom = 6 — content inside bubble with padding
- bubbleRect computed in layout, painting uses it directly (no ad-hoc computation)
- contentRight = bubbleLeft + bubbleW — hover buttons positioned from this
- Selection state: m_selectionMode + m_selectedIds (QSet<int>)

### Installer build (scripts/build-release.sh)
```bash
bash scripts/build-release.sh              # generic
bash scripts/build-release.sh --brand 123NET  # branded
```
Requires: Qt 6.8.2, MSYS2 at C:\msys64, Inno Setup, debug build's gst-plugins dir.
Installs to AppData\Local\Programs (per-user, no admin, no junction issues).

### Call flow (MCU mode)
1. startCall → POST /call/{token} → join call on server
2. PublishPipeline: always starts with dummy 16x16 VP8, camera replaces if available
3. Offer sent to own session (HPB creates Janus publisher room)
4. Remote joins → requestOffer → SubscribePipeline receives remote audio/video
5. ICE: STUN + TURN servers from /signaling/settings
6. Media state broadcast via signaling mute/unmute messages
7. Hangup: DELETE /call/{token}?all=true + teardown pipelines

### Video display
- VideoFrameProvider converts I420 GstSample → QImage via BT.601 YUV→RGB
- CallDialog::VideoWidget paints QImage with QPainter (aspect-ratio preserving)
- Remote video shown only when frames >32px and remote video not muted
- Local preview: small 120x90 overlay positioned at bottom-right of remote video
