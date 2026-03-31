# TalQ v0.14.7 Continue Prompt

## Current status
Full QWidget app. Released v0.14.7. QPainter rendering, no QML.
**Audio calls WORKING** — bidirectional through MCU/Janus, confirmed with Ilko.
**Video calls WORKING** — outbound video reaches browser. Confirmed with Ilko.
**Known issue**: starting local camera disrupts incoming video stream (forceReconnect side effect).

## Machine setup

### HOME machine (primary dev)
- **Repo:** `C:\Users\bogat\Desktop\My Projects\talk-desktop-qt`
- **Junction:** `C:\src\talk-desktop-qt` → above path
- **Claude Code working dir:** `C:\src` (start claude from here)
- **MSYS2:** `C:\msys64` (GStreamer packages installed)
- **Qt:** `C:\Qt\6.8.2\mingw_64`
- **Build dirs:** `C:\build\talq` (debug), `C:\build\talq-release`, `C:\build\talq-123net`

### OFFICE machine
- Same layout via junction `C:\src\talk-desktop-qt`
- `cd C:\src && claude` → "read the continue.md"

### Unified paths (both machines)
| Path | Purpose |
|------|---------|
| `C:\src\talk-desktop-qt` | Source (junction on both machines) |
| `C:\build\talq` | Debug build |
| `C:\msys64` | MSYS2 (GStreamer, runtime DLLs) |
| `C:\Qt\6.8.2\mingw_64` | Qt SDK |

## How audio/video was fixed (2026-03-31)

Five protocol compliance issues found by comparing TalQ's SDP with browser WebRTC-internals dump:

1. **Opus codec format**: GStreamer generated `OPUS/48000` — Janus requires `opus/48000/2` (with channel count). Fixed by adding `encoding-params=(string)2` to transceiver codec-preferences caps.

2. **Codec declaration in signaling**: The signaling server reads `audiocodec`/`videocodec` from the offer message `data` object and passes them to Janus room creation. Without them, this Janus build defaults to `audiocodec=none`. Fixed by adding `extra["audiocodec"] = "opus"` to `sendOffer()`.

3. **SSRC consistency**: GStreamer webrtcbin's internal rtpbin rewrites SSRC on the wire. A capsfilter between payloader and webrtcbin forces a known SSRC. The payloader MUST also have the same SSRC set (otherwise capsfilter rejects the payloader's output). Applied to audio, dummy video, and camera video.

4. **Data channel**: Janus videoroom publisher requires an `m=application` section. Added via `g_signal_emit_by_name(webrtcbin, "create-data-channel", "status", ...)`.

5. **Keep `a=ssrc` lines**: Browser keeps them and Janus validates RTP against them. Stripping them didn't help because this Janus build doesn't do dynamic SSRC learning.

### Key insight
The browser WebRTC-internals dump (saved at `C:\src\webrtc_dump`) was the breakthrough — it showed the exact working SDP format to match.

## Known bug: camera start disrupts incoming stream

When TalQ starts its camera (enableCamera), the incoming video from the remote peer stalls. This is because `forceReconnectPublisher` in CallManager tears down the entire publisher pipeline and recreates it, which disrupts the subscriber connection.

**Fix approach**: Use async `forceReconnectPublisher` — stop old pipeline on QThread, create new in callback. See git history commit `9807058` (v0.13.1) for the working renegotiation flow.

## What was done

### Session 4 (v0.14.7, 2026-03-31 office)
- **Audio calls fixed** — 5 protocol compliance issues (see above)
- **Video SSRC fix** — same capsfilter approach for dummy + camera video
- **Deep protocol analysis** — NC Talk source (spreed) + signaling server studied
- **Browser WebRTC-internals dump** captured and analyzed for reference
- Installers built and tested with Ilko

### Session 3 (2026-03-30 home)
- Audio SSRC sync (per-section extraction + payloader name)
- Video source swap refactor (permanent encoder approach)
- SSRC root cause identified (rtpbin rewrite)

### Session 2 (v0.14.5, 2026-03-30 office)
- Installer DLL fix (23 missing GStreamer transitive deps)
- Call fixes: auto-refresh, ICE candidate queuing, async teardown
- File caption via talkMetaData

### Session 1 (v0.14.0–v0.14.4, 2026-03-29)
- Multi-message selection, chat layout redesign, notification popup
- Upload progress, file caption, per-user install, junction resolution

## Build commands
```bash
# Debug build + run
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq
cd /c/build/talq && bash /c/src/talk-desktop-qt/scripts/deploy-dev.sh

# Kill running TalQ
cmd.exe //c "taskkill /IM talq.exe /F"

# Release installers
bash scripts/build-release.sh              # generic
bash scripts/build-release.sh --brand 123NET  # branded

# Server access
ssh -i ~/.ssh/id_ncloud root@ncloud.123net.link
docker logs talk-hpb_janus_1 2>&1 | grep -v "Unknown SSRC" | tail -20
docker logs talk-hpb_signaling_1 2>&1 | tail -20
```

## Architecture notes

### Call flow (MCU mode)
1. startCall → POST /call/{token} → join call on server
2. PublishPipeline: starts with dummy 16x16 VP8 + audio, camera via enableCamera
3. SSRC forced via capsfilter on audio + video payloaders
4. Data channel "status" added for Janus compatibility
5. Offer sent to own session with `audiocodec: "opus"`, `videocodec: "vp8"`
6. Remote joins → requestOffer → SubscribePipeline receives remote audio/video
7. ICE: STUN + TURN servers from /signaling/settings
8. Media state broadcast via signaling mute/unmute messages
9. Hangup: DELETE /call/{token}?all=true + async teardown

### Key files
| File | Purpose |
|------|---------|
| `src/core/PublishPipeline.cpp` | Send-only webrtcbin, SSRC capsfilter, video source |
| `src/core/SubscribePipeline.cpp` | Receive-only pipeline for remote streams |
| `src/core/CallManager.cpp` | Call state machine, pipeline lifecycle |
| `src/core/SignalingClient.cpp` | HPB WebSocket protocol, codec declaration |
| `src/ui/CallDialog.cpp` | Call UI (video display, buttons, preview) |

### Reference
- NC Talk source: `C:\src\spreed`
- Browser WebRTC dump: `C:\src\webrtc_dump`
- Browser SDP reference: line 3414 of the dump file

### Testers
- **Ilko** (Talk token: `ycy3ht4n`) — gets **generic** TalQ installer
- **Rakesh** (Talk token: `bv86wo4c`) — gets **123NET branded** installer
