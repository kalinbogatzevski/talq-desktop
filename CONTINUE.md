# TalQ v0.9.0 Continue Prompt

## CRITICAL BUG: Chat history shows wrong/old messages

### Symptom
- Open Sumeshini's conversation — shows "thats valid" as last message
- Server DB has message id 11813 "#55968 - no sub task..." as latest
- Cache has 49 messages up to id 11813 (correct)
- But the model displays older messages, not the latest ones

### Root cause (not yet fixed)
The `refreshLatest()` function in `MessageListModel.cpp` has a data merge bug:
- Cache loads 49 messages into the model
- `refreshLatest()` fetches 50 newest from server
- The merge logic fails — either duplicates, overwrites, or misordering
- Was observed running 6x in a row adding 29 "missing" messages each time
- The `loadHistory()` callback also calls `emit newMessagesAtEnd()` after prepending OLD messages at position 0

### What NOT to do
- Do NOT try to fix this by changing scroll logic in ChatView.qml
- Do NOT remove `emit newMessagesAtEnd()` from the model — it breaks data loading
- The scroll and data issues are separate problems that were entangled today

### How to debug
1. SSH to server: `ssh -i ~/.ssh/id_ed25519 root@ncloud.123net.link`
2. Query actual messages: `mysql -u root nextcloud -e "SELECT id, actor_id, SUBSTRING(message,1,80) FROM oc_comments c JOIN oc_talk_rooms r ON c.object_id=CAST(r.id AS CHAR) WHERE r.token='4pv7k2fj' AND c.object_type='chat' ORDER BY c.id DESC LIMIT 10"`
3. Compare with cache: `sqlite3 C:/Users/bogat/AppData/Roaming/TalQ/TalQ/message_cache.db "SELECT message_id FROM messages WHERE token='4pv7k2fj' ORDER BY message_id DESC LIMIT 10"`
4. Add logging to `refreshLatest()` to see what IDs it fetches and what it considers "missing"

### Scroll management (separate issue, not fixed)
- `onContentHeightChanged` with `positionViewAtEnd` causes snap-back when user scrolls up
- Qt's recommended approach is `BottomToTop` layout (official chat tutorial)
- All incremental patches attempted today were reverted — back to c826479 state
- Needs a proper redesign, possibly BottomToTop with newest-first model

## What was done (2026-03-24, home machine)

### Video call improvements
- PLI keyframe direction fix (src pad not sink) + periodic 5s PLI timer
- Video included in initial PublishPipeline (no mid-session renegotiation)
- Camera error: give up immediately instead of retry-flooding signaling
- SSH access to ncloud.123net.link server established (via dev.netline.bg key)
- SSH key unified across machines (Windows now uses same key as dev server)

### Scroll attempts (all reverted)
- Tried: remove onContentHeightChanged, atYEnd tracking, debounced scroll, contentY direct manipulation, model signal changes
- All caused worse issues (missing data, wrong position, empty history)
- Reverted to c826479 state

## What was done (2026-03-24, office session)
- Video SDP fix: add-transceiver approach for m=video (untested on home machine)
- Audio tested: Ilko heard Kalin (office machine calls work)
- Camera preview leaky queue fix
- 20 missing DLLs added to installer
- mfvideosrc instead of ksvideosrc

## Known bugs

| Bug | Priority | Notes |
|-----|----------|-------|
| Chat shows wrong/old messages | CRITICAL | refreshLatest merge bug |
| Scroll snaps back on scroll-up | HIGH | onContentHeightChanged too aggressive |
| m=video 0 in renegotiation SDP | HIGH | add-transceiver fix untested |
| Phone doesn't hear audio (home) | MEDIUM | Built-in mic broken, works on office |
| "Not allowed to request offer" | MEDIUM | Race condition |
| Push notification not received | MEDIUM | hasCall already true on app start |
| Call not ending when phone hangs up | LOW | participantLeftCall not detected |

## Build (home machine)
```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/msys64/mingw64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:/c/Qt/6.8.2/mingw_64/bin:$PATH"
rm -rf /c/build/talq
cmake -B /c/build/talq -S /c/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build /c/build/talq --target talq
mkdir -p /c/build/talq/gst-plugins && cp /c/msys64/mingw64/lib/gstreamer-1.0/libgst{coreelements,audioconvert,audioresample,autodetect,dtls,nice,opus,rtp,rtpmanager,srtp,wasapi,wasapi2,webrtc,app,level,vpx,openh264,videoconvertscale,winks,sctp,jpeg,mediafoundation}.dll /c/build/talq/gst-plugins/
QT_FORCE_STDERR_LOGGING=1 /c/build/talq/talq.exe
```

## SSH access
```bash
ssh -i ~/.ssh/id_ed25519 root@ncloud.123net.link
# DB: mysql -u root nextcloud
# NC config: /var/www/ncloud.123net.link/config/config.php
# HPB: docker logs talk-hpb_signaling_1
# Push: journalctl -u notify_push
```

## Test user
- test-talq / talQing123@ on https://ncloud.123net.link
- Sumeshini token: 4pv7k2fj
- Rakesh token: bv86wo4c
- Test TalQ token: u2f3gbu4
