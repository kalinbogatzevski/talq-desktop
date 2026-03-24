# TalQ v0.9.0 Continue Prompt

## What was done today (2026-03-24, office session)

### Video SDP fix (committed, untested)
- **add-transceiver fix** — Use `g_signal_emit_by_name(webrtcbin, "add-transceiver", SENDONLY, h264_caps)` BEFORE `request_pad_simple`. This should prevent `m=video 0` by ensuring the transceiver is explicitly created as sendonly with proper caps, so webrtcbin generates an active video m-line in the renegotiation offer.
- Applied to both PublishPipeline.cpp and PeerPipeline.cpp
- **NOT YET TESTED** — office machine has 0xC0000139 (Qt6Multimedia ABI mismatch), can't run debug builds. Release builds work only when packaged with windeployqt (installed version bundles its own Qt DLLs).

### Call testing with Ilko (office → home)
- **Audio: working** — Ilko (TalQ 0.8.3 generic) and Kalin (123NET 0.8.3) can hear each other via MCU
- **Camera preview: working** — local PIP shows live camera feed, good speed, zero visible latency
- **Camera preview was frozen before** — fixed by adding `leaky=downstream, max-size-buffers=3` to tee queues (both enc-queue and preview-queue)
- **Video streaming to remote: NOT working** — renegotiation offer sent, MCU answered, but remote peer doesn't receive video. The `m=video 0` SDP issue is the likely blocker.
- **P2P mode failed** — we hardcoded `m_useP2P = true` but with MCU enabled on server, signaling messages get intercepted by Janus. Reverted to `m_useP2P = !m_signaling->hasMcu()`. P2P requires MCU to be disabled on server.

### Installer pipeline refined
- **20 missing DLLs identified and added** — liborc, libcrypto, libssl, libsrtp2, libbrotli*, libgmp, libgnutls, libhogweed, libnettle, libp11-kit, libtasn1, libidn2, libunistring, libopenh264, libvpx (all transitive GStreamer deps)
- **App icon fixed** — regenerated talq.ico with ImageMagick (multi-size 16/32/48/256 from logo.png), old was corrupted
- **Version info fixed** — talq.rc updated to 0.8.3 (was stuck at 0.7.0)
- **HiDPI installer images** — 123NET wizard BMPs regenerated from corporate logo (410x797, 138x138)
- **Code signing** — new self-signed cert "123 NET CPT (PTY) LTD" (old had unknown password), SHA256, 5yr expiry. Both exe and installer signed.
- **Camera source switched** — `mfvideosrc` (Media Foundation, shared mode) instead of `ksvideosrc` (exclusive access). Fallback to ksvideosrc if mfvideosrc unavailable.

### Office machine issues
- **0xC0000139 on debug builds** — Qt6Multimedia from aqt is ABI-incompatible with this MinGW. Release builds fail the same way when run from build dir (uses system Qt DLLs). Works ONLY when packaged with windeployqt (installed version bundles correct DLLs).
- **Multiple repo copies** — `C:/Users/bogat/talk-desktop-qt` (pulled, current), `C:/src/talk-desktop-qt` (stale v0.8.0), `C:/Projects/talk-desktop-qt` (stale). Only the first is current.

## What was done (2026-03-23, evening session at home)

### Video call fixes (major progress)
- NC Talk signaling compatibility — mute/unmute broadcast, updateCallFlags, participantFlagsChanged
- Subscriber re-request after MCU renegotiation
- Auto-camera on video calls, dual call buttons (phone + camera)
- Call status breadcrumbs: Joining → Fetching → Starting → ICE → Connected
- GStreamer plugin check, detailed pipeline errors, SDP validation
- Audio source fallback: wasapi2src → wasapisrc → autoaudiosrc; TALQ_TEST_AUDIO=1 env var
- Keyframe PLI requests (0s, 1s, 2s, 4s) on subscriber video chain link

### Home machine audio driver
- Realtek v6.0.9929.1 (CCleaner auto-update) broke mic capture
- Downgraded to v6.0.9231.1 — wasapi2src/wasapisrc work from CLI
- Built-in mic still not capturing (possible hardware issue, ASUS ZenBook Pro Duo UX582LR)
- Proven with audiotestsrc: full pipeline works end-to-end (phone hears 440Hz tone)

## Priority for this session (home machine)

### 1. Test the add-transceiver video SDP fix
```bash
git pull
export PATH="/c/msys64/mingw64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:/c/Qt/6.8.2/mingw_64/bin:$PATH"
rm -rf /c/build/talq && mkdir -p /c/build/talq
cd /c/build/talq
cmake C:/Users/bogat/Desktop/My\ Projects/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build . --target talq
mkdir -p gst-plugins && cp /c/msys64/mingw64/lib/gstreamer-1.0/libgst{coreelements,audioconvert,audioresample,autodetect,dtls,nice,opus,rtp,rtpmanager,srtp,wasapi,wasapi2,webrtc,app,level,vpx,openh264,videoconvertscale,winks,sctp,jpeg}.dll gst-plugins/
export QT_FORCE_STDERR_LOGGING=1
./talq.exe
```
- Call test-talq from Android phone, enable camera
- Check logs for: `"added sendonly video transceiver"` then `"offer created, SDP length="`
- The SDP MUST have `m=video <non-zero-port>` — if it still shows `m=video 0`, the transceiver fix didn't work and we need approach B (start pipeline with video from the beginning)
- If video port is non-zero, check if phone sees the camera feed

### 2. Test real mic audio
- Office machine test showed audio works (Ilko heard Kalin)
- Home machine mic may be broken (hardware) — use `TALQ_TEST_AUDIO=1` if needed
- Try external USB mic if available

### 3. Fix "Not allowed to request offer" race
- Delay requestOffer until participant flags confirm inCall
- In `onParticipantJoinedCall()`, the request is sent immediately but the remote peer might not have their inCall flag set on the server yet
- Fix: add a short delay (500ms) or check flags before sending

### 4. Build and release v0.9.0 if video works
- Same packaging pipeline as v0.8.3 (build → windeployqt → copy DLLs → sign → Inno Setup → sign → upload)
- Remember to include `libgstwasapi.dll` (v1) in gst-plugins/ alongside wasapi2

## Known bugs

| Bug | Status | Notes |
|-----|--------|-------|
| `m=video 0` in renegotiation SDP | Fix committed, untested | add-transceiver approach |
| Phone doesn't hear audio (home) | Likely hardware | Works on office machine |
| "Not allowed to request offer" | Open | Race condition, needs delay |
| Notification click loses fullscreen | Open | Window state not preserved |
| Notification click doesn't navigate | Open | Token not passed from action |
| Ilko's hang-up button broken | Open | Not investigated |
| Link cursor in chat | Open | Need HoverHandler on links |
| P2P mode with MCU server | By design | P2P only works when MCU disabled |

## Test user
- Username: `test-talq` / Password: `talQing123@`
- 1:1 conversation with kalin: token `u2f3gbu4`
- Log in at `https://ncloud.123net.link` in browser or Android Nextcloud Talk app

## Build (home machine)
```bash
export PATH="/c/msys64/mingw64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:/c/Qt/6.8.2/mingw_64/bin:$PATH"
cd /c/build/talq
cmake C:/Users/bogat/Desktop/My\ Projects/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build . --target talq
# Deploy GStreamer plugins (including wasapi v1!)
mkdir -p gst-plugins && cp /c/msys64/mingw64/lib/gstreamer-1.0/libgst{coreelements,audioconvert,audioresample,autodetect,dtls,nice,opus,rtp,rtpmanager,srtp,wasapi,wasapi2,webrtc,app,level,vpx,openh264,videoconvertscale,winks,sctp,jpeg}.dll gst-plugins/
```

## Build (office machine — Release only, debug crashes 0xC0000139)
```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
cd C:/build/talq
cmake C:/Users/bogat/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -Wno-dev
cmake --build . --target talq
# Must package with windeployqt to run — can't run from build dir (Qt DLL ABI mismatch)
```

## Run
```bash
export QT_FORCE_STDERR_LOGGING=1
# Normal:
C:/build/talq/talq.exe
# Test audio (440Hz tone instead of mic):
TALQ_TEST_AUDIO=1 C:/build/talq/talq.exe
```
