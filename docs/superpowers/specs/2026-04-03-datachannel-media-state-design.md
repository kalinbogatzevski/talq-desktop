# Data Channel Media State

**Date:** 2026-04-03
**Status:** Approved
**Scope:** Send and receive media state over WebRTC data channels, matching the Nextcloud Talk browser protocol.

## Problem

TalQ currently broadcasts media state (mute/unmute, camera on/off) only via the signaling WebSocket path. The browser Talk client also sends this state over the publisher's `"status"` data channel, and reads it from subscriber data channels. Without data channel support, browser users may not see TalQ's mute/video state correctly (and vice versa in some edge cases).

Additionally, the browser sends `speaking`/`stoppedSpeaking` events exclusively over data channels — these are never sent via signaling. TalQ has audio level detection but doesn't broadcast speaking state at all.

## Wire Protocol

All messages are JSON strings sent on a WebRTC data channel labeled `"status"`.

### Messages sent/received

| Event | JSON |
|---|---|
| Audio unmuted | `{"type":"audioOn"}` |
| Audio muted | `{"type":"audioOff"}` |
| Camera on | `{"type":"videoOn"}` |
| Camera off | `{"type":"videoOff"}` |
| Speaking | `{"type":"speaking"}` |
| Stopped speaking | `{"type":"stoppedSpeaking"}` |

### Messages not implemented (out of scope)

| Event | JSON | Reason |
|---|---|---|
| Nick changed (guest) | `{"type":"nickChanged","payload":"name"}` | Not useful without multi-party UI |
| Nick changed (user) | `{"type":"nickChanged","payload":{"name":"n","userid":"id"}}` | Same |

### MCU routing

In MCU mode (Janus), the publisher's data channel messages are distributed by the MCU to all subscribers. Each subscriber connection can also receive data channel messages from the remote publisher. This means:
- **Send:** TalQ sends on the publisher pipeline's `"status"` data channel
- **Receive:** TalQ listens on each subscriber pipeline's incoming data channel

## Design

### PublishPipeline (send side)

**Current state:** Creates a `"status"` data channel at line 274 of `PublishPipeline.cpp` but discards the pointer after a debug log.

**Changes:**
- Store `GstWebRTCDataChannel *m_statusDataChannel` as a member
- Add public method `void sendStatusMessage(const QByteArray &json)`
  - Calls `gst_webrtc_data_channel_send_string()` on `m_statusDataChannel`
  - No-op if channel is null or pipeline not running
- Ref the channel on creation, unref in `cleanup()`

### SubscribePipeline (receive side)

**Current state:** No data channel handling.

**Changes:**
- Connect to webrtcbin's `"on-data-channel"` signal in `start()`
- In the callback, connect to the data channel's `"on-message-string"` signal
- Parse JSON, emit new Qt signal: `void mediaStateReceived(const QString &type)`
  - Emitted for `audioOn`, `audioOff`, `videoOn`, `videoOff`, `speaking`, `stoppedSpeaking`
  - Unknown message types are logged and ignored
- Marshal from GStreamer thread to Qt thread via `QMetaObject::invokeMethod`

### CallManager (orchestration)

**Sending media state:**
- In `broadcastMediaState()`: after existing signaling sends, also call `m_publishPipeline->sendStatusMessage(...)` with the appropriate JSON
  - `audio` + `enabled=true` → `{"type":"audioOn"}`
  - `audio` + `enabled=false` → `{"type":"audioOff"}`
  - `video` + `enabled=true` → `{"type":"videoOn"}`
  - `video` + `enabled=false` → `{"type":"videoOff"}`

**Receiving media state:**
- When creating a SubscribePipeline, connect to `mediaStateReceived`
- Map incoming messages to existing state:
  - `audioOn` → `m_remoteAudioMuted = false; emit remoteMediaChanged()`
  - `audioOff` → `m_remoteAudioMuted = true; emit remoteMediaChanged()`
  - `videoOn` → `m_remoteVideoMuted = false; emit remoteMediaChanged()`
  - `videoOff` → `m_remoteVideoMuted = true; emit remoteMediaChanged()`
  - `speaking` / `stoppedSpeaking` → stored but no UI action for now (future: highlight speaker)

**Speaking detection:**
- New member: `bool m_speaking = false`
- New member: `QTimer m_speakingGrace` (single-shot, 500ms)
- In `onAudioLevelUpdated()`:
  - If level > 0.05 and not muted and not already speaking: set `m_speaking = true`, send `{"type":"speaking"}`
  - If level <= 0.05 and speaking: start grace timer. On timeout, if still below threshold: set `m_speaking = false`, send `{"type":"stoppedSpeaking"}`
  - If level > 0.05 while grace timer running: stop the timer (false alarm)
- On mute: if speaking, immediately send `stoppedSpeaking` and reset

## What stays the same

- Signaling-based `mute`/`unmute` messages continue (browser does both paths too — redundancy is intentional)
- `remoteMediaChanged` signal and all CallDialog UI handling are untouched
- No changes to P2P pipeline path (PeerPipeline) — data channel state is MCU-only for now

## Files modified

| File | Change |
|---|---|
| `src/core/PublishPipeline.h` | Add `m_statusDataChannel` member, `sendStatusMessage()` method |
| `src/core/PublishPipeline.cpp` | Store DC pointer, implement send, cleanup |
| `src/core/SubscribePipeline.h` | Add `mediaStateReceived` signal, DC callback declarations |
| `src/core/SubscribePipeline.cpp` | Handle `on-data-channel`, parse JSON, emit signal |
| `src/core/CallManager.h` | Add `m_speaking`, `m_speakingGrace` members |
| `src/core/CallManager.cpp` | Send DC messages in `broadcastMediaState()`, connect subscriber signal, speaking detection |

## Testing

- Start TalQ call with browser peer
- Mute/unmute in TalQ → browser should show mute indicator via data channel (not just signaling)
- Mute/unmute in browser → TalQ should show mute indicator
- Speak in TalQ → browser should show speaking indicator
- Toggle camera in both directions → video state reflected
