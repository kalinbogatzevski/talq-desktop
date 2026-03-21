# TalQ v0.7.0 Continue Prompt — Call Audio Almost Working

## Session Summary (2026-03-21)

### Major breakthrough: MCU signaling WORKS
- **Publisher ICE: new → checking → connected → completed** — our audio reaches the MCU
- **Subscriber ICE: checking** — MCU offer received, answer created, ICE starts
- **Subscriber ICE failed** — because candidate `sid` didn't match MCU's offer sid. FIXED in latest commit but untested.

### Architecture (confirmed correct)
Split pipeline approach matching NC Talk web client:
1. **PublishPipeline** (send-only): `wasapi2src → opusenc → rtpopuspay → webrtcbin`
   - Offer sent to OWN session ID via HPB
   - MCU answers immediately → ICE connects
2. **SubscribePipeline** (receive-only): `webrtcbin → rtpopusdepay → opusdec → wasapi2sink`
   - `requestOffer` sent to REMOTE session ID
   - MCU sends offer → we create answer → ICE should connect
   - `pad-added` builds audio receive chain

### What was fixed this session
- Split `CallPipeline` into `PublishPipeline` + `SubscribePipeline`
- Hello v1.0 (not v2.0 — v2.0 needs JWT auth)
- Per-pipeline `sid` using `Date.now()` format (matches NC Talk)
- Subscriber must use MCU's `sid` from offer (not generate own)
- `nick` in offer/answer payloads
- `data.to`, `data.sid` in all signaling messages
- GLib main context pump (shared 20ms timer in CallManager)
- Auth error messages (network vs auth vs server)
- Typing indicator shows display name not email
- Version from CMake define, shown on welcome screen
- Lazy message loading (scroll-up pagination)
- Cache-first display (20 msgs from SQLite instantly)
- Request cancellation on chat switch
- Clickable links in messages
- Call system messages filtered/styled

## What to test next
1. **Call Rakesh or Ilko** — the `sid` fix should resolve "Processing of the message failed" errors
2. If subscriber ICE connects → audio should flow!
3. If still failing, log the full candidate message JSON to compare with NC Talk format

## Known issues
- Scroll-to-bottom button: visible logic works but positioning/UX needs polish
- History scroll position jumps when older messages load
- Typing indicator name lookup needs participant list from room join (not just call events)

## Build (home machine)
```bash
export PATH="/c/msys64/mingw64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:/c/Qt/6.8.2/mingw_64/bin:$PATH"
cd C:/build/talq
cmake C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64
cmake --build . --target talq
# Run:
export QT_FORCE_STDERR_LOGGING=1
C:/build/talq/talq.exe > /tmp/talq-debug.log 2>&1 &
```

## Key new files
- `src/core/PublishPipeline.h/.cpp` — send-only GStreamer webrtcbin
- `src/core/SubscribePipeline.h/.cpp` — receive-only GStreamer webrtcbin
- NC Talk source: `/tmp/spreed/` (may need re-clone: `git clone --depth 1 https://github.com/nextcloud/spreed.git /tmp/spreed`)
