# Changelog

## v0.39.4 "Aprilsko Vastanie" — BETA (2026-05-24)

Post-ship code review of 0.39.1-0.39.3 caught several issues that need
their own beta to land. **Anyone running 0.39.1-0.39.3 should upgrade**;
the headline item below means BG-enabled calls in those three betas
sent broken video to the encoder.

### Fixed

* **Critical: BG bridge elements never went to PLAYING.** Phase 3.3b
  bin-added `m_bgAppsink` / `m_bgAppsrc` while the pipeline was already
  running but did not call `gst_element_sync_state_with_parent` on
  either of them, so they stayed in NULL state and the camera feed
  never reached the encoder branch. Anyone with BG enabled in
  0.39.1-0.39.3 was sending no video to peers. (Slipped through because
  the harness env couldn't reach the verdict measurement -- see
  separate harness item below.)
* **Critical: bridge pointers were dangling across stop/start cycles.**
  `cleanup()` nulled the rest of the camera-chain elements but not the
  two new bridge ones. A second start() on the same PublishPipeline
  instance would dereference freed GstObject memory.
* **ORT per-frame failures now log once.** A persistent Run failure
  (GPU device lost, internal panic) used to flood the log with 30
  identical warnings per second. Now: one warning on the first
  failure, silence after; future frame fallback to the centred-gradient
  stub continues quietly.
* **ORT init failure now emits `engineDisabled`.** A missing DLL or a
  model-load fault used to leave the UI showing Blur/Image as
  functional while the engine ran in fallback. The segmenter now emits
  an `unavailable(reason)` signal that BackgroundEngine forwards as
  its existing `engineDisabled` so the UI can toast the user and flip
  the QSetting.
* **Window-activation user-status refresh is now rate-limited to one
  fetch per 5 s.** A busy desktop's repeated focus toggles used to
  pile up redundant in-flight HTTP requests to /apps/user_status/...
* **`talq-call-test` now compiles with `TALQ_BG_ORT`** so the
  `TALQ_TEST_BG_*` harness scenarios exercise the SAME segmenter code
  path the shipped binary uses (real ONNX inference), instead of
  silently running on the centred-gradient stub.

---

## v0.39.3 "Aprilsko Vastanie" — BETA (2026-05-24)

Phase 2e of the video-background feature (#20): the mock centred-radial
gradient mask is replaced with real selfie segmentation from MediaPipe's
`selfie_segmenter` model. Person-shaped now, not ellipse-shaped.

### Changed — video backgrounds use real segmentation (#20 Phase 2e)

* **ONNX Runtime 1.20.1 vendored** under `third_party/onnxruntime/`
  (Microsoft official Windows x64 prebuilt, MIT-licensed redistribution)
  with a mingw-generated import library so it links into the talq.exe
  build without MSVC.
* **Model conversion:** the bundled `selfie_segmenter.tflite` (MediaPipe,
  Apache-2.0) was converted to ONNX in dev via `tf2onnx --opset 18`. One
  TFLite-only fused op (`Convolution2DTransposeBias`) was hand-replaced
  by the equivalent `ConvTranspose + Add` pair so the model loads with
  stock ORT. The patched `selfie_segmenter.onnx` (~450 KB) is bundled
  alongside the original tflite under `:/bg/models/`.
* **Runtime:** `TfliteSegmenter` (class name kept for callsite stability)
  loads the ONNX from qrc, runs CPU-EP inference at 256×256 per frame
  (~10 ms on a modern CPU), upscales the sigmoid mask back to source
  resolution. On any failure (DLL missing, model load fault, Run error)
  it falls back to the centred-gradient mask the 0.39.x betas shipped
  so calls never black-frame.
* **Installer payload** grew by ~11.5 MB (onnxruntime.dll) + 450 KB
  (.onnx). Total .exe ~54 MB → ~65 MB.

---

## v0.39.2 "Aprilsko Vastanie" — BETA (2026-05-24)

Tiny hotfix on top of 0.39.1 for one defense-in-depth nicety:

* **Periodic talq.client re-announce.** SignalingClient now re-broadcasts
  the "TalQ/version" hello every 5 minutes while in a room, on top of the
  existing room-join one-shot. Fixes the field case where a peer who
  joined the room before us upgraded silently keeps a stale-version
  cached value for the lifetime of their session. Receivers de-duplicate
  by info-string equality so an unchanged version triggers no churn.

---

## v0.39.1 "Aprilsko Vastanie" — BETA (2026-05-24)

Second 0.39.x beta. Extends 0.39.0's video-background scaffolding to the
encoded stream and bundles three small fixes from field use.

### Added — video backgrounds reach the encoder path (#20)

* **Bridge upstream of the camera tee.** Phase 3.3a only routed engine
  output to the local PiP; remote peers still saw raw camera. Now an
  appsink+appsrc bridge between the post-decode capsfilter and the tee
  runs every camera frame through the BackgroundEngine BEFORE the
  encoder branch, so receivers see your selected background too.
  Off-mode is zero-copy push-through (gst_buffer_ref + push, no Qt
  thread hop). On-mode preserves PTS+DTS+duration so rtpgccbwe pacing
  and the receiver's jitter buffer stay coherent.
* **Bridge throughput counters** on PublishPipeline
  (`bgBridgeFramesPassThrough` / `bgBridgeFramesProcessed`) plus three
  new talq-call-test scenarios — `TALQ_TEST_BG_BLUR` / `_IMAGE` /
  `_FALLBACK` — assert the right counter advances and B-side delivery
  stays clean.

### Fixed

* **Settings codename credit** still read the 0.38.x "Bangaranga"
  Eurovision text; now reflects the Aprilsko Vastanie 150th-anniversary
  release.
* **Sidebar splitter width** is persisted across restarts (debounced
  splitterMoved + flush on saveWindowState). Previously the chat-list
  pane snapped back to its 280 px seed on every launch.
* **Sidebar user-status dots** refresh on window-activation instead of
  waiting up to a minute for the next 60 s poll, so the green/away/
  busy indicator is current as soon as you return to TalQ.

---

## v0.39.0 "Aprilsko Vastanie" — BETA (2026-05-24)

**TalQ 0.39 carries this codename in honour of the 150th anniversary of
the Aprilsko Vastanie — the Bulgarian April Uprising of 1876, declared
at the Oborishte assembly near Panagyurishte and led, district by
district, against five centuries of Ottoman rule. The uprising failed
militarily but its suppression drew European attention, catalysed the
Russo-Turkish War of 1877-78, and ended with the restoration of the
Bulgarian state at San Stefano. Raina Knyaginya, age 20, sewed and
carried the Panagyurishte uprising's tricolour flag — the enduring
symbol of the revolt.** 2026 = 1876 + 150. The codename was locked in
on 24 May 2026, the Day of Bulgarian Enlightenment, while the project
lead was physically in Panagyurishte for the long weekend.

### Added — video backgrounds (#20, beta preview)

This release ships the **scaffolding, UI, and self-preview rendering
path** for camera background blur + image replace. The actual
person-segmentation model + the encoded-stream integration land in
0.39.1-beta. What you get today:

* **Settings → Audio & Video → Background:** pick Off / Blur / Image,
  set blur strength (1-20, default 10), Choose… an image background
  from disk. Settings persist across sessions; keys mirror upstream
  Nextcloud Talk web (`Talk/Backgrounds/virtualBackground{Enabled,Type,
  BlurStrength,Url}`).
* **Call screen chip** next to the Quality chip — cycles BG OFF → BG
  BLUR → BG IMG → BG OFF on left-click. Right-click jumps to the
  Settings page. The chip's dot turns accent-coloured when the
  feature is on.
* **Self-PiP composite:** the local preview now routes through the
  new GL compositor — you see your own background blurred/replaced in
  the self-preview during a call. The three GLSL fragment shaders
  are direct ports of Talk's WebGLCompositor.js (joint bilateral mask
  refinement is a Phase 2e stub; 9-tap mask-aware separable Gaussian
  blur + smoothstep edge-feather + lightWrapping screen-blend compose
  are byte-faithful to Talk).
* **Test harness:** new BackgroundEngine unit test target with 24
  assertions covering lifecycle, GL init, shader compilation, mock
  mask, and a synthetic-frame three-pass composite producing visible
  blur (red bleeds across a vertical blue/red split boundary).

### Known limitations (intentional, 0.39.x cycle)

* **Mock person mask.** The segmentation step uses a centred radial
  gradient as a placeholder for real per-frame inference. 0.39.1-beta
  replaces it with Talk's `selfie_segmenter.tflite` model running on
  TFLite + GPU delegate. Until then, the "person" zone is a centred
  ellipse — fine for proving the compositor works, not yet
  person-shaped.
* **Preview-only.** Receivers still see the raw camera. The encoded
  stream → BackgroundEngine integration is queued for 0.39.1-beta
  (it needs a GStreamer appsink/appsrc bridge in the publisher chain).
* **Image bundling.** Talk's 8 bundled background JPGs are not yet
  shipped — pick a user-supplied image via Choose… for now.

## v0.38.2 "Bangaranga" — STABLE (2026-05-24)

### Fixed
- **Settings checkboxes now align with their row labels.** The checkbox
  indicator (17 px tall) and the row's name label (~20 px tall) used to
  be top-aligned to separate columns, so the indicator sat visibly
  higher than the text. The row layout now puts the name + control on
  the same line, both vertically centred against each other; the
  description hangs below across the full row.
- **Build script: stable cuts now actually delete the stale beta
  update-channel manifest.** The DELETE was gated on `[ -z "$BETA" ]`
  but the script sets `BETA=0` (not unset), so the check was always
  false and the manifest persisted. Beta-channel clients on a build
  whose version equalled the stale manifest's version were stranded —
  exactly what happened to 0.37.3-beta installs after 0.38.0/0.38.1
  shipped. Gate fixed to `[ "$BETA" != "1" ]`.

## v0.38.1 "Bangaranga" — STABLE (2026-05-23)

### Changed
- **Single installer per release.** Removed the separate `Update.exe`;
  every release ships only `Setup.exe` now. Both were always the same
  size (full payload) since the 0.29.5 slim-update incident, and the
  in-app updater already downloads `Setup.exe`, so `Update.exe` was 36 MB
  of duplicate. `Setup.exe` now declares an Inno AppId so it reliably
  detects existing installs and runs as an upgrade (reuses install dir
  and prior Tasks selections, no re-prompting).

## v0.38.0 "Bangaranga" — STABLE (2026-05-23)

First stable cut to ship simulcast video publishing, end-to-end-verified.
Includes every chat / call / UX improvement from the 0.35.x and 0.37.x
beta lines, validated by a new self-test suite that catches regressions
without a human in the loop.

### Added — simulcast & call quality
- **Simulcast publishing.** The publisher now sends three layers (180p / 360p /
  720p) instead of one, so receivers on weak networks can drop to a lower
  layer without forcing everyone else down. Auto-select adapts the layer to
  each remote tile's size on the call screen.
- **Manual Quality chip on the call screen.** Click the chip next to the
  RX-resolution indicator to cycle Auto → LOW → MED → HIGH → Auto. Right-click
  resets to Auto. Forces every remote tile to the requested layer.
- **Pre-share quality picker.** The screen-share dialog now lets you pick
  720p / 1080p / 1440p / Native before sharing, persisted across sessions.

### Added — call UX
- **Mission Control telemetry panel** on the call stage — live outbound
  bandwidth sparkline, codec / encoder / TX-RX resolution metric cards, and
  per-participant subsystem chips. Open with the telemetry button.
- **Pre-answer self-preview** PiP on incoming video calls.
- **Callee chooses Video / Audio / Decline** on incoming video calls.
- **Live thumbnails** in the share-screen picker (windows + monitors), with a
  program-icon fallback when capture isn't possible.

### Added — chat & app
- **Mission Control header strip** on Settings — version, codename, build
  timestamp, channel chip — matching the call-screen telemetry idiom.
- **Always-on detailed debug logging** to `talq_debug.log`. Settings → General →
  Diagnostics lets you turn it off if you want a smaller log.
- **Image viewer loading indicator** while the full-res preview downloads.

### Fixed
- **User status no longer stuck "In a call"** after a call ends — your previous
  status (e.g. Vacationing) is restored once the server acknowledges leave.
- **General API outbox** for must-complete calls (leaveCall, status revert)
  so transient signaling drops don't strand server-side state.
- **Audio + video routing** across reconnects + status transitions.

### Internal — self-test scaffolding (does not affect runtime)
- `TALQ_TEST_SIMULCAST` / `_SELECT_SUBSTREAM` — end-to-end simulcast +
  substream-switch harness scenarios.
- `TALQ_TEST_SCREENSHARE` — end-to-end screen-share scenario, headless via a
  synthetic capture source (no real desktop session needed).
- `TALQ_TEST_MUTE_TOGGLE` — verifies remote mute/unmute propagation.
- Pure-C++ unit tests for the substream-policy tile-size mapping and the
  auto-update version comparator.

---

## v0.37.3 "Bangaranga" — PRE-RELEASE (2026-05-23)

### Fixed
- **Simulcast video quality switching now works.** Calls were stuck at the
  lowest layer (180p) regardless of which quality the receiver asked for —
  the publisher's offer was missing the SSRC-style `a=ssrc-group:SIM` lines
  that Nextcloud Talk's web client adds to make HPB/Janus build the
  substream map. The publisher now applies the same Talk JS-style munge
  (signaling-only — webrtcbin's internal state untouched) on the outgoing
  offer, replicating the format HPB has accepted from Chrome publishers
  for years. Verified end-to-end: receiver locked to 1280×720 after
  `selectStream{substream:2}`. Pre-release only; stable channel stays on
  the proven single-stream 720p path.

## v0.37.2 "Bangaranga" — PRE-RELEASE (2026-05-23)

### Added
- **Detailed debug logging is now on by default** so call/screen-share
  issues self-diagnose from `talq_debug.log` without relaunching with a
  flag. Settings → General → Diagnostics → "Detailed debug logging" lets
  you turn it off for a smaller log.
- Screen-share now records every ICE state transition in the log
  (helps pinpoint the "share didn't start" failures).

## v0.37.1 "Bangaranga" — PRE-RELEASE (2026-05-23)

### Added
- **See yourself before you answer a video call.** Incoming video calls
  now show your local camera preview on the call window so you can check
  framing before picking up. The camera releases instantly on
  Accept / Decline so the actual call (or the next one) can grab it.

## v0.37.0 "Bangaranga" — PRE-RELEASE (2026-05-22)

Maintenance beta. No new user-facing features over 0.35.0; consolidates
the must-complete API retry logic (status revert + leave-call now share
one bounded-retry primitive) and lands the diagnosis of the simulcast
substream issue (the layers reach the server, but the media server won't
accept the grouping needed to switch them — a deeper fix is queued).
All the 0.35.0 call/notification improvements carry forward.

## v0.35.0 "Bangaranga" — PRE-RELEASE (2026-05-22)

First 0.35.x beta (odd = pre-release channel). Simulcast is back on in
beta builds.

### Fixed
- **You now hear an audio-only peer immediately.** Previously a peer with
  their camera off wasn't subscribed at all, so you heard nothing from
  them until they turned the camera on. We now subscribe as soon as a peer
  is sending any audio or video.
- **Status no longer gets stuck "In a call" on a slow/distant connection.**
  The "revert my status" request now retries (briefly, with backoff) if it
  doesn't reach the server, instead of being lost — important when the
  server is far away.
- **Hanging up reliably ends the call for the other side too**, even on a
  slow/distant link — the leave request now retries if it doesn't reach
  the server the first time.

### Added
- **Answer incoming video calls with Video, Audio, or Decline.** A video
  call now lets you pick whether to join with your camera on or audio-only,
  instead of always answering audio-only.
- **Incoming stream resolution chip** on the call screen, next to the
  codec pill — shows the live decoded resolution (handy for connection /
  quality awareness).
- **Image viewer** is named after the file, comes to the front when
  opened, and shows a "Loading full image…" state while the full-size
  image downloads (so a slow link no longer looks like a stuck thumbnail).

## v0.34.0 "Bangaranga" (2026-05-22)

First stable release since 0.32.0, rolling up the 0.33.x call and
notification work. Video stays a single 720p stream in stable for now
(multi-layer simulcast continues to bake in the pre-release channel).

### Fixed
- **Hanging up now ends the call for the other person too.** A regression
  had left the other party still "in the call" after you hung up.
- **Your other devices no longer ring when you place a call.** Signed in
  on more than one device, starting a call from one no longer makes the
  others ring as if it were incoming.
- **Telemetry codec/readability:** the in-call telemetry CODEC row now
  resolves instead of showing "—", and its text is larger/legible.
- **Screen share** surfaces a clear error (instead of a silent dead share)
  when the capture source fails to start, so you can retry.
- Edit-message events no longer render as stray chat bubbles.
- Stuck "on call" / "in a call" user-status after a call is reliably
  cleared.

### Added
- **Selectable notification sound** (Settings → Notifications → Sounds):
  None / System default / six bundled tones, with preview on pick, mirrored
  in the tray menu.
- **Selectable call ringtone:** Classic / Bright / Soft bell, the TalQ
  tone, or None, played when someone calls you.
- **"You're sharing your screen"** indicator while a screen share is live.

## v0.33.6 "Bangaranga" — PRE-RELEASE (2026-05-22)

### Fixed
- **Calls dropped immediately (0.33.5 regression).** The new echo
  cancellation prevented the call pipeline from starting, so every call —
  audio or video — ended the instant it began. Echo cancellation has been
  reverted; it needs a different approach and will return later. All other
  0.33.5 call fixes (hang-up ends both sides, no self-ring, selectable
  ringtone) remain.

## v0.33.5 "Bangaranga" — PRE-RELEASE (2026-05-22)

### Fixed
- **Hanging up now ends the call for the other person too.** A
  regression left the other party still "in the call" after you hung up,
  because the leave request to the server was skipped.
- **Your other devices no longer ring when you place a call.** If you're
  signed in on more than one device, starting a call from one no longer
  makes your other devices ring as if it were incoming.

### Added
- **Acoustic echo cancellation.** Stops the person you're talking to from
  hearing themselves echo back when you're on open speakers. On by
  default; toggle under Settings if needed. (Headphones never had this
  problem.)
- **Selectable call ringtone.** Settings → Notifications → Sounds now has
  a "Call ringtone" picker (Classic / Bright / Soft bell, the TalQ tone,
  or None) that plays when someone calls you. Picking one previews it.
- Notifications settings are grouped under a clearer "Sounds" section.

## v0.33.4 "Bangaranga" — PRE-RELEASE (2026-05-22)

### Fixed
- **Simulcast video was stuck at 180p for everyone.** The SFU parks a
  new subscriber on the lowest layer until the client asks for a higher
  one, and we never asked. The client now automatically requests the
  substream that matches how large each peer is shown on screen — full
  / pinned speaker → 720p, gallery tile → 360p, small strip thumbnail →
  180p — re-evaluated whenever the layout changes. (In a 1:1 call the
  remote peer is the main view, so you now get 720p.) The server still
  drops to a lower layer on its own if the link can't carry the
  requested one.

## v0.33.3 "Bangaranga" — PRE-RELEASE (2026-05-22)

### Fixed
- **Notification sound choice now persists across restart.** The sound
  setting was read from a different settings store than the one the
  Settings dialog wrote to, so the picked tone could revert on the next
  launch. Both now use the same store.
- Internal: plugged GStreamer element leaks on the simulcast builder's
  error-exit paths; minor doc/clarity cleanups.

## v0.33.2 "Bangaranga" — PRE-RELEASE (2026-05-22)

### Added
- **Selectable notification sounds.** Settings → Notifications now has a
  Sound dropdown (None / System default / Chime / Pop / Ding / Notify /
  Soft / Tone) replacing the old internal/system/none radios. Picking a
  tone auditions it once; the same roster appears in the tray-icon Sound
  submenu. Six original tones ship bundled (synthesized, no licensing);
  swap in your own by dropping `resources/sounds/<id>.wav` and rebuilding.
  The old `Notifications/soundMode` setting auto-migrates (internal →
  Chime).

## v0.33.1 "Bangaranga" — PRE-RELEASE (2026-05-22)

Backlog cleanup on top of the 0.33.0 simulcast beta.

### Fixed
- **Telemetry CODEC row read "—" for the whole call.** It only checked
  subscriber pipelines; now falls back to the publish pipeline's own
  codec (the Janus room runs one codec for everyone, so our send codec
  is the call codec). Telemetry font bumped 11 → 13 pt (was unreadable
  on HiDPI call windows) with matching row pitch + value-column
  alignment.
- **Screen-share could silently fail to start with no feedback.**
  Added a 6-second capture-frame watchdog distinct from the existing
  10-second ICE watchdog: a pad probe on the screen capture source
  flags the first frame; if none flows within 6 s (the WGC-failed-to-
  attach case where ICE connects but the receiver is stuck on
  "Starting remote screen share…" forever), a clear error is surfaced
  so the user can retry or pick a different target instead of staring
  at a dead share.

### Added
- **Local "You're sharing your screen" badge** — a persistent
  top-center pill with a live red dot whenever screen-share is active,
  so the publisher always has a clear local cue. (Stop via the existing
  share control-bar toggle.)

## v0.33.0 "Bangaranga" — PRE-RELEASE (2026-05-21)

First 0.33.x beta carrying the simulcast + dynamic resolution drop
work from `docs/superpowers/specs/2026-05-21-simulcast-design.md`.

### Added
- **3-layer simulcast publisher.** `PublishPipeline` now sends three
  rid-tagged substreams (`l`=180p@150k, `m`=360p@500k, `h`=720p@2.5M)
  in parallel from a new outputTee + per-branch encoders +
  `rtpfunnel` → single webrtcbin sink. The SDP offer carries the
  canonical RFC 8853 simulcast block (`a=simulcast: send l;m;h` plus
  three `a=rid:* send` lines) on one `m=video` line. Janus videoroom
  on the HPB routes the appropriate substream per subscriber based on
  per-subscriber REMB/TWCC; multi-party calls no longer drag the
  whole room to the weakest subscriber's link.
- **Publisher-side BWE-driven layer gate.** `rtpgccbwe`'s aggregate
  estimate drives valve open/close per branch: estimate < 1.8 Mbps
  closes `h`; estimate < 600 kbps closes `m`; `l` always alive. 200
  kbps hysteresis prevents flapping at thresholds.
- **Per-branch encoder error isolation.** A single layer's encoder
  failure now closes only that branch's valve, leaves the other
  layers alive. Only when ALL three branches die does the publish
  path propagate to teardown. Mirrors v0.32.0 #138 policy.
- **`TALQ_TEST_SIMULCAST=1`** harness scenario — asserts the
  publisher SDP carries `a=simulcast` + three `a=rid` lines
  (canonical) or three `m=video` lines with rid in fmtp (the
  gst-webrtcbin 1.28 alternate shape, functionally identical).
- **`TALQ_TEST_SIMULCAST_DROP=1`** harness scenario — env-gated BWE
  override (`TALQ_TEST_BWE_OVERRIDE_KBPS`) drives synthetic
  1500→400→100 kbps steps; verdict is the qInfo
  `simulcast layer '<rid>' -> MUTED (BWE gate)` lines.

### Validated
- **v0.32.0 callee-mid-call camera-on fix preserved.**
  `TALQ_TEST_CAMERA_TOGGLE=1` continues to PASS at avg distinct ≥75%
  of delivered after the simulcast refactor.
- **All three harness scenarios green:** `PUBPIPE`, `CAMERA_TOGGLE`,
  `SIMULCAST` (canonical SDP + 29/29 distinct on peer-B).

## v0.32.0 "Bangaranga" (2026-05-21)

Promotes 0.31.10's structural fixes to stable. All seven scoped
call/media bugs from the 0.31.x betas are now field-verified or
defensively guarded.

### Fixed
- **Callee mid-call camera-on chop (the #111 saga, structural fix —
  field-verified with Ilko).** The 16×16 1 fps black dummy that fed
  the funnel forever while the camera was off is gone. We now mirror
  upstream's BlackVideoEnforcer: the dummy runs for a 5-second grace
  window after every "camera off" transition, then closes the dummy
  valve so no RTP reaches webrtcbin — the wire goes silent, exactly
  like Chrome's `track.enabled=false`. When the camera enables, the
  halt timer is stopped and valves flip as before. Autonomous harness
  reproduces the previously-broken scenario at 79–87 % distinct /
  delivered (was ~30 % in the field) and Ilko confirms the chop is
  gone on live calls.
- **Stuck "Connecting" status pill until peer enables camera.** Call
  state now flips Active on the publisher PC's ICE-connected, not on
  the subscriber's. Matches upstream's `VideoVue.vue` connection-state
  wiring. Audio-only joins correctly show "LIVE" immediately.
- **Screen-share monitor sharing now picks the user's selection.**
  ScreenSharePipeline no longer falls back to `dx9screencapsrc`/
  `gdiscreencapsrc` when `d3d11screencapturesrc` can't construct —
  those legacy paths interpret `monitor` as a DXGI index (wrong order
  vs Qt's `QApplication::screens()`) and can't honor `window-handle`.
  A clear error is emitted instead of silently capturing the wrong
  target.
- **Window-share property-set ordering.** d3d11screencapturesrc's
  `capture-api` is now configured BEFORE `window-handle`, in separate
  `g_object_set` calls; mixing them in one call risked the element
  auto-resolving a monitor target from the (still-default) capture-api
  mode and ignoring the late HWND. A readback log line confirms which
  HWND the capture src actually accepted.
- **Screen-share "stream sometimes doesn't start" silently.**
  ScreenSharePipeline has a 10-second start watchdog: if ICE doesn't
  reach connected within 10 s, emit error() so the UI surfaces a clear
  failure instead of an apparently-active share that's dead on the
  wire. Cleared on ICE-connected; cleaned up on stop().
- **Frozen last frame from a prior screen-share when a new one starts.**
  CallStage now drops the cached `m_scrFrame[participant]` when the
  participant's screen-share session ends (provider becomes null).
  Mirrors upstream's `ScreenShare.vue:228` `srcObject = null` clear.
- **Screen-share failure no longer drops the whole call.** A new
  `m_screenShareTearingDown` flag is set during stopScreenShare()'s
  50-iteration GLib flush; if the publisher's ICE transiently emits
  "failed" as collateral from the screen pipeline teardown perturbing
  the shared signaling agent, the recovery counter is short-circuited
  and the call stays up. Only the main audio publish stream failing
  can hang up the call.
- **Edited messages replaced by "You edited a message" placeholder.**
  Added `Message::isEditMessage()` and filtered `message_edited` /
  `message_edited_everyone` system messages from the chat scroll at
  all three model-load sites. The in-place body update path already
  refreshes the edited message's text; the system event was duplicating
  it visually. Mirrors upstream's filter.
- **User-status "On call" stuck after call ends on the caller side.**
  Hang-up race: `callEnded` fired synchronously, the revert-call API
  call ran BEFORE the server had processed the DELETE /call, so the
  server returned 404 ("no stuck status") and the user stayed pinned
  on "On call" until manual fix. Now CallManager emits a separate
  `callServerLeaveAcked` signal that fires when the leaveCall ACK
  actually arrives; MainWindow's revert hook listens there. UI is
  never blocked — even on a dead network the call window closes
  immediately; if the ACK never arrives, the server's participant
  timeout still cleans up server-side, no retries, no deadlocks.

### Added
- **"Starting (remote) screen share…" caption on the receiver's tile**
  while the share is negotiating (provider bound, no frame yet).
  Previously the receiver saw only an avatar disc with no indication
  that anything was happening.
- **`TALQ_TEST_CAMERA_TOGGLE=1` harness scenario** in `talq-call-test`
  that defers `enableCamera()` 8 s after publish-pipeline start, then
  measures distinct/delivered RX over an averaged 5-s window. The
  exact field bug, autonomously verifiable, no humans / Ilko required.

## v0.31.10 "Bangaranga" — PRE-RELEASE (2026-05-21)

This beta lands the structural camera/screen-share/status fixes informed
by a ground-truth read of the upstream `nextcloud/spreed` v23.0.4 source.
Five user-reported bugs fixed; one harness-verified end-to-end on real
HPB/MCU; the rest ready for live 2-peer field check.

### Fixed
- **Callee mid-call camera-on chop (the #111 saga, structural fix).**
  The 16×16 1 fps black dummy that fed the funnel forever while the
  camera was off is gone. We now mirror upstream's BlackVideoEnforcer:
  the dummy runs for a 5-second grace window after every "camera off"
  transition, then `m_dummyHaltTimer` closes the dummy valve so no RTP
  reaches webrtcbin — the wire goes silent, exactly like Chrome's
  `track.enabled=false`. When camera enables, the halt timer is stopped
  and valves flip as before. Killed the receiver-side dup-pad pattern
  (30 fps cadence, ~10 distinct frames) in the autonomous harness:
  RX 30 fps / 26 distinct under the exact callee-mid-call scenario.
- **Stuck "Connecting" status pill until peer enables camera.**
  Call state now flips Active on the publisher PC's ICE-connected, not
  on the subscriber's. Matches upstream's `VideoVue.vue` connection-
  state wiring. Audio-only joins now correctly show "LIVE" immediately.
- **Screen-share "wrong display every time" silently picking the
  wrong target.** ScreenSharePipeline no longer falls back to
  `dx9screencapsrc`/`gdiscreencapsrc` when `d3d11screencapturesrc`
  can't construct — those legacy paths interpret `monitor` as a DXGI
  index (wrong order vs Qt) and can't honor `window-handle` at all, so
  a window pick became a wrong-monitor capture. Now we emit a clear
  error explaining gstd3d11 is required.
- **Screen-share "stream sometimes doesn't start" silently.**
  ScreenSharePipeline has a 10-second start watchdog: if ICE doesn't
  reach connected within 10 s, emit error() so the UI surfaces a clear
  failure instead of an apparently-active share that's dead on the
  wire. Cleared on ICE-connected; cleaned up on stop().
- **Frozen last frame from a prior screen-share when a new one starts.**
  CallStage now drops the cached `m_scrFrame[participant]` when the
  participant's screen-share session ends (provider becomes null).
  Mirrors upstream's `ScreenShare.vue:228` `srcObject = null` clear.
- **Edited messages replaced by "You edited a message" placeholder.**
  Added `Message::isEditMessage()` and filtered `message_edited` /
  `message_edited_everyone` system messages from the chat scroll at
  all three model-load sites. The in-place body update path already
  refreshes the edited message's text; the system event was duplicating
  it visually. Mirrors upstream's filter for the same reason.
- **User-status "On call" stuck after call ends on the caller side.**
  Hang-up race: `callEnded` fired synchronously, the revert-call API
  call ran BEFORE the server had processed the DELETE /call, so the
  server returned 404 ("no stuck status") and the user stayed pinned
  on "On call" until manual fix. Now CallManager emits a separate
  `callServerLeaveAcked` signal that fires when the leaveCall ACK
  actually arrives; MainWindow's revert hook listens there. UI is
  never blocked — even on a dead network the call window closes
  immediately; if the ACK never arrives, the server's participant
  timeout still cleans up server-side, no retries, no deadlocks.

### Added
- **"Starting (remote) screen share…" caption on the receiver's tile**
  while the share is negotiating (provider bound, no frame yet).
  Previously the receiver saw only an avatar disc with no indication
  that anything was happening.
- **`TALQ_TEST_CAMERA_TOGGLE=1` harness scenario** in `talq-call-test`
  that defers `enableCamera()` 8 s after publish-pipeline start, then
  measures distinct/delivered RX over an averaged 5-s window. The
  exact field bug, autonomously verifiable, no humans / Ilko required.

## v0.31.9 "Bangaranga" — PRE-RELEASE (2026-05-21)

This beta carries the upstream-compliance work + all deferred items
planned for 0.32.0. Stable users remain on 0.31.2.

### Fixed
- **Upstream Talk compliance — caller-side subscribe.** Two deviations
  from the nextcloud/spreed v23.0.4 web client's MCU flow were aligned
  with upstream:
  - Removed the eager immediate `pollParticipants()` REST poll on
    caller-publisher-up. Upstream waits for the signaling layer's
    `usersInCallChanged` event before calling `requestOffer`; the eager
    poll could land at the MCU before the peer's publish was fully
    registered, leaving the resulting subscriber bound to an
    incomplete publish state. The 3-s poll remains as a backup for
    documented mobile / internal-signaling paths.
  - `onParticipantJoinedCall` now only calls `requestPeerStream` when
    the peer's flags include video, exactly matching upstream's
    `userHasStreams` gate. Audio-only joiners get their subscriber
    when video toggles on (existing `participantFlagsChanged` path).
- **Screen-share enable → disable → enable race hardening.** On stop
  we now flush pending GStreamer/GLib callbacks (50 iterations) before
  the next-share path can build a fresh webrtcbin, preventing
  stale-callback ↔ new-resource collisions in fast re-share cycles.

### Changed
- **PIP becomes the "You" tile in the participants strip while
  screen-sharing.** The floating self-PiP would otherwise obscure
  shared content; during any active screen share (your own or a
  peer's) the self camera now lives in the rail alongside the other
  participant tiles.

### Includes (from prior 0.31.x betas, carried)
- 0.31.8: receiver issues PLI on ICE-connected; status "in call"
  clears on every hangup; telemetry TX RES + RX resolution +
  responsive value column.
- 0.31.7: camera GOP 30; force-keyframe on `enableCamera()`.
- 0.31.6: pre-release indicator in title bar + Update-available banner.

## v0.31.8 "Bangaranga" — PRE-RELEASE (superseded by 0.31.9)

### Fixed
- **Subscriber requests a keyframe the moment its ICE connects.** Targets
  the deterministic "caller-side sees the callee's camera choppy"
  pattern: any RTP loss during the receive pipeline's bootstrap window
  (jitter buffer / DTLS / ICE checks still settling) leaves the decoder
  in concealment until the next periodic publisher keyframe. The
  receiver now emits an RTCP PLI on ICE-connected so the publisher
  sends a fresh I-frame within ~1 RTT.
- **Status "in call" no longer sticks after hangup.** Every call-end
  path now calls `revertStuckCall()` (idempotent), so Talk's server-side
  automation can't leave the user reading as "in call" indefinitely.

### Changed
- **Call telemetry overlay:** the per-peer RX line now shows the
  decoded **resolution** alongside fps/distinct/ptsΔ; a new **TX RES**
  row shows the encoder's input resolution; the value column scales
  with panel width so long values stop clipping on a narrow window.

## v0.31.7 "Bangaranga" — PRE-RELEASE (superseded by 0.31.8)

### Fixed
- **Choppy peer video when the peer enables their camera mid-call.**
  The encoder was emitting P-frames against the just-replaced black
  dummy baseline, so the receiver's decoder produced blocky/smeary
  output until the next periodic I-frame (up to ~2 s away at the old
  GOP=60). Two targeted changes: the camera GOP is shortened to 30
  (~1 s) and `enableCamera()` now issues an immediate
  `GstForceKeyUnit` upstream event so the very next encoded frame is
  an I-frame — the receiver gets a clean baseline of real camera
  content with no transitional artifact window.

### Includes
- 0.31.5: caller-side `requestPeerStream` in-flight dedupe (defensive;
  field-tested as no-op for the chop symptom on its own).
- 0.31.6: "Update available" banner says PRE-RELEASE when the offered
  update is from the beta channel.

## v0.31.6 "Bangaranga" — PRE-RELEASE (superseded by 0.31.7)

### Added
- **"Update available" banner now says PRE-RELEASE** when the offered
  update comes from the beta channel — testers see at a glance what
  they're about to install.

## v0.31.5 "Bangaranga" — PRE-RELEASE (superseded by 0.31.6)

Still a pre-release for opt-in beta testers (Settings → Updates →
"Pre-release updates"). Stable users remain on 0.31.2.

### Fixed
- **Caller-side chop on the peer's video** (deterministic in 0.31.4
  and earlier — the *caller* always saw the *callee* choppy, the
  callee always saw the caller fine). Root cause: the caller's
  CallManager could request the peer's subscribe offer twice — once
  when the publisher came up with the remote already present, and
  again when the participant-joined event fired — and the second
  offer triggered a stale-subscriber rebuild that cost a frame-loss
  spike. The callee never double-requested. `requestPeerStream` now
  skips in-flight duplicates; retries still happen via the existing
  retry timer.

## v0.31.4 "Bangaranga" — PRE-RELEASE (superseded by 0.31.5)

This is a pre-release intended for opt-in beta testers (Settings →
Updates → "Pre-release updates"). Stable users remain on 0.31.2.

### Added
- **Pre-release builds say so in the title bar.** When a binary is
  compiled with `--beta`, the title bar reads `TalQ <version> —
  PRE-RELEASE`, so testers always know which channel they're on.

### Fixed
- **Checkbox / radio button indicator visibility.** Unchecked
  indicators were filled with the input-well tone, which is too close
  to the page background on some tabs (Settings → Updates) — the boxes
  almost vanished. Indicators now sit one tonal step up the ladder
  (`bgSurface`), giving a clear raised affordance in every theme.

### Includes (from the v0.31.3 pre-release that this supersedes)
- 600 kbps publisher GCC floor (down from 1.2 Mbps) so a moderate
  uplink isn't clamped above what it can carry → no more "very choppy"
  decoder-artifact video at distinct ≈ delivered fps.
- Screen share now shares the actual chosen window / monitor
  (`d3d11screencapturesrc` in WGC mode for window-handle, HMONITOR for
  monitor — DXGI's index was unrelated to Qt's screen order).
- Right-click the share segment during a screen share → quality menu
  (720p / 1080p / 1440p / Native) for a live re-share at the new cap.

## v0.31.3 "Bangaranga" (pre-release, superseded by 0.31.4)

### Fixed
- **Choppy / blocky remote camera on moderate uplinks.** The camera
  send-bitrate floor was 1.2 Mbps, which on a marginal uplink clamped
  the encoder above what the link could actually carry, leading to
  packet loss and decoder artifacts that read as "choppy" even when
  distinct-frame counts were near 30 fps. The floor is now 600 kbps
  (the same range libwebrtc / Zoom use for 720p30) and congestion
  control still ramps up freely to the server ceiling on good links.
- **Screen-share now shares the window / monitor you actually picked.**
  Window capture needs `d3d11screencapturesrc` in WGC mode (it silently
  ignored the chosen window in the default DXGI mode); monitor capture
  uses HMONITOR derived from the chosen screen's geometry, instead of a
  DXGI index that isn't guaranteed to match what the picker showed.

### Added
- **Change screen-share quality during a share.** Right-click the share
  segment on the call control bar to switch between 720p / 1080p /
  1440p / Native; the share continues at the new quality.

## v0.31.2 "Bangaranga" (2026-05-20)

### Fixed
- **Camera Quality dropdown no longer loses entries between calls.**
  Windows enumerates webcam capabilities differently across runs (and
  especially after camera use); rows like `720p · 30fps · MJPEG` could
  appear in one launch and vanish the next, including the row your
  Settings pick referenced. The list is now cached per device and
  unioned with each enumeration, so any mode the camera has ever
  reported stays available.
- **A camera that can't deliver your chosen mode now self-recovers
  instead of staying dark.** If the camera doesn't produce a frame
  within a few seconds of starting (the chosen mode is no longer
  negotiable on the actual open instance), the choice is reset to Auto
  and the camera is re-started — the indicator light comes on and the
  preview/call works.
- **Screen-share stop now fully clears its session identity** so a
  subsequent re-share starts cleanly rather than racing stragglers from
  the previous share.

### Changed
- **In-call telemetry panel is semi-transparent**, so the camera / call
  view stays visible behind it instead of being hidden by a hard side
  panel.

## v0.31.1 "Bangaranga" (2026-05-19)

### Fixed
- **Camera works again on every webcam (critical).** v0.31.0's new
  "Auto" camera quality forced one exact capture mode onto the camera;
  if the actual camera couldn't deliver that exact mode the camera
  failed to start entirely — no indicator light, no self-preview, no
  video. **Auto** now lets the camera negotiate normally (the long
  stable behavior, always starts); a specific resolution/frame-rate is
  only forced when you deliberately pick one in Settings (and switching
  back to Auto always restores a working camera).
- **Camera no longer listed twice; Camera Quality lists real modes.**
  On Windows the same physical camera is enumerated once per capture
  backend (Media Foundation and KS/DirectShow). Duplicates are now
  collapsed by name and their capability sets merged, so the camera
  appears once and every supported resolution/frame-rate/format is
  offered regardless of which backend exposed it. (Diagnostics also
  record a camera's raw capabilities if it still reports none.)

## v0.31.0 "Bangaranga" (2026-05-19)

### Added
- **Settings, rebuilt.** A calmer, clearer Settings window: a quiet
  header, every option as a two-column row (name + one-line description
  on the left, the control aligned in one column on the right), grouped
  by rhythm instead of boxes. Fully theme-driven across all four themes.
- **Pre-release (beta) updates, opt-in.** Settings → Updates has a new
  toggle. On the generic build it tracks GitHub pre-releases; on the
  branded build it follows a separate beta manifest and falls back to
  stable automatically if no beta is available.
- **Screen-share quality is selectable and changeable mid-share.**
  720p / 1080p / 1440p / Native, chosen in the share picker; changing it
  during a share re-shares the same screen/window at the new quality
  without re-picking.
- **The share picker now shows live thumbnails** of every screen and
  window (refreshed while open), so you can see what you're about to
  share. Single-window / app capture is selectable alongside whole
  screens.

### Fixed
- **Choppy peer camera, transmit-side root cause.** When the local
  camera/self-view is smooth but the far end sees stuttery ~10 fps, the
  cause was the send-side congestion-control floor (300 kbps) being far
  too low for 720p30: the encoder starved, frames were dropped, and the
  constant-rate stage padded them back with duplicates, so the receiver
  got "30 fps" of mostly repeated pictures. The floor is now a
  720p30-viable 1.2 Mbps, matching what browsers/Zoom use for a 720p
  camera. Per-peer RX telemetry (delivered vs distinct fps) makes the
  result measurable.

Includes the v0.30.12 camera capability picker below.

## v0.30.12 "Bangaranga" (2026-05-19)

### Added
- **Camera quality now lists what your camera can actually do.** The old
  fixed "Full HD / HD" radios are replaced by a per-camera list built
  from the device's real advertised capabilities (resolution × frame
  rate × format, e.g. "1080p · 60fps · MJPEG"), plus an **Auto** entry
  that picks the absolute best mode the camera supports (most pixels,
  then highest fps, MJPEG preferred). The chosen mode is now *forced* as
  a single exact capture format instead of leaving the camera source to
  silently fall back to a low-rate raw mode, and it applies even on a
  fresh install where Settings was never opened. Universal across
  cameras — it reads each device's own caps rather than assuming.

### Note
- This is the camera *capture-mode* control/determinism fix. It does not
  by itself change the separate transmit-side investigation into choppy
  remote video on links where the local camera/self-preview is already
  smooth (that work continues, instrumented, pending a live test).

## v0.30.11 "Bangaranga" (2026-05-19)

### Fixed
- **Incoming call shows the caller's name, not a generic "Call".** Some
  servers omit the display name in participant events; the name is now
  resolved from the cached participant list (or the user id) instead of
  falling back to a literal "Call".

Includes the v0.30.10 camera (MJPEG capture) and taskbar-window fixes
below.

## v0.30.10 "Bangaranga" (2026-05-19)

### Fixed
- **Peer camera no longer stuck at a low frame rate (real root cause).**
  Proven on hardware: a USB webcam at 1280×720 *raw* advertises 30 fps
  but only delivers ~10 (raw 720p exceeds USB bandwidth); the constant-
  rate stage then padded it to 30 with duplicate frames, so the far end
  saw "30 fps" but only ~10 distinct pictures — choppy. The camera now
  accepts the webcam's **MJPEG** mode (≈10:1 compressed, fits USB →
  true 720p30) via a decode stage, falling back to raw for cameras
  without MJPEG. This is the same approach Zoom/Telegram use, which is
  why they were always smooth on the affected camera.
- **The in-call window now has its own Windows taskbar button.** It was
  an owned window (shared the main window's button), so when another
  app covered it you couldn't bring it back from the taskbar. It is now
  an independent top-level window.

## v0.30.9 "Bangaranga" (2026-05-19)

### Changed
- **Camera keeps the hardware encoder.** The v0.30.8 B-frame gate, which
  fell back to software x264enc when mfh264enc couldn't prove B-frames
  off, has been reverted: its premise (B-frames cause the choppy peer
  camera) was disproven by a controlled real-MCU reproduction, and the
  software fallback could itself starve a modest CPU at 720p30 and
  produce a low-frame-rate stream. The hardware H.264 encoder is now
  always preferred; B-frames=0 stays best-effort but is no longer a
  reason to abandon hardware encoding.

### Removed
- **Receiver no longer auto-re-requests keyframes on concealment.** That
  masked packet loss rather than delivering a real continuous stream;
  removed in favour of addressing the sender encode directly.

### Kept
- The per-peer **distinct-content fps** telemetry (delivered vs actually-
  new frames) — the validated instrument that localises the fault.

## v0.30.8 "Bangaranga" (2026-05-19)

### Changed
- **Peer camera ~1 fps: hardening + instrumentation (not yet field
  confirmed).** The leading hypothesis is B-frames in the outgoing
  camera H.264 stream: where the Media Foundation encoder's B-frame
  property is unavailable it was silently left on, producing a
  high-profile stream a remote hardware decoder may conceal frame by
  frame. The camera encoder now proves B-frames are off and, if it
  cannot, falls back to a software encoder that guarantees a
  constrained-baseline, B-frame-free stream — this matches what every
  other WebRTC client does and has no downside. Honest status: a
  controlled end-to-end reproduction with B-frames over the real MCU did
  NOT reproduce the ~1 fps symptom, so this is defensible hardening, not
  a confirmed fix; the defect needs the specific remote hardware to
  confirm. (The v0.30.7 note below addressed a different failure mode
  that did not trigger on the affected hardware.)
- **Camera off now stops the outbound video stream.** With the camera
  disabled the encoder is collapsed to a trickle instead of continuing
  to push ~300 kbps of padded black, matching the official client.
- **Call survives a transient publisher-ICE failure.** A momentary
  network blip no longer tears down the whole call: publisher ICE
  "failed" is grace-debounced and recovered, mirroring the existing
  subscriber recovery, so an 11-minute call no longer dies on a hiccup.

### Added
- **Live per-peer receive diagnostics in Telemetry.** Each remote peer's
  line now shows delivered fps, **distinct-content fps**, and mean
  inter-frame timing. delivered≈30 with distinct≈1 pinpoints decoder
  concealment; delivered≈distinct rules it out — turning the next real
  call into a definitive root-cause measurement rather than a guess.

## v0.30.7 "Bangaranga" (2026-05-19)

### Fixed
- **Peer camera no longer stuck at ~1 fps.** On hardware where the Media
  Foundation H.264 encoder could not be held to a target bitrate, it
  silently kept the image pristine and collapsed the frame rate to about
  1 fps. The camera now detects that case and falls back to a software
  H.264 encoder that honours the rate, restoring smooth full-frame-rate
  video. Screen sharing is unaffected.

## v0.30.6 "Bangaranga" (2026-05-19)

### Fixed
- **Peer camera no longer choppy.** Outgoing camera video is now held to a
  constant frame rate before encoding, so a webcam that delivers an uneven
  or low rate no longer starves the encoder into a stuttering, low-bitrate
  stream.
- **Screen sharing no longer balloons memory.** Capture is downscaled to
  1080p before encoding instead of pushing full native resolution; a 4K
  desktop previously allocated hundreds of MB of raw-frame buffers the
  instant sharing started and forced real-time 4K encoding.
- **Stop then re-share now works.** Starting a second screen share after
  stopping the first showed a frozen last frame of the previous share; it
  now rebuilds cleanly.
- **Double-click on the call controls no longer toggles fullscreen.**
  Rapidly clicking a control such as the camera switch could bounce the
  window in and out of fullscreen.

## v0.30.5 "Bangaranga" (2026-05-19)

### Fixed
- **Calls no longer hang up on their own.** When the server migrated a
  peer's video feed mid-call (e.g. the other side toggled their camera, a
  routine event on the conferencing backend), the client treated the
  resulting subscriber drop as fatal and tore down the whole call —
  typically several minutes in. It now recovers just that peer's stream
  in place and keeps the call and audio running; only a failure of your
  own outbound connection or hanging up ends a call.

## v0.30.4 "Bangaranga" (2026-05-19)

### Fixed
- **Message input was unreadable** — the chat box rendered black text on
  a black field on every theme. The input was baking its colours from the
  widget palette at construction time, before the theme was applied, so
  it never matched. It now inherits the themed input styling like every
  other field, correct on all four themes and updated live on theme
  switch.
- **Status menu was cramped.** Moving the status popover onto the shared
  app styling stripped its custom spacing, squishing the rows. It now
  carries its own complete theme-driven styling again, restoring the
  original comfortable layout on all four themes.

## v0.30.2 "Bangaranga" (2026-05-18)

### Changed
- **One design language across the whole app.** Every dialog, button,
  input, menu and list is now generated from the active theme instead of
  hand-styled per window, so all four themes track consistently and the
  calm call-bar look extends app-wide. Buttons share one quiet family with
  accent primary actions; the composer's attach / emoji / send are now
  crisp vector icons matching the call bar.
- **Smoother call start.** Your self-preview appears the instant a call
  starts (no more black box until it connects), and toggling your camera
  off hides the preview rather than showing an empty tile. While
  ringing/connecting, controls that don't apply yet (screen share,
  telemetry, participants) stay hidden and appear once connected — the
  participants control only shows in an established group call.

### Added
- **Live mic meter.** Each call tile's name plate shows a real-time level
  meter (fast attack, gentle decay) so you can see your mic working at a
  glance — accent when you're speaking, calm green otherwise.
- **Stream bandwidth in telemetry.** The flight-log telemetry panel now
  shows the current outbound stream bandwidth (congestion-controlled
  video + audio).
- Clicking the pinned taskbar icon now reopens TalQ when it's been closed
  to the tray (previously only the tray icon worked).

### Fixed
- A startup crash and clipped/blank toolbar icons introduced while
  unifying the design system — both resolved; the theme system no longer
  recurses and icon buttons are never boxed.

## v0.30.1 "Bangaranga" (2026-05-18)

### Changed
- **Redesigned the in-call control bar.** The buttons were color-emoji
  glyphs that ignored the theme — a muted mic or an off camera looked
  identical to on. They're now crisp vector icons on a calm segmented
  pill: muted mic / camera-off show a warm-clay chip with a slash so the
  state is unmissable, active screen-share / panels light up, and
  hang-up is a detached red pill. Each control has a themed tooltip whose
  label reflects the current state ("Mute" ↔ "Unmute", "Turn camera
  on" ↔ "Turn camera off", and so on).

## v0.30.0 "Bangaranga" (2026-05-18)

Graduates the 0.29.x line: the new WebRTC call engine, screen sharing,
the dependency gate, always-on diagnostics, account/identity fixes and
the user-status feature are all verified working end to end.

### Fixed
- **Status popover behaves like a proper dropdown.** It now closes as
  soon as you pick a status, set or clear a message, and dismisses when
  you click away — and it no longer floats on the desktop when TalQ is
  minimized or sent to the tray.

## v0.29.10 "Bangaranga" (2026-05-18)

### Added
- **Set your status.** A status control on your sidebar profile (a
  presence dot on your avatar plus a glanceable pill by your name): pick
  Online / Away / Do not disturb / Invisible, set a custom message with
  an emoji, choose from the server's presets, and a "clear after" timer
  so a status can't get stuck forever.

### Fixed
- **No more stuck "In a call".** If TalQ ever crashed during a call you
  could be left showing "In a call / Busy" on the server. On every login
  TalQ now clears that automatically. It also leaves the call cleanly
  when you close the window, quit, or log out, so it can't leak in the
  first place.
- **Switching accounts no longer keeps the previous user.** A stale
  session cookie made TalQ keep showing the old account after logging
  out and back in as someone else; the session is now fully reset on
  every credential change.
- **The window always appears at the login screen.** Starting TalQ with
  no saved login (e.g. just after logging out) could leave it running
  with no visible window; it now always shows the login screen.

## v0.29.9 "Bangaranga" (2026-05-18)

### Fixed
- **Turning a camera on mid-call now reliably streams the video.** When a
  participant enabled their camera, TalQ asked the server for their
  stream exactly once; if the server wasn't ready yet ("not allowed to
  request offer") the request was dropped and the video never appeared.
  It now retries the same way the initial stream request does.
- **A peer's video is no longer abandoned too early.** The stream request
  used to give up after ~48s, which could leave one direction of a call
  permanently black if the server took longer to be ready. The retry
  window is now much more patient.

### Changed
- The always-on log now records the call and signaling lifecycle, so
  call problems are diagnosable from a normal run without any flags.

## v0.29.8 "Bangaranga" (2026-05-17)

### Changed
- TalQ now always keeps a small local diagnostic log and always writes a
  crash report if it ever stops unexpectedly, with no special flags
  needed. If a call ever misbehaves, the evidence is already on disk, so
  problems can be fixed instead of guessed at. (`--debug` still switches
  on the full verbose trace.)

## v0.29.7 "Bangaranga" (2026-05-17)

### Fixed
- **Video calls now actually connect.** Two bugs kept calls stuck on
  "Connecting" with black video: when the other person enabled their
  camera mid-call the new stream offer was silently dropped (the
  subscriber is now rebuilt for the new session), and the subscriber's
  connection state was never reported to the call UI (it is now), so the
  call could never go live even when media was flowing.
- **Your camera starts immediately on a video call**, not only after the
  call connects, so your own preview is live right away.

### Added
- The call view never shows a silent black tile: it now says why there
  is no picture ("Starting camera…", "Waiting for video…", "Camera off",
  "Connecting…").
- Control-bar buttons have clear hover feedback; the chat-header buttons
  were redesigned with consistent states and correctly anchored tooltips.
- A small credit for the "Bangaranga" release codename in Settings.

## v0.29.6 "Bangaranga" (2026-05-17)

### Fixed
- **The video-call crash is actually fixed now.** v0.29.5's installer
  was missing media components the new call engine needs (the stream
  decoder and the Opus audio parser). The moment a call connected, the
  person being called would have TalQ vanish instantly. Those components
  are now always bundled, so calls connect and stay up on both ends.
- **TalQ now checks its media components at startup** and refuses to run
  with a clear message if any are missing, instead of appearing to work
  and then dying mid-call. A broken install can no longer masquerade as
  a working one.

### Changed
- Updates always carry the complete set of dependencies and clear out
  files removed since the previous version, so an upgrade can never
  leave a half-installed, crash-prone TalQ behind.

## v0.29.5 "Bangaranga" (2026-05-17)

### Fixed
- **Video calls no longer crash the app.** Receiving a participant's
  video could hard-crash TalQ the instant the call connected. The
  receive side has been rebuilt on a new, robust media engine, so
  joining a video call is stable.
- **Remote video and audio now actually come through.** Previously a
  call could connect but stay black and silent; the other person's
  camera and microphone now arrive and play reliably, in both
  directions, through the conference server.

### Changed
- The whole video-receive path was re-engineered end to end and is now
  verified — connection, encryption, and live audio + video decoding —
  against the real server with automated testing, so calls are
  considerably more dependable than v0.29.4.

## v0.29.4 "Bangaranga" (2026-05-17)

### Fixed
- **Video calls no longer drop the moment you start them.** After the app
  had been idle a few minutes the server closed the pooled connection;
  the next request (joining the call) failed before it ever reached the
  server, the session never registered, and the call ended immediately.
  Requests that fail this way are now retried once on a fresh connection,
  so joining a call after an idle period works.
- **Adaptive bitrate now actually negotiates.** The congestion-control
  header extension was not being offered, so the server sent no feedback
  and the video could stall to black. It is now advertised correctly and
  the encoder follows the live network estimate (and no longer thrashes
  the hardware encoder with a reconfigure every fraction of a second).

### Changed
- Hardware H264, adaptive bitrate, and full-resolution screen sharing
  from v0.29.2/v0.29.3 (see below), now verified negotiating end to end.

## v0.29.3 "Bangaranga" (2026-05-17)

### Changed
- **Calls now use hardware H264 for far higher quality.** The camera and
  screen share encode with the GPU's H264 encoder when available (NVIDIA
  NVENC, Intel QuickSync, or Windows MediaFoundation), falling back to
  software only if no hardware encoder exists. This is dramatically
  sharper at the same CPU cost, and the conference negotiates H264
  end to end (VP8 remains a compatibility fallback).
- **Adaptive bitrate.** Video now rides a congestion controller that
  continuously raises or lowers quality to match the network and server,
  instead of a fixed guess, up to the server's limit (camera up to
  4 Mbit/s, screen share up to 12 Mbit/s). Smoother under load, much
  higher quality when the link is good.
- **Screen sharing is now full native resolution and high bitrate**, so
  shared screens stay crisp and readable rather than soft.

### Notes
- For best quality the High Performance Backend must allow these
  bitrates; see `docs/SELF-HOSTED-HPB.md`. The conference is H264; a
  client with no H264 support will not show video (by design, no
  quality-reducing transcoding) — keep all clients updated.

## v0.29.2 "Bangaranga" (2026-05-17)

### Fixed
- **Calls no longer collapse a few seconds after you turn your camera
  on.** The publisher encoded at a forced 1080p through a shared scaler
  with no fixed output size, so enabling the camera made the encoder
  reconfigure mid-call and allocate uncontrollably (a multi-hundred-MB
  spike in two seconds), stalling the app until the server dropped the
  publisher. The encoder now runs at a constant, device-supported
  resolution (≤1280×720 @ ≤30fps, matching the official client; the
  camera is captured within device capabilities, never forced to 1080p),
  and the publish bitrate is capped to the signaling server's limit. The
  call now survives enabling the camera.

## v0.29.1 "Bangaranga" (2026-05-17)

**"Bangaranga"**, for Bulgaria's Eurovision 2026 win in Vienna.

### Added
- **Reworked video calls: "The Bridge".** Both 1:1 and multi-party calls
  now share one adaptive surface that scales from two people to a full
  room without switching modes. It promotes whatever matters, a shared
  screen or the active speaker, to the stage and keeps everyone else on a
  warm rail; your own camera rides as a draggable picture-in-picture you
  can park in any corner. Click a tile to pin it, click again to hand
  control back to the room.
- **The call stays watchable while you work.** Navigate back into any
  conversation during a call and it tucks into a compact always-on-top
  corner; double-click it to bring it back full.
- **Calm Mission Control instrumentation.** A single breathing status
  pill carries call state at a glance (it warms to amber when a peer is
  catching up, to clay when reconnecting); every tile has a quiet
  connection light. A summonable telemetry drawer (press T) shows codec,
  decoder, per-peer link health and live stats for when you want the
  detail, hidden by default so the call stays quiet.
- **The call is now a real window**, not a fixed dialog: resizable,
  full-screen on double-click or F, with mic (M), camera (V) and
  screen-share (S) on the keyboard. The control bar fades when you stop
  moving and returns on the first nudge.
- A small release-codename pill on the Mission Control home.

### Changed
- The whole call UI is rendered on the warm four-theme system (no more
  black letterbox or cold chrome); reduced-motion is honored throughout.

## v0.28.5 (2026-05-16)

### Fixed
- **The message box now zooms with the rest of the interface.** The
  composer's text size was pinned by a stylesheet `font-size`, which
  overrides the zoom font, so `Ctrl+=` / `Ctrl+-` left the input text
  unchanged while everything else scaled.
- **The placeholder is no longer clipped at higher zoom.** The input
  box reserved too little vertical space for its own padding, border and
  text margin, so the bottom of the placeholder/first line got cut once
  the font grew. The box now budgets its full chrome and scales padding
  with the zoom level.
- **Saved zoom is restored on launch.** A zoom level kept from a previous
  session was applied to the chat but not the composer until you pressed
  a zoom shortcut again; both now restore together at startup.

## v0.28.4 (2026-05-16)

### Fixed
- **Installers now bundle the full Qt runtime.** The release pipeline ran
  `windeployqt` without Qt on PATH, so it failed silently and earlier
  0.28.x installers shipped with no Qt DLLs / platform plugin — a clean
  install would not launch. Fixed and the build now hard-fails if the Qt
  runtime is absent. **Upgrade from any 0.28.x installer to this build.**

## v0.28.3 (2026-05-16)

### Fixed
- **Clearer conversation-create errors.** When the server rejects creating
  a conversation, TalQ now shows the real HTTP/OCS status instead of a bare
  "Server refused" — and explicitly names the Talk "who can start
  conversations" restriction on HTTP 403, so the cause is obvious.

### Changed
- **Generic build shows the TalQ logo on the welcome screen** (where the
  branded build shows its logo).
- **Open-source build auto-updates from GitHub Releases.** No bundled
  update server or credentials; integrity rests on HTTPS to GitHub.

(Project also went open-source under Apache-2.0 at
github.com/kalinbogatzevski/talq-desktop.)

## v0.28.2 (2026-05-16)

### Fixed
- **Group rooms can be created with no members.** Creating a named group no
  longer requires selecting at least one person, so you can make an empty
  room and then add a bot (or invite people later). Previously the Create
  button stayed disabled, an internal guard rejected the submit, and even
  when forced through the dialog hung on "Creating room…" because the
  add-participant loop never completed for an empty member list. A 1:1 chat
  still requires exactly one counterpart.

## v0.28.1 (2026-05-16)

Release-notes formatting fix.

### Fixed
- **"What's new" release notes render with proper structure.** The update
  manifest was flattening every newline to a space and truncating notes to
  500 characters, so the in-app release notes collapsed into one run-on
  paragraph. The manifest now preserves the full changelog section verbatim
  (headings, lists, blank lines) as valid JSON.
- **"What's new" dialog and the welcome flight-log are themed.** Both were
  using Qt's cramped default markdown styling; they now use theme colors and
  a proper heading/list hierarchy.
- **Release script no longer crashes on non-Latin-1 changelog characters.**
  Manifest generation writes UTF-8 directly instead of relying on Windows'
  redirected-stdout encoding, which threw on arrows / em-dashes.

## v0.28.0 (2026-05-16)

Feature release porting the client-side improvements from the upstream Nextcloud
Talk beta.

### Added
- **Sidebar sort and filter.** A funnel control in the search row sorts the
  conversation list by recent activity, unread first, or name, and filters to
  all / unread / favorites / direct messages / groups. Favorites stay grouped
  on top. Choices persist across restarts.
- **Shared files panel.** Conversation info now lists files shared in recent
  messages with a type glyph and size; click a row to open it in the browser.
- **Call noise suppression.** Microphone audio is run through WebRTC noise
  suppression during calls (high-pass filter on, gain control off). Toggle it
  in Settings → Audio & Video; on by default. Degrades gracefully if the
  GStreamer `webrtcdsp` plugin is unavailable.

### Verified
- Private (1:1) replies render the quoted parent message, same as group
  conversations (already correct; confirmed and kept).

## v0.27.3 (2026-05-16)

Performance and polish. The redesign had introduced severe GUI-thread stalls;
this release removes them and tightens the new chrome.

### Fixed (performance)
- **Sidebar hover/scroll is fast again.** Conversation-list preview text was
  elided character-by-character, re-shaping the whole remaining string every
  step (O(n²)) on every visible row on every repaint: ~445 ms (up to ~2 s) per
  sidebar paint. Now uses Qt's optimized elide: ~6 ms per paint (~75x).
- **Chat background glow is cached.** The per-theme ambient radial gradient is
  rasterised once per size/theme instead of every paint.
- **Theme switching is cheap.** The Mission Control home is no longer rebuilt
  (with a full CHANGELOG re-parse) when it's hidden; it rebuilds lazily on
  next show. Off-screen telemetry refresh is skipped while chatting.

### Changed
- Theme switch shows a high-contrast accent toast at the top.
- The "Search conversations" field, sidebar buttons (incl. hover), profile,
  splitter, and update banner now re-tint on theme change.
- Auto-update banner is taller and more prominent (bold message, accent
  Install button, full borders, theme-tokenized); the side-stripe is gone.
- Startup update check now runs ~3 s after launch (was 30 s).
- Branded build: the 123NET logo appears top-right on the welcome screen.

## v0.27.2 (2026-05-16)

A small corrective follow-up to 0.27.1.

### Fixed
- **The Mission Control home now re-tints when you change theme.** It was
  built once at startup and kept the launch theme until restart; it is now a
  persistent host whose content is rebuilt on every theme change, so Ctrl+D,
  the sidebar swatch, and the Settings picker all update it live.

### Changed
- **Settings: the "Dark theme" checkbox is now a theme picker.** Choose
  Ember, Warm, Vivid, or Paper directly; the choice persists and applies
  immediately. Ctrl+D and the sidebar swatch still cycle.
- Design-system docs (`PRODUCT.md`, `DESIGN.md`, `.impeccable/design.json`)
  rewritten to the shipped "calm, warm, fast" direction and the four-theme
  system; the abandoned editorial/serif North Star is gone.

## v0.27.1 (2026-05-15)

A corrective release. The v0.27.0 editorial direction ("The Field Notebook",
Instrument Serif) was the wrong fit for a chat app and is fully reverted. The
new direction is calm, warm, fast: a clean Inter surface, distinctive through a
warm palette and craft rather than a typeface. Adds a user-selectable theme
system and a redesigned home screen. No protocol or feature changes.

### Typography
- **Instrument Serif removed.** The header conversation title and the welcome
  name return to clean Inter. The bundled serif font, its registration, and
  its resource entry are gone. Hierarchy is size/weight/color only.

### Themes (user-selectable)
- **Four warm themes: Ember, Warm, Vivid, Paper** (default Vivid). Three warm
  dark levels plus one warm light. Every color is a `PainterTheme` token.
- **Pick or cycle:** `Ctrl+D` or the sidebar swatch cycles themes; the choice
  is persisted across restarts. The painters (chat, sidebar, header, threads)
  re-tint live.

### Flavour
- Soft ambient accent glow behind the thread, a colored halo on the active
  conversation row and the send button, and a quiet breathing presence dot.
  Tasteful state glows, not decoration.

### Mission Control home
- The empty-state home is now a live status board: a system-status pill,
  telemetry tiles (server, signaling, push, Nextcloud, Talk, GPU) with status
  LEDs, a GStreamer subsystems strip, and the changelog framed as a flight
  log. All theme-tokenized; degrades to amber when a subsystem drops.

## v0.27.0 (2026-05-15)

A focused UX/visual redesign pass, driven by a two-assessment design critique
against a new project design system (`PRODUCT.md` + `DESIGN.md`, North Star
"The Field Notebook"). Identity, color discipline, contrast, shape, and
typography. No protocol or feature changes.

### Typography — editorial display tier
- **New bundled display face: Instrument Serif** (SIL OFL). The header
  conversation title and the welcome name now use a real editorial serif at
  genuinely larger sizes (20px / 26px). Body, labels, and all dense chrome
  stay Inter for legibility. Fixes the "no real display tier / Inter
  everywhere" finding without risking body readability.
- **Italics removed as a hierarchy lever** (system messages, header
  placeholder, typing subtitle). Differentiation is now size/weight/color
  (Two-Lever Rule).

### Color discipline (One-Signal / No-Gray)
- **The teal accent is now reserved for one meaning.** The TalQ peer marker
  no longer uses a foreign cobalt `#2563eb`; the favorite dot no longer uses
  the teal accent (now amber); the rogue off-palette teal `#2EC4B6` driving
  unread/selection/highlight is gone; the full-viewport unread teal wash was
  removed (the separator pill carries it).
- **No more `#fff`/`#000` or cold gray.** Badge/initial/checkmark glyphs use
  a warm ink token; autocomplete popups and the message selection control
  were re-palened from cold grays to the warm ladder; presence "DND" is the
  calm clay, not a fire-engine red; light-theme surface is warm, not pure
  white.
- **Contrast tuned to WCAG AA.** `textMuted` (~2:1) and `textTime` (~3.5:1)
  were below AA for normal text in both themes; retuned to clear 4.5:1.
  Unread-badge text moved to ink (was white-on-teal ~2:1).

### Shape + structure
- **Composer is no longer a pill.** Input and buttons move from 17–20px
  full-round to the system 8px control radius.
- **Side-stripes removed.** The 3px teal/amber left bars on the reply quote
  and the reply/editing bars are replaced by a hairline and a leading glyph
  (side-stripe ban).
- **Composer stylesheet-leak fixed.** Every `setStyleSheet` on the composer's
  bars/labels is now scoped by `#objectName`, so container styles can no
  longer cascade into child controls (the defect class behind the dark/clipped
  bot buttons in 0.25.6). Off-ladder invented surface tones snapped to the
  documented tonal ladder.

### Known follow-up
- The header trailing-edge can still show up to five icon buttons; collapsing
  to three + an overflow was scoped out of this pass as a riskier interaction
  change and is deferred to a later release.

## v0.25.7 (2026-05-15)

### Fixes — TalQ peer identification ("Q" badge)
- **The "Q" badge / peer TalQ version is now reliable and persistent.** Root cause: peer client identity lived only in an in-memory map populated *solely* by a live signaling-room broadcast. It required you and the peer to be simultaneously joined to the same conversation's signaling room at the same instant, was one-shot (no replay, no catch-up), and was never persisted — so it appeared for some peers (a live overlap happened to occur) and never for others, with no way to refresh, and was lost on every restart. Fixes:
  - **Persisted across sessions** via `QSettings` (`peerClients` group, percent-encoded user IDs). Once a peer is seen on TalQ even once, the badge sticks — across rooms and restarts.
  - **Wider handshake:** the TalQ hello is now re-announced when a new peer appears in a `participants` update, not only on the `room/join` event. HPB does not reliably deliver `join` for all peers, which is why genuinely co-present peers were still missed.
  - **Badge moved to the avatar's top-right** in the sidebar — it was being painted at the bottom-right, *under* the presence status dot, and hidden.
  - **The correspondent's TalQ version now shows in the 1-on-1 chat header subtitle** (e.g. `Online · TalQ 0.25.7`) — always visible, instead of only as a per-message author-name suffix that almost never rendered in a 1-on-1 conversation.

  Note: this is a "best known" indicator — if a peer later switches from TalQ to the web client and you are never co-present again, it can show stale-positive until a newer hello arrives. Deliberate trade-off versus the previous near-useless behavior.

### Features — bot management
- **Inline "Enable" button on disabled bot rows** in Conversation Info → Bots. Previously a disabled bot row only offered "Remove", so enabling required the roundabout "+ Add bot" dialog. Any conversation moderator (no admin/CLI) can now enable an installed bot in two clicks, provided the bot was not installed with `--no-setup`.
- **Clear error when the server blocks enabling.** If the bot was installed with `occ talk:bot:install --no-setup` (state "no setup via GUI"), the per-conversation enable API is refused; TalQ now states exactly that ("Server blocked it — this bot was installed with --no-setup") instead of a bare HTTP code.

### Fixes — UI rendering
- **Bot "Enable" button was unreadable** (near-black text on the dark panel background). `m_botsContainer` sets a selector-less `background` stylesheet that leaks into descendant buttons and overrode the `#primary` rule. Both Enable buttons now use an explicit, self-contained stylesheet that is immune to the cascade.
- **"Add bot" dialog: the per-bot Enable button rendered as a thin line with no text.** Its `QListWidget` item used `setSizeHint(row->sizeHint())` computed before the inherited stylesheet was polished, so the styled button was clipped. Now uses an explicit fixed row height.
- **Bot "B" icon was clipped on the right/bottom edge.** `drawEllipse(0, 0, size, size)` painted to the exact pixmap bounds, so the antialiased edge was cut. Inset by 0.5px.
- **The call-screen avatar circle had the identical edge clipping** (`CallDialog`) — same fix applied.

## v0.25.6 (2026-05-13)

### Fixes — @-mention composer
- **Mention popup now actually appears.** `ApiClient::fetchMentions` was hitting `apps/spreed/api/v4/chat/{token}/mentions`, but the NC Talk chat API mounts mentions under `v1` — `v4` is for the room API. Every request returned HTTP 404 with an empty body, which the code treated as "no candidates" and silently hid the popup. Net effect: typing `@` did nothing visible. Fixed to `v1`.
- **Popup no longer steals keyboard input.** The mention popup (and the emoji-shortcode popup, which had the same code) was created with `Qt::Popup` window flag. That flag is appropriate for combo-box dropdowns where the user MUST commit, but wrong for autocomplete-style popups where the user wants to keep typing past the trigger character. Replaced with `Qt::FramelessWindowHint | Qt::Tool | Qt::WindowDoesNotAcceptFocus` + `WA_ShowWithoutActivating` so the composer keeps receiving keystrokes while the popup floats. Single-click now selects (was double-click only). The popup auto-dismisses when the composer loses focus.

### Features — NC Talk bot framework support
- **Bots panel in Conversation Info dialog.** Lists every bot currently enabled in the conversation, with a "Remove" button per row for moderators. Each row shows a teal "B" badge as the default bot icon (no font dependency).
- **"+ Add bot" sub-dialog.** Admins see the full server-installed bot list via `/api/v1/bot/admin` with per-row "Enable" buttons. Non-admin moderators get a manual "Bot ID" input (since `/admin` is admin-only); they can still enable a bot by ID if shared out-of-band.
- **Bots appear in @-mention popup.** When the user types `@`, TalQ now fetches `/api/v1/bot/{token}` in parallel with the mention candidates and merges enabled bots into the dropdown with a "(bot)" suffix on the label. This works even on NC server versions whose `/mentions` endpoint doesn't include bots. Mention text is sent as `@<bot-name>`; whether the server resolves that into a structured mention parameter depends on the server's bot framework version. The bot receives the message via its webhook regardless.
- **New `ApiClient` methods:** `fetchEnabledBots(token, …)`, `fetchAllBots(…)` (admin), `setBotEnabled(token, botId, on, …)`. New header `core/BotInfo.h` for the data type.

### Note on bot interaction
The NC Talk bot framework does not allow user-initiated 1-on-1 conversations with bots — bots aren't real NC users and don't appear in user search. For a "personal assistant" feel, install the bot with `occ talk:bot:install --no-setup` so it auto-attaches to every conversation, or have an admin pre-create a per-user room with the bot enabled.

## v0.25.5 (2026-05-13)

### Features
- **One-click theme toggle in the sidebar.** A small sun/moon icon button sits between New-chat and Settings in the top of the sidebar profile bar. Shows a sun while in dark mode (click to switch to light) and a moon in light mode (click to switch to dark). Same code path as the existing Ctrl+D shortcut and the Settings → General "Dark theme" checkbox — `MainWindow::applyTheme` keeps all three in sync.

## v0.25.4 (2026-05-13)

### Changes — update channel
- **Poll interval reduced from 4 h to 5 min.** During active development we ship multiple point releases per day, and a 4 h window meant testers stayed on the previous build until they restarted the app. The check is a single sub-1KB JSON GET against the update manifest — CPU and bandwidth cost is negligible. Initial 30 s on-launch check is unchanged.
- **"Check for updates now" button in Settings → Updates.** Triggers an immediate manifest fetch via `UpdateChecker::checkNow()` over a signal so testers don't have to wait out the poll interval. Button shows "Checking…" while in flight, then a neutral "if no banner appeared, you're on the latest version" message — relying on the existing update banner for positive signal.

## v0.25.3 (2026-05-13)

### Fixes
- **Topic tab bar lingers after Home.** Clicking the Home button while inside a group chat with topics ("All messages / General / …") left the tab bar visible above the welcome screen. The `homeRequested` handler in `MainWindow` reset the message model and hid the chat painter but never called `updateTopicMode(false)`, so `m_showTopics`, the header flag, and the tab bar widget visibility all stayed in the previous group's state. Fix: invoke `updateTopicMode(false)` from the home handler, before clearing the active conversation.

## v0.25.2 (2026-05-13)

### Features — message rendering
- **Markdown subset in chat messages.** Bold (`**text**`), italic (`*text*` / `_text_`), strikethrough (`~~text~~`), inline code (`` `text` ``), and fenced code blocks (` ```...``` `) now render formatted instead of literal. Implemented in `Message::fromJson` between HTML escape and `messageParameters` substitution; code content is stashed under sentinel tokens so its inner `**` / `_` aren't re-interpreted as emphasis. Italic rules follow CommonMark "flanking delimiter" — `*` must be adjacent to non-whitespace and not be part of a bold pair; `_` requires word boundary so snake_case_names survive. Headings, lists, and horizontal rules are deliberately not parsed: people don't use them in chat and they invite false positives on punctuation-heavy plain text. Compatible with messages from the official NC web client (which renders the same subset).

## v0.25.1 (2026-05-12)

### Fixes
- **TalQ peer identification now actually works between v0.25.x clients.** The HPB broadcast was being parsed in the wrong branch of `SignalingClient::onTextMessage`: client-originated room broadcasts arrive as `type:"message"` (same path as typing indicators and WebRTC signaling), but the `talq.client` handler was sitting in the `type:"event"` branch — which only handles server-originated broadcasts like chat-refresh hints. The broadcast was being sent and routed correctly; nothing was ever reading it on the receiver side. Defense-in-depth fixes layered on top:
  - The payload now self-identifies (`data.userid = m_userId`) instead of relying on the spreedbackend's `sender` annotation, which only carries sessionid on some configurations.
  - `m_sessionToUserId` is populated from participants events so a sender's userId is resolvable even on servers that strip it from broadcasts.
  - The original `event/room/message` handler is retained as a fallback for server variants that route differently, and the diagnostic logs are now emitted at default verbosity (not `--debug` only).

### Features — Windows taskbar
- **Unread badge on the taskbar button.** Qt6 dropped `QtWinExtras`, so we go straight to `ITaskbarList3::SetOverlayIcon`. A 16×16 red badge with the unread count overlays the TalQ icon on the Windows taskbar whenever `totalUnread > 0`, mirroring the existing tray-icon behavior. Tooltip on the taskbar button reads "N unread".

## v0.25.0 (2026-05-12)

### Features — peer-client identification
- **TalQ users now identify themselves to other TalQ peers.** Two-channel announcement:
  - **HPB room broadcast** — on every room join (and re-broadcast when a new peer joins the room), TalQ sends a transient `{type:"talq.client", client:"TalQ", version:"X.Y.Z"}` message to the room. The signaling backend's "sender" annotation lets receivers cache the version by NC userId. Works regardless of call state.
  - **WebRTC data channel** — during a call, the same payload is sent on the existing status data channel (audioOn/videoOn/speaking already uses it). Each new subscriber triggers a fresh announcement so latecomers don't miss it.
- **Visible identification:**
  - **Sidebar avatar badge** — small blue circular "Q" overlay on the avatar bottom-right for 1-on-1 contacts whose userId is in the TalQ peer cache. Non-TalQ peers show nothing (no false positives).
  - **Chat author tag** — `· TalQ/X.Y.Z` appears next to the author name on incoming messages from known TalQ users. Muted color, smaller font, doesn't compete with the name itself.
- **Cache survives room switches** because TalQ identity is keyed on NC userId, not signaling sessionId. Once you've seen "user X uses TalQ/0.24.0" in any room, the badge sticks.
- **Cross-version compatibility** — v0.24.0 and earlier clients don't broadcast and don't display badges. v0.25.0 sees v0.25.0 only; older peers appear non-identified until they upgrade.

### Performance / memory
- **Emoji pixmap cache cap** — `EmojiData::g_pixmapCache` is now FIFO-capped at 800 entries (~20MB worst case). Previously unbounded; in long sessions with many rendered glyphs at multiple sizes the cache could quietly accumulate.
- **Real cache stats in DebugMonitor** — the `[MEM-DETAIL]` line (once/minute in `talq_debug.log`) now reflects the actual painter-side caches (sidebar avatars, chat avatars, layout cache, preview cache, emoji cache) instead of the dead QML-era providers that always reported zero. Use this to chase memory growth without guessing.

### Cleanup — dead code purge
- **Removed `src/qml/` (20 files)** — all QML views from the pre-QPainter era, never referenced by any source file or build target.
- **Removed `installer/qtifw/`** — 87 MB / 1300 files of a Qt Installer Framework bundle from before we standardized on Inno Setup. Tracked in git so recoverable if ever needed.
- **Removed `cmake/win64-mingw-cross.cmake` and `scripts/package-windows.sh`** — the Linux→Windows cross-compile workflow, unused since we moved to native Windows builds.
- **Removed `core/AvatarProvider.{h,cpp}` and `core/FilePreviewProvider.{h,cpp}`** — QML image providers that became no-op stubs; their cache-size accessors fed 0 to DebugMonitor for months.
- **Removed `DebugMonitor::visible` property** — paired with the deleted QML overlay; Ctrl+D now controls the theme toggle only.
- **Scripts: `--qmldir` → `--no-qml-import-scan`** in `deploy-dev.sh` / `build-release.sh`. windeployqt no longer scans for a directory that doesn't exist.
- **README rewritten** to describe the actual QPainter-on-QWidget architecture (was still documenting the QML structure).

## v0.24.0 (2026-05-12)

### Features — message lifecycle
- **Mark a message as unread** — right-click any incoming message → "📩 Mark as unread". POSTs `lastReadMessage = id - 1` to `/chat/{token}/read`, refreshes the "New messages" divider immediately above the targeted message, and mirrors the new value into the ConversationListModel cache so the sidebar badge and unread divider don't pop back on a chat switch. The entry is hidden for own messages — those are read by definition the moment they're sent. Side-fix: `ChatPainter`'s unread divider used to re-summon on chat switch because the server-side `lastReadMessage` never advanced when the user dismissed the divider visually; the painter now emits `unreadSeparatorDismissed`, `MainWindow` wires that to `markAsRead`, and `ConversationListModel::markReadAt` mirrors the result locally.
- **Forward from the message context menu** — "↗️ Forward" between Reply and Pin opens the existing ConversationPickerDialog. Previously you had to enter selection mode first.
- **Scheduled messages** — right-click the Send button → "⏰ Send later" with presets (in 1 h / 3 h, tomorrow 08:00, tomorrow 18:00, next Monday 09:00) and a custom QDateTimeEdit picker. POSTs `/chat/{token}/schedule`. Confirmation tooltip "✓ Scheduled for …" pops near the Send button once the server accepts. The reply target carries through, same as a normal send.
- **Scheduled-message manager** — "📋 Manage scheduled…" in the same menu opens a non-modal dialog listing pending items with per-row **Edit** and **Cancel** buttons. Edit pops a popup with message text + datetime (`POST /chat/{token}/schedule/{id}`); Cancel deletes (`DELETE /chat/{token}/schedule/{id}`). Empty state and live reload after each mutation.
- **Silent-message receipt** — the sender's "Send silently" flag is now honored on the receiver. `Message::silent` is parsed from the wire, `Conversation::lastMessageSilent` from `lastMessage.silent` in the room API. The active-chat notifier and `ConversationListModel::newUnreadMessage` both skip emitting when the latest message is silent, so right-click → "Send silently" stops popping desktop toasts on the other end.

### Features — image viewer
- **Right-click on the image** pops the same Copy / Save-as menu the ⋯ button shows. Most people reach for right-click first; the ⋯ stays as a fallback.
- **"Copied to clipboard" pill** — Ctrl+C / "Copy image" now flashes a bottom-centered toast for ~1.4 s instead of mutating the title-bar suffix. Repositions on resize.

### Fixes — main window
- **Fullscreen survives a notification click or tray-restore** — `MainWindow` now tracks `m_wasFullScreen` alongside `m_wasMaximized` (the latter is `false` while fullscreen, which is why a fullscreen user got dropped to normal on restore). `openConversation` clears `WindowMinimized` from the state when un-minimizing — Qt retains the prior fullscreen/maximized bit through the minimize cycle, so the window resumes in whatever state you left it. `restoreFromTray` branches `m_wasFullScreen → m_wasMaximized → showNormal`. Visible-but-not-focused was already correct; the regression was specifically minimize/hidden → restore.

## v0.23.5 (2026-05-11)

### Fixes — read receipts
- **Read tick (◉) now updates in near real-time** instead of only when the correspondent sends a reply or you switch conversations. Four cooperating fixes:
  - **Stale layout cache.** `ChatPainter`'s per-message layout cache (introduced in v0.23.4) bakes `isRead` into each cached `MessageLayout`. The role-filtered `dataChanged({IsReadRole})` shortcut correctly skipped the layout rebuild, but never refreshed the baked-in value — so `update()` repainted the same stale glyph. The fix walks the affected model range and patches `isRead`/`sendStatus` on both `m_layouts` and `m_layoutCache` before repainting.
  - **`X-Chat-Last-Common-Read` request header.** `MessagePoller` now sends the last known common-read value as a request header on the chat long-poll. Per the NC Talk docs this is the hint the server uses to break the long-poll early when the room's read marker advances (a 304 cannot carry custom response headers, so without the request hint the server has nothing to compare against).
  - **HPB signaling chat-event handler.** `SignalingClient` now recognizes `target=room, type=message, data.type=chat` events from the standalone signaling server and emits `chatRefreshNeeded(roomToken)`. `main.cpp` wires this to `messages.refresh()` for the open chat. This is the channel the official spreed client uses for instant chat updates — without it we'd only see new messages via the slower chat long-poll.
  - **5 s periodic read-marker pull.** A small `QTimer` in `MessageListModel` issues a tiny `lookIntoFuture=0&limit=1&setReadMarker=0` request every 5 s while a chat is open, just to harvest a fresh `X-Chat-Last-Common-Read`. Reliable fallback for servers whose HPB only relays new-message events (not read-marker advances).

### Build / packaging
- **`ccache` integration in `build-release.sh`.** If `ccache.exe` is found at `$MSYS2/ccache.exe`, the configure pass adds `-DCMAKE_C_COMPILER_LAUNCHER` / `-DCMAKE_CXX_COMPILER_LAUNCHER`. Cold release builds take the same ~3 min as before; repeat clean rebuilds drop to ~30–60 s (>95 % cache hits, bit-identical output).

## v0.23.4 (2026-04-25)

### Performance
- **Long-chat scroll lag fixed** — `ChatPainter::rebuildAllLayouts()` was redoing every message's `QTextDocument::setHtml` + grapheme scan on every model change, including read-receipt-only updates from polling. With ~1000 messages this could block the UI thread for a second or more, several times in a row, producing the multi-second freezes during scrollback.
  - **Per-message layout cache.** Each `MessageLayout` is keyed on width/theme/font and a content fingerprint; rows whose key matches are reused with a y-translation instead of recomputed. Steady-state rebuilds drop from O(N · HTML-parse) to O(N · hash-compare).
  - **Role-filtered `dataChanged`.** When the only changed roles are `IsReadRole` / `SendStatusRole` (paint-only), we skip the rebuild and just `update()`. This kills the polling-induced rebuild storm.
  - **Resize debounce.** Window-drag resize ticks now coalesce through a 50 ms single-shot timer instead of rebuilding on every pixel.
  - **`--debug` instrumentation.** Each rebuild now logs `[layout] N msgs in X ms (cached/fresh)` to `talq_debug.log` so future regressions are easy to spot.

### Appearance
- **Theme switch in Settings** — Settings → General now has an "APPEARANCE" section with a "Dark theme" checkbox. Toggle it to switch live; choice persists. Ctrl+D shortcut is unchanged.

## v0.23.3 (2026-04-22)

### Fixes
- **Emoji autoreplace on Enter-send** — `:)`, `:D`, `:shortcode:` etc. used to substitute only on trailing space, so hitting Enter right after the shortcut sent the literal text. `sendAction()` now flushes a pending substitution before sending, so `:) <Enter>` goes out as 🙂.

## v0.23.2 (2026-04-21)

### Notifications
- **Notification stack** — multiple toasts no longer overwrite each other. New `NotificationStack` widget stacks popups bottom-up on the primary screen, caps at 4 visible, ages out the oldest when a 5th arrives. Rapid repeats from the same conversation (within 3 s) coalesce into "N new messages" instead of stacking separately.
- Clicking any toast opens that conversation + removes that specific toast; the rest reposition to fill the gap.
- **Toast layout rebuilt** — 360×92 body with warm-dispatch palette, title elided with `…` instead of hard-cut, message linebreaks flattened so the 2-line budget shows real content. `Qt::Tool` so toasts no longer steal focus or show in the taskbar.

### Sidebar / thread list
- **Preview text flattens newlines** — last-message previews collapse `\r\n\t` to spaces and squeeze runs of whitespace, so multi-line messages show their first real content instead of getting cut at the first linebreak.

### Platform / support
- **AppData writability probe at startup** — if the app can't write to its data folder (common on corporate Windows profiles where Roaming is redirected to an unreachable network share), a `QMessageBox` surfaces the path and explains what's wrong, instead of silently running with broken caching.

### Chat header icon polish
- All five header icons (video, phone, search, bell, info) render at matching optical size (16 px for SVGs, 18 px font for glyphs).
- Phone + video redrawn from tight-viewBox inline SVGs with stroke-widths compensated for aspect-ratio differences, so both land at ~1.4 px rendered stroke and match the Fluent Icons weight.
- Removed the legacy `•••` (and follow-up `↻`) loading indicator that sat next to the call buttons — polling doesn't need header chrome; the sidebar already surfaces new-message state.
- `Qt6::Svg` linked for color-tinted SVG rendering.

## v0.23.1 (2026-04-21)

### Fixes — image paste
- **No more UI freeze on paste** — PNG encoding moved off the main thread (`QtConcurrent::run` + `QFutureWatcher`). Large clipboard images no longer lock up the window.
- **Visible "Preparing image…" feedback** — the pending-file bar now shows an hourglass immediately when you paste, replaced with the real thumbnail when encoding finishes.
- **Thumbnails appear for freshly sent images** — Nextcloud's preview service is async and 404s for a few seconds after upload. Thumbs used to get cached as empty on that first miss and never retry. Now retries with `1.5s → 3s → 6s → 12s` backoff (4 attempts, capped at 30s).

### Fixes — PR-review backlog
- **Teal accent now consistent everywhere** — swept `#2ec4b6` → `#14b8a6` in Message/CallDialog/SharePicker/SelectionBar/ConversationPicker that earlier missed the warm-dispatch refactor.
- **Memory leak fix — `setChatThreadTitle`** — the `shared_ptr<function>` self-reference cycle from v0.23.0 is gone; the endpoint-chain is now a heap state machine that explicitly self-deletes on terminal.
- **Memory leak fix — group creation** — the shared counter/errors used while inviting participants now use `shared_ptr`, so they're freed if the dialog is dismissed mid-flight.
- **Wrong-conversation race fix — new topic** — creating a topic while switching rooms no longer yanks you into the topic view on the wrong conversation.
- **Member list UX — right-click for actions** — left-clicking a member row no longer pops a context menu on every click. The promote/demote/remove menu is on right-click only, and doesn't appear at all on non-actionable rows (yourself, owner, or if you're not a moderator).

## v0.23.0 (2026-04-21)

### Conversation management
- **Group info dialog** — new ℹ button in the chat header opens a modal to rename the group, edit description, add/remove members, promote/demote moderators, leave, or (owner only) delete. Reachable from any group or public room.
- **Promote & demote** — click a member in the info dialog → context menu with promote-to-moderator, demote-to-member, or remove actions.

### Topics, properly
- **Telegram-style topic strip** replaces the old 3-column threads panel. Horizontal chip bar above the chat: "All messages", every topic as its own chip with unread count, trailing "+" for new.
- **One-click topic creation** — click +, type a name, done. TalQ sends a silent seed message and best-effort sets the thread title; you land in the new topic ready to chat.
- **Threads clear on group switch** — the old list no longer leaks into the new group during the fetch round-trip; cache path now actually runs on switch.
- **1-on-1 chats correctly hide the topic strip** — previously flickered visible during model reset.

### Design — "warm dispatch" identity
- **Inter** bundled as the body font across the entire app; consistent typography from sidebar to composer.
- Warm paper-dark palette replaces flat gray.
- **Electric teal** accent (`#14b8a6`) with proper hover/pressed states.
- **Segoe Fluent Icons** for all chrome (sidebar controls, header buttons, composer). Bell icon for reminders now renders consistently across Windows versions.
- Dialogs (New Chat, Conversation Info, Upcoming Reminders, Nextcloud File Picker) share a token system — same eyebrow labels, same primary-button shape, same rounded corners.
- Composer input corners round properly now (QTextEdit viewport transparency fix).
- Sidebar preview (last message under chat name) uses readable secondary text color.
- `EmojiTextRenderer::elide` bug fix — long one-word previews used to collapse to just "…"; now they trim character-by-character.

### ApiClient additions
- `setRoomName`, `setRoomDescription`, `deleteRoom`, `leaveRoom`
- `fetchRoomParticipants`, `removeRoomParticipant`, `promoteModerator`, `demoteModerator`
- `sendChatMessage` (returns new message id), `setChatThreadTitle` (best-effort chained endpoint attempts)

### Fixes
- Description field can no longer silently overwrite the server value with an empty string on focus change (guard via `QLineEdit::isModified`).

## v0.22.1 (2026-04-20)

### Fixes
- **Chat header clears when returning to Home** — stale conversation name stayed visible; `m_header` now hides on Home navigation and shows on conversation open.
- **Update banner stays visible across navigation** — switching between Home and a chat used to leave the banner in an unpredictable state. Added an internal active flag so once the banner is shown, it's re-shown and Z-raised whenever the chat-column contents switch.

## v0.22.0 (2026-04-20)

### New chats
- **Create new conversations from TalQ** — ➕ button in the sidebar (next to ⚙ settings) opens a **New chat** dialog.
  - **Direct:** search for a user, double-click → creates a 1-on-1 conversation and opens it.
  - **Group:** name the group, search and add multiple participants, create → room is created and everyone invited in one step.
  - User autocomplete via `GET /ocs/v2.php/core/autocomplete/get?itemType=call`.
  - Room creation via `POST /ocs/v2.php/apps/spreed/api/v4/room` (`roomType=1` for direct, `2` for group).
  - Group participants added via `POST /room/{token}/participants`.

## v0.21.0 (2026-04-20)

### Reminders
- **Right-click any message → "Remind me…"** with quick presets (20 min, 1 h, 3 h, Tomorrow 8:00, Next Monday 9:00) and a custom date/time picker. Server persists the reminder and sends a push notification when the time comes.
- **⏰ Upcoming reminders** button in the chat header opens a list of all pending reminders across conversations. Double-click to jump to the original message.
- Internal `Reminder` struct carries a `source` enum (`NextcloudTalk`, `Erp`) so future ERP integration can contribute entries into the same upcoming-reminders view.

## v0.20.2 (2026-04-19)

### Hardening from PR review

- **Share errors now say what actually went wrong** — the picker and the share dialog both branch on HTTP status (401 "session expired", 403 "not permitted", 404 "no longer exists", 503 "maintenance", …) and include the server's OCS `meta.message` when present. Previously everything collapsed to "Are you offline?" / "HTTP 400".
- **Context-safe share callback** — `ApiClient::shareNextcloudFileToChat` now takes a `QObject *context` and disconnects automatically if the caller dies, matching `listNextcloudFolder`'s existing contract. Prevents a theoretical dangling-`this` crash if the composer were destroyed mid-share.
- **PROPFIND treats non-207 responses as failure** — an HTML captive-portal page (common on corporate networks that intercept) used to parse as "empty folder"; now surfaces as an explicit error.
- **PROPFIND checks `QXmlStreamReader::hasError()`** — malformed/truncated XML was silently producing a short list reported as success; now fails loudly.
- **`--debug` log-setup failure is visible** — if `freopen(stderr)` fails (AppData on unreachable share, permissions, AV holding the file), TalQ now shows a `MessageBox` explaining why there's no log, instead of silently running with logging defeated.
- **fileId widened to `qint64`** — NC fileids routinely exceed 2³¹ on busy instances.

## v0.20.1 (2026-04-19)

### Fix
- **Share from Nextcloud now actually works** — v0.20.0 used the wrong endpoint (`/chat/{token}/share`, which is for generic objects like polls). Switched to the Files Sharing API (`/apps/files_sharing/api/v1/shares`) with `shareType=10` (Talk conversation) + `shareWith=<roomToken>` — same call the official NC Talk web frontend makes. Server rejected the old call with HTTP 400.

## v0.20.0 (2026-04-19)

### Share from Nextcloud
- **Attach → From Nextcloud** — the 📎 button now opens a menu with "From this device…" and "From Nextcloud…". The Nextcloud path opens a file browser showing your Nextcloud Files; pick a file and TalQ tells the server to create a share link and post it into the chat (matching how the official NC Talk client works). WebDAV `PROPFIND` for listings, POST to `/ocs/v2.php/apps/spreed/api/v1/chat/{token}/share` to post the link.
- **Breadcrumb navigation** — clickable path segments at the top; double-click a folder to dive in, double-click a file to share immediately.

## v0.19.2 (2026-04-19)

### Fixes
- **Auto-upgrade now actually relaunches** — the `[Run]` section of the installer had the `skipifsilent` flag, which made Inno Setup skip the post-install launch during `/VERYSILENT` installs. After an auto-upgrade, the old TalQ quit but the new one never came back up. Removed the flag so silent installs auto-launch the freshly installed binary.

## v0.19.1 (2026-04-19)

### Composer polish
- **Multi-line input that actually grows** — the message box now expands as you add lines (up to ~5) instead of staying locked at one line. Existing line still hidden behind paragraph returns finally visible.
- **"New messages" divider clears on engagement** — clicking in the composer, typing, or sending a message now dismisses the divider immediately. Previously only scrolling past it dismissed it, which felt stale when you'd clearly acknowledged the new messages.

### Diagnostic
- **`--debug` CLI flag** — Release builds now accept `--debug` (or `-d`) to write a verbose log to `%APPDATA%/talq_debug.log` including GStreamer/Qt warnings. Helpful for remotely diagnosing startup problems on tester machines. Also redirects stderr into the same file so anything printed at C-level is captured.

## v0.19.0 (2026-04-19)

### Auto-upgrade
- **Auto-update** — TalQ now checks an update manifest at startup and every 4 hours. When a newer version is available, a teal banner appears at the top of the chat: "Install now", "Later", or "What's new" (release notes popover). Downloads stream to the temp folder, verify against SHA-256 from the manifest, then launch Inno Setup silently (`/VERYSILENT /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS`) so the session restarts into the new version automatically.
- **Call-safe** — if you're in a call or sharing your screen when the update is ready, the launch is deferred until the call ends; the banner says so.
- **Opt-out** — Settings → Updates → "Automatically check for updates" toggle. On by default.
- **Release script integration** — `scripts/build-release.sh` uploads both installers + a freshly generated manifest when `NC_APP_PASSWORD` is set, so every future release reaches clients automatically.

## v0.18.0 (2026-04-18)

### Presence
- **Rich status line** — chat header now shows the peer's custom status message + icon (e.g. "🎯 in a meeting") under the conversation title. Falls back to "Online" / "Away" / "Do not disturb" / "Offline" when no custom message is set. Typing state still takes precedence.
- **Multi-color sidebar dots** — away shows amber, DND red, offline hidden; previously only online was surfaced.
- **Dedicated 60-second poll** — status now refreshes on its own timer instead of riding on the conversation-list poll, so changes appear faster.

### Message search
- **In-conversation search** — 🔍 button in the chat header opens a search bar. Results dropdown shows actor + snippet. Click a result to scroll to the message with a 2-second teal flash. If the message is older than what's loaded, history pages backward up to 5 pages automatically.

### Silent send
- **Right-click Send** or **Alt+Enter / Alt+Click Send** to send without triggering notifications. Uses NC Talk server's `silent=true` flag. Send button flashes 🔕 briefly to confirm.

### Time on hover
- Hovering a message's time now shows the full date and time in a tooltip, including the edit timestamp for edited messages.

## v0.17.4 (2026-04-18)

### Fixes
- **Edited messages update in place** — v0.17.2's `editMessage` was double-unwrapping the OCS envelope, which silently replaced your bubble with a blank row. Now the edit shows immediately without a conversation switch.
- **Unread divider auto-dismisses** — once the "New messages" pill has scrolled above the viewport for 2 seconds, it fades out automatically (Discord-style). Switching conversations still resets the state.
- **Scrollbar drag** — grabbing the chat scrollbar now scrolls instead of triggering multi-message selection. Clicking the track jumps the thumb to that point; dragging the thumb follows the cursor.

## v0.17.3 (2026-04-18)

### Unread divider
- **"New messages" pill** — reopening a conversation with unread messages draws a teal accent pill across the chat above the first unread, with a subtle tinted region below it. Matches Nextcloud Talk web.
- Divider is anchored by message id, so scrolling up to load older history keeps it in place.
- Divider is session-frozen: it stays put until you leave the conversation; on the next visit it's at the new boundary or absent.

## v0.17.2 (2026-04-18)

### Message edit
- **Edit your own messages** — right-click an own text message → "✏️ Edit" to open an amber editing bar above the composer pre-filled with the original text. Change the text and hit Enter to update; Esc or the ✕ button cancels. Switching conversations mid-edit cancels safely.
- **"(edited)" marker** — edited messages show `(edited)` prefixed to the time label in the chat, matching Nextcloud Talk web.
- Server enforces the edit window (~6 hours for regular users) and permission rules; failures surface as a toast with the server's error message.
- Edit is not offered on messages with file attachments — server doesn't permit it.

## v0.17.1 (2026-04-18)

### Critical fixes
- **Emoji recents persist across restarts** — `EmojiData::initialize()` now runs after the app/organization names are set, so `QSettings` reads from the correct storage.
- **Mention popup no longer leaks across conversations** — the callback now checks the active token at response time, and switching conversations hides both autocomplete popups and clears pending state.
- **Autoreplace cursor position** — after `:) ` → 🙂, the cursor now sits past the trailing space. Previously the next typed char landed between the emoji and the space.
- **Nextcloud subpath installations supported for mentions** — `fetchMentions` now preserves the URL prefix (e.g. `/nextcloud/...`) instead of stripping it.
- **Paste-image save failure reported** — if Qt can't write the temp file on paste, the user sees a dialog instead of an error when they try to send.

### Important fixes
- **Async callbacks can't use-after-free** — `fetchFileImage` and `fetchMentions` take a `QObject *context` parameter; the connection is severed automatically if the caller widget dies mid-flight. Failures also log HTTP status for debugging.
- **OCS envelope status checked** — `fetchMentions` honors the `ocs.meta.statuscode` so server-side errors (403/404/429 wrapped in HTTP 200) no longer appear as an empty candidate list.
- **Profile avatar failures logged** — the top-right avatar's fetch errors are now visible in the debug log (canary for auth/session problems).
- **`MentionCandidate::Source` is now an enum** — scoped enum with `parseSource()`; prevents future silent classification bugs.

### Polish
- `EmojiRun::widthPx` dead field removed; struct docblock refreshed.
- `ImageViewerDialog` tracks the filename in a dedicated member instead of round-tripping through the title bar's text (fixes rapid-double Ctrl+C visual glitch).
- Phantom 4-pixel home-click strip at the top of the sidebar removed; the 🏠 button is the real entry point.
- Welcome screen shows "Release notes unavailable" instead of an empty panel when the `CHANGELOG.md` resource is missing.
- `fetchFileImage` takes a `maxDim` parameter — the image viewer now sizes requests to the actual screen instead of always asking for 4096×4096.
- `EmojiPickerWidget` emits a dedicated `cancelled()` signal on Esc instead of an empty-string sentinel via `emojiSelected`.

### Refactors (no behavior change)
- `isEmojiCluster` consolidated into `EmojiData` (previously duplicated across `LayoutEngine` and `EmojiTextRenderer`).
- Basic auth extracted to `ApiClient::applyBasicAuth` helper; five callers dedup'd.
- Shortcode walk-back and autocomplete popup stylesheet deduplicated in the composer.
- `ChatPainter::paintMessageEmojis` tightened (dead variable removed, font metrics hoisted out of the loop, line-search extracted as a helper).

## v0.17.0 (2026-04-18)

### Mentions
- **Compose @mentions** — typing `@` in the chat composer opens an autocomplete popup of conversation participants (with avatars) plus `@all`. Picking a row inserts `@userid` (quoted for ids with spaces) into the message; the server then notifies the mentioned user and renders the pill on all clients. Typing `foo@bar.com` does not trigger — email fragments are ignored.

## v0.16.6 (2026-04-18)

### Polish
- **Twemoji in sidebar + thread previews** — the conversation list's last-message preview now renders Twemoji instead of the system emoji font, matching the chat view.
- **Image viewer Ctrl+C / Ctrl+S** — copy the displayed image to the clipboard or save it to disk. A new ⋯ menu button in the viewer's title strip exposes both for mouse users.

## v0.16.5 (2026-04-17)

### Emoji
- **Twemoji everywhere** — chat messages, reactions, composer all render via bundled Twemoji (Nextcloud Talk style). Consistent across Windows versions.
- **Autoreplace** — `:)`, `:D`, `<3`, `:P`, `;)`, etc. → emoji on space. Also `:shortcode:` (Slack-style): `:smile:`, `:rocket:`, `:heart:`, …
- **Emoji picker** — new 😀 button in the composer opens a Telegram-style picker: 9 category tabs, search, recents (24), skin-tone variants via long-press/right-click.
- **Shortcode autocomplete** — typing `:smi` shows a popup of matches; Up/Down/Enter/Tab to pick, Esc to dismiss.

### Image viewer
- New standalone image viewer: **Esc closes**, single-instance (no more stacked viewers on repeated clicks), click-drag to **pan**, Ctrl+wheel to **zoom**, `+`/`-` keys, `0` fits, `1` is 100%, double-click toggles. Opens as its own top-level window with its own taskbar entry. Remembers last geometry.

### Welcome screen
- **What's New panel** — scrollable changelog (this file) rendered as Markdown beside the existing server status card. Collapses to single column below 1100 px.
- **Home navigation** — 🏠 button in the sidebar (next to search) returns to the welcome screen from any open chat.

## v0.16.4 (2026-04-17)

### Bugfixes
- **Multi-message selection** — dragging across messages now enters selection mode without needing to hold Ctrl (regression from v0.15.x char-level text selection work). Drag on body text still does character-level text selection; drag elsewhere (avatar/timestamp/padding) selects whole messages.
- **Infinite scroll history** — scrolling to the top of loaded messages now triggers loading of older history from the server. Viewport position is preserved when older messages are prepended.
- **Installer GPU plugins** — release installer now ships the GStreamer support libraries needed by the `d3d11` and `nvcodec` plugins: `libgstcodecs`, `libgstcodecparsers`, `libgstcuda`, `libgstd3d12`, `libgstdxva`, `libgstgl`. Fixes red "d3d11" and "nvcodec" pills on fresh installs without MSYS2.
- BUILD.md updated to document the full DLL dependency set for the GPU plugins.

## v0.16.3 (2026-04-04)

### Screen sharing — bidirectional
- TalQ→browser screen sharing now works (defer sendoffer until ICE connected)
- Screen/window picker dialog with Screens and Windows tabs
- Monitor selection across multiple displays
- Window capture via d3d11screencapturesrc window-handle
- Proper unshareScreen cleanup (send to self for HPB publisher teardown)
- Camera/share/blur buttons hidden until call reaches Active state

### Bugfixes
- Fixed sendoffer timing — publisher must exist in Janus before notifying peers
- Fixed re-share: send unshareScreen to own session to trigger HPB cleanup
- Single sendoffer on ICE connected (no over-offering/flickering)
- Video fullscreen exits cleanly on call end

## v0.16.0 (2026-04-04)

### Screen sharing
- Receive screen shares from browser users (full MCU signaling support)
- Share button in call dialog (monitor icon)
- Dialog maximizes when viewing screen share, restores when stopped
- Remote camera hidden during screen share (no fighting frames)
- Handles unshareScreen message — clean teardown when remote stops sharing
- dx9screencapsrc capture source (d3d11 fallback for discrete GPU)
- ScreenSharePipeline with roomType "screen" signaling
- SignalingClient extended: roomType on all methods, sendMinimalMessage, sendBroadcastMessage

### Hardware acceleration
- NVIDIA NVDEC VP8 hardware decoding (GPU) with Intel DXVA fallback
- BGRx direct frame path — no CPU YUV→RGB conversion
- Pre-scale large frames in VideoWidget to reduce QPainter load
- GStreamer plugin status pills on welcome page (green=loaded, red=missing)
- GPU acceleration status on welcome page
- Codec + decoder pills in call dialog (VP8/NVDEC/DXVA/Software)

### Video fullscreen
- Double-click any video widget to go fullscreen
- Esc to return to normal view

### Background blur
- Button opens Windows Camera Settings for Studio Effects (NPU hardware)

## v0.15.8 (2026-04-04)

### Data channel media state
- Send audioOn/Off, videoOn/Off on publisher "status" data channel (matches browser Talk protocol)
- Receive media state from subscriber data channels (browser mute/unmute reflected in TalQ)
- Speaking detection with 500ms grace timer — sends speaking/stoppedSpeaking to peers

### In-bubble text selection (Telegram-style)
- Click and drag on message text selects characters with teal highlight
- Drag across messages extends selection seamlessly
- Double-click selects a word; continue dragging extends word-by-word
- Ctrl+C copies selected text; right-click shows Copy menu
- Escape clears selection; links and reactions still clickable

### Screen sharing
- New ScreenSharePipeline: d3d11screencapturesrc (primary monitor, 1080p cap, 30fps, VP8 2Mbps)
- Separate webrtcbin with roomType "screen" (matches browser protocol)
- Share button in call dialog (monitor icon, teal when active)
- Receive screen shares from browser users
- SignalingClient extended with roomType parameter on all methods

## v0.15.6 (2026-04-03)

### Camera architecture — funnel + valve (replaceTrack equivalent)
Based on studying Nextcloud Talk browser source (spreed repo BlackVideoEnforcer):
- **Funnel element** merges dummy and camera sources into shared encoder chain
- Both sources permanently linked — **zero unlinking, zero relinking, zero SRTP errors**
- Camera toggle = two valve property flips (instant, no element recreation)
- **videoconvert + videoscale** between funnel and encoder handles resolution changes
- Camera auto-negotiates resolution (no fixed 720p cap)
- Camera auto-enabled when call reaches Active state (no UI blocking at startup)
- Camera source paused on disable (saves CPU), resumed on enable
- Preview signal disconnected on camera off (prevents frozen frame)
- Remote video reconnects on remote unmute

### Stability
- SRTP seqnum continuity for dummy→camera transition
- `gst_element_link_filtered` invisible capsfilter bug fixed
- Pipeline recovery to PLAYING after transient errors
- cameraChanged signal emitted after disable completes

## v0.15.4 (2026-04-02)

### Outbound video FIXED — camera video now reaches remote
- **Root cause:** v0.15.3 stopped the dummy video source immediately after pipeline start, leaving webrtcbin's video transport dead — its internal rtpbin never saw a video frame, so camera frames linked later were never forwarded to the DTLS/SRTP transport.
- **Fix:** Dummy now runs continuously (16x16 black @ 1fps, negligible bandwidth) keeping the video transport warm. When camera enables, dummy is stopped, `gst_bin_remove`d from the pipeline, and camera links to the same pad with the same SSRC. Seamless swap, no renegotiation needed.
- Confirmed working: TalQ → browser (Talk web app) video call with bidirectional video.

## v0.15.3 (2026-04-02)

### Camera toggle — fully working
- **Direct pad swap** — camera on/off swaps SSRC filters on the same webrtcbin pad. No input-selector, no renegotiation, no element recreation. Instant switch.
- **mfvideosrc fixed** — Media Foundation camera source now loads (added libgstd3d11/libgstd3dshader DLLs). Shared-mode access = no device contention on toggle.
- **Camera capped at 720p** — prevents memory explosion from raw 1080p frames
- **Leaky queues** — max 3 buffers, drops old frames when encoder can't keep up
- **Valve blocks encoding** when camera is off (saves CPU)
- **Camera source paused** on disable (releases device gracefully)
- **Preview hides** on camera off
- **Dialog stays large** when remote stops video but local camera is still on

### Stability
- **2nd-call crash fixed** — QPointer for video provider pointers in CallDialog (use-after-free)
- **Synchronous pipeline cleanup** — all three pipelines (Publish, Subscribe, Peer) use synchronous GST_STATE_NULL instead of detached threads
- **PLI timer thread fix** — QTimer created on Qt thread (was GStreamer thread → crash)
- **SRTP error filtering** — transient transport errors during renegotiation no longer kill camera
- **Pipeline recovery** — audio survives camera errors

## v0.15.1 (2026-04-01)

### Audio fix — v0.15.0 review regression
- **Synchronous pad linking** — v0.15.0 marshalled `onPadAdded` to the Qt thread, creating a race where RTP data arrived before the audio chain was linked. GStreamer returned `NOT_LINKED`, killing all audio. Reverted to synchronous linking on the GStreamer thread.

### Camera toggle fix
- **Dummy video removal** — `enableCamera()` now removes the dummy 16x16 black video track before adding the real camera, preventing a second conflicting video transceiver
- **Transceiver reuse** — camera on/off reuses the same webrtcbin pad instead of creating new ones, avoiding accumulating `m=video 0` lines in the SDP

### Subscriber improvements
- **Re-offer reuse** — MCU re-offers reuse the existing subscriber pipeline (preserves ICE/DTLS connection) instead of tearing down and rebuilding
- **SID tracking** — subscriber signal connections use hash lookup for MCU session IDs, enabling seamless re-offer support
- Removed dead `forceReconnectPublisher()` and unnecessary subscriber re-request on publisher renegotiation

## v0.15.0 (2026-03-31)

### Code Review — 59 issues fixed across 6 review rounds

#### Security
- **HTML injection** — server message content now HTML-escaped before rendering (prevents pixel tracking/IP disclosure)
- **TURN credentials** masked in debug logs
- **Signaling ticket** redacted from log output
- **WebDAV path** sanitization hardened (`?`, `#`, `%`, `..` stripped)
- **HTTPS warning** shown when user enters HTTP server URL
- **File size guard** — uploads over 100MB rejected before loading into memory

#### Crash/Stability
- **QPointer guards** on all 13 GStreamer→Qt callback marshalling sites (use-after-free race)
- **QTimer thread affinity** — onPadAdded marshalled to Qt thread (SubscribePipeline, PeerPipeline)
- **Null pipeline guard** in disableCamera
- **SDP null guard** — set-local-description protected against concurrent cleanup
- **Video provider disconnect** — old provider disconnected before connecting new one
- **invokeMethod target** — onPreviewSample uses QPointer-guarded target
- **Integer overflow** guard in YUV buffer size calculation

#### Protocol Compliance
- **DELETE /call** sends `{all: true}` as JSON body (was query param — hangup was broken for remote)
- **Subscriber re-offer** — stale subscriber evicted and recreated on new offer
- **ICE server race** — offers queued until STUN/TURN servers are fetched

#### Performance
- **Async cache init** — SQLite init no longer blocks UI at startup
- **Async lastCommonRead** — conversation switch no longer blocks on SQLite query
- **Cache saves only new messages** (was saving all 200 on every poll)
- **DebugMonitor** uses QStringList instead of O(n) string trimming

#### Robustness
- **CallSignaling poll delay** — 100ms minimum between polls (was busy-loop)
- **ICE failure recovery** — hangUp on ICE failed in MCU mode
- **acceptCall state guard** — roomJoined callback checks state before proceeding
- **PushClient stop race** — m_stopped flag prevents reconnect after logout
- **Conversation generation counter** — stale async callbacks bail out
- **Participant map pruning** — SignalingClient removes entries when participants leave
- **Camera SSRC capsfilter** cleaned up on disableCamera
- **Subscriber cleanup** — m_pliTimer, m_videoAppsink, m_videoDepay properly nulled
- **Duplicate chain guard** — onPadAdded won't create duplicate audio/video chains
- **trimOldMessages** — now trims oldest (was incorrectly trimming newest)

#### UX Polish
- **Branded splash screen** — dark theme, logo, app name, version, 1.5s minimum display
- **Call dialog centered** on primary screen
- **Mic icon** — text labels "Mic"/"Muted" (was emoji that looked like a light)
- **Squeezed sidebar** — search and settings hidden, avatar only in narrow mode
- **Avatar click** opens settings in all modes
- **Mic level indicator** — teal bar showing live audio signal during calls

#### Other
- **Dead QML provider code** removed (273 lines)
- **Remote video** hides and dialog shrinks when remote stops camera
- **Reaction counts** fixed (array length, not toInt)

## v0.14.7 (2026-03-31)

### Audio calls FIXED — bidirectional through MCU
Root cause: three protocol compliance issues found by comparing TalQ's SDP with browser WebRTC-internals dump:
- **Opus codec format** — GStreamer generated `OPUS/48000` (missing channel count), Janus requires `opus/48000/2`
- **Codec declaration** — signaling offer must include `audiocodec: "opus"` in message data for Janus room creation
- **SSRC consistency** — GStreamer's webrtcbin internal rtpbin rewrites SSRC on the wire; fixed by forcing matching SSRC on both payloader and capsfilter before webrtcbin
- **Data channel** — Janus publisher requires an `m=application` section (data channel) in the SDP; added `status` channel matching browser behavior
- **SSRC lines kept** — `a=ssrc` lines must be present in SDP (not stripped); Janus validates incoming RTP against them

### Video SSRC fix
- Same capsfilter SSRC approach applied to dummy video and camera video pipelines
- Camera SSRC forced via capsfilter between payloader and webrtcbin

### Other improvements
- File caption sent inline via `talkMetaData.caption` (not as separate message)
- Auto-refresh polling for incoming call detection
- ICE candidate queuing (all 3 pipeline types)
- Async pipeline teardown (no more UI freeze on hangup)

## v0.14.5 (2026-03-30)

### Installer fix — calls were unavailable on clean machines
- **GStreamer transitive DLLs** — installer was missing 23 DLLs that GStreamer plugins depend on (libnice, libopus, libsrtp2, OpenSSL, GnuTLS chain). Worked at home because MSYS2 was in PATH; failed on clean installs.
- **build-release.sh** — now copies all transitive deps and pulls GStreamer plugins directly from MSYS2 (no longer depends on a prior debug build)
- **deploy-dev.sh** — same DLL additions for dev build consistency
- **$USER fallback** — uses $USERNAME when $USER is unset (Windows bash)

### Call fixes (in progress)
- **Auto-refresh for call detection** — `startAutoRefresh()` was never called; incoming calls relied 100% on push notifications with no polling fallback
- **ICE candidate queuing** — candidates arriving before remote SDP is set are now queued and flushed after setRemoteOffer/setRemoteAnswer (all 3 pipeline types)
- **Audio pad link diagnostics** — audio receive chain now checks gst_pad_link return value (was silently failing)
- **Hangup freeze fix** — moved gst_element_set_state(GST_STATE_NULL) to background thread (was blocking UI for 20+ seconds during WebRTC teardown)
- **SSRC sync** — publisher payloader SSRC now synced to SDP offer value (Janus drops packets with mismatched SSRC)

### File sharing
- **Caption with file** — captions now sent via talkMetaData.caption in the share API (was sent as a separate message)

## v0.14.4 (2026-03-29)

### Fixes
- **File upload through junctions** — Qt6 blocks NTFS junction traversal; now resolves junctions to real paths before opening files
- **Per-user install** — installs to AppData\Local\Programs (no admin required)
- **Temp-copy fallback** — if direct file open fails, copies to temp before uploading
- **Error dialog** — file upload errors now shown to user instead of silently failing

## v0.14.3 (2026-03-29)

### Fixes
- **Online status live updates** — header status refreshes when user statuses change (was stale after select)
- **File size in attachments** — file pills show size (KB/MB) below filename
- **Sidebar last message** — preview updates instantly when new messages arrive
- **Notification click restore** — uses SetForegroundWindow on Windows to force-bring to front

## v0.14.2 (2026-03-29)

### Notifications
- **Custom notification popup** — Telegram-style dark rounded popup at bottom-right of screen
- **Click notification opens conversation** — restores window and switches to the chat
- **Cross-chat notifications** — shows even when app is focused (different conversation)
- **Notification text fix** — was showing oldest message instead of newest (model index bug)

### File Upload
- **Upload progress bar** — filename + percentage + teal progress line above composer
- **Caption via composer** — type caption in main input, no separate field
- **Enter sends file** — pending file sent on Enter key (with caption from composer)
- **Simplified pending bar** — file preview + name + cancel only, send via composer
- **Scroll to bottom** — chat scrolls down after file upload completes

### Other Fixes
- **123NET branding** — brand logo + TalQ sub-logo on login and welcome screens
- **Instant read status** — push events trigger message refresh
- **Chat scrollbar** — thin scrollbar thumb on right edge
- **Reaction counts** — fixed showing 0 (array length, not toInt)
- **Placeholder consistency** — unified to "Message..." across all code paths
- **Non-branded login** — empty server URL (was hardcoded to 123NET)
- **Dead signal removed** — unused `popupRequested` from NotificationManager

## v0.14.0 (2026-03-29)

### Multi-Message Selection (Telegram-style)
- **Drag-to-select** — click and drag to sweep-select messages
- **Selection mode** — right-click → "Select" or Ctrl+Click to enter, Esc to exit
- **Visual feedback** — teal row highlight + circular checkboxes on the right
- **Action bar** — replaces composer with Forward, Copy, Delete, Cancel buttons
- **Copy** — formats as `[Author, HH:MM]\nMessage` for each selected message
- **Forward** — conversation picker dialog, sends messages as text to target conversation
- **Delete** — bulk delete with confirmation (only available when all selected are own)
- **Ctrl+C** shortcut copies selected messages when in selection mode

### Chat Layout Redesign
- **Unified left-aligned layout** — all messages (own + others) left-aligned with avatar column
- **Own message avatars** — shown for non-grouped messages, same as other users
- **Author name above bubble** — not inside the bubble, with proper full-width display
- **Bubble backgrounds** — own (teal), others (subtle transparent gray), with proper internal padding
- **Timestamp inside bubble** — right-aligned, with read status icon for own messages
- **Proper spacing** — consistent gaps between messages, no bubble overlap

### Fixes
- **Instant read status** — push events trigger message refresh for near-instant read indicators
- **Chat scrollbar** — thin scrollbar thumb on right edge
- **Reaction counts** — fixed showing 0 (was using toInt on array, now uses array length)
- **Drag-to-scroll removed** — scroll via mouse wheel and scrollbar only
- **HTML stripping** — uses QTextDocument::toPlainText() instead of fragile regex
- **Composer focus** — reply button focuses the text input via focusProxy

## v0.13.1 (2026-03-29)

### Bidirectional Video Calls
- **Remote video display** — I420→QImage conversion via QPainter in CallDialog VideoWidget
- **Camera sends VP8** — matches Janus MCU (was H264), auto-negotiates caps (no hardcoded format/resolution/framerate)
- **Camera preview** — local VideoFrameProvider with 120x90 overlay at bottom-right of remote video
- **Dialog auto-resize** — expands to 400x500 when remote enables camera, shrinks to 300x340 when disabled
- **Remote avatar** — replaces video area when remote camera is muted
- **Dummy video track** — 16x16 black VP8 frame for audio-only calls (Janus MCU requires video from all publishers)
- **Skip dummy frames** — only show remote video when frames >32px

### Remote Media State Tracking
- **Mute/unmute signaling** — parse incoming mute/unmute messages, emit remoteMuteChanged signal
- **Mic muted indicator** — show muted icon on peer name when remote mic is muted
- **remoteVideoMuted/remoteAudioMuted** properties tracked on CallManager

### Call Dialog UX
- **Circular avatar** of remote party in header
- **circleButtonStyle()** helper for consistent button styling
- **Publisher ICE status** doesn't overwrite "Connected" when Active

### Code Review Fixes
- Ring timeout bypasses userActionReady (was stalling call forever)
- SubscribePipeline::onNewVideoSample uses QPointer guard (was use-after-free)
- leaveCallOnServer checks m_joinedCall (was double-DELETE on decline)
- STUN URL prefix replacement uses mid() (was corrupting mid-string matches)
- Video provider flags reset on provider change (was freezing after camera toggle)

### Code Simplification
- Extracted helpers: circleButtonStyle, indexOfToken, makeCandidateJson, callFlags, onAudioLevelUpdated
- Removed dead m_jpegDec code path and unused m_busWatchId
- Net -67 lines

## v0.13.0 (2026-03-28)

### Audio Calls — Fully Working with MCU/HPB
- **CallDialog** — QWidget-based call window with status breadcrumbs, mic/hangup/camera buttons
- **OPUS codec fix** — was MULTIOPUS, Janus rejected it
- **Accept/Decline** — setUserActionReady wiring for incoming call popup
- **Hangup** — DELETE /call with all=true to end call for both parties
- **Initial media state broadcast** — sent when remote peer joins (not just on ICE)
- **Stale call detection** — first conversation load seeds call state silently
- **Re-request subscriber stream** — when remote enables video mid-call

### Logging
- **TalqLog** — build-type-controlled logging system with file-based debug output
- Debug logs written to file for call troubleshooting

### Fixes
- Fixed crash on conversation select during active call
- Fixed call 404 errors from participant polling
- Fixed Janus codec config and participant polling that was breaking calls

## v0.12.2 (2026-03-28)

### UI Polish
- **Reply bar** — teal accent bar above composer shows "Author: preview..." with X to cancel
- **Context menu** — Telegram-style dark popup with emoji row, Download, Open in NC, Copy, Reply, Pin, Copy link, Thread (groups only), Delete (own + confirmation)
- **Emoji toggle** — clicking same emoji again removes it (POST 409 → DELETE)
- **Hover buttons fixed** — hit test at press time (hover state was clearing during click)
- **Call buttons** — subtle transparent icons with hover highlight + tooltips
- **Note to self** — bookmark icon in gray circle
- **Sidebar rows** — 62px compact height
- **Composer** — rounded pill input, dark background, larger buttons with hover
- **Welcome screen** — larger server card (420px), 14px fonts
- **Smooth avatars** — SmoothPixmapTransform on all painters
- **Splitter divider** — 1px line between sidebar and chat

### Bug Fixes
- **Code review fixes** — QDateTime include, screen() null guard, preview aspect clear, static regex
- **NC/Talk versions** — update on serverInfoChanged (was empty on first load)
- **Profile avatar** — reload on userInfoChanged
- **Thread action** — hidden in 1:1 chats

## v0.12.1 (2026-03-28)

### Fixes
- **Push + signaling not starting on session restore** — services only started on fresh login, not when restoring a saved session. Now starts on both paths.
- **Welcome screen status labels** — signaling and push status now update live when services connect/disconnect (was showing stale "disconnected" state).
- **Image click → preview viewer** — clicking image previews now opens a full-size viewer dialog.
- **Reaction clicks** — clicking reaction pills now toggles the reaction.

## v0.12.0 (2026-03-28)

### Full QWidget Conversion — QML Engine Eliminated
- **86% memory reduction** — 83MB private memory (was 600MB+ with QML)
- **Qt Quick, Qt QML, Qt QuickControls2 dependencies removed** — build: 38 targets (was 86)
- **QMainWindow** replaces QML ApplicationWindow — splitter layout, tray icon, shortcuts, geometry save/restore
- **ComposerWidget** — QTextEdit-based message input with send/attach buttons, font zoom
- **LoginWidget** — QWidget login form with server URL input
- **SettingsDialog** — 4-tab QDialog: Audio/Video, Notifications, General, Account
- **SidebarPainter** — QPainter conversation list with avatars, badges, search, squeeze mode
- **HeaderPainter** — QPainter chat header with avatar, call buttons, typing indicator
- **ThreadsPainter** — QPainter topics sidebar
- **Welcome screen** — logo, server info card, push/signaling status
- **User profile header** — avatar + display name + settings gear in sidebar

### Fixes
- **Missing messages after restart** — refreshLatest merge logic rewrote: dedup by ID, sort, reset model
- **Read status broken** — refreshLatest wasn't reading X-Chat-Last-Common-Read header
- **Image preview sizing** — uses actual aspect ratio from loaded image, compact placeholder until loaded
- **Empty message gaps** — skip messages with no text/file/reply for all senders
- **Shared avatar cache** — sidebar passes cached avatar to header (no duplicate HTTP fetch)
- **Camera dropdown empty** — deploy winks + mediafoundation GStreamer plugins
- **Font zoom scoped** — Ctrl+/- only affects chat messages + composer, not sidebar/header/settings
- **Code review fixes** — sendAction slot, QMenu leak, djb2 unsigned overflow, consolidated QSettings

## v0.11.0 (2026-03-27)

### ChatPainter — QPainter-based Message Renderer
- **Complete rewrite** of message rendering: replaced QML ListView + MessageBubble delegates with a single `QQuickPaintedItem` that renders all messages via QPainter
- **50% memory reduction** — ~385MB vs ~700MB+ with the old QML delegate approach
- **Zero delegate overhead** — only visible messages are painted (viewport culling), no QML item tree
- **Dynamic bubble widths** — message bubbles shrink to fit content, max 75% of chat width
- **Rich text** via QTextDocument with HTML links, mentions, word wrap
- **Async image loading** — avatars and file previews fetched via API, cached in memory, placeholder shown while loading
- **Inline image previews** — image attachments shown with rounded corners, click for full-screen preview
- **File attachment pills** — non-image files shown as rounded rect with document icon + filename
- **Reactions row** — emoji + count pills rendered inline
- **Clickable links** — cursor changes on hover, left-click opens in browser
- **Right-click context menu** — emoji quick-react + Copy/Reply/Delete actions, positioned at cursor with edge clamping
- **Hover action bar** — react (others) + reply (all) buttons appear on message hover
- **Emoji quick-react bar** — lightweight popup opens beside the smiley icon
- **Read status** — green filled circle (read) / empty circle (delivered) with cached lastCommonRead
- **Dark/light mode** — PainterTheme mirrors Theme.qml, auto-updates on toggle
- **Scroll** — mouse wheel + drag-to-scroll via QML MouseArea overlay
- **Date separators** — pill-style day headers between messages
- **System messages** — centered italic text for calls, joins, etc.
- **Message grouping** — consecutive same-author messages within 5 minutes share avatar/name

### Bug Fixes
- **Read status broken since v0.9.x** — `refreshLatest()` wasn't reading `X-Chat-Last-Common-Read` header. Fixed + cached in SQLite per conversation for instant status on app restart.
- **Empty green bars** — own messages with no text/file no longer render as tiny bubbles

### Cleanup
- Removed QML ListView + MessageBubble delegate path entirely
- MessageBubble.qml removed from QML module (file kept for reference)
- 130 lines of legacy toggle code removed

## v0.9.5 (2026-03-26)

### Video Call Fixes
- **Video renegotiation (m=video 0)** — enabling camera mid-call now works. Root cause: `enableCamera()` created two transceivers (one orphaned → `m=video 0`). Fixed with single transceiver approach in both PeerPipeline and PublishPipeline.
- **STUN URL format** — applied `stun:` → `stun://` conversion to PublishPipeline and SubscribePipeline (was only in PeerPipeline). STUN was silently failing in MCU mode.
- **Audio device selection** — mic and speaker settings from Settings dialog were silently ignored. PublishPipeline never passed `audioDeviceId`; `autoaudiosink` doesn't propagate `device` property. Fixed with explicit `wasapi2sink` → `wasapisink` → `directsoundsink` fallback chain.
- **Incoming call detection race** — overlapping conversation refreshes could both miss the `hasCall` false→true transition. Fixed with persistent `m_callState` map that survives across refresh cycles.

### Test Harness
- Video renegotiation test phase: enables camera mid-call, validates SDP has active `m=video` line
- `videotestsrc` support in PeerPipeline and PublishPipeline for headless testing

## v0.9.4 (2026-03-26)

### Major: BottomToTop ListView (conversation switch freeze fix)
- **Root cause**: `positionViewAtEnd()` / `positionViewAtIndex()` forced Qt to instantiate ALL MessageBubble delegates simultaneously, causing 1-4GB memory explosion and UI freeze on every conversation switch
- **Fix**: Reversed message storage to newest-first + `ListView.BottomToTop` layout. The view naturally starts at the bottom (newest messages visible) — no scroll calls needed
- Messages now stored newest-first in the model (`m_messages[0]` = newest)
- New messages from poller prepend at index 0 (appear at bottom)
- History loads append at end (appear at top on scroll-up)
- `cacheBuffer: 200` — only creates delegates near the viewport

### Memory & Stability Fixes
- **Poller backoff**: HTTP 401/403/404 stops polling; 5xx retries with exponential backoff (2s→60s)
- **Poller lastKnown:0 guard**: never start polling with lastKnownMessageId=0 (was downloading entire conversation history — 2000+ messages)
- **Message trimming**: cap at 200 messages per conversation, oldest trimmed on overflow
- **m_messageIds in postAndReplace**: prevents duplicate messages from poller
- **refreshLatest after file share**: sent files now appear immediately
- **cancelAll() safe iteration**: copies list before aborting
- **Cross-thread image providers**: FilePreviewProvider and AvatarProvider now moveToThread to main thread before network calls (eliminates "Cannot create children for a parent that is in a different thread" warnings)
- **MessageBubble anchors fix**: reply background Rectangle moved outside ColumnLayout (was causing 4000+ re-layout cycles)

### Other Fixes
- PushClient WebSocket error handler added
- PushClient auth failure reconnects instead of permanent dead state
- m_hasMoreHistory reset in setThreadId/setHideThreadMessages
- ConversationItem required property notificationLevel (eliminates "model is not defined" errors)
- markAsRead uses proper method on conversation open

## v0.9.3 (2026-03-26)

### Fixes
- **Duplicate message prevention** — `m_messageIds` updated in `postAndReplace()` and `deleteMessage()`, preventing poller from re-adding sent/deleted messages
- **Refresh after file share** — conversation refreshes immediately after uploading a file
- Poller diagnostic logging added

## v0.9.2 (2026-03-26)

### Call Fixes
- **MCU ICE candidate parsing** — fixed double-nested candidate extraction from Janus MCU signaling (was likely causing intermittent call failures)
- **STUN URL format** — convert Nextcloud's `stun:host:port` to `stun://host:port` for GStreamer compatibility
- **Test audio mode** — PeerPipeline supports `TALQ_TEST_AUDIO` env var for headless testing (audiotestsrc + fakesink)

### Automated Call Testing
- **talq-call-test.exe** — headless two-user MCU call test harness
- Authenticates kalin + test-talq, joins call via HPB, creates WebRTC pipelines, verifies ICE connection through real STUN/TURN, validates 3s stability, tears down cleanly
- Catches SDP issues, ICE failures, and signaling bugs without GUI

### Build & Deploy
- **deploy-dev.sh** — added audiotestsrc to GStreamer plugin list, fixed MSYS2 runtime DLL handling
- **Unified build paths** — all docs use `/c/build/talq` consistently

## v0.9.1 (2026-03-25)

### Room Avatars
- **Group chat avatars** — conversations without a specific user (group chats, public rooms) now show the room avatar fetched via authenticated OCS API
- **TqAvatar token fallback** — avatar component falls back to `image://avatar/room/{token}` when no userId is available

### Build & Deploy
- **deploy-dev.sh** — new script handles Qt + GStreamer DLL deployment after build, resolves MinGW runtime ABI conflict automatically
- **Unified build paths** — all docs and scripts now use `/c/build/talq` and `/c/src/talk-desktop-qt` consistently

### Fixes
- Triple-pass scrollToBottom for first-load delegate sizing

## v0.9.0 (2026-03-25)

### Memory Leak & Scroll Fix
- **Orphan network callbacks** — rapid conversation switching stacked duplicate refreshLatest() replies. Fixed: `m_refreshReply` member, cancelled on switch.
- **loadHistory() cascade** — `onContentYChanged` re-triggered after each prepend, loading entire history into memory. Fixed: 500ms debounce + `userHasScrolled` guard.
- **Unbounded FilePreviewProvider cache** — every preview (~3MB each) cached forever. Fixed: 50MB LRU eviction cap.
- **refreshLatest() ordering** — partition missing messages into older (prepend) / newer (append)
- **Persistent m_messageIds** — QSet replaces per-poll O(n) rebuild
- **onLastCommonReadChanged** scoped to changed range (was emitting for ALL messages every 15s)
- **scrollToBottom()** coalesced via 16ms timer
- **AvatarProvider** memory cache capped at 200 entries

### Video Calls — Compatibility
- **NC Talk video compatibility** — media state broadcasting, auto-camera detection, dual call buttons (audio/video)
- **SVG icon system** — geometric thin-stroke icons replace emoji throughout the app
- **Call status breadcrumbs** — shows signaling progress during connection (joining → signaling → connecting → active)
- **GStreamer plugin check** — disable call buttons when required plugins are missing
- **mfvideosrc** — use Media Foundation camera source instead of ksvideosrc
- **PLI keyframe fix** — request on src pad (not sink), periodic every 5s
- **Add-transceiver fix** — prevents m=video 0 in renegotiation SDP (untested)
- **Audio fallback chain** — autoaudiosrc/autoaudiosink with device selection override
- **MCU video renegotiation** — re-request subscriber streams after video accepted

### Fixes
- Duplicate "Reply" in context menu removed
- imageViewer.open() crash → downloadFile()
- Scroll-up no longer blocked by aggressive auto-scroll

## v0.8.3 (2026-03-23)

### Camera & P2P
- **Local camera preview** — PIP overlay in CallWindow via tee + appsink in PublishPipeline
- **P2P call mode** — direct peer-to-peer WebRTC for 1:1 calls (PeerPipeline), MCU used for group calls
- **Camera renegotiation** — enabling camera mid-call triggers proper SDP renegotiation

### Fixes
- Refresh latest messages from server after cache load (stale cache issue)
- Signal disconnect and QPointer guard in pipeline callbacks

## v0.8.2 (2026-03-22)

### Warm Carbon Design System
- **Phase 1: Theme foundation** — comprehensive dark/light theme with warm teal accent, standardized spacing/radius/font tokens
- **Phase 2: Component library** — 5 reusable QML components: TqAvatar, TqIconButton, TqBadge, TqSwitch, TqComboBox
- **Refactored UI** — replaced duplicated code across views with Tq* components

### Fixes
- Scroll-to-bottom catches late delegate height changes
- Negative window position restore fixed
- Reply bubble min width applied to other-person messages (was only own)
- TqIconButton: icon→iconText (AbstractButton.icon is FINAL)

## v0.8.1 (2026-03-22)

### Settings Dialog
- **Full settings dialog** — 4-tab layout (Audio & Video, Notifications, General, Account) with dark mode styling
- **Device persistence** — mic, speaker, and camera selections saved via QSettings, restored on app restart (matches by name, falls back to device ID for disambiguation)
- **Notification settings** — enable/disable, style (in-app popup vs Windows toast), sound mode (TalQ chime / system / none), all persisted
- **General settings** — start with Windows (registry auto-start), start minimized to tray, close to tray toggle
- **Account tab** — avatar, display name, server URL, Nextcloud/Talk versions, logout button
- **Click avatar to open** — click your avatar or display name in the sidebar header to open settings

### Per-Conversation Mute
- **Right-click mute** — right-click any conversation in the sidebar to Mute/Unmute
- **Server-synced** — uses Nextcloud Talk API (`/api/v4/room/{token}/notify`), persists across devices
- **Visual indicator** — "Muted" label shown on muted conversations

## v0.8.0 (2026-03-22)

### Video Calls
- **Receive remote video** — VP8 and H.264 codec auto-detection, decoded via GStreamer appsink to Qt VideoOutput. Tested working via MCU (Janus).
- **Send camera** — ksvideosrc (JPEG capture + jpegdec for HD) → openh264enc → rtph264pay, 1080p/720p with auto-fallback, toggle mid-call
- **SCTP data channel support** — required for MCU SDP compatibility (Janus includes datachannel m-line)
- **CallWindow video layout** — remote video fills window, controls auto-hide after 3s, camera toggle button, duration overlay
- **Settings** — camera device selection, video quality preset (Full HD / HD), persisted via Qt.labs.settings

### Call Polish
- **TURN server support** — credentials parsed from signaling settings, URL-encoded, configured on webrtcbin
- **Device selection wired to pipelines** — selected mic/speaker actually used (wasapi2src/wasapi2sink device property)
- **Incoming call decline fix** — leave room instead of call API (no more 404), m_joinedCall tracking
- **Auto-decline race fix** — m_userActionReady flag gates accept/decline on popup Component.onCompleted

### Chat UX
- **Scroll-to-bottom fixed** — no more jumping during image load, history prepend doesn't disrupt position
- **Image previews reserve height** — 200px placeholder prevents layout shift during async load
- **Reply bubble width** — own reply bubbles expand to fit quoted text (min 260px when quoting)
- **In-app image viewer** — click any image to view full-size in dark window (Esc to close)
- **Context menu** — Download and Open in Nextcloud items for file/image messages

### Dependencies
- Qt6::Multimedia added (QVideoSink, QVideoFrame, VideoOutput)
- GStreamer plugins: vpx, openh264, videoconvertscale, winks (camera)

## v0.7.1 (2026-03-21)

### Message Cache Fix
- Fixed file attachments (images, documents) disappearing after switching conversations
- Cache now stores original server JSON for lossless round-trip (previously reconstructed a lossy subset, dropping messageParameters)
- Mentions, thread metadata, and all parameter-dependent content now survive cache reload
- Schema v2 migration auto-purges stale entries on first launch

### Chat Scroll Stability
- Fixed chat jumping to older history during actions (image upload, footer changes)
- Replaced `onContentHeightChanged` auto-scroll with targeted `onCountChanged` handler
- Auto-scroll no longer disabled by programmatic scrolling (uses `onDraggingChanged` instead of `onMovingChanged`)
- Auto-scroll re-enables when user scrolls back to bottom
- Scroll-to-bottom button now correctly shows/hides based on position
- Scrollbar fades in during scrolling, fades out after 400ms of inactivity

### Text Selection
- Message text is now selectable with click-drag (Ctrl+C to copy)
- ListView switched to `interactive: false` with WheelHandler for mouse wheel scrolling

### File Handling Improvements
- Ctrl+V now handles files copied from Explorer (not just screenshots)
- Image files get preview confirmation; other files show file icon with name
- All file sends (paste, file dialog, drag-drop) show confirmation bar with caption field
- Files are never sent directly without user confirmation

## v0.6.1 (2026-03-20)

### Thread/Topic Fixes
- Fixed false thread detection in 1:1 chats — regular replies no longer create phantom topics
- Thread detection now uses the API-provided `isThread`/`threadId`/`threadTitle` fields instead of heuristic parent scanning
- "All Messages" renamed to "General" — now shows only non-thread messages (like Telegram's #General)
- Back arrow added to topics column header for returning to full chat list
- "+ New Topic" button moved from header to bottom of topic list to prevent overlap
- Sidebar can now be expanded/collapsed via toggle even when topics are active
- Draggable resize handles on sidebar and topic list dividers

### Scroll Fix
- Fixed scroll-to-bottom not working when opening conversations
- Replaced timer-based scroll with callback-driven approach using `onContentHeightChanged`

## v0.6.0 (2026-03-19)

### Threads / Topics (Telegram-style)
- 3-column layout for group chats with threads: squeezed sidebar icons | topics list | messages
- Sidebar auto-squeezes to 56px icon-only mode with smooth animation
- Manual squeeze toggle chevron at sidebar bottom
- Topics list with colored dots, unread badges, selection highlight
- "All Messages" row for unfiltered conversation view
- Inline topic creation: "+" button → type name → Enter
- Topic-aware chat header with color dot + group subtitle
- Dynamic composer placeholder: "Reply in [topic name]..."
- Thread index persisted in SQLite across app restarts
- Server capability check (requires Talk v22+ threads feature)
- Topic color palette centralized in Theme.topicColor()

### Upload Progress
- File upload shows progress bar in footer (filename + percentage)
- Animated progress indicator during WebDAV upload

### Paste Confirmation
- Ctrl+V no longer auto-sends images — shows preview bar first
- Caption field for adding text description before sending
- Send button (or Enter) to confirm, Escape or X to cancel

### Scroll to Bottom
- Floating ↓ button appears when scrolled up in chat history
- Click to jump to newest messages, teal hover highlight

### Fixes
- Typing indicator no longer leaks across conversations (room-scoped)
- Memory safety: QPointer guard in API callbacks, disconnect in setCache
- hasTopics flag preserved across conversation list refresh
- ThreadItem selection state properly bound in delegate
- thread_index cleared on conversation/account clear
- MessageCache wait timeout removed (prevents dangling lambdas)
- Deduplicated avatar images in squeezed ConversationItem
- Stale callback guard in ThreadListModel.fetchThreads
- refreshAfterCreate timer guards against conversation switch

---

## v0.5.3 (2026-03-19)

### Clipboard Paste
- Ctrl+V to paste screenshots and images directly into chat
- Clipboard images saved as temp PNG and sent via existing file upload pipeline

### User Status
- Online status heartbeat — TalQ now sets own status to "online" every 2 minutes
- Fixed status dots disappearing: dual-source status (user_status API primary, room API fallback)

### Context Menu Polish
- Layered shadow for depth (Telegram-style)
- Faster enter/exit transitions (100ms/60ms)
- Stronger emoji row separator and hover states
- Reduced emoji count to 6 to prevent overflow
- Quadrant-aware positioning with edge clamping

### Fixes
- Window no longer grows on every restart (geometry save guard during restore)
- Context menu no longer appears far above the cursor (correct popup height estimate)

---

## v0.5.2 (2026-03-19)

### Image Previews
- Authenticated inline image previews via `image://preview/` provider
- Thumbnails fetched from NC with auth headers, memory-cached per fileId
- Click image to open in Nextcloud browser

### Fixes
- Image attachments were invisible (preview disabled, pill excluded images)
- File shares now include read permission for recipients
- Deleted messages removed instantly from list
- Typing indicator filtered by current room
- Version bumped correctly across all files
- Auto-refresh disabled (push-only for real-time updates)

---

## v0.5.1 (2026-03-19)

### File Uploads
- 📎 button in composer for file picker
- Drag-and-drop files onto chat to upload
- WebDAV PUT upload + share to conversation

### Branding
- 123NET TalQ dedicated build (`cmake -DTALQ_BRAND=123NET`)
- Hardcoded server URL, no URL field on login
- Dual logos on splash (TalQ + brand), brand logo on welcome screen
- Separate Inno Setup installer for branded builds
- Self-signed code signing: 123 NET CPT (PTY) LTD

### Fixes
- Typing indicator filtered by current room (was showing wrong chat)
- Deleted messages removed instantly from list
- Window minimum width lowered to 500px
- Missing QML modules in installer (Qt.labs.settings, QtQuick.Dialogs)
- windeployqt used for complete dependency gathering

---

## v0.5.0 (2026-03-19)

### Real-Time Communication
- Push notifications via Nextcloud Notify Push WebSocket (instant message delivery)
- Typing indicators via standalone signaling (HPB) WebSocket
- Push status LED on avatar: green (push), amber (polling), red (disconnected)
- Signaling status on welcome screen

### Notifications & System Tray
- System tray icon with context menu (Show, Sound mode, Notifications, Quit)
- Sound modes: TalQ chime (default, bypasses DND), system sound, or none
- Tray icon badge with unread count (dynamically painted)
- Cross-chat notifications with sound even when window is focused
- Minimize to tray on close (X hides, tray Quit exits)
- Conversation list auto-refresh (30s fallback for push)

### File Attachments
- Inline display of shared files in messages
- Image files shown with preview placeholder (authenticated provider TODO)
- Other files shown as styled pills with MIME-type icons (video, audio, PDF, etc.)
- Click to open/download in browser

### Read Markers
- Mark conversation as read on open (POST /chat/{token}/read)
- Instant sidebar badge clear (local + server)
- Auto mark-as-read on new polled messages

### Font Zoom
- Ctrl+= zoom in, Ctrl+- zoom out, Ctrl+0 reset (0.7x to 2.0x)
- Scale persisted between sessions
- All font sizes scale via Theme.fontScale

### UX Polish
- Welcome screen: server info card with GridLayout alignment, push/signaling status
- Context menu: bigger icons (18px), aligned columns, taller rows (36px)
- Emoji bar positions inline with react button
- Hover action buttons: 32px with 20px icons
- Stronger hover backgrounds on menu items
- Reply scroll fix (delayed 100ms for footer resize)

### Stability Fixes
- Memory leak fix: broken beginInsertRows + unauthenticated image retries (5.5GB → 260MB)
- Conversation switch freeze fix: SQLite cache blocking main thread
- MessageCache moved to worker thread (fully async saves, non-blocking)
- Lazy-loaded Popups in MessageBubble (eliminates 100+ window handles)
- In-place conversation list updates (no beginResetModel during refresh)
- Participants/active join for signaling (prevents "not invited" error)
- Thread-safe messagesLoaded signal (double-invoke for main thread delivery)
- PushClient WebSocket state guard in authenticate callback
- Popup Loaders deactivated on close (memory cleanup)
- Tray icon pixmap cached (no disk reload per refresh)

### Other
- Exe renamed from talk-qt to talq
- Build timestamp in window title for debug tracking
- Code review findings addressed (8 fixes)

---

## v0.4.0 (2026-03-19)

### Threads / Topics
- Thread navigation for group conversations — topic list overlay with colored dots, reply counts, last activity
- Thread-aware message loading and polling with `threadId` filter
- "Reply in thread" context menu action
- ThreadItem and ThreadListView QML components
- Back navigation from thread to topic list

### Notifications & System Tray
- System tray icon with context menu (Show, Sound mode, Notifications, Quit)
- Windows toast notifications for new messages
- Sound modes: TalQ chime (default, bypasses DND), system sound, or none
- Cross-chat notifications — sound plays even when window is focused in another chat
- Auto-refresh conversation list every 10s for near-real-time unread detection
- Tray tooltip shows total unread count

### UX Polish
- Server info card on welcome page (server URL, NC version, Talk version, signaling)
- Clickable reaction pills with hover states (replaces plain text)
- Hover action button Y-clamping (no more clipping outside bubbles)
- Refined empty states with icons and helpful copy
- Context menu actions wired: Delete (API), Pin (API), Copy link, Reply in thread

### Window Management
- Splash screen centered on primary display, stays small (380x420)
- Reliable window state save/restore including maximized (QGroundControl pattern)
- Debounced geometry saving with `chatMode` guard
- Window grows to saved size/position after login

### Fixes
- Duplicate message prevention — poller vs POST response race condition resolved
- User agent shows "TalQ" without version during login
- Exe renamed from `talk-qt` to `talq`

---

## v0.3.0 (2026-03-19)

### Features
- SQLite message cache for instant conversation loading
- Avatar provider with 3-tier cache (memory, disk, server) and circular crop
- Dark/light mode toggle with warm teal accent (#2ec4b6)
- Emoji reactions with context menu and quick-react bar
- Reply-to with preview bar in composer
- Optimistic message sending with retry on failure
- Mention resolution in message text
- DWM dark title bar on Windows
- Inno Setup installer

### UI
- Hybrid message layout: flat for others, teal bubble for own messages
- Page-based ChatView with header/footer
- Connection status LED in sidebar header
- Window position/size persistence

---

## v0.2.0 (2026-03-18)

### Features
- Read receipts (sent/read indicators from X-Chat-Last-Common-Read)
- Date separators (Today/Yesterday/date pills)
- Custom app icon (blue chat bubble with Q)
- Splash screen with loading animation

### UI
- Complete UI redesign with refined dark theme
- Smooth hover/selection animations
- Conversation list: selection bar, search focus glow, thin scrollbar
- Send button with press animation

---

## v0.1.0 (2026-03-18)

### Initial Release
- Login Flow v2 (browser-based Nextcloud auth)
- Conversation list with unread badges, favorites, search
- Chat messaging with message bubbles
- Long-polling for live message updates
- Session persistence (auto-login on restart)
- Cross-compiled from Linux with mingw-w64
