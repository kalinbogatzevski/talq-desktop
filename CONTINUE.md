# TalQ v0.6.2 → v0.7.0 Continue Prompt

## What was done this session

### Fixes shipped (v0.6.2)
- **Memory leaks fixed**: `m_pendingReplies` never cleaned (added `trackReply` auto-remove), message model capped at 200
- **File download fixed**: uses `/index.php/f/{fileId}/download` — works for all participants, not just sender
- **DLL dependencies**: added 22 missing GStreamer/GnuTLS transitive DLLs + fixed libwinpthread `clock_gettime64` mismatch
- **Debug monitor**: Ctrl+D overlay showing memory, cache sizes, pending requests (permanent dev tool)
- **Installer improvements**: wizard logos, VERSIONINFO resource, version bumped

### Call progress (v0.7.0 in progress)
- Call UI: CallWindow with SVG icons, pulse animation, avatar, ringtone (440Hz WAV)
- Signaling: HPB feature detection — server reports MCU support
- SDP exchange works: offer → answer → remote description set
- **BLOCKER**: Audio not connecting. ICE stays at "new" state.

## Current state of calls

### Architecture (learned from NC Talk source)
The NC server at `ncloud.123net.link` has a **standalone signaling server (HPB)** with **MCU (Janus)** enabled.
The NC web client uses this MCU architecture:
1. Each participant publishes to MCU (sends offer addressed to **own session ID**)
2. MCU answers back (establishes publish channel)
3. Each participant sends `requestOffer` for each remote peer's stream
4. MCU sends offer → client creates answer (establishes subscribe channel)
5. MCU bridges audio/video between publish and subscribe channels

### What we implemented
- `CallManager::joinCallOnServer` → detects MCU via `m_signaling->hasMcu()`
- MCU flow: publish ownPeer (offer to self), then `requestOffer` for remote
- P2P fallback (for servers without MCU): larger session ID creates offer
- `CallSignaling` class for internal HTTP polling (disabled — server returns 400 when HPB is active)
- GLib main context pump (`m_glibTimer` at 20ms) for GStreamer signal dispatch

### What's NOT working
- **ICE state stuck at "new"**: after setting remote description (answer), ICE never transitions to "checking".
  - GLib pump is running (bus messages dispatch)
  - ICE gathering completes (`gathering=2`)
  - But no ICE connectivity checks start
  - The `notify::ice-connection-state` signal never fires
  - Poll confirms ICE stays at "new" permanently
- **MCU message routing**: "No MCU client found to send message to" — MCU might need the ownPeer publish step to work first before candidates can be relayed
- **Possible root cause**: The MCU answer to our ownPeer offer may not be arriving, or the pipeline needs TWO webrtcbin elements (one for publish, one for subscribe) like the NC web client does

### NC Talk source code
Cloned at `/tmp/spreed/` (shallow clone). Key files:
- `src/utils/signaling.js` — HPB WebSocket, internal signaling, `requestOffer`, `sendCallMessage`
- `src/utils/webrtc/webrtc.js` — `usersChanged`, `checkStartPublishOwnPeer`, MCU vs P2P logic
- `src/utils/webrtc/simplewebrtc/peer.js` — RTCPeerConnection wrapper, offer/answer, ICE candidates
- `src/utils/webrtc/simplewebrtc/simplewebrtc.js` — message routing to peers

### Key insight from NC Talk code
With MCU, NC Talk creates **separate RTCPeerConnection objects** for:
1. **ownPeer** (publisher): `createPeer({id: ownSessionId, receiveMedia: {offerToReceiveAudio: 0, offerToReceiveVideo: 0}})` — send-only
2. **Subscriber peers**: `createPeer({id: remoteSessionId, receiverOnly: true})` — receive-only

Our `CallPipeline` uses a single `webrtcbin` for both send and receive. This may need to be split into two pipelines for MCU mode.

## What to do next

### Priority 1: Fix audio calls
1. Read the NC Talk `peer.js` and `webrtc.js` MCU flow in detail (`/tmp/spreed/`)
2. Split `CallPipeline` into two webrtcbin elements for MCU:
   - **Publish pipeline**: `wasapi2src → opusenc → rtpopuspay → webrtcbin` (offer to own session, MCU answers)
   - **Subscribe pipeline**: `webrtcbin → rtpopusdepay → opusdec → wasapi2sink` (MCU sends offer, we answer)
3. Verify SDP exchange: log full SDP contents to see if ICE candidates are present
4. Test ICE: check if ICE transitions happen with the split pipeline

### Priority 2: Polish call UX
- Use frontend-design skill for proper CallWindow design
- Show debug log in call window during dev
- Fix "Unknown" name (need to pass peer info earlier)

### Priority 3: v0.6.2 installer for Ilko
- Current installer has all DLL fixes but old call code
- Rebuild installer after call audio works

## Build instructions (office machine)
```bash
export PATH="/c/msys64/mingw64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:/c/Qt/6.8.2/mingw_64/bin:$PATH"
cd C:/build/talq
cmake --build . --target talq
# Run with debug logging:
export QT_FORCE_STDERR_LOGGING=1
C:/build/talq/talq.exe > /tmp/talq-debug.log 2>&1 &
```

## Key files modified this session
- `src/core/DebugMonitor.h/.cpp` — NEW: real-time memory/cache monitor
- `src/core/CallSignaling.h/.cpp` — NEW: internal HTTP signaling (unused when HPB active)
- `src/core/CallManager.h/.cpp` — Major rewrite: MCU flow, ringtone, peer info
- `src/core/CallPipeline.h/.cpp` — GLib pump, ICE monitoring, bus watch
- `src/core/SignalingClient.h/.cpp` — MCU detection, requestOffer, features
- `src/core/ApiClient.h/.cpp` — trackReply, pendingCount, URL fix
- `src/models/MessageListModel.cpp` — 200-message cap, file download fix
- `src/qml/CallWindow.qml` — Full redesign with SVG icons, avatar, pulse
- `src/qml/DebugOverlay.qml` — NEW: Ctrl+D debug panel
