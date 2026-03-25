# TalQ v0.9.0 Continue Prompt

## CRITICAL: Chat history scroll/data bug

### What happens
- Open a conversation (e.g., Sumeshini) — shows messages, then jumps to wrong position
- Latest messages exist in cache DB (verified: id 11825) but view shows ~id 11391
- With FRESH cache (deleted DB), everything works correctly

### Root cause (identified, NOT fixed)
1. Cache loads 49 messages in correct order (newest = 11825)
2. `refreshLatest()` and `loadHistory()` run concurrently from cache callback
3. `positionViewAtEnd()` fires but internal flick triggers `onFlickStarted` → `autoScrolling = false`
4. View stuck in wrong position, can't scroll to actual bottom

### What WORKS
- Fresh cache (delete message_cache.db) → loads and scrolls correctly
- Original v0.8.3 code works on office machine

### What was tried and FAILED (ALL REVERTED to c826479)
- BottomToTop ListView + reversed model (multiple proxy approaches)
- Remove/modify onContentHeightChanged, onFlickStarted, atYEnd
- Sequential loadHistory, delayed scrollToBottom, debounce timers
- All caused worse issues — code reverted to c826479 (PLI fix commit)

### How to fix (next session)
1. Add logging to MessageListModel: print IDs after each operation
2. Compare model state with server DB via SSH
3. Fix data ordering/insertion at model level
4. Consider proper BottomToTop with correct QAbstractProxyModel (study nheko/Quaternion source)

## What was done (2026-03-24/25, home machine)

### Video call
- PLI keyframe fix (src pad, periodic 5s timer)
- Video in initial PublishPipeline (withVideo param) — untested
- Camera error: give up immediately, no retry flood
- mfvideosrc with ksvideosrc fallback
- NC Talk signaling study: browser uses forceReconnect, not renegotiation

### Chat
- Newline rendering: \n → <br> in MessageBubble RichText
- Cache/scroll investigation — root cause identified but not fixed

### Infrastructure
- SSH to ncloud.123net.link via dev.netline.bg key (copied to Windows)
- Server DB: mysql -u root nextcloud
- Realtek driver downgraded 6.0.9929.1 → 6.0.9231.1

## Known bugs

| Bug | Priority | Notes |
|-----|----------|-------|
| Chat scroll shows wrong messages | CRITICAL | Works with fresh cache |
| m=video 0 in renegotiation SDP | HIGH | add-transceiver fix untested |
| Phone doesn't hear audio (home) | MEDIUM | Mic broken, works on office |
| Push notification not received | MEDIUM | hasCall race |
| Call not ending on remote hangup | LOW | participantLeftCall |
| Window height grows on restore | LOW | Unsigned int wrapping |

## Build

### Home machine
```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/msys64/mingw64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:/c/Qt/6.8.2/mingw_64/bin:$PATH"
rm -rf /c/build/talq && cmake -B /c/build/talq -S /c/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev && cmake --build /c/build/talq --target talq
mkdir -p /c/build/talq/gst-plugins && cp /c/msys64/mingw64/lib/gstreamer-1.0/libgst{coreelements,audioconvert,audioresample,autodetect,dtls,nice,opus,rtp,rtpmanager,srtp,wasapi,wasapi2,webrtc,app,level,vpx,openh264,videoconvertscale,winks,sctp,jpeg,mediafoundation}.dll /c/build/talq/gst-plugins/
QT_FORCE_STDERR_LOGGING=1 /c/build/talq/talq.exe
```

### SSH
```bash
ssh -i ~/.ssh/id_ed25519 root@ncloud.123net.link
mysql -u root nextcloud -e "SELECT id, SUBSTRING(message,1,80) FROM oc_comments c JOIN oc_talk_rooms r ON c.object_id=CAST(r.id AS CHAR) WHERE r.token='4pv7k2fj' AND c.object_type='chat' ORDER BY c.id DESC LIMIT 10"
```

## Test user
- test-talq / talQing123@ on https://ncloud.123net.link
- Sumeshini: 4pv7k2fj | Rakesh: bv86wo4c
