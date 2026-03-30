# Video Source Swap Design — PublishPipeline Refactor

## Problem

When toggling camera mid-call, `forceReconnectPublisher` tears down the entire publish pipeline (GStreamer + WebRTC + ICE + DTLS) and recreates it. `gst_element_set_state(NULL)` blocks the UI thread for seconds, freezing the app.

Additional bugs in the current design:
- Dummy and camera use separate webrtcbin sink pads → two video transceivers → malformed SDP
- Camera payloader has no name → SSRC sync silently fails
- `disableCamera` releases the camera pad but doesn't renegotiate
- Dummy pad leaks when camera takes over (reference lost)

## Solution

Single permanent video sink pad on webrtcbin. Swap what feeds into the shared `rtpvp8pay` payloader using GStreamer pad probes (block → unlink → link → unblock).

## Architecture

```
[source chain] → vp8enc ("pub-videoenc") → rtpvp8pay ("pub-videopay") → m_videoSinkPad → webrtcbin
```

The payloader and webrtcbin pad are **permanent** — created once in `start()`, never released until `cleanup()`. Only the elements upstream of `pub-videoenc` get swapped.

### Dummy source chain
```
videotestsrc ("pub-dummyvideo") → videoconvert ("pub-dummyconv")
    → [caps: video/x-raw,width=16,height=16,framerate=1/1]
    → pub-videoenc
```

### Camera source chain
```
mfvideosrc → videoconvert → capsfilter → tee ("camera-tee")
    ├── queue ("enc-queue") → pub-videoenc
    └── queue ("preview-queue") → videoconvert ("preview-convert") → appsink ("preview-sink")
```

### Shared tail (permanent)
```
pub-videoenc (vp8enc) → pub-videopay (rtpvp8pay) → m_videoSinkPad → webrtcbin
```

Wait — the encoder config differs between dummy (10kbps, 16x16) and camera (1.5-3Mbps, 720p/1080p). So the encoder must also be swapped, OR reconfigured dynamically. Since VP8 encoder properties can be changed on a running element, the simplest approach:

**Revised: encoder is also permanent, reconfigured on swap.**

```
[source chain] → pub-videoenc (vp8enc) → pub-videopay (rtpvp8pay) → m_videoSinkPad → webrtcbin
```

On dummy→camera swap: change `target-bitrate` on `pub-videoenc` from 10000 to 1500000/3000000.
On camera→dummy swap: change back to 10000.

The caps change (16x16@1fps → 720p@30fps) is handled by the source chain — the encoder accepts whatever raw video it receives.

## Swap Procedure (enableCamera)

1. Add a BLOCKING probe on `pub-videoenc:sink` pad
2. In the probe callback (runs on GStreamer streaming thread):
   a. Unlink dummy source chain from `pub-videoenc:sink`
   b. Set dummy elements to NULL state
   c. Remove dummy elements from pipeline bin
   d. Add camera elements to pipeline bin
   e. Link camera chain to `pub-videoenc:sink` (via tee → enc-queue if preview, or direct)
   f. Sync camera elements to pipeline state (PLAYING)
   g. Reconfigure encoder: `target-bitrate` = 1.5M/3M
   h. Remove the blocking probe (data flows again)
3. Emit signal to notify UI (camera preview available)

## Swap Procedure (disableCamera)

1. Add a BLOCKING probe on `pub-videoenc:sink` pad
2. In the probe callback:
   a. Unlink camera chain from encoder
   b. Set camera elements to NULL, remove from bin
   c. Add dummy elements back to bin
   d. Link dummy chain to `pub-videoenc:sink`
   e. Sync dummy elements to PLAYING
   f. Reconfigure encoder: `target-bitrate` = 10000
   g. Remove the blocking probe
3. Free camera element pointers, emit signal

## Member Variables

### Permanent (pipeline lifetime)
- `m_pipeline` — GstPipeline
- `m_webrtcbin` — webrtcbin element
- `m_videoSinkPad` — permanent requested pad on webrtcbin
- `m_videoEncoder` — vp8enc ("pub-videoenc"), permanent
- `m_videoPayloader` — rtpvp8pay ("pub-videopay"), permanent

### Dummy source (swappable)
- `m_dummySrc` — videotestsrc
- `m_dummyConv` — videoconvert
- `m_dummyCapsFilter` — capsfilter (16x16@1fps)

### Camera source (swappable)
- `m_cameraSrc` — mfvideosrc / ksvideosrc / videotestsrc
- `m_cameraConv` — videoconvert
- `m_cameraCapsFilter` — capsfilter
- `m_tee` — tee (camera-tee)
- `m_encQueue` — queue (enc-queue, leaky)
- `m_previewQueue` — queue (preview-queue, leaky)
- `m_previewConvert` — videoconvert
- `m_previewAppsink` — appsink

## CallManager Changes

`toggleCamera` no longer calls `forceReconnectPublisher`. Instead:
```cpp
if (m_cameraOn)
    m_publishPipeline->enableCamera(deviceIndex, hd1080);
else
    m_publishPipeline->disableCamera();
```

For both P2P and MCU modes — same API, same behavior.

`forceReconnectPublisher` is kept only for the rare case of network reconnection, not camera toggle.

## SSRC Handling

Since `pub-videopay` is permanent, its SSRC is synced once in `onOfferCreated` and stays correct through all source swaps. No per-swap SSRC fix needed.

The audio payloader `pub-rtpopuspay` is also permanent — its SSRC sync is already correct.

## No Renegotiation

Source swapping does NOT require SDP renegotiation. The webrtcbin transceiver stays sendonly VP8. The resolution/framerate change is handled by RTP — the receiver adapts automatically. This matches the browser's `replaceTrack` behavior.

## Error Handling

- If camera fails to initialize in `enableCamera`, the dummy source stays connected (probe is removed, data continues flowing). Emit `cameraError` signal.
- If any linking fails during swap, fall back to dummy and emit error.
- Camera elements are always cleaned up in `disableCamera` before `cleanup()` in `stop()`.
