# TalQ v0.15.1 Continue Prompt

## Current status
Full QWidget app. Released v0.15.1. QPainter rendering, no QML.
**Audio calls WORKING** — bidirectional through MCU/Janus, confirmed with Ilko (v0.15.1 fix).
**Video calls WORKING** — outbound video reaches browser. First camera enable works. Confirmed with Ilko.
**Camera toggle** — first on/off cycle works; second enable sends still image (needs testing with transceiver reuse fix, not yet confirmed).
**59 code review fixes** from v0.15.0 retained, except onPadAdded marshalling reverted to synchronous.

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

### Keeping machines in sync
Both machines must have matching MSYS2/GStreamer packages. Mismatched DLLs cause silent audio failure and crashes.
```bash
# Run on each machine when starting work (from MSYS2 terminal):
pacman -Syu
# Then re-deploy debug DLLs:
cd /c/build/talq && bash /c/src/talk-desktop-qt/scripts/deploy-dev.sh --no-run
```

## What was done

### Session 6 (v0.15.1, 2026-04-01 office)
**Audio regression fix:**
- v0.15.0 review marshalled onPadAdded to Qt thread → race condition → audio silence
- Fix: synchronous pad linking on GStreamer thread (reverted that one review change)
- Root cause took ~2h to isolate: first suspected GStreamer DLL mismatch between machines

**Camera toggle fix:**
- enableCamera() now removes dummy video before adding real camera (was creating second transceiver)
- disableCamera() keeps webrtcbin pad alive for reuse (no more accumulating m=video 0 lines)
- Transceiver reuse across on/off cycles (untested — Ilko on lunch break)

**Subscriber improvements:**
- Re-offer reuses existing pipeline (preserves ICE/DTLS) instead of teardown/rebuild
- SID tracked via hash for seamless re-offer support
- Removed dead forceReconnectPublisher() and unnecessary subscriber re-request

**MSYS2 sync:** office machine updated GStreamer 1.26.9→1.28.1, libvpx 1.15→1.16, opus, srtp, orc, openssl

### Session 5 (v0.15.0, 2026-03-31 home)
**6 review rounds, 59 issues found and fixed:**

Round 1 (11): QPointer guards, thread affinity, null guards, trim logic, cache save
Round 2 (9): Dead QML code, acceptCall lifetime, PushClient stop race, selection by token, cache efficiency
Round 3 (11): HTML injection fix, HTTPS warning, ticket redaction, path sanitization, async cache, file size guard
Round 4 (4): DELETE body for hangup, subscriber re-offer, ICE server race, data channel TODO
Round 5 (13): Poll delay, ICE recovery, state guards, capsfilter leak, generation counter, participant pruning
Round 6 (11): SDP null guard, subscriber cleanup, video provider disconnect, invokeMethod target, overflow guard

**UX polish:**
- Branded splash screen (dark theme, logo, 1.5s)
- Call dialog centered on screen
- Mic icon fixed (text labels)
- Squeezed sidebar: search/settings hidden, avatar only
- Avatar click → settings in all modes
- Mic level indicator (teal bar)

### Session 4 (v0.14.7, 2026-03-31 office)
- Audio/video calls fixed (5 protocol compliance issues)
- SSRC capsfilter approach
- Browser WebRTC-internals dump analyzed

### Session 3 (2026-03-30 home)
- SSRC root cause identified (rtpbin rewrite)
- Video source swap refactor attempts

### Session 2 (v0.14.5, 2026-03-30 office)
- Installer DLL fix, call fixes, file caption

### Session 1 (v0.14.0–v0.14.4, 2026-03-29)
- Multi-message selection, chat layout, notifications, upload progress

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
# Note: Inno Setup at /c/Users/bogat/InnoSetup/ISCC.exe (script uses $USER which may differ)

# Server access
ssh root@ncloud.123net.link
docker logs talk-hpb_janus_1 2>&1 | tail -20
```

## Next steps
- **Test camera toggle** — second enable showed still image; transceiver reuse fix committed but untested
- **Screen sharing** — d3d11screencapturesrc
- **Background blur** — Windows Studio Effects API
- **Data channel media state** — browser sends audioOn/Off via data channel
- In-bubble text selection
- Notification stacking/monitor

## Architecture notes

### Key files
| File | Purpose |
|------|---------|
| `src/core/PublishPipeline.cpp` | Send-only webrtcbin, SSRC capsfilter |
| `src/core/SubscribePipeline.cpp` | Receive-only pipeline |
| `src/core/CallManager.cpp` | Call state machine |
| `src/core/SignalingClient.cpp` | HPB WebSocket protocol |
| `src/ui/CallDialog.cpp` | Call UI, mic level, video display |
| `src/ui/MainWindow.cpp` | Main window, sidebar, selection |
| `src/painter/ChatPainter.cpp` | QPainter message rendering |

### Testers
- **Ilko** (Talk token: `ycy3ht4n`) — gets **generic** TalQ installer
- **Rakesh** (Talk token: `bv86wo4c`) — gets **123NET branded** installer
