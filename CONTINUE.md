# TalQ v0.14.5+ Continue Prompt

## Current status
Full QWidget app. Released v0.14.5. QPainter rendering, no QML.
**Audio calls working** via MCU (SSRC fix resolved it).
**Video calls NOT working** — outbound video SSRC mismatch. See "Active bug" below.
Call dialog: remote video hide/show on camera toggle works. Camera preview works locally.

## Machine setup

### HOME machine (primary dev)
- **Repo:** `C:\Users\bogat\Desktop\My Projects\talk-desktop-qt`
- **Junction:** `C:\src\talk-desktop-qt` → above path
- **Claude Code working dir:** `C:\src` (start claude from here)
- **MSYS2:** `C:\msys64` (GStreamer packages installed)
- **Qt:** `C:\Qt\6.8.2\mingw_64`
- **Build dirs:** `C:\build\talq` (debug), `C:\build\talq-release`, `C:\build\talq-123net`

### OFFICE machine (first-time setup)
Run once from **admin** command prompt:
```cmd
mkdir C:\src 2>nul
mklink /J C:\src\talk-desktop-qt "C:\Users\bogat\Desktop\My Projects\talk-desktop-qt"
mkdir C:\build 2>nul
```
Then: `cd C:\src && claude` → "read the continue.md"

### Unified paths (both machines)
| Path | Purpose |
|------|---------|
| `C:\src\talk-desktop-qt` | Source (junction on both machines) |
| `C:\build\talq` | Debug build |
| `C:\msys64` | MSYS2 (GStreamer, runtime DLLs) |
| `C:\Qt\6.8.2\mingw_64` | Qt SDK |

## Active bug: Video SSRC mismatch — PRIORITY #1

### What works
- Audio calls bidirectional through MCU ✓
- Receiving browser/phone video in TalQ (subscriber) ✓
- Camera preview locally ✓
- Call dialog remote video hide/show ✓
- Camera toggle without freeze (source swap on permanent encoder) ✓

### What doesn't work
- TalQ outbound video → browser: Janus drops all video RTP with "Unknown SSRC"

### Root cause (confirmed)
GStreamer `webrtcbin` internal `rtpbin` **rewrites the SSRC on the wire** to a different value than the SDP. Browser WebRTC doesn't have this problem (monolithic implementation).

Evidence:
- Payloader caps SSRC = SDP SSRC (they match)
- But Janus receives a DIFFERENT SSRC on the wire (rtpbin's internal session SSRC)
- Audio has NO `a=ssrc` lines in SDP → works (Janus learns from first packet)
- Video HAS `a=ssrc` lines → fails (Janus validates against them)
- Browser-to-browser calls: ZERO "Unknown SSRC" errors in Janus logs
- TalQ calls: constant "Unknown SSRC" errors

### v0.13.1 video DID work — here's why
In v0.13.1, `enableCamera` created a **new** encoder + payloader + webrtcbin pad and triggered **renegotiation**. The `forceReconnectPublisher` in CallManager tore down the entire pipeline and recreated it. The fresh pipeline's SDP had matching SSRCs because webrtcbin read the SSRC from the new payloader's caps.

**The freeze** was from `gst_element_set_state(NULL)` blocking the UI thread during teardown (ICE/DTLS shutdown takes seconds).

### What was tried (2026-03-30)
1. ✗ Source swap (permanent encoder, swap upstream) — no renegotiation → SSRC mismatch
2. ✗ Pad probes for swap — deadlock with wasapi2src
3. ✗ Input-selector — worked with test sources, SSRC still wrong on wire
4. ✗ Pre-set payloader SSRC — webrtcbin ignores it
5. ✗ Strip video `a=ssrc` from SDP — Janus still rejects
6. ✗ Rewrite SDP with rtpbin internal SSRC — can't access (session returns NULL)
7. ✓ Renegotiation after source swap — SSRC errors stopped! But subscriber froze.

### RECOMMENDED FIX (next session)
**Use v0.13.1's `forceReconnectPublisher` approach (confirmed working) but fix the freeze:**

1. Run `gst_element_set_state(NULL)` on a **QThread** (not the UI thread)
2. Emit a `pipelineStopped()` signal when cleanup finishes
3. `CallManager` connects to `pipelineStopped` and creates the new pipeline in the callback
4. No timers, no delays — proper signal/slot callback chain
5. `forceReconnectPublisher` becomes async: stop old → (callback) → create new

This preserves the working v0.13.1 video flow (new pad + renegotiation) while eliminating the UI freeze.

**Key files to modify:**
- `PublishPipeline.h/cpp` — add `pipelineStopped` signal, async cleanup
- `CallManager.cpp` — split `forceReconnectPublisher` into stop + callback

### Server-side issue (separate)
Janus logs `Unsupported codec 'none'` on subscriber path. This affects ALL clients (browser too) but doesn't block video. The HPB signaling server creates Janus rooms without specifying `audiocodec`. Fix: update HPB or patch Janus config.

### Reference code
- NC Talk source: `C:\src\spreed` (cloned)
- NC signaling server: `C:\src\nextcloud-spreed-signaling` (cloned)
- Browser's `replaceTrack`: `spreed/src/utils/webrtc/simplewebrtc/peer.js` line 847
- HPB room creation: `nextcloud-spreed-signaling/sfu/janus/janus.go` line 734

## What was done (sessions 2026-03-29 to 2026-03-30)

### Session 1 (v0.14.0–v0.14.4, 2026-03-29)
- Multi-message selection (drag-to-select, Forward/Copy/Delete)
- Chat layout redesign (unified left-aligned, bubbles, avatars)
- Custom notification popup
- Upload progress bar, file caption via composer
- Per-user install path, junction resolution
- Online status, file size, sidebar preview fixes

### Session 2 (v0.14.5, 2026-03-30 office)
- Installer DLL fix (23 missing GStreamer transitive deps)
- Call fixes: auto-refresh, ICE candidate queuing, async teardown, SSRC sync
- File caption via talkMetaData

### Session 3 (2026-03-30 home)
- Audio calls fixed (per-section SSRC sync + payloader name fix)
- Video source swap refactor (permanent encoder approach)
- Extensive SSRC debugging — root cause identified
- Call dialog: remote video hide on camera stop
- NC Talk + signaling server source studied for reference

## Build commands
```bash
# Debug build + run
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq
cd /c/build/talq && bash /c/src/talk-desktop-qt/scripts/deploy-dev.sh

# Kill running TalQ
cmd.exe //c "taskkill /IM talq.exe /F"

# Launch with test video (SMPTE bars instead of camera)
TALQ_TEST_VIDEO=1 cmd.exe //c "start talq.exe"

# Launch with test audio + video
TALQ_TEST_AUDIO=1 cmd.exe //c "start talq.exe"

# Release installers
bash scripts/build-release.sh              # generic
bash scripts/build-release.sh --brand 123NET  # branded

# Server access
ssh root@ncloud.123net.link
docker logs talk-hpb_janus_1 2>&1 | tail -20   # Janus logs
docker logs talk-hpb_signaling_1 2>&1 | tail -20  # Signaling logs
```

## Architecture notes

### Call flow (MCU mode)
1. startCall → POST /call/{token} → join call on server
2. PublishPipeline: starts with dummy 16x16 VP8, camera replaces via enableCamera
3. Offer sent to own session (HPB creates Janus publisher room)
4. Remote joins → requestOffer → SubscribePipeline receives remote audio/video
5. ICE: STUN + TURN servers from /signaling/settings
6. Media state broadcast via signaling mute/unmute messages
7. Hangup: DELETE /call/{token}?all=true + teardown pipelines

### Key files
| File | Purpose |
|------|---------|
| `src/core/PublishPipeline.cpp` | Send-only webrtcbin pipeline, video source management |
| `src/core/SubscribePipeline.cpp` | Receive-only pipeline for remote streams |
| `src/core/CallManager.cpp` | Call state machine, pipeline lifecycle, signaling wiring |
| `src/core/SignalingClient.cpp` | HPB WebSocket protocol |
| `src/ui/CallDialog.cpp` | Call UI (video display, buttons, preview) |

### Testers
- **Ilko** (Talk token: `ycy3ht4n`) — gets **generic** TalQ installer
- **Rakesh** (Talk token: `bv86wo4c`) — gets **123NET branded** installer
