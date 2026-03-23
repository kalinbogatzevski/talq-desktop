# TalQ v0.9.0 Continue Prompt

## What was done (2026-03-23, evening session)

### Video call fixes (major progress)
- **NC Talk signaling compatibility** — mute/unmute broadcast, updateCallFlags, participantFlagsChanged detection
- **Subscriber re-request** — after MCU video renegotiation, re-request all subscriber streams
- **Auto-camera on video call** — enableCamera() called automatically, delayed 1s after pipeline start to avoid blocking subscriber discovery
- **Dual call buttons** — phone icon (green, audio) + camera icon (teal, video) in chat header
- **Call status breadcrumbs** — shows signaling progress: Joining → Fetching servers → Starting pipeline → ICE checking → Connected
- **GStreamer plugin check** — callsAvailable property hides call buttons when plugins missing, shows tooltip with missing plugin names
- **Detailed pipeline errors** — bus error messages extracted on start failure (wasapi2src "Failed to open device" etc.)
- **SDP validation** — reject offers with no media lines before sending to MCU
- **Audio source fallback** — wasapi2src → wasapisrc → autoaudiosrc chain; TALQ_TEST_AUDIO=1 env var for audiotestsrc
- **Keyframe PLI** — send aggressive PLI requests (0s, 1s, 2s, 4s) on subscriber video chain link, fixes 4-min corruption

### Design system
- **Warm Carbon Phase 1** — all Theme.qml colors migrated, 30+ new tokens
- **Phase 2 components** — TqAvatar, TqIconButton, TqBadge, TqSwitch, TqComboBox (484 lines removed)
- **TqIcon SVG system** — 22 geometric thin-stroke icons via Qt Shapes, replace emoji in sidebar/chat/composer
- **Settings dialog** — full 4-tab dialog with dark mode styling, per-conversation mute

### Error handling hardening
- Connected pipeline error() signals (publish, peer, subscribe)
- Check pipeline start() return values
- Log failures for updateCallFlags, leaveCallOnServer
- WebSocket errorOccurred handler, JSON validation
- Named constants for call flags, DRY helpers

### Home machine audio driver
- Realtek driver v6.0.9929.1 (CCleaner auto-update) broke all mic capture
- Downgraded to v6.0.9231.1 — wasapi2src/wasapisrc work from command line
- Built-in mic still not capturing audio (possible hardware issue on this ASUS ZenBook Pro Duo UX582LR)
- **Proven with audiotestsrc**: full audio pipeline works end-to-end (phone hears 440Hz tone through MCU)

### GStreamer plugins deployment
- Must include `libgstwasapi.dll` (v1) alongside `libgstwasapi2.dll` in gst-plugins/
- Without wasapi v1, wasapisrc element can't be created and autoaudiosrc fails

## Known issues to fix

### CRITICAL: Video send SDP has `m=video 0` (port 0 = disabled)
- Camera enables locally (preview works), renegotiation offer created
- But SDP has `m=video 0` — webrtcbin marks video as inactive after mid-session add
- MCU accepts the offer but doesn't forward video (port 0 = no media)
- Phone showed TalQ camera in previous test but not consistently
- **Root cause**: webrtcbin renegotiation with dynamically added video pads
- **Fix needed**: ensure video transceiver is properly activated before create-offer, or create the pipeline with video from the start and just not send until camera is enabled

### Audio: phone doesn't hear test tone despite ICE completed
- Publisher ICE: checking → connected → completed (all green)
- SDP has m=audio with OPUS, MCU answers with recvonly
- But phone shows "muted" — MCU might not be forwarding
- Could be DTLS/SRTP issue, or MCU not associating publisher stream with subscriber session
- **Test on office machine with real mic** to rule out audiotestsrc quirks

### Other
- "Not allowed to request offer" — race condition, sent before peer's inCall flag is set on server
- Incoming call not detected if hasCall was already true on app start
- Camera `ksvideosrc` exclusive access issue — needs Windows Frame Server Mode
- Home machine built-in mic broken (hardware/driver issue, not TalQ)

## Test user
- Username: `test-talq` / Password: `talQing123@`
- 1:1 conversation with kalin: token `u2f3gbu4`
- Can log in at `https://ncloud.123net.link` in browser or Android Nextcloud Talk app

## Build

### Home machine
```bash
export PATH="/c/msys64/mingw64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:/c/Qt/6.8.2/mingw_64/bin:$PATH"
cd /c/build/talq
cmake C:/Users/bogat/Desktop/My\ Projects/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build . --target talq
# Deploy GStreamer plugins (including wasapi v1!)
mkdir -p gst-plugins && cp /c/msys64/mingw64/lib/gstreamer-1.0/libgst{coreelements,audioconvert,audioresample,autodetect,dtls,nice,opus,rtp,rtpmanager,srtp,wasapi,wasapi2,webrtc,app,level,vpx,openh264,videoconvertscale,winks,sctp,jpeg}.dll gst-plugins/
```

### Office machine
```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
cd C:/build/talq
cmake C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build . --target talq
```

### Run
```bash
export QT_FORCE_STDERR_LOGGING=1
# Normal:
C:/build/talq/talq.exe
# Test audio (440Hz tone instead of mic):
TALQ_TEST_AUDIO=1 C:/build/talq/talq.exe
```

## Priority for next session

1. **Fix `m=video 0` in renegotiation** — the video send SDP must have an active video port
2. **Test real mic audio on office machine** — verify phone hears real voice
3. **Fix "Not allowed to request offer" race** — delay requestOffer until participant flags confirm inCall
4. **Release v0.9.0** with working video calls
