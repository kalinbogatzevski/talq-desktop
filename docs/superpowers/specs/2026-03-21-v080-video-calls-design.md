# TalQ v0.8.0 — Video Calls + Call Polish Design

## Summary

Add video call support (receive + optional send) and polish the existing audio call flow. This builds on the v0.7.0 MCU-based audio call infrastructure.

**Scope:**
- **P1 — Video Calls:** Receive remote video (VP8/H.264), optional camera send (H.264), 1080p default
- **P2 — Call Polish:** TURN credentials, echo cancellation, decline fix, auto-decline race fix, device selection wired to pipelines

**Out of scope for v0.8.0:** Group calls, screen sharing, self-preview PIP, mid-call device switching, `qml6glsink` (deferred to custom GStreamer build pass).

**Extensibility:** Architecture supports multiple participants (v0.9.0 grid layout + screen sharing) without redesign.

---

## 1. Video Receive (SubscribePipeline)

### New class: `VideoFrameProvider` (`src/core/VideoFrameProvider.h/.cpp`)

- Inherits `QObject`, owns a `QVideoSink*`
- Exposes `Q_PROPERTY(QVideoSink* videoSink)` for QML binding
- Method `feedFrame(GstSample*)`:
  - `gst_buffer_map()` → raw bytes
  - Detect pixel format from caps (I420/NV12) → map to `QVideoFrameFormat::PixelFormat`
  - Construct `QVideoFrame` from data + format
  - `m_videoSink->setVideoFrame(frame)`
- Thread safety: `appsink` callback fires on GStreamer thread. `feedFrame()` marshals to Qt main thread via `QMetaObject::invokeMethod(Qt::QueuedConnection)`

### SubscribePipeline changes

- `onPadAdded()` — remove `if (!isAudio) return;`, add video branch:
  - Detect codec from caps `encoding-name`:
    - `VP8` → `rtpvp8depay` + `vp8dec`
    - `H264` → `rtph264depay` + `avdec_h264`
  - Chain: `depay → decoder → videoconvert → appsink`
  - `appsink` caps: `video/x-raw,format=I420` (consistent format for QVideoFrame)
  - Connect `appsink` `new-sample` signal → `VideoFrameProvider::feedFrame()`
- New member: `VideoFrameProvider* m_videoProvider` — created on first video pad
- Getter exposed so `CallManager` can pass it to QML
- Audio pad handling unchanged

---

## 2. Video Send (PublishPipeline)

### Camera capture chain

New methods: `enableCamera(const QString &deviceId)` / `disableCamera()`

When enabled, creates video branch alongside existing audio chain:
```
mfvideosrc (device-path from MediaDeviceManager)
  → videoconvert
  → capsfilter (video/x-raw,width=1920,height=1080,framerate=30/1)
  → openh264enc (bitrate=3000000, complexity=medium)
  → rtph264pay
  → webrtcbin sink_%u (second requested pad)
```

When disabled: unlink, remove elements, release webrtcbin pad.

### Resolution

- **Default:** 1080p (1920x1080 @ 30fps, 3 Mbps)
- **Fallback:** 720p (1280x720 @ 30fps, 1.5 Mbps) — selectable in SettingsDialog
- `mfvideosrc` negotiates down if camera doesn't support requested resolution
- Resolution stored in `QSettings`, read at camera enable time
- Changing resolution requires camera restart (toggle off/on)

### SDP handling

- Camera on: SDP offer contains two m-lines (audio + video)
- Camera off: audio-only offer (current behavior)
- Mid-call camera toggle: triggers `onNegotiationNeeded` → new offer/answer cycle (standard webrtcbin behavior)

---

## 3. CallWindow Video Layout

### QML changes

- `VideoOutput` fills the window, bound to `callManager.remoteVideoSink`
- State logic:
  - Remote video active → `VideoOutput` visible, avatar hidden
  - Remote video absent (audio-only) → `VideoOutput` hidden, current layout (avatar + waveform)
- Controls (mute, hangup, camera toggle, stats) overlay at bottom with semi-transparent background
- Controls auto-hide after 3s of no mouse movement, reappear on mouse move
- Waveform moves to bottom overlay bar (compact, behind controls)
- Call duration overlay top-center, semi-transparent
- Camera toggle button added to controls bar (next to mute), shows camera-off icon when disabled
- No self-preview for v0.8.0

---

## 4. TURN Server Configuration

### Current state

`SignalingClient::fetchSettings()` already fetches the full signaling settings response including `turnservers` array, but only STUN is extracted.

### Changes

- Parse `turnservers` from settings response: extract `urls`, `username`, `credential`
- Store as struct: `TurnServer { QStringList urls; QString username; QString credential; }`
- Pass to `PublishPipeline` and `SubscribePipeline` alongside STUN
- On webrtcbin: set via `add-turn-server` signal: `turn://username:credential@host:port?transport=udp`
- Credentials are time-limited (24h TTL from Nextcloud), fresh ones fetched per call (already the case)

---

## 5. Echo Cancellation

### GStreamer `webrtcdsp` plugin (from `gst-plugins-bad`)

Two elements:
- `webrtcdsp` — processing (AEC + noise suppression + auto gain), placed on capture chain
- `webrtcechoprobe` — placed on playback chain, provides reference signal

### Pipeline changes

**PublishPipeline** audio chain:
```
wasapi2src → audioconvert → audioresample → webrtcdsp → level → opusenc → rtpopuspay
```

**SubscribePipeline** audio chain:
```
rtpopusdepay → opusdec → audioconvert → audioresample → webrtcechoprobe → wasapi2sink
```

Both elements linked by name via the `probe` property on `webrtcdsp`.

### Graceful fallback

Check that `libgstwebrtcdsp.dll` exists at startup. If absent, skip both elements, log a warning. Audio works without AEC.

---

## 6. Incoming Call Decline Fix

### Problem

`POST /call/{token}` to leave returns 404 because we never joined the call. NC Talk 1:1 uses a "waiting room" model — callee isn't a participant until they accept.

### Fix

1. **Track join state:** `bool m_joinedCall` in `CallManager` — set `true` after successful join API call, checked before leave API call
2. **Decline action:** Leave the room (`DELETE /participants/active`) instead of trying to leave the call. This triggers a `participantLeftRoom` event that the caller's client can interpret as a decline.
3. Don't call leave-call API if `!m_joinedCall`

---

## 7. Auto-Decline Race Condition Fix

### Problem

QML signal race causes instant decline on incoming calls. Current mitigation: 2s guard timer.

### Root cause

Likely: `IncomingCallPopup` initialization triggers an unintended signal (e.g., button `onClicked` binding evaluating during component creation, or `visible` change triggering a handler).

### Fix

1. `IncomingCallPopup` — ensure `accepted`/`declined` signals are only emitted from explicit user `onClicked` handlers, never from property bindings
2. `CallManager` — add `m_userActionReady` flag, set `true` only after `IncomingCallPopup` emits `Component.onCompleted` (confirms UI fully loaded)
3. `declineCall()` early-returns when `!m_userActionReady`
4. Remove the 2s timer hack

---

## 8. Device Selection Wired to Pipelines

### MediaDeviceManager changes

During device enumeration, save GStreamer device path alongside display name:
- `gst_device_get_properties()` → extract `device.strid` (WASAPI2 device ID)
- New getters: `selectedInputDeviceId()`, `selectedOutputDeviceId()` returning device path strings

### Pipeline changes

**PublishPipeline** — accept device ID:
- `wasapi2src` property `device` set to selected input device ID
- If empty/null, use default (current behavior)

**SubscribePipeline** — accept device ID:
- `wasapi2sink` property `device` set to selected output device ID

**CallManager** — pass device IDs when creating pipelines:
```cpp
m_publishPipeline->start(stunServer, turnServers, deviceManager->selectedInputDeviceId());
```

### Limitations (v0.8.0)

- Mid-call device switch not supported — changing device requires pipeline restart (renegotiation)
- SettingsDialog shows note: "Changes apply to next call"
- Save mid-call switching for v0.9.0

---

## Dependencies

### New CMake dependency

- `Qt6::Multimedia` — for `QVideoSink`, `QVideoFrame`, `QVideoFrameFormat`

### GStreamer plugins required (additions)

- `libgstvpx.dll` — VP8 decode/encode
- `libgstlibav.dll` or `libgstopenh264.dll` — H.264 decode/encode
- `libgstvideotestsrc.dll` — (debug only)
- `libgstvideoconvertscale.dll` — format/resolution conversion
- `libgstwebrtcdsp.dll` — echo cancellation (optional, graceful fallback)
- `libgstmediafoundation.dll` — `mfvideosrc` camera capture

### Existing plugins (unchanged)

- `wasapi2`, `opus`, `rtp`, `webrtc`, `dtls`, `nice`, `srtp`, `level`, `audioconvert`, `audioresample`, `app`, `coreelements`

---

## Files to modify

| File | Changes |
|------|---------|
| `CMakeLists.txt` | Add `Qt6::Multimedia`, new source files |
| `src/core/VideoFrameProvider.h/.cpp` | **New** — GStreamer appsink → QVideoFrame bridge |
| `src/core/SubscribePipeline.h/.cpp` | Video pad handling, video decode chain, `VideoFrameProvider` |
| `src/core/PublishPipeline.h/.cpp` | Camera capture chain, resolution presets, device ID |
| `src/core/CallManager.h/.cpp` | Wire video provider to QML, TURN, device IDs, decline fix, race fix |
| `src/core/SignalingClient.h/.cpp` | Parse TURN credentials from settings |
| `src/core/MediaDeviceManager.h/.cpp` | Store device paths, expose getters |
| `src/qml/CallWindow.qml` | VideoOutput, overlay controls, auto-hide, camera toggle |
| `src/qml/IncomingCallPopup.qml` | Fix signal discipline for race condition |
| `src/qml/SettingsDialog.qml` | Camera selection, resolution preset, "applies next call" note |
