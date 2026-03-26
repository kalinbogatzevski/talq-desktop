# Changelog

## v0.9.4 (2026-03-26)

### Major: BottomToTop ListView (conversation switch freeze fix)
- **Root cause**: `positionViewAtEnd()` / `positionViewAtIndex()` forced Qt to instantiate ALL MessageBubble delegates simultaneously, causing 1-4GB memory explosion and UI freeze on every conversation switch
- **Fix**: Reversed message storage to newest-first + `ListView.BottomToTop` layout. The view naturally starts at the bottom (newest messages visible) — no scroll calls needed
- Messages now stored newest-first in the model (`m_messages[0]` = newest)
- New messages from poller prepend at index 0 (appear at bottom)
- History loads append at end (appear at top on scroll-up)
- `cacheBuffer: 200` — only creates delegates near the viewport

### Memory & Stability Fixes
- **Poller backoff**: HTTP 401/403/404 stops polling; 5xx retries with exponential backoff (2s→60s)
- **Poller lastKnown:0 guard**: never start polling with lastKnownMessageId=0 (was downloading entire conversation history — 2000+ messages)
- **Message trimming**: cap at 200 messages per conversation, oldest trimmed on overflow
- **m_messageIds in postAndReplace**: prevents duplicate messages from poller
- **refreshLatest after file share**: sent files now appear immediately
- **cancelAll() safe iteration**: copies list before aborting
- **Cross-thread image providers**: FilePreviewProvider and AvatarProvider now moveToThread to main thread before network calls (eliminates "Cannot create children for a parent that is in a different thread" warnings)
- **MessageBubble anchors fix**: reply background Rectangle moved outside ColumnLayout (was causing 4000+ re-layout cycles)

### Other Fixes
- PushClient WebSocket error handler added
- PushClient auth failure reconnects instead of permanent dead state
- m_hasMoreHistory reset in setThreadId/setHideThreadMessages
- ConversationItem required property notificationLevel (eliminates "model is not defined" errors)
- markAsRead uses proper method on conversation open

## v0.9.3 (2026-03-26)

### Fixes
- **Duplicate message prevention** — `m_messageIds` updated in `postAndReplace()` and `deleteMessage()`, preventing poller from re-adding sent/deleted messages
- **Refresh after file share** — conversation refreshes immediately after uploading a file
- Poller diagnostic logging added

## v0.9.2 (2026-03-26)

### Call Fixes
- **MCU ICE candidate parsing** — fixed double-nested candidate extraction from Janus MCU signaling (was likely causing intermittent call failures)
- **STUN URL format** — convert Nextcloud's `stun:host:port` to `stun://host:port` for GStreamer compatibility
- **Test audio mode** — PeerPipeline supports `TALQ_TEST_AUDIO` env var for headless testing (audiotestsrc + fakesink)

### Automated Call Testing
- **talq-call-test.exe** — headless two-user MCU call test harness
- Authenticates kalin + test-talq, joins call via HPB, creates WebRTC pipelines, verifies ICE connection through real STUN/TURN, validates 3s stability, tears down cleanly
- Catches SDP issues, ICE failures, and signaling bugs without GUI

### Build & Deploy
- **deploy-dev.sh** — added audiotestsrc to GStreamer plugin list, fixed MSYS2 runtime DLL handling
- **Unified build paths** — all docs use `/c/build/talq` consistently

## v0.9.1 (2026-03-25)

### Room Avatars
- **Group chat avatars** — conversations without a specific user (group chats, public rooms) now show the room avatar fetched via authenticated OCS API
- **TqAvatar token fallback** — avatar component falls back to `image://avatar/room/{token}` when no userId is available

### Build & Deploy
- **deploy-dev.sh** — new script handles Qt + GStreamer DLL deployment after build, resolves MinGW runtime ABI conflict automatically
- **Unified build paths** — all docs and scripts now use `/c/build/talq` and `/c/src/talk-desktop-qt` consistently

### Fixes
- Triple-pass scrollToBottom for first-load delegate sizing

## v0.9.0 (2026-03-25)

### Memory Leak & Scroll Fix
- **Orphan network callbacks** — rapid conversation switching stacked duplicate refreshLatest() replies. Fixed: `m_refreshReply` member, cancelled on switch.
- **loadHistory() cascade** — `onContentYChanged` re-triggered after each prepend, loading entire history into memory. Fixed: 500ms debounce + `userHasScrolled` guard.
- **Unbounded FilePreviewProvider cache** — every preview (~3MB each) cached forever. Fixed: 50MB LRU eviction cap.
- **refreshLatest() ordering** — partition missing messages into older (prepend) / newer (append)
- **Persistent m_messageIds** — QSet replaces per-poll O(n) rebuild
- **onLastCommonReadChanged** scoped to changed range (was emitting for ALL messages every 15s)
- **scrollToBottom()** coalesced via 16ms timer
- **AvatarProvider** memory cache capped at 200 entries

### Video Calls — Compatibility
- **NC Talk video compatibility** — media state broadcasting, auto-camera detection, dual call buttons (audio/video)
- **SVG icon system** — geometric thin-stroke icons replace emoji throughout the app
- **Call status breadcrumbs** — shows signaling progress during connection (joining → signaling → connecting → active)
- **GStreamer plugin check** — disable call buttons when required plugins are missing
- **mfvideosrc** — use Media Foundation camera source instead of ksvideosrc
- **PLI keyframe fix** — request on src pad (not sink), periodic every 5s
- **Add-transceiver fix** — prevents m=video 0 in renegotiation SDP (untested)
- **Audio fallback chain** — autoaudiosrc/autoaudiosink with device selection override
- **MCU video renegotiation** — re-request subscriber streams after video accepted

### Fixes
- Duplicate "Reply" in context menu removed
- imageViewer.open() crash → downloadFile()
- Scroll-up no longer blocked by aggressive auto-scroll

## v0.8.3 (2026-03-23)

### Camera & P2P
- **Local camera preview** — PIP overlay in CallWindow via tee + appsink in PublishPipeline
- **P2P call mode** — direct peer-to-peer WebRTC for 1:1 calls (PeerPipeline), MCU used for group calls
- **Camera renegotiation** — enabling camera mid-call triggers proper SDP renegotiation

### Fixes
- Refresh latest messages from server after cache load (stale cache issue)
- Signal disconnect and QPointer guard in pipeline callbacks

## v0.8.2 (2026-03-22)

### Warm Carbon Design System
- **Phase 1: Theme foundation** — comprehensive dark/light theme with warm teal accent, standardized spacing/radius/font tokens
- **Phase 2: Component library** — 5 reusable QML components: TqAvatar, TqIconButton, TqBadge, TqSwitch, TqComboBox
- **Refactored UI** — replaced duplicated code across views with Tq* components

### Fixes
- Scroll-to-bottom catches late delegate height changes
- Negative window position restore fixed
- Reply bubble min width applied to other-person messages (was only own)
- TqIconButton: icon→iconText (AbstractButton.icon is FINAL)

## v0.8.1 (2026-03-22)

### Settings Dialog
- **Full settings dialog** — 4-tab layout (Audio & Video, Notifications, General, Account) with dark mode styling
- **Device persistence** — mic, speaker, and camera selections saved via QSettings, restored on app restart (matches by name, falls back to device ID for disambiguation)
- **Notification settings** — enable/disable, style (in-app popup vs Windows toast), sound mode (TalQ chime / system / none), all persisted
- **General settings** — start with Windows (registry auto-start), start minimized to tray, close to tray toggle
- **Account tab** — avatar, display name, server URL, Nextcloud/Talk versions, logout button
- **Click avatar to open** — click your avatar or display name in the sidebar header to open settings

### Per-Conversation Mute
- **Right-click mute** — right-click any conversation in the sidebar to Mute/Unmute
- **Server-synced** — uses Nextcloud Talk API (`/api/v4/room/{token}/notify`), persists across devices
- **Visual indicator** — "Muted" label shown on muted conversations

## v0.8.0 (2026-03-22)

### Video Calls
- **Receive remote video** — VP8 and H.264 codec auto-detection, decoded via GStreamer appsink to Qt VideoOutput. Tested working via MCU (Janus).
- **Send camera** — ksvideosrc (JPEG capture + jpegdec for HD) → openh264enc → rtph264pay, 1080p/720p with auto-fallback, toggle mid-call
- **SCTP data channel support** — required for MCU SDP compatibility (Janus includes datachannel m-line)
- **CallWindow video layout** — remote video fills window, controls auto-hide after 3s, camera toggle button, duration overlay
- **Settings** — camera device selection, video quality preset (Full HD / HD), persisted via Qt.labs.settings

### Call Polish
- **TURN server support** — credentials parsed from signaling settings, URL-encoded, configured on webrtcbin
- **Device selection wired to pipelines** — selected mic/speaker actually used (wasapi2src/wasapi2sink device property)
- **Incoming call decline fix** — leave room instead of call API (no more 404), m_joinedCall tracking
- **Auto-decline race fix** — m_userActionReady flag gates accept/decline on popup Component.onCompleted

### Chat UX
- **Scroll-to-bottom fixed** — no more jumping during image load, history prepend doesn't disrupt position
- **Image previews reserve height** — 200px placeholder prevents layout shift during async load
- **Reply bubble width** — own reply bubbles expand to fit quoted text (min 260px when quoting)
- **In-app image viewer** — click any image to view full-size in dark window (Esc to close)
- **Context menu** — Download and Open in Nextcloud items for file/image messages

### Dependencies
- Qt6::Multimedia added (QVideoSink, QVideoFrame, VideoOutput)
- GStreamer plugins: vpx, openh264, videoconvertscale, winks (camera)

## v0.7.1 (2026-03-21)

### Message Cache Fix
- Fixed file attachments (images, documents) disappearing after switching conversations
- Cache now stores original server JSON for lossless round-trip (previously reconstructed a lossy subset, dropping messageParameters)
- Mentions, thread metadata, and all parameter-dependent content now survive cache reload
- Schema v2 migration auto-purges stale entries on first launch

### Chat Scroll Stability
- Fixed chat jumping to older history during actions (image upload, footer changes)
- Replaced `onContentHeightChanged` auto-scroll with targeted `onCountChanged` handler
- Auto-scroll no longer disabled by programmatic scrolling (uses `onDraggingChanged` instead of `onMovingChanged`)
- Auto-scroll re-enables when user scrolls back to bottom
- Scroll-to-bottom button now correctly shows/hides based on position
- Scrollbar fades in during scrolling, fades out after 400ms of inactivity

### Text Selection
- Message text is now selectable with click-drag (Ctrl+C to copy)
- ListView switched to `interactive: false` with WheelHandler for mouse wheel scrolling

### File Handling Improvements
- Ctrl+V now handles files copied from Explorer (not just screenshots)
- Image files get preview confirmation; other files show file icon with name
- All file sends (paste, file dialog, drag-drop) show confirmation bar with caption field
- Files are never sent directly without user confirmation

## v0.6.1 (2026-03-20)

### Thread/Topic Fixes
- Fixed false thread detection in 1:1 chats — regular replies no longer create phantom topics
- Thread detection now uses the API-provided `isThread`/`threadId`/`threadTitle` fields instead of heuristic parent scanning
- "All Messages" renamed to "General" — now shows only non-thread messages (like Telegram's #General)
- Back arrow added to topics column header for returning to full chat list
- "+ New Topic" button moved from header to bottom of topic list to prevent overlap
- Sidebar can now be expanded/collapsed via toggle even when topics are active
- Draggable resize handles on sidebar and topic list dividers

### Scroll Fix
- Fixed scroll-to-bottom not working when opening conversations
- Replaced timer-based scroll with callback-driven approach using `onContentHeightChanged`

## v0.6.0 (2026-03-19)

### Threads / Topics (Telegram-style)
- 3-column layout for group chats with threads: squeezed sidebar icons | topics list | messages
- Sidebar auto-squeezes to 56px icon-only mode with smooth animation
- Manual squeeze toggle chevron at sidebar bottom
- Topics list with colored dots, unread badges, selection highlight
- "All Messages" row for unfiltered conversation view
- Inline topic creation: "+" button → type name → Enter
- Topic-aware chat header with color dot + group subtitle
- Dynamic composer placeholder: "Reply in [topic name]..."
- Thread index persisted in SQLite across app restarts
- Server capability check (requires Talk v22+ threads feature)
- Topic color palette centralized in Theme.topicColor()

### Upload Progress
- File upload shows progress bar in footer (filename + percentage)
- Animated progress indicator during WebDAV upload

### Paste Confirmation
- Ctrl+V no longer auto-sends images — shows preview bar first
- Caption field for adding text description before sending
- Send button (or Enter) to confirm, Escape or X to cancel

### Scroll to Bottom
- Floating ↓ button appears when scrolled up in chat history
- Click to jump to newest messages, teal hover highlight

### Fixes
- Typing indicator no longer leaks across conversations (room-scoped)
- Memory safety: QPointer guard in API callbacks, disconnect in setCache
- hasTopics flag preserved across conversation list refresh
- ThreadItem selection state properly bound in delegate
- thread_index cleared on conversation/account clear
- MessageCache wait timeout removed (prevents dangling lambdas)
- Deduplicated avatar images in squeezed ConversationItem
- Stale callback guard in ThreadListModel.fetchThreads
- refreshAfterCreate timer guards against conversation switch

---

## v0.5.3 (2026-03-19)

### Clipboard Paste
- Ctrl+V to paste screenshots and images directly into chat
- Clipboard images saved as temp PNG and sent via existing file upload pipeline

### User Status
- Online status heartbeat — TalQ now sets own status to "online" every 2 minutes
- Fixed status dots disappearing: dual-source status (user_status API primary, room API fallback)

### Context Menu Polish
- Layered shadow for depth (Telegram-style)
- Faster enter/exit transitions (100ms/60ms)
- Stronger emoji row separator and hover states
- Reduced emoji count to 6 to prevent overflow
- Quadrant-aware positioning with edge clamping

### Fixes
- Window no longer grows on every restart (geometry save guard during restore)
- Context menu no longer appears far above the cursor (correct popup height estimate)

---

## v0.5.2 (2026-03-19)

### Image Previews
- Authenticated inline image previews via `image://preview/` provider
- Thumbnails fetched from NC with auth headers, memory-cached per fileId
- Click image to open in Nextcloud browser

### Fixes
- Image attachments were invisible (preview disabled, pill excluded images)
- File shares now include read permission for recipients
- Deleted messages removed instantly from list
- Typing indicator filtered by current room
- Version bumped correctly across all files
- Auto-refresh disabled (push-only for real-time updates)

---

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
