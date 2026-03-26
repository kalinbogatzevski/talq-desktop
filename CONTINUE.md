# TalQ v0.9.x Continue Prompt

## CRITICAL: Conversation switch freeze (UNSOLVED)

### Symptom
Switching from one conversation to another causes the app to freeze and memory to grow from ~270MB to 1-4GB. Happens on EVERY conversation switch, even on clean v0.9.2 baseline. Previously thought to be intermittent — it's consistent.

### What we know
- Freeze happens right after `Cache loaded 20 messages for "..."` when opening 2nd conversation
- The MEM timer and event loop stop completely — main thread is blocked
- `refreshLatest()` network response never arrives (event loop dead)
- NOT caused by: our C++ fixes, loadHistory cascade, poller loop, image provider threading
- The `MessageBubble.qml:578` anchors-in-layout issue caused 4000+ re-layout cycles per conversation open — FIXED (moved Rectangle outside ColumnLayout) — but freeze persists
- `positionViewAtEnd()` triple-call replaced with single `positionViewAtIndex` — but freeze persists
- Happens with empty cache too (deleted message_cache.db)

### Root cause hypothesis
The QML ListView delegate creation for 20+ messages blocks the main thread. Each MessageBubble is complex (RowLayout, ColumnLayout, avatars, images, Canvas elements). Creating 20 simultaneously saturates the event loop. Possible that `positionViewAtIndex` still forces ALL delegates to be measured.

### Next steps
1. **Profile with QML profiler** — attach `QT_QML_DEBUG` to see which QML operations take time
2. **Reduce delegate complexity** — use Loader for reply/reaction/file sections, only instantiate visible parts
3. **Async delegate creation** — set `ListView.cacheBuffer: 0` and `displayMarginBeginning/End: 0` to force minimal delegate creation
4. **Try `ListView.reuseItems: true`** (Qt 6.6+) to recycle delegates instead of creating/destroying

## What was done (v0.9.3, 2026-03-26)

### C++ fixes (committed, pushed)
- **Poller backoff**: HTTP 401/403/404 stops polling; 5xx retries with exponential backoff 2s→60s
- **m_messageIds in postAndReplace**: swap temp→real ID, prevents poller duplicates
- **refreshLatest after file share**: server-generated file message now appears immediately
- **m_hasMoreHistory reset**: in setThreadId/setHideThreadMessages
- **m_messageIds before endInsertRows**: in refreshLatest, prevents timing-dependent duplicates
- **cancelAll() safe iteration**: copies list before aborting (was UB)
- **PushClient error handler**: WebSocket errors now logged
- **PushClient auth failure**: closes and reconnects instead of permanent dead state
- **markAsRead**: uses proper method on conversation open

### QML fixes (committed, pushed)
- **MessageBubble anchors loop**: reply background Rectangle moved outside ColumnLayout (was causing 4000+ re-layout cycles)
- **scrollToBottom simplified**: single positionViewAtIndex instead of triple positionViewAtEnd

### Review findings (not yet fixed — lower priority than the freeze)
- Cross-thread image providers (QQuickPixmapReader calls getAbsoluteUrl from render thread)
- FilePreviewProvider duplicate fileId overcounting
- Avatar cache failure entries not evictable
- Deferred finished() for cache hits
- ConversationItem required property notificationLevel
- Message rawJson doubles per-message memory

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

## Next: Stabilize video calls

| Bug | Priority | Status |
|-----|----------|--------|
| P2P mode broken (MCU-only server) | HIGH | m_useP2P codepath dead on HPB servers |
| m=video 0 in renegotiation SDP | HIGH | add-transceiver fix untested |
| Phone doesn't hear audio (home) | MEDIUM | Mic broken, works on office |
| Push notification not received | MEDIUM | hasCall race |
| Call not ending on remote hangup | LOW | participantLeftCall only handles MCU subs |
| Window height grows on restore | LOW | Unsigned int wrapping |

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
