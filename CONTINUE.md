# TalQ v0.8.0 → v0.9.0 Continue Prompt

## What was done (2026-03-22)

### v0.8.0 — Video Calls (MAJOR RELEASE)
- **Video receive**: VP8/H.264 auto-detection, GStreamer appsink → QVideoFrame → Qt VideoOutput
- **Camera send**: ksvideosrc → openh264enc, 1080p/720p, toggle mid-call
- **CallWindow**: VideoOutput fills window, overlay controls auto-hide, camera toggle, duration overlay
- **Settings**: camera selection, video quality preset (Qt.labs.settings)
- **TURN servers**: parsed from signaling settings, URL-encoded credentials, configured on webrtcbin
- **Device selection**: mic/speaker IDs wired to wasapi2src/wasapi2sink
- **Decline fix**: leave room (not call), m_joinedCall tracking, no more 404
- **Auto-decline fix**: m_userActionReady gate on popup Component.onCompleted
- **Chat scroll**: stable scroll-to-bottom, image height reservation, no layout shift
- **Image viewer**: in-app dark window, click/Esc to close
- **Context menu**: Download + Open in Nextcloud for file messages
- **Reply bubbles**: min width 260px when quoting, prevents clipping
- **Qt6::Multimedia**: QVideoSink, QVideoFrame, VideoOutput
- **GStreamer plugins**: vpx, openh264, videoconvertscale, winks

### v0.7.1 — Chat Reliability & UX (PATCH RELEASE)
- **Fixed message cache** — stores original server JSON instead of reconstructed subset; file attachments, mentions, and thread metadata no longer lost after conversation switch
- **Schema migration** — v2 cache schema auto-purges stale v1 entries on first launch
- **Fixed chat scroll** — replaced aggressive `onContentHeightChanged` with targeted `onCountChanged`; auto-scroll no longer breaks during image upload or footer changes
- **Text selection** — message text is now selectable (click-drag); ListView uses WheelHandler instead of drag-to-scroll
- **Scroll-to-bottom button** — properly shows/hides based on scroll position; scrollbar fades in/out on activity
- **Ctrl+V file paste** — now handles files from Explorer (not just screenshots); images get preview, other files show icon
- **Caption for all file sends** — file dialog, paste, and drag-drop all show confirmation bar with caption field before sending
- **v0.8.0 design spec written** — video calls + call polish planned (docs/superpowers/specs/)

### v0.7.0 — Audio Calls (MAJOR RELEASE)
- **Bidirectional audio calls working** via Nextcloud Talk MCU (Janus/HPB)
- Split pipeline: PublishPipeline (send-only) + SubscribePipeline (receive-only)
- MCU signaling: publish ownPeer offer to self, requestOffer for remote subscribers
- Incoming call detection via push notifications + conversation `hasCall` field
- Critical fix: must wait for HPB room join confirmation BEFORE calling the call API
- GStreamer `level` element for real-time audio metering
- Bus polling (not watch) for level messages — watch was consuming them before pollBus could read
- GValueArray extraction with `G_TYPE_VALUE_ARRAY` via `gst_structure_get`
- Call window with scrolling waveform (Canvas bars), mic level fill in button, call stats panel
- Settings dialog (Ctrl+,) with mic/speaker device selection
- Ringtones: ding-dong chime (incoming), brrr-brrr ringback (outgoing)
- All security fixes: Windows Credential Manager, path sanitization, memory zeroing
- All code review fixes: GStreamer thread safety, bus watch cleanup, dead code removal

### Test user
- Username: `test-talq` / Password: `talQing123@`
- 1:1 conversation with kalin: token `u2f3gbu4`
- Can log in at `https://ncloud.123net.link` in browser for call testing

## Known issues to fix next

### Call flow
- **Incoming call auto-decline race**: QML signal race causes instant decline; mitigated with 2s guard but root cause (likely IncomingCallPopup signal) not fully resolved
- **Decline doesn't notify caller**: NC Talk 1:1 calls use "waiting room" model; caller waits indefinitely. Our decline attempt gets 404 because we're not in the call yet
- **Incoming call not detected if `hasCall` was already true on app start** (no false→true transition)
- **"Unknown" remote peer name** in some call flows — name set in `onIncomingCallDetected` but may not persist through all state transitions
- **Conversation list doesn't auto-update** when a new conversation is created externally

### Audio
- **Mic level scaling**: peaks at -90dB in silence, speech around -30 to -10dB. Current range -100 to 0 with squared curve works but could be tuned
- **No TURN server support**: STUN works (using own server), but TURN credentials from signaling settings not yet configured on webrtcbin. Needed for calls behind NAT
- **No echo cancellation**: GStreamer has `webrtcdsp` plugin (AEC/NS/AGC) from `gst-plugins-bad` — add to publish pipeline

### UI
- **Scroll-to-bottom button**: visibility logic works (`!autoScrolling`) but UX needs polish
- **Call window design**: functional but needs frontend-design skill for proper polish
- **Video calls**: v0.8.0 — SubscribePipeline already receives video pads from MCU but skips them. Need `vp8dec`/`h264dec` + QML VideoOutput via `qmlglsink`

## Architecture reference

### MCU Call Flow (working)
```
Outgoing:
1. startCall → POST /api/v4/call/{token} (join call)
2. Fetch STUN from /api/v3/signaling/settings
3. PublishPipeline starts → offer to own session via HPB
4. MCU answers → publisher ICE connects
5. participantJoinedCall event → requestOffer for remote peer
6. MCU sends subscriber offer → SubscribePipeline answers → ICE connects → audio flows

Incoming:
1. Push notification → conversation refresh → hasCall detected
2. IncomingCallPopup shown with ringtone
3. Accept → POST participants/active (join room)
4. Wait for HPB roomJoined signal (CRITICAL)
5. POST /api/v4/call/{token} (join call)
6. Same as outgoing steps 2-6
```

### Key signaling message format
```json
{
  "type": "message",
  "message": {
    "recipient": {"type": "session", "sessionid": TARGET},
    "data": {
      "to": TARGET, "sid": "Date.now()", "roomType": "video",
      "type": "offer|answer|candidate|requestoffer",
      "payload": { ... }
    }
  }
}
```

### Server config
- HPB: `wss://ncloud.123net.link/standalone-signaling/spreed`
- MCU: enabled (Janus) — confirmed via `server.features: ["mcu", ...]`
- STUN: `stun:turn-za.123net.link:3478`, `stun:turn-bg.123net.link:3478`
- TURN: `turn:turn-za.123net.link`, `turn:turn-bg.123net.link` (with time-limited credentials)
- Hello: v1.0 (v2.0 needs JWT auth we don't have)

## Build (home machine)
```bash
export PATH="/c/msys64/mingw64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:/c/Qt/6.8.2/mingw_64/bin:$PATH"
cd C:/build/talq
cmake C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64
cmake --build . --target talq
# Ensure GStreamer plugins are in gst-plugins/ next to exe:
mkdir -p gst-plugins && cp /c/msys64/mingw64/lib/gstreamer-1.0/libgst{coreelements,audioconvert,audioresample,autodetect,dtls,nice,opus,rtp,rtpmanager,srtp,wasapi2,webrtc,app,level,vpx,openh264,videoconvertscale,winks}.dll gst-plugins/
# Run:
export QT_FORCE_STDERR_LOGGING=1
C:/build/talq/talq.exe > /tmp/talq-debug.log 2>&1 &
```

## What was done (v0.8.0)

### P1: Video calls (DONE)
1. VideoFrameProvider: GStreamer appsink → QVideoFrame → QVideoSink bridge
2. SubscribePipeline: video decode branch (VP8/H264) with codec auto-detection
3. PublishPipeline: camera capture (ksvideosrc → openh264enc), 1080p/720p
4. CallWindow: VideoOutput, overlay controls with auto-hide, camera toggle
5. SettingsDialog: camera selection, resolution preset (Qt.labs.settings)

### P2: Call polish (DONE)
1. TURN server configuration (parsed from signaling settings, URL-encoded credentials)
2. Device selection wired to pipelines (wasapi2src/wasapi2sink device property)
3. Incoming call decline fix (leave room, not call; m_joinedCall tracking)
4. Auto-decline race condition fix (m_userActionReady gate)

## What to do next (v0.9.0)

### Priority 1: Group calls + screen sharing
- Multiple SubscribePipelines with video (grid layout)
- Screen sharing capture (dxgiscreencapsrc or similar)
- Self-preview PIP

### Priority 2: Call improvements
- Echo cancellation (shared pipeline or custom GStreamer build)
- Mid-call device switching
- qml6glsink for zero-copy video rendering (custom GStreamer build)

### Priority 3: Chat improvements
- Message search
- Emoji picker
- Custom GStreamer build (Meson, selective plugins)

### Priority 4: UX polish
- Use frontend-design skill for CallWindow, settings, chat UI
- Proper app icon and branding
- System tray improvements
- Dark/light theme refinement

## NC Talk source reference
- Clone: `git clone --depth 1 https://github.com/nextcloud/spreed.git /tmp/spreed`
- Signaling: `src/utils/signaling.js`
- WebRTC: `src/utils/webrtc/webrtc.js`
- Peer: `src/utils/webrtc/simplewebrtc/peer.js`
- SimpleWebRTC: `src/utils/webrtc/simplewebrtc/simplewebrtc.js`
