# TalQ v0.8.3 → v0.9.0 Continue Prompt

## What was done (2026-03-23)

### v0.8.3 — Camera Preview, P2P Support, Message Fix

#### Local camera preview (new)
- **PIP in CallWindow** — 160x120 VideoOutput, bottom-right corner, z:8, mirrored horizontally
- **GStreamer tee** — camera frames split: encoder branch (queue → openh264enc → rtph264pay → webrtcbin) + preview branch (queue → videoconvert → appsink → VideoFrameProvider)
- **Leaky queues** — `max-size-buffers=3, leaky=downstream` on encoder queue, `max-size-buffers=2` on preview queue, prevents tee pipeline stall
- **Zero-latency** — appsink with `drop=TRUE, max-buffers=1`, QPointer guard + signal disconnect for thread safety
- **localVideoProvider** — new Q_PROPERTY on CallManager, wired to QML Connections block

#### Mid-call camera renegotiation
- **Manual offer creation** — after `enableCamera()`, explicitly calls `create-offer` on webrtcbin (the `on-negotiation-needed` signal doesn't reliably fire mid-pipeline)
- **MCU accepts video** — renegotiation SDP goes from 663 bytes (audio) to 1347 bytes (audio+video), MCU answers successfully
- **Known limitation** — MCU doesn't automatically push video to other subscribers; they need to re-request the stream (v0.9.0 task)

#### PeerPipeline for P2P calls (new)
- **Single webrtcbin** — sends audio+video and receives audio+video in one pipeline (vs MCU's separate publish/subscribe)
- **P2P mode selection** — `m_useP2P = !m_signaling->hasMcu()`: P2P when no MCU, MCU when server has it
- **MCU fallback** — if P2P ICE fails and server has MCU, automatically restarts call in MCU mode
- **Full CallManager integration** — toggleCamera, toggleMute, candidateReceived, onOfferReceived, onAnswerReceived all route to correct pipeline based on mode

#### Message refresh fix
- **Background refresh** — after loading from cache, fetches latest 50 messages from server (lookIntoFuture=0, no lastKnownMessageId)
- **Gap filling** — missing messages inserted into model, edited messages updated via dataChanged
- **No visual disruption** — appends missing messages at end, saves to cache, restarts poller from verified latest ID

#### Installer fixes
- **20 missing DLLs** — liborc, libcrypto, libssl, libsrtp2, libbrotli*, libgmp, libgnutls, libhogweed, libnettle, libp11-kit, libtasn1, libidn2, libunistring, libopenh264, libvpx (transitive GStreamer dependencies)
- **App icon** — regenerated talq.ico with ImageMagick (multi-size 16/32/48/256 from logo.png)
- **Version info** — talq.rc updated to 0.8.3 (was stuck at 0.7.0)
- **HiDPI installer images** — 123NET wizard BMPs regenerated at 410x797 and 138x138 from corporate logo
- **Code signing** — new self-signed cert "123 NET CPT (PTY) LTD", SHA256, expires 2031. Both exe and installer signed.

#### Multiple repo copies discovered
- `C:/Users/bogat/talk-desktop-qt` — home machine, primary (pulled to v0.8.3)
- `C:/src/talk-desktop-qt` — stale copy at v0.8.0 (NOT a junction, separate git clone)
- `C:/Projects/talk-desktop-qt` — stale copy (old build dir pointed here)
- **Only `C:/Users/bogat/talk-desktop-qt` is current.** Others should be deleted or synced.

### Test user
- Username: `test-talq` / Password: `talQing123@`
- 1:1 conversation with kalin: token `u2f3gbu4`

## Known bugs (to fix in v0.9.0)

### Remote peer cannot see outgoing video
- MCU accepts renegotiated SDP with video, but doesn't notify subscribers to re-request the stream
- Fix: after renegotiation answer received, send `requestOffer` for each existing subscriber so MCU re-sends with video
- Browser Talk client listens for `update` signaling event — we may need to do the same
- Ref: `src/utils/webrtc/webrtc.js` in NC Talk source

### Notification click doesn't navigate to conversation
- Clicking notification opens app but doesn't switch to the conversation where the message came from
- Also loses fullscreen/maximized window state (restores to normal window size)
- Fix: pass conversation token from notification action, use it in Main.qml to set conversationToken, preserve window flags

### Ilko's hang-up button not working
- Reported during v0.8.3 testing, not yet investigated
- Could be CallManager state issue or QML button binding

### Link cursor in chat
- Links in message bubbles are clickable but mouse cursor doesn't change to pointing hand
- Fix: add `cursorShape: Qt.PointingHandCursor` or `HoverHandler` on link areas in MessageBubble.qml

## What to do next (v0.9.0)

### Priority 1: Video streaming to remote peer (MCU)
- After publisher renegotiation, re-request subscriber streams so MCU includes video
- Listen for HPB `update` signaling events that indicate stream changes
- Test with browser Talk client as remote peer

### Priority 2: P2P video (when MCU disabled)
- Currently untestable because server has MCU enabled
- To test: comment out `type = janus` in signaling server's `server.conf`, restart signaling
- P2P renegotiation should work (createOffer after enableCamera already implemented)
- Need to verify both sides handle the updated SDP

### Priority 3: Call status progress display
- Show negotiation stages in CallWindow: "Joining...", "Signaling...", "ICE checking...", "Connected"
- Add `callStatusDetail` property to CallManager, update at each stage
- Display below the main status text in CallWindow.qml

### Priority 4: Design System Phase 3 — Apply Theme tokens
- Sweep all QML files replacing hardcoded colors, font sizes, dimensions with Theme references
- Target: MessageBubble.qml, CallWindow.qml, Main.qml

### Priority 5: UX polish
- Notification click → navigate to correct conversation
- Window state preservation
- Link cursor in chat
- Ilko's hang-up button fix

## Architecture reference

### Call Pipelines
```
MCU mode (server has Janus):
  PublishPipeline  — send audio+video to MCU via webrtcbin (offer → MCU answers)
  SubscribePipeline — receive audio+video from MCU per peer (MCU offers → we answer)
  CallManager orchestrates both, routes ICE candidates by session ID

P2P mode (no MCU):
  PeerPipeline — single webrtcbin, sends and receives
  Caller: createOffer → send to peer → peer answers
  Callee: setRemoteOffer → create answer → send back
  Renegotiation: enableCamera → createOffer again

Camera preview (both modes):
  ksvideosrc → capsfilter(JPEG) → jpegdec → videoconvert → tee
    ├─ enc-queue(leaky) → openh264enc → rtph264pay → webrtcbin
    └─ preview-queue(leaky) → videoconvert → appsink(I420,drop) → VideoFrameProvider → QML VideoOutput
```

### Design System
- **Theme.qml** — Singleton, Warm Carbon palette. All colors, fonts, spacing, dimensions.
- **Tq* components** — TqAvatar, TqIconButton, TqBadge, TqSwitch, TqComboBox. In `src/qml/`, registered via qt_add_qml_module.

### Settings
- **SettingsDialog.qml** — TabBar + StackLayout, 4 tabs.
- **AppSettings.h/cpp** — Q_INVOKABLE setAutoStart/isAutoStart for Windows Run registry key.
- **MediaDeviceManager** — saveDevices()/restoreDevices() with QSettings, name+ID matching.

### MCU Call Flow (working — audio)
```
Outgoing:
1. startCall → POST /api/v4/call/{token} (join call)
2. Fetch STUN from /api/v3/signaling/settings
3. PublishPipeline starts → offer to own session via HPB
4. MCU answers → publisher ICE connects
5. participantJoinedCall event → requestOffer for remote peer
6. MCU sends subscriber offer → SubscribePipeline answers → ICE connects → audio flows

Incoming:
1. Push notification → conversation refresh → hasCall detected
2. IncomingCallPopup shown with ringtone
3. Accept → POST participants/active (join room)
4. Wait for HPB roomJoined signal (CRITICAL)
5. POST /api/v4/call/{token} (join call)
6. Same as outgoing steps 2-6
```

### Server config
- HPB: `wss://ncloud.123net.link/standalone-signaling/spreed`
- MCU: enabled (Janus) — confirmed via `server.features: ["mcu", ...]`
- STUN: `stun:turn-za.123net.link:3478`, `stun:turn-bg.123net.link:3478`
- TURN: `turn:turn-za.123net.link`, `turn:turn-bg.123net.link` (with time-limited credentials)

## Build

### Office machine
```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
cd C:/build/talq
cmake C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build . --target talq
```

**NOTE**: Office machine may need Qt6Multimedia reinstall. If build crashes with 0xC0000139, the aqt-installed multimedia module is ABI-incompatible. Workaround: use online Qt installer instead of aqt for the multimedia module.

### Home machine
```bash
export PATH="/c/msys64/mingw64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:/c/Qt/6.8.2/mingw_64/bin:$PATH"
cd C:/build/talq
cmake C:/Users/bogat/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -Wno-dev
cmake --build . --target talq
```

### Run
```bash
export QT_FORCE_STDERR_LOGGING=1
C:/build/talq/talq.exe
```

### GStreamer plugins (copy to gst-plugins/ next to exe)
```bash
mkdir -p gst-plugins && cp /c/msys64/mingw64/lib/gstreamer-1.0/libgst{coreelements,audioconvert,audioresample,autodetect,dtls,nice,opus,rtp,rtpmanager,srtp,wasapi2,webrtc,app,level,vpx,openh264,videoconvertscale,winks,sctp,jpeg}.dll gst-plugins/
```

### Runtime DLLs (all required, copy from msys64/mingw64/bin/)
```
libgstapp-1.0-0 libgstaudio-1.0-0 libgstbase-1.0-0 libgstcheck-1.0-0 libgstnet-1.0-0
libgstpbutils-1.0-0 libgstreamer-1.0-0 libgstrtp-1.0-0 libgstsdp-1.0-0 libgsttag-1.0-0
libgstvideo-1.0-0 libgstwebrtc-1.0-0 libgstsctp-1.0-0 libgstwebrtcnice-1.0-0
libffi-8 libgcc_s_seh-1 libstdc++-6 libwinpthread-1
libgio-2.0-0 libglib-2.0-0 libgmodule-2.0-0 libgobject-2.0-0
libiconv-2 libintl-8 libpcre2-8-0 libnice-10 libopus-0
libzstd zlib1 libzbar-0 libjpeg-8
liborc-0.4-0 libcrypto-3-x64 libssl-3-x64 libsrtp2-1
libbrotlicommon libbrotlidec libbrotlienc
libgmp-10 libgnutls-30 libhogweed-6 libnettle-8
libp11-kit-0 libtasn1-6 libidn2-0 libunistring-5
libopenh264-7 libvpx-1
```

### Packaging
- Inno Setup at `C:\Users\bogat\InnoSetup\ISCC.exe`
- `windeployqt6.exe --no-translations --qmldir src/qml talq.exe`
- Copy QtMultimedia QML module: `cp -r /c/Qt/6.8.2/mingw_64/qml/QtMultimedia dist/qml/`
- Branded build: `cmake -DTALQ_BRAND=123NET`
- Code signing: `Set-AuthenticodeSignature` with cert from `Cert:\CurrentUser\My` matching `*123 NET*`
- GitLab API token "Talk QT" (id: 17) with `api` scope for uploads

## NC Talk source reference
- Clone: `git clone --depth 1 https://github.com/nextcloud/spreed.git /tmp/spreed`
- Signaling: `src/utils/signaling.js`
- WebRTC: `src/utils/webrtc/webrtc.js`
