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
  - Extract `GstCaps` from sample → detect width, height, pixel format (I420/NV12)
  - Map to `QVideoFrameFormat::PixelFormat` (e.g., `Format_YUV420P` for I420)
  - Construct `QVideoFrame(QVideoFrameFormat(...))`, then `frame.map(QVideoFrame::WriteOnly)`, `memcpy()` pixel data from `GstBuffer`, `frame.unmap()`
  - `m_videoSink->setVideoFrame(frame)`
- Thread safety: `appsink` callback fires on GStreamer thread. `feedFrame()` marshals to Qt main thread via `QMetaObject::invokeMethod(Qt::QueuedConnection)`

### SubscribePipeline changes

- `onPadAdded()` — remove `if (!isAudio) return;`, add video branch:
  - Detect codec from caps `encoding-name`:
    - `VP8` → `rtpvp8depay` + `vp8dec`
    - `H264` → `rtph264depay` + `openh264dec`
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
ksvideosrc (device-index from MediaDeviceManager)
  → videoconvert
  → capsfilter (video/x-raw,width=1920,height=1080,framerate=30/1)
  → openh264enc (rate-control=bitrate, bitrate=3000000, complexity=medium)
  → rtph264pay
  → webrtcbin sink_%u (second requested pad)
```

When disabled: unlink elements, set to NULL state, remove from bin, release webrtcbin pad (in that order).

**Error handling:** If `ksvideosrc` fails to open the camera (in use, permission denied), emit `cameraError(reason)` signal. `CallManager` falls back to audio-only and shows a user-visible notification in CallWindow.

### Resolution

- **Default:** 1080p (1920x1080 @ 30fps, 3 Mbps)
- **Fallback:** 720p (1280x720 @ 30fps, 1.5 Mbps) — selectable in SettingsDialog
- `ksvideosrc` negotiates down if camera doesn't support requested resolution via caps negotiation
- Resolution stored in `QSettings`, read at camera enable time
- Changing resolution requires camera restart (toggle off/on)

### SDP handling

- Camera on: SDP offer contains two m-lines (audio + video)
- Camera off: audio-only offer (current behavior)
- Mid-call camera toggle: triggers `onNegotiationNeeded` → new offer/answer cycle (standard webrtcbin behavior)
- When adding video elements mid-call: `gst_element_sync_state_with_parent()` after adding to bin

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
- On webrtcbin: set via `add-turn-server` signal: `turn://username:credential@host:port` (or `turns://` for TLS)
- Parse Nextcloud-provided TURN URLs (RFC 7065 format with `?transport=`) into GStreamer-compatible URIs:
  - `turn:host:port?transport=udp` → `turn://user:pass@host:port`
  - `turn:host:port?transport=tcp` → `turn://user:pass@host:port` (libnice handles transport)
  - `turns:host:port?transport=tcp` → `turns://user:pass@host:port`
- **Credential escaping:** Nextcloud time-limited credentials use `timestamp:username` format — the colon must be URL-encoded (`%3A`) in the URI
- Credentials are time-limited (24h TTL from Nextcloud), fresh ones fetched per call (already the case)

---

## 5. Echo Cancellation

### Problem

GStreamer's `webrtcdsp` + `webrtcechoprobe` elements must be in the **same GstPipeline** — the probe is found by `gst_bin_get_by_name()` which only searches within the parent bin. PublishPipeline and SubscribePipeline are separate `GstPipeline` objects, so `webrtcdsp` cannot find the probe across pipelines.

### Status: Deferred to v0.9.0

Neither approach is viable for v0.8.0:

1. **`webrtcdsp` cross-pipeline**: Requires merging PublishPipeline and SubscribePipeline into a shared `GstPipeline`, or routing audio through inter-pipeline elements (`interaudiosrc`/`interaudiosink` from the `inter` plugin). Significant refactor — better suited to v0.9.0 when pipelines may be restructured for group calls.

2. **WASAPI2 built-in AEC**: The `wasapi2src` element in MSYS2's GStreamer 1.26.9 does not expose audio processing properties (no `processing`, no AEC/NS/AGC controls). This feature exists in the official GStreamer Windows installer but not the MinGW build.

**v0.8.0 behavior:** No echo cancellation. Audio works as in v0.7.0. Users with external echo cancellation (headphones, hardware AEC) are unaffected.

**v0.9.0 plan:** Implement shared pipeline approach when restructuring for group calls, or switch to a custom GStreamer build (P3 in CONTINUE.md) that enables WASAPI2 audio processing.

---

## 6. Incoming Call Decline Fix

### Problem

`POST /call/{token}` to leave returns 404 because we never joined the call. NC Talk 1:1 uses a "waiting room" model — callee isn't a participant until they accept.

### Fix

1. **Track join state:** `bool m_joinedCall` in `CallManager` — set `true` after successful join API call, checked before leave API call
2. **Decline action:** Leave the room (`DELETE /participants/active`) instead of trying to leave the call. This triggers a `participantLeftRoom` event that the caller's client can interpret as a decline.
3. Don't call leave-call API if `!m_joinedCall`
4. **Sequencing fix:** Move the leave-call API call to AFTER the `m_joinedCall` check — currently the API fires before the guard check in `declineCall()`

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
- Audio devices: `gst_device_get_properties()` → extract `device.strid` (WASAPI2 device ID)
- Video devices: `gst_device_get_properties()` → extract `device.path` (kernel streaming device path) or use device index for `ksvideosrc`
- New getters: `selectedInputDeviceId()`, `selectedOutputDeviceId()`, `selectedVideoDeviceIndex()` returning device identifiers

### Pipeline changes

**PublishPipeline** — accept device ID:
- `wasapi2src` property `device` set to selected input device ID
- If empty/null, use default (current behavior)

**SubscribePipeline** — accept device ID:
- `wasapi2sink` property `device` set to selected output device ID

**CallManager** — pass device IDs when creating pipelines.

New pipeline signatures:
```cpp
// PublishPipeline
bool start(const QString &stunServer, const QList<TurnServer> &turnServers,
           const QString &audioDeviceId);
void enableCamera(int videoDeviceIndex, int resolutionPreset);
void disableCamera();

// SubscribePipeline
bool start(const QString &stunServer, const QList<TurnServer> &turnServers,
           const QString &audioOutputDeviceId);
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

- `libgstvpx.dll` — VP8 decode (`vp8dec`)
- `libgstopenh264.dll` — H.264 decode/encode (`openh264dec`, `openh264enc`)
- `libgstvideoconvertscale.dll` — format/resolution conversion (`videoconvert`)
- `libgstwinks.dll` — camera capture (`ksvideosrc`)
- `libgstapp.dll` — video frame extraction (`appsink`)

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
