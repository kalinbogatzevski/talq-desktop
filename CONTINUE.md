# TalQ v0.11.x Continue Prompt

## Next: QWidget full rewrite — eliminate QML engine (~150MB)

### Goal
Replace ALL QML with QWidget/QPainter to drop from ~385MB to ~50MB. The QML engine itself takes ~150MB just being loaded.

### Order (incremental)
1. **Sidebar** (ConversationList + ConversationItem) — QPainter list like ChatPainter
2. **Composer** (MessageComposer) — QLineEdit-based input with attach/reply bar
3. **Header** — QPainter or QWidget bar
4. **Main window** — QMainWindow replaces ApplicationWindow
5. **Login** — QDialog or QWidget
6. **Settings** — QDialog with tabs
7. **Call window** — QWidget
8. **Drop QML engine** — remove Qt Quick, Qt QML from dependencies

### Current status
ChatPainter (messages) is already pure QPainter. Sidebar is next.

## What was done (v0.11.0, 2026-03-27)

### ChatPainter — complete QPainter message renderer
- Replaced QML ListView + MessageBubble with QQuickPaintedItem
- 50% memory reduction (~385MB vs ~700MB+)
- Dynamic bubble widths, async images, clickable links
- Right-click context menu, hover action bar, emoji quick-react
- Read status cached in SQLite, dark/light mode
- Phase 10: old ListView removed, MessageBubble.qml dropped from build

## What was done (v0.9.5+, 2026-03-27)

### Decision
Replace QML `ListView + MessageBubble` with a single `QQuickPaintedItem` that renders messages via QPainter. This is the Telegram Desktop approach — zero delegate overhead, perfect scroll, ~50MB instead of ~700MB.

### Why
QML ListView with complex delegates (MessageBubble = ~80 QML items each) has fundamental performance issues:
- `positionViewAtEnd()` freezes because it instantiates ALL delegates
- BottomToTop layout works but has positioning/margin quirks
- Loaders reduce delegate count but not enough to prevent freeze
- Memory is 700MB+ for 50 messages — QML scene graph overhead
- After extensive debugging (2026-03-27), concluded QML ListView is not suitable for chat message rendering

### Architecture
```
Keep existing QML              New C++ component
├── ConversationList.qml       ChatPainter : QQuickPaintedItem
├── ChatView.qml ──────→        - paint() draws visible messages
│   └── ChatPainter {}           - pre-computed heights (QFontMetrics)
├── MessageComposer.qml         - scroll via QScrollBar / Flickable
├── CallWindow.qml               - takes MessageListModel
└── Settings, Login, etc.        - handles click/hover/right-click
```

### Files
- Create: `src/core/ChatPainter.cpp` + `.h` (~600 lines)
- Modify: `src/qml/ChatView.qml` (replace ListView with ChatPainter)
- Delete: `src/qml/MessageBubble.qml` (no longer needed)
- Untouched: MessageListModel, all backend C++, all other QML

### Reference
- Telegram Desktop: custom QWidget, paints messages in paintEvent()
- Implementation plan: `docs/superpowers/plans/2026-03-27-chat-history-refactor.md`

## What was done (2026-03-27)

### Chat history debugging (extensive)
- **Root cause identified**: `positionViewAtEnd()` / `positionViewAtIndex()` forces Qt to instantiate ALL MessageBubble delegates (~80 QML items each × 50 = 4000 items = freeze)
- **Loaders added** to MessageBubble: avatar, reply, file, reactions wrapped in Loader components (Task 3 of refactor plan)
- **TopToBottom revert attempted** but `positionViewAtIndex` still freezes even with Loaders
- **BottomToTop kept** as interim — no freeze, positioning 90% correct (last message slightly clipped)
- **Decision**: abandon QML ListView for messages, implement QPainter-based ChatPainter

### Janus H264 fix
- Janus videoroom plugin configured with `videocodec = "vp8,h264"` (was vp8-only)
- Root cause of "Unsupported codec 'none'" error in Janus logs
- Docker container recreated with custom `janus.plugin.videoroom.jcfg`
- SSH key from storm.123net.link copied to access ncloud.123net.link from office

### C++ fixes (from review agents)
- Poller: exponential backoff on 401/403/5xx, stop on fatal errors
- Poller: guard against lastKnown=0 downloading entire history
- Message trim: 200 message cap per conversation
- m_messageIds consistency in postAndReplace/deleteMessage
- refreshLatest after file share
- cancelAll() safe iteration
- Cross-thread image providers: moveToThread to main thread
- PushClient: WebSocket error handler, auth failure reconnect
- m_hasMoreHistory reset in setThreadId/setHideThreadMessages
- ConversationItem required property notificationLevel

## What was done (v0.9.5+, 2026-03-27)

### Video call bugs — ALL FIXED
- **STUN URL format** — `stun:` → `stun://` conversion applied to PublishPipeline + SubscribePipeline (was only in PeerPipeline). STUN was silently failing in MCU mode.
- **m=video 0 in renegotiation** — Root cause: `enableCamera()` created two transceivers (orphaned one → `m=video 0`). Fixed with single transceiver approach in both PeerPipeline and PublishPipeline.
- **Audio device selection** — mic/speaker from Settings were silently ignored. PublishPipeline never passed `audioDeviceId`; `autoaudiosink` doesn't propagate `device` property. Fixed with `wasapi2sink` → `wasapisink` → `directsoundsink` fallback.
- **Incoming call detection race** — overlapping conversation refreshes both missed `hasCall` false→true. Fixed with persistent `m_callState` map.
- **Window height grows on restore** — save guard during Maximized→Windowed transition + clamp to screen bounds.
- **Window restore from tray** — now restores maximized state (was always Windowed).

### forceReconnect for camera toggle (matches browser behavior)
**Key discovery**: Nextcloud Talk browser does `forceReconnect()` when toggling camera — tears down entire publisher and recreates with video from the start. The MCU only forwards what the publisher includes at initial offer time. Renegotiating video onto existing connection doesn't update MCU routing.

**Implementation**: `CallManager::toggleCamera()` now calls `forceReconnectPublisher()` in MCU mode: stops PublishPipeline + all SubscribePipelines, creates new publisher with/without video, re-requests subscriber streams after ICE connects.

### MCU video delivery — UNSOLVED
Publisher SDP includes video (`m=video` with H264). MCU accepts it. But MCU subscriber offers come back **audio-only**. Both `enableCamera` renegotiation and `forceReconnect` approaches produce the same result — MCU doesn't include video in subscriber offers.

**Suspected cause**: GStreamer's webrtcbin with `MAX_BUNDLE` policy encodes video as `m=video 0` with `a=bundle-only` (valid per RFC 8843). Janus may not recognize this as active video and skips it in subscriber offers. The browser likely sends `m=video 9` (non-zero port).

**Next steps**:
1. Capture browser's actual SDP to compare format (WebSocket intercept or Janus logs)
2. Check if setting `bundle-policy` to `BALANCED` or `NONE` changes the m=video port
3. Or check Janus videoroom plugin config on server
4. SSH to server: `ssh -i ~/.ssh/id_ed25519 root@ncloud.123net.link` and check HPB/Janus logs

### Call test harness improvements
- Video renegotiation test phase with SDP validation
- `videotestsrc` support in PeerPipeline and PublishPipeline
- SubscribePipeline integration — requests subscriber streams from MCU via `requestOffer`
- `forceReconnect` pattern in test (matches real app behavior)
- `VideoFrameProvider.frameCount` for headless frame tracking
- Test consistently PASSES for: signaling, ICE, SDP validation, local preview frames
- Remote video frames = 0 (MCU subscriber offers are audio-only — server-side issue)

## What was done (v0.9.4, 2026-03-26)

### Conversation switch freeze — RESOLVED
**Root cause**: `positionViewAtEnd()` / `positionViewAtIndex()` forced Qt to instantiate ALL MessageBubble delegates simultaneously. With complex delegates (RowLayout, ColumnLayout, avatars, images), this caused 1-4GB memory explosion and UI freeze.

**Solution**: BottomToTop ListView with newest-first model storage.
- Messages stored newest-first: `m_messages[0]` = newest
- ListView `verticalLayoutDirection: BottomToTop` — index 0 renders at the bottom
- View naturally starts at bottom — **no scroll calls needed**
- New messages from poller prepend at index 0 (appear at bottom automatically)
- History appends at end (appears at top on scroll-up)
- `cacheBuffer: 200` limits delegate creation to viewport + small buffer

### C++ fixes
- Poller backoff (401/403/404 stops, 5xx exponential backoff)
- Poller lastKnown:0 guard (was downloading 2000+ messages)
- Message trimming: 200 message cap per conversation
- m_messageIds in postAndReplace (prevents duplicates)
- refreshLatest after file share
- cancelAll() safe iteration
- Cross-thread image providers: moveToThread to main thread
- PushClient error handler + auth failure reconnect
- m_hasMoreHistory reset in setThreadId/setHideThreadMessages
- ConversationItem required property notificationLevel

### QML fixes
- MessageBubble reply background outside ColumnLayout (was 4000+ re-layout cycles)
- BottomToTop layout eliminates all positionViewAtEnd/positionViewAtIndex calls

## What was done (v0.9.2, 2026-03-26)

### Call bugs fixed
- **MCU ICE candidate double-nesting** — Janus sends `payload.candidate = {candidate: "..."}`, our code was passing outer payload. Fixed in SignalingClient.cpp. This likely caused intermittent call failures.
- **STUN URL format** — Nextcloud returns `stun:host:port`, GStreamer needs `stun://host:port`. Fixed in PeerPipeline.cpp.
- **P2P won't work on this server** — HPB intercepts all signaling, "No MCU client found" for direct P2P. All calls must go through MCU (Janus).

### Automated call test harness
- `talq-call-test.exe` — two-user MCU call test, uses audiotestsrc, verifies ICE through real STUN/TURN
- Run: `powershell.exe -Command '...'` from build dir (see scripts/deploy-dev.sh)
- Test token: `u2f3gbu4` (kalin <-> test-talq 1:1 room)
- PASSED: both peers ICE connected + 3s stability + clean teardown

### Room avatars (v0.9.1)
- Group chat avatars via authenticated OCS API
- TqAvatar token fallback for rooms without userId

## What was done (v0.9.0, 2026-03-25)

### Memory leak / scroll fix (RESOLVED)
Root cause was a triple hit:
1. **Untracked `refreshLatest()` reply** — rapid conversation switching stacked orphan network callbacks that injected duplicate messages and spawned duplicate pollers. Fixed: added `m_refreshReply` member, cancelled on conversation switch.
2. **`loadHistory()` cascade** — `onContentYChanged` re-triggered after each prepend, loading entire history into memory. Fixed: 500ms debounce timer + `userHasScrolled` guard.
3. **Unbounded `FilePreviewProvider` cache** — every preview (~3MB each at 1024x768 ARGB32) cached forever. Fixed: 50MB LRU eviction cap.

Additional fixes in same commit (af0084f):
- `refreshLatest()` ordering: partition missing messages into older (prepend) / newer (append)
- Persistent `m_messageIds` QSet replaces per-poll O(n) rebuild
- `onLastCommonReadChanged` scoped to changed range (was emitting for ALL messages every 15s)
- `scrollToBottom()` coalesced via 16ms timer
- `onContentHeightChanged` removed (infinite loop source)
- `onFlickStarted` guarded against programmatic scrolls
- `AvatarProvider` memory cache capped at 200 entries
- Duplicate "Reply" in context menu removed
- `imageViewer.open()` crash → `downloadFile()`

### Video call
- PLI keyframe fix (src pad, periodic 5s timer)
- Video in initial PublishPipeline (withVideo param) — untested
- Camera error: give up immediately, no retry flood
- mfvideosrc with ksvideosrc fallback
- NC Talk signaling study: browser uses forceReconnect, not renegotiation

### Chat
- Newline rendering: \n → <br> in MessageBubble RichText

### Infrastructure
- SSH to ncloud.123net.link via dev.netline.bg key (copied to Windows)
- Server DB: mysql -u root nextcloud
- Realtek driver downgraded 6.0.9929.1 → 6.0.9231.1
- v0.9.0 release created on GitLab with both installers

## Next: MCU video delivery

| Bug | Priority | Status |
|-----|----------|--------|
| MCU subscriber offers audio-only | HIGH | Publisher includes video but MCU doesn't forward — SDP format issue |
| ~~STUN URL in MCU pipelines~~ | ~~HIGH~~ | FIXED — stun: → stun:// |
| ~~m=video 0 in renegotiation~~ | ~~HIGH~~ | FIXED — single transceiver |
| ~~Audio device selection~~ | ~~MEDIUM~~ | FIXED — device IDs passed to sinks |
| ~~Incoming call detection race~~ | ~~MEDIUM~~ | FIXED — persistent m_callState |
| ~~Window height grows on restore~~ | ~~LOW~~ | FIXED — clamp to screen bounds |
| ~~Window restore from tray~~ | ~~LOW~~ | FIXED — wasMaximized tracking |

## Build

```bash
# Clean build
taskkill.exe //IM talq.exe //F 2>/dev/null; sleep 3
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
rm -rf /c/build/talq
cmake -B C:/build/talq -S C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build C:/build/talq

# Deploy DLLs + launch (handles Qt/MSYS2 runtime conflict)
bash scripts/deploy-dev.sh

# Incremental build + run
cmake --build C:/build/talq && bash scripts/deploy-dev.sh
```

### SSH
```bash
ssh -i ~/.ssh/id_ed25519 root@ncloud.123net.link
mysql -u root nextcloud -e "SELECT id, SUBSTRING(message,1,80) FROM oc_comments c JOIN oc_talk_rooms r ON c.object_id=CAST(r.id AS CHAR) WHERE r.token='4pv7k2fj' AND c.object_type='chat' ORDER BY c.id DESC LIMIT 10"
```

## Test user
- test-talq / talQing123@ on https://ncloud.123net.link
- Sumeshini: 4pv7k2fj | Rakesh: bv86wo4c
