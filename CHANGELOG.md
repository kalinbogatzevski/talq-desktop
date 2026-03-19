# Changelog

## v0.5.1 (2026-03-19)

### File Uploads
- 📎 button in composer for file picker
- Drag-and-drop files onto chat to upload
- WebDAV PUT upload + share to conversation

### Branding
- 123NET TalQ dedicated build (`cmake -DTALQ_BRAND=123NET`)
- Hardcoded server URL, no URL field on login
- Dual logos on splash (TalQ + brand), brand logo on welcome screen
- Separate Inno Setup installer for branded builds
- Self-signed code signing: 123 NET CPT (PTY) LTD

### Fixes
- Typing indicator filtered by current room (was showing wrong chat)
- Deleted messages removed instantly from list
- Window minimum width lowered to 500px
- Missing QML modules in installer (Qt.labs.settings, QtQuick.Dialogs)
- windeployqt used for complete dependency gathering

---

## v0.5.0 (2026-03-19)

### Real-Time Communication
- Push notifications via Nextcloud Notify Push WebSocket (instant message delivery)
- Typing indicators via standalone signaling (HPB) WebSocket
- Push status LED on avatar: green (push), amber (polling), red (disconnected)
- Signaling status on welcome screen

### Notifications & System Tray
- System tray icon with context menu (Show, Sound mode, Notifications, Quit)
- Sound modes: TalQ chime (default, bypasses DND), system sound, or none
- Tray icon badge with unread count (dynamically painted)
- Cross-chat notifications with sound even when window is focused
- Minimize to tray on close (X hides, tray Quit exits)
- Conversation list auto-refresh (30s fallback for push)

### File Attachments
- Inline display of shared files in messages
- Image files shown with preview placeholder (authenticated provider TODO)
- Other files shown as styled pills with MIME-type icons (video, audio, PDF, etc.)
- Click to open/download in browser

### Read Markers
- Mark conversation as read on open (POST /chat/{token}/read)
- Instant sidebar badge clear (local + server)
- Auto mark-as-read on new polled messages

### Font Zoom
- Ctrl+= zoom in, Ctrl+- zoom out, Ctrl+0 reset (0.7x to 2.0x)
- Scale persisted between sessions
- All font sizes scale via Theme.fontScale

### UX Polish
- Welcome screen: server info card with GridLayout alignment, push/signaling status
- Context menu: bigger icons (18px), aligned columns, taller rows (36px)
- Emoji bar positions inline with react button
- Hover action buttons: 32px with 20px icons
- Stronger hover backgrounds on menu items
- Reply scroll fix (delayed 100ms for footer resize)

### Stability Fixes
- Memory leak fix: broken beginInsertRows + unauthenticated image retries (5.5GB → 260MB)
- Conversation switch freeze fix: SQLite cache blocking main thread
- MessageCache moved to worker thread (fully async saves, non-blocking)
- Lazy-loaded Popups in MessageBubble (eliminates 100+ window handles)
- In-place conversation list updates (no beginResetModel during refresh)
- Participants/active join for signaling (prevents "not invited" error)
- Thread-safe messagesLoaded signal (double-invoke for main thread delivery)
- PushClient WebSocket state guard in authenticate callback
- Popup Loaders deactivated on close (memory cleanup)
- Tray icon pixmap cached (no disk reload per refresh)

### Other
- Exe renamed from talk-qt to talq
- Build timestamp in window title for debug tracking
- Code review findings addressed (8 fixes)

---

## v0.4.0 (2026-03-19)

### Threads / Topics
- Thread navigation for group conversations — topic list overlay with colored dots, reply counts, last activity
- Thread-aware message loading and polling with `threadId` filter
- "Reply in thread" context menu action
- ThreadItem and ThreadListView QML components
- Back navigation from thread to topic list

### Notifications & System Tray
- System tray icon with context menu (Show, Sound mode, Notifications, Quit)
- Windows toast notifications for new messages
- Sound modes: TalQ chime (default, bypasses DND), system sound, or none
- Cross-chat notifications — sound plays even when window is focused in another chat
- Auto-refresh conversation list every 10s for near-real-time unread detection
- Tray tooltip shows total unread count

### UX Polish
- Server info card on welcome page (server URL, NC version, Talk version, signaling)
- Clickable reaction pills with hover states (replaces plain text)
- Hover action button Y-clamping (no more clipping outside bubbles)
- Refined empty states with icons and helpful copy
- Context menu actions wired: Delete (API), Pin (API), Copy link, Reply in thread

### Window Management
- Splash screen centered on primary display, stays small (380x420)
- Reliable window state save/restore including maximized (QGroundControl pattern)
- Debounced geometry saving with `chatMode` guard
- Window grows to saved size/position after login

### Fixes
- Duplicate message prevention — poller vs POST response race condition resolved
- User agent shows "TalQ" without version during login
- Exe renamed from `talk-qt` to `talq`

---

## v0.3.0 (2026-03-19)

### Features
- SQLite message cache for instant conversation loading
- Avatar provider with 3-tier cache (memory, disk, server) and circular crop
- Dark/light mode toggle with warm teal accent (#2ec4b6)
- Emoji reactions with context menu and quick-react bar
- Reply-to with preview bar in composer
- Optimistic message sending with retry on failure
- Mention resolution in message text
- DWM dark title bar on Windows
- Inno Setup installer

### UI
- Hybrid message layout: flat for others, teal bubble for own messages
- Page-based ChatView with header/footer
- Connection status LED in sidebar header
- Window position/size persistence

---

## v0.2.0 (2026-03-18)

### Features
- Read receipts (sent/read indicators from X-Chat-Last-Common-Read)
- Date separators (Today/Yesterday/date pills)
- Custom app icon (blue chat bubble with Q)
- Splash screen with loading animation

### UI
- Complete UI redesign with refined dark theme
- Smooth hover/selection animations
- Conversation list: selection bar, search focus glow, thin scrollbar
- Send button with press animation

---

## v0.1.0 (2026-03-18)

### Initial Release
- Login Flow v2 (browser-based Nextcloud auth)
- Conversation list with unread badges, favorites, search
- Chat messaging with message bubbles
- Long-polling for live message updates
- Session persistence (auto-login on restart)
- Cross-compiled from Linux with mingw-w64
