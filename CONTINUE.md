# TalQ v0.9.x Continue Prompt

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

| Bug | Priority | Notes |
|-----|----------|-------|
| m=video 0 in renegotiation SDP | HIGH | add-transceiver fix untested |
| Phone doesn't hear audio (home) | MEDIUM | Mic broken, works on office |
| Push notification not received | MEDIUM | hasCall race |
| Call not ending on remote hangup | LOW | participantLeftCall |
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
