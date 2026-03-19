# TalQ v0.7.0 — 1:1 Calls with Screen Sharing

## Overview

Add 1:1 audio + video calling with screen sharing to TalQ Desktop, using GStreamer's `webrtcbin` for the WebRTC stack and the existing HPB (standalone signaling) WebSocket for offer/answer/ICE exchange. Calls open in a separate OS window so the chat remains fully usable.

## Scope

**In scope (v0.7.0):**
- 1:1 audio calls
- 1:1 video calls (camera)
- Screen sharing (entire screen or specific window)
- Incoming call popup with ringtone + system notification fallback
- Call controls: mute, camera toggle, screen share toggle, device picker, hang up
- Hardware-accelerated video rendering (D3D11)
- Auto-hangup when remote peer leaves (no manual "Leave Call" for 1:1)

**Out of scope:**
- Group calls (future version)
- Call recording
- SIP dial-out
- Breakout rooms
- Internal signaling fallback (HPB required)

## Component Architecture

### New C++ Classes (src/core/)

**CallManager** — Single instance, owned by `main()` and exposed to QML via `setContextProperty("callManager", &callManager)`, consistent with the existing ApiClient/AuthManager/SignalingClient pattern. Orchestrates the call lifecycle, owns the GstPipeline instance, listens to SignalingClient for WebRTC signaling messages, and exposes call state + video sinks to QML. Rejects new incoming/outgoing calls when `state != Idle` (busy guard).

Key properties exposed to QML:
- `state`: Idle | Outgoing | Incoming | Connecting | Active | Ending
- `remoteVideoSink`: QVideoSink for remote video rendering
- `localVideoSink`: QVideoSink for self-view PiP
- `isMuted`, `isCameraOn`, `isScreenSharing`: toggle states
- `callDuration`: seconds elapsed since Active state
- `remotePeerName`, `remotePeerAvatar`: caller info
- `audioInputDevices`, `audioOutputDevices`, `videoInputDevices`: device lists

Key methods:
- `startCall(token, withVideo)` — initiate outgoing call
- `acceptCall(withVideo)` — accept incoming call
- `declineCall()` — decline incoming call
- `hangUp()` — end active call
- `toggleMute()`, `toggleCamera()`, `toggleScreenShare()`
- `setAudioInput(deviceId)`, `setAudioOutput(deviceId)`, `setVideoInput(deviceId)`

**GstPipeline** — Wraps GStreamer pipeline construction, teardown, and element management. Not exposed to QML directly — only CallManager interacts with it.

Responsibilities:
- Build pipelines for audio-only, audio+video, and screen-share configurations
- Connect `webrtcbin` signals: `on-negotiation-needed`, `on-ice-candidate`, `pad-added`
- Create/set local and remote SDP descriptions
- Add ICE candidates from SignalingClient
- Swap source elements for device changes and screen share toggle
- Feed decoded remote video frames to QVideoSink
- Hardware element selection with software fallback

**GstVideoFrameBridge** — Adapter class that receives GStreamer video buffers from `appsink` and converts them to `QVideoFrame` for Qt's rendering pipeline. Primary path: extracts `GstD3D11Memory` → wraps as `QVideoFrame` backed by the same D3D11 texture (zero-copy). Fallback path: maps buffer to CPU memory → creates CPU-backed `QVideoFrame`. Feeds frames to `QVideoSink` instances owned by CallManager.

**MediaDeviceManager** — Enumerates audio/video devices via GStreamer's `GstDeviceMonitor`. Emits signals on device hot-plug/removal. Provides device lists to CallManager. Lazy-initialized on first call attempt (after `gst_init`).

### Extended: SignalingClient (src/core/SignalingClient.h/cpp)

New public accessor:
- `sessionId()` — returns the HPB session ID (already stored as `m_sessionId` from the `hello` response). CallManager needs this to identify the local peer to the remote side.

New outgoing messages:
- `sendOffer(sessionId, sdp)` — SDP offer via HPB
- `sendAnswer(sessionId, sdp)` — SDP answer
- `sendCandidate(sessionId, candidate)` — ICE candidate trickle
- `sendEndOfCandidates(sessionId)` — ICE gathering complete

New signals (from incoming WebSocket messages):
- `offerReceived(sessionId, sdp)`
- `answerReceived(sessionId, sdp)`
- `candidateReceived(sessionId, candidate)`
- `endOfCandidatesReceived(sessionId)`
- `participantJoinedCall(sessionId, flags)` — remote peer joined call
- `participantLeftCall(sessionId)` — remote peer left call

**Session-targeted message routing:** Call signaling messages (`offer`/`answer`/`candidate`) arrive as `type: "message"` with `data.type` set to the signaling type. These are session-targeted (not room broadcasts). In `onTextMessageReceived`, call-signaling message types must be dispatched to CallManager signals **before** the existing room-filter check (which would otherwise drop them).

**Event message parsing:** The existing `onTextMessage` handler for `type == "event"` is currently an empty block. It must be extended to parse HPB event messages for call state detection:

```json
{
  "type": "event",
  "event": {
    "target": "participants",
    "type": "update",
    "update": {
      "users": [
        {
          "inCall": 7,
          "sessionId": "<remote-session-id>",
          "participantType": 3
        }
      ]
    }
  }
}
```

When `inCall` changes from 0 → non-zero: emit `participantJoinedCall(sessionId, flags)`.
When `inCall` changes from non-zero → 0: emit `participantLeftCall(sessionId)`.

**Obtaining the remote peer's session ID:** When CallManager joins a call via `POST apps/spreed/api/v4/call/{token}`, the response includes the participant list with session IDs. Additionally, the HPB `event` message with `target: "participants"` provides session IDs for all participants. CallManager stores the remote peer's session ID from whichever arrives first.

**Outgoing message format** (HPB protocol):
```json
{
  "type": "message",
  "message": {
    "recipient": { "type": "session", "sessionid": "<remote>" },
    "data": {
      "type": "offer",
      "roomType": "video",
      "payload": { "type": "offer", "sdp": "..." }
    }
  }
}
```

### New QML Files (qml/)

**CallWindow.qml** — Separate OS window for the active call.

**IncomingCallPopup.qml** — Always-on-top ringing popup.

**CallButton.qml** — Phone/video icon for ChatView header.

## Call Lifecycle

### State Machine

```
Idle → Outgoing → Connecting → Active → Ending → Idle
Idle → Incoming → Connecting → Active → Ending → Idle
```

### Outgoing Call Flow

1. User clicks call button in chat header
2. CallManager sets state = Outgoing
3. `POST apps/spreed/api/v4/call/{token}` with flags (1 + 2 + 4 for video, 1 + 2 for audio)
4. GstPipeline creates local capture → encode → webrtcbin pipeline
5. `webrtcbin` emits `on-negotiation-needed` → generates SDP offer
6. SignalingClient sends offer through HPB WebSocket
7. State = Connecting
8. Receive answer SDP → `webrtcbin.setRemoteDescription(answer)`
9. ICE candidates trickle in both directions via SignalingClient
10. ICE connection state = `connected` → state = Active, start duration timer

### Incoming Call Flow

1. SignalingClient receives room event indicating remote participant joined call (flags > 0)
2. CallManager sets state = Incoming, emits `incomingCall` signal
3. IncomingCallPopup window appears + system tray notification fires
4. User clicks Accept → `POST apps/spreed/api/v4/call/{token}`, create pipeline, exchange SDP (same as outgoing from step 3)
5. User clicks Decline → popup closes, state = Idle
6. Timeout after 30 seconds → auto-decline

### Hang Up

1. User clicks hang up → `DELETE apps/spreed/api/v4/call/{token}`
2. GstPipeline.stop() → tears down pipeline, releases devices
3. CallWindow closes → state = Idle

### Remote Hang Up (1:1 specific)

When remote peer leaves (participant flags = 0 detected via SignalingClient room event):
- CallManager immediately tears down pipeline and closes CallWindow
- No "waiting for others" state — call is over
- Brief "Call ended" toast notification in main TalQ window (3 second fade)

### Edge Cases

- **Network loss**: ICE connection state → `failed` → show "Connection lost" overlay for 10s → auto hang-up
- **Call timeout**: Incoming call rings for 30s → auto-decline
- **Caller cancels**: Remote participant leaves before local user answers → dismiss popup
- **Already in call (busy)**: CallManager rejects new incoming/outgoing calls when `state != Idle`. Incoming calls while busy are silently ignored. UI disables the call button while a call is active.
- **Remote busy**: Remote user doesn't answer within 30s → auto-cancel outgoing call, show "No answer" toast

## GStreamer Pipeline

### Codec Selection

- **Audio**: Opus (universally supported, low latency)
- **Video**: Offer both VP8 and H.264 in SDP. Prefer H.264 when hardware encoding is available (`mfh264enc`/`amfh264enc`/`nvh264enc`), fall back to VP8 software encoding (`vp8enc`) otherwise. VP8 is Talk's default and has widest compatibility.

### Pipeline Configurations

**Audio-only:**
```
Local:  wasapi2src → audioconvert → audioresample → opusenc → rtpopuspay → webrtcbin
Remote: webrtcbin → rtpopusdepay → opusdec → audioconvert → wasapi2sink
```

**Audio + Video:**
```
Local audio:  wasapi2src → audioconvert → opusenc → rtpopuspay ──┐
Local video:  mfvideosrc → d3d11convert → vp8enc → rtpvp8pay ──┤→ webrtcbin
                                                                 │
Remote audio: webrtcbin → rtpopusdepay → opusdec → wasapi2sink ──┘
Remote video: webrtcbin → rtpvp8depay → d3d11vp8dec → d3d11convert → QVideoSink
```

**Screen share (replaces local video track):**
```
d3d11screencapturesrc → d3d11convert → vp8enc → rtpvp8pay → webrtcbin
```

### Hardware Acceleration

**Decode (remote video):**
- Primary: `d3d11vp8dec` / `d3d11vp9dec` / `d3d11h264dec` — keeps frames as D3D11 textures
- Fallback: `vp8dec` / `avdec_h264` software decoders

**Encode (local camera/screen):**
- Primary: `mfh264enc` (Media Foundation) or `amfh264enc` (AMD) / `nvh264enc` (NVIDIA)
- Fallback: `vp8enc` software encoder

**Rendering:**
- GStreamer outputs `GstD3D11Memory` (D3D11 textures)
- Zero-copy bridge to Qt's RHI: extract D3D11 texture → shared with Qt's Direct3D 11 backend
- No GPU → CPU → GPU roundtrip
- `VideoOutput` in QML renders via Qt Quick scene graph (GPU-backed)

**Screen capture:**
- `d3d11screencapturesrc` — Windows Desktop Duplication API, outputs D3D11 textures directly
- Supports specific window capture via `window-handle` property

**Pipeline lifecycle:**
- Created on call start, torn down on call end
- Device changes (switch mic/camera) → swap source element without full pipeline rebuild
- Mute → insert `valve` element to drop audio buffers (or set wasapi2src to PAUSED)
- Camera off → stop pushing video frames, brief black frame, pause video branch

### GStreamer Initialization

`gst_init(&argc, &argv)` is called in `main()` before `QQmlApplicationEngine` creation, consistent with Qt's recommendation for third-party multimedia libraries. This ensures GStreamer is available for both MediaDeviceManager and GstPipeline.

### STUN/TURN Configuration

Retrieved from server via `GET /apps/spreed/api/v3/signaling/settings` (already fetched by SignalingClient). Pass ICE servers to `webrtcbin` via the `stun-server` and `turn-server` properties.

## Call Window UI

### CallWindow.qml

- Separate OS window, minimum 480x360, default 640x480, resizable
- Dark background (`#1a1a2e`) regardless of app theme
- Dark title bar: apply `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` via C++ helper invoked from QML `Component.onCompleted` after the native window handle is created (same pattern as main window in `main.cpp`)
- Layout:
  - **Remote video**: fills window via `VideoOutput` bound to `CallManager.remoteVideoSink`
  - **Self-view PiP**: 160x120, bottom-right corner, draggable to any corner, bound to `CallManager.localVideoSink`. Hidden when camera off.
  - **Control bar**: bottom center, semi-transparent background, auto-hides after 3s idle, reappears on mouse move
    - Mute/unmute mic (toggle icon)
    - Camera on/off (toggle icon)
    - Screen share start/stop (toggle icon)
    - Device picker arrows (dropdown next to mic/camera icons)
    - Hang up (red circle, always visible)
  - **Call duration**: top-left, `HH:MM:SS`
  - **Remote user name**: top-center, shown on connect, fades after 3s, reappears on hover
- Camera off (remote): show avatar centered on dark background with name below
- Audio-only calls: avatar view, smaller default window (400x200)

### IncomingCallPopup.qml

- Small always-on-top frameless window, centered on screen. QML flags: `Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint` with `transientParent: null` (matches existing `desktopNotif` pattern in Main.qml)
- Caller avatar (with pulse animation), caller name
- "Incoming video call" / "Incoming audio call" label
- Three buttons: Accept video (green + camera), Accept audio (green + phone), Decline (red)
- Plays ringtone via existing notification sound system
- Auto-dismisses after 30s or on caller cancel

### ChatView.qml Changes

- Call button added to header bar (right side)
- Single button with dropdown: "Audio call" / "Video call"
- Hidden for group conversations (1:1 only in v0.7.0)
- Active call indicator: green dot + "In call — 02:15" in header

## Dependencies

### GStreamer

- **Version**: 1.24+ (latest stable)
- **Installation**: MSYS2 MinGW package or GStreamer official Windows installer
- **Required plugins**:
  - `gstreamer` (core)
  - `gst-plugins-base` (audioconvert, audioresample, videoconvert, rtpopuspay/depay, rtpvp8pay/depay)
  - `gst-plugins-good` (wasapi2src, wasapi2sink, vpx codecs)
  - `gst-plugins-bad` (webrtcbin, dtls, srtp, d3d11 elements, mfvideosrc)
  - `gst-plugins-ugly` (optional, for H.264 if needed)
- **Size impact**: ~50-80MB added to installer
- **CMake**: Find via `pkg-config` or `FindGStreamer.cmake`

### Qt Multimedia

- **Module**: `Qt6::Multimedia` — required for `QVideoSink`, `QVideoFrame`, and `VideoOutput` QML type
- **CMake**: Add `find_package(Qt6 REQUIRED COMPONENTS Multimedia)` and link `Qt6::Multimedia`

### CMakeLists.txt Changes

- Add `Qt6::Multimedia` to `find_package` and `target_link_libraries`
- Add GStreamer include dirs and link libraries
- New source files: CallManager.cpp, GstPipeline.cpp, GstVideoFrameBridge.cpp, MediaDeviceManager.cpp
- New QML files registered

## Testing Plan

- **Unit**: GstPipeline construction/teardown, CallManager state transitions, SignalingClient message formatting
- **Integration**: Full call flow against the 123NET Nextcloud server (manual testing with two TalQ instances)
- **Devices**: Test with multiple microphones/cameras, device hot-plug
- **Screen share**: Test full screen + specific window capture
- **Edge cases**: Remote hang-up auto-teardown, network loss recovery, call timeout
