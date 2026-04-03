# Screen Sharing

**Date:** 2026-04-03
**Status:** Approved
**Scope:** Share primary monitor to MCU and receive screen shares from remote participants.

## Problem

TalQ has no screen sharing support. Browser users can share their screen but TalQ users can't see it (no `roomType: "screen"` handling) and can't share their own.

## Protocol

The NC Talk browser client uses a **separate WebRTC peer connection** for screen sharing, distinct from the audio/video peer:

- **roomType:** `"screen"` (vs `"video"` for camera/audio)
- **Direction:** send-only, video-only (no audio)
- **Data channels:** disabled for screen share peers
- **MCU mode:** publisher creates a screen share offer with `roomType: "screen"`, MCU distributes to subscribers
- **Stop sharing:** sends room message `{roomType: "screen", type: "unshareScreen"}`

### Signaling flow (sending)

1. Create ScreenSharePipeline (webrtcbin, send-only)
2. webrtcbin emits `on-negotiation-needed` → create offer
3. Send offer via signaling with `roomType: "screen"` and our own sessionId as target
4. MCU answers back → set remote description
5. ICE candidates exchanged as normal
6. On stop: tear down pipeline, send `unshareScreen` room message

### Signaling flow (receiving)

1. MCU sends offer with `roomType: "screen"` from the sharing participant
2. CallManager creates a SubscribePipeline for the screen share (reuse existing class)
3. Answer sent back to MCU
4. Video frames displayed in CallDialog
5. On `unshareScreen` room message: tear down the subscriber

## ScreenSharePipeline (new class)

Send-only pipeline for screen capture. Simpler than PublishPipeline (no audio, no funnel/valve):

```
d3d11screencapturesrc monitor-index=-1 show-cursor=true
  → videorate max-rate=30
  → videoconvert
  → videoscale
  → video/x-raw,width=1920,height=1080  (cap to 1080p)
  → vp8enc deadline=1 threads=4 target-bitrate=2000000
  → rtpvp8pay
  → capsfilter (SSRC)
  → webrtcbin
```

Key details:
- `monitor-index=-1` = primary monitor
- `show-cursor=true` = include mouse cursor
- Cap to 1080p via videoscale + capsfilter (screen res may be higher)
- `videorate max-rate=30` prevents high-refresh monitors from overwhelming the encoder
- `vp8enc deadline=1` = realtime encoding, `target-bitrate=2000000` = 2 Mbps
- SSRC capsfilter approach matches PublishPipeline
- Creates a `"status"` data channel (Janus requires at least one for publisher registration)

### Public API

```cpp
class ScreenSharePipeline : public QObject {
    bool start(stunServer, turnServers);
    void stop();
    void setRemoteAnswer(sdp);
    void addIceCandidate(candidate, sdpMLineIndex, sdpMid);
    bool isRunning() const;

signals:
    void localOfferReady(sdp);
    void iceCandidateReady(candidate, sdpMLineIndex, sdpMid);
    void iceStateChanged(state);
    void error(message);
};
```

## SignalingClient changes

### sendSessionMessage — add roomType parameter

Current: hardcodes `roomType: "video"` (line 411).

Change: add `const QString &roomType = "video"` parameter. All existing callers continue to work unchanged. Screen share calls pass `"screen"`.

### offerReceived — add roomType parameter

Current: `offerReceived(fromSessionId, sdp, sid)` — no roomType.

Change: `offerReceived(fromSessionId, sdp, sid, roomType)`. Parse `roomType` from `msgData["roomType"]` in the message handler. Default to `"video"` if absent.

### answerReceived — add roomType parameter

Same pattern: `answerReceived(fromSessionId, sdp, roomType)`.

### sendOffer / sendAnswer — add roomType parameter

Add `const QString &roomType = "video"` parameter, pass it in the message data.

### New: sendRoomMessage

For sending `unshareScreen`:

```cpp
void sendRoomMessage(const QJsonObject &data);
```

Sends a room-broadcast message (not session-targeted).

## CallManager changes

### New members

```cpp
ScreenSharePipeline *m_screenSharePipeline = nullptr;
bool m_screenSharing = false;
QHash<QString, SubscribePipeline*> m_screenSubscribers;  // remote screen shares
VideoFrameProvider *m_remoteScreenProvider = nullptr;
```

### toggleScreenShare()

- If not sharing: create ScreenSharePipeline, start it, connect signals
- If sharing: stop pipeline, send `unshareScreen` room message, delete pipeline

### Signaling routing

When `offerReceived` fires with `roomType == "screen"`:
- Create a SubscribePipeline for the screen share (same as video subscribers)
- Store in `m_screenSubscribers` (separate from `m_subscribePipelines`)
- Video frames go to `m_remoteScreenProvider`

When `answerReceived` fires with `roomType == "screen"`:
- Route to `m_screenSharePipeline->setRemoteAnswer()`

When `unshareScreen` room message received:
- Tear down the corresponding screen subscriber

### Teardown

On call end: stop screen share pipeline, destroy all screen subscribers.

## CallDialog changes

### New button

Add a screen share button (monitor emoji or "Share" text) between hangup and camera in the active call row. Styled same as camera button — teal when active.

### Remote screen share display

When `m_remoteScreenProvider` is set, display it in the main video area (takes priority over camera video). When remote stops sharing, revert to camera video.

### New signal connections

- `screenShareChanged` → update button style
- `remoteScreenProviderChanged` → connect video provider

## Files

| File | Change |
|---|---|
| `src/core/ScreenSharePipeline.h` | **New** |
| `src/core/ScreenSharePipeline.cpp` | **New** |
| `src/core/SignalingClient.h` | Modify — add roomType to signals and methods |
| `src/core/SignalingClient.cpp` | Modify — parse/pass roomType, add sendRoomMessage |
| `src/core/CallManager.h` | Modify — add screen share members and signals |
| `src/core/CallManager.cpp` | Modify — screen share orchestration, signaling routing |
| `src/ui/CallDialog.h` | Modify — add screen share button |
| `src/ui/CallDialog.cpp` | Modify — button + remote screen display |
| `CMakeLists.txt` | Modify — add ScreenSharePipeline sources |

## What stays the same

- PublishPipeline (camera/audio) untouched
- Existing SubscribePipeline class reused for receiving screen shares
- P2P path not affected (screen sharing is MCU-only)
- Video frame rendering in CallDialog's VideoWidget unchanged

## Testing

- Start call TalQ→browser, click Share in TalQ → browser should show screen share
- Start call TalQ→browser, share screen in browser → TalQ should show screen share
- Stop sharing (either side) → screen share disappears, camera video returns
- Hang up during screen share → clean teardown, no crash
