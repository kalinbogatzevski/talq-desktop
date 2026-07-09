# Changelog

## v0.58.1 "Blue Fiesta" (2026-07-09)

A quick follow-up to 0.58.0 with three call-handling fixes.

### Fixed
* **The minimized call window now stays fully on screen.** Shrinking a call to
  the little floating window could leave it half off the edge of the monitor —
  or in the gap between monitors on a multi-display setup. It now docks as a
  small tile fully within the current screen.
* **No more phantom re-ring right after you hang up.** Ending a call could make
  it look, for a second, like the same person was calling you straight back. The
  app now recognises that flicker as part of hanging up and ignores it — a
  genuine call-back a few seconds later still rings.
* **A call that arrives while you're already on a call is no longer silently
  dropped.** You now get a "called while you were on another call" notification,
  and — in a one-to-one conversation — the caller automatically gets a short
  "on another call right now, I'll get back to you" reply, so they know why you
  didn't pick up.

## v0.58.0 "Blue Fiesta" (2026-07-08)

First stable release since 0.56.1, rolling up the call-reliability work from
the 0.57.x series. The headline is that calls now connect dependably in the
situations that used to strand them.

### Fixed
* **Calls connect reliably in the situations that used to strand them.**
    * Calling someone signed in on **several devices at once** (a laptop and a
      phone, a second window, a VPN session) now follows the device that
      actually answers, fails over to another of their devices if one drops,
      treats a slightly late answer as an answer rather than a new call, and
      hangs up cleanly no matter which device they were on — instead of leaving
      you on "Connecting" or them on "Waiting for others to join."
    * Calling someone who is **already in a call, or who answers late**, now
      connects promptly instead of ringing out — the app actively finds who is
      in the call rather than waiting on an event that may never arrive.
* **Calls work across regions, and settle on the closest server.** A call
  between people on different regional servers establishes video and audio
  reliably, and each person now lands on the nearest server (chosen over
  several measurements, so a single noisy reading can't send you far away) —
  with the full server list discovered from your Nextcloud, so it works on
  every build.
* **Screen sharing is solid now, including re-sharing.**
    * Stopping a share and starting a new one comes up quickly on the hardware
      encoder at full resolution, instead of failing, showing a green picture,
      or needing a long wait.
    * The shared picture is sharper — text stays readable and the "interlaced /
      missing detail" look is gone.
    * A re-shared screen appears for the other side in a second or two, rather
      than after a long delay.
* **No phantom incoming call right after you hang up.**
* **Muting fully cuts your audio,** and the room stays put when you navigate
  between chats during a call.

### Notes
* Windows installers are code-signed. On a standalone PC, Microsoft Defender
  may still flag a brand-new build until its reputation builds up; a one-time
  per-machine exclusion clears it.

## v0.57.24 "July Morning" -- BETA (2026-07-07)

### Fixed
* **Calling someone who is signed in on several devices now connects
  reliably.** When the person you called was logged in on more than one
  device at the same time (say a laptop and a phone, or a second window),
  the call could latch onto a device that was not actually the one
  answering — so you sat on "Connecting" with nothing coming through, or
  they were left looking at "Waiting for others to join." The call now
  follows the device that actually joins with audio and video, and moves
  to another of their devices automatically if the first one drops.
* **One device dropping no longer ends a healthy call.** If the other
  person was on several devices and one of them briefly lost its
  connection, the call could wrongly wind down even though a working
  device of theirs was still in the call. It now keeps going as long as
  any of their devices is connected.
* **A late answer is treated as an answer, not a new call.** When you
  called someone whose devices took a few seconds longer to ring, their
  answer arriving just after the call gave up could show up as a brand-new
  incoming call. Answering within a short window now simply reconnects the
  call you were placing.
* **Hanging up reaches the right device.** The "call ended" signal is now
  sent to every device the other person has in the call, so the call
  clears promptly on their side no matter which one they were looking at.

## v0.57.23 "July Morning" -- BETA (2026-07-07)

### Fixed
* **Sharing a window again after stopping now reliably shows up for the other
  side.** A re-started screen share could occasionally get stuck never
  appearing for the viewer (the receiver connected but the first frame never
  arrived, and the recovery attempt was being suppressed). The viewer now
  rebuilds a stalled share promptly instead of waiting it out.
* **No more phantom incoming call right after you hang up.** Ending a call
  could briefly make it look like the other person was starting a new call,
  ringing you on the call you had just left. That momentary flicker is now
  ignored.
* **Steadier choice of the nearest server.** The app picks its signaling
  server by measuring which regional server is closest; a single noisy
  measurement could send you to a far-away region. It now averages several
  measurements and only moves off your current server when another is clearly
  closer — so you stay on the nearest one instead of hopping around.

## v0.57.22 "July Morning" -- BETA (2026-07-07)

### Fixed
* **More reliable call connections when several regional relay servers are
  available.** With the full regional server set configured, the client could
  silently drop the closest relay from its connection list, occasionally
  causing a call to fail to connect. The client now always keeps the nearest
  relays, so calls connect more dependably — especially across regions.

## v0.57.21 "July Morning" -- BETA (2026-07-07)

### Fixed
* **Muting your microphone now fully cuts your audio on every audio device.**
  On some setups the mute button changed the on-screen indicator but the
  microphone kept transmitting — peers could still hear you. Mute is now
  enforced locally at the audio source, so nothing audible leaves your
  machine.
* Hardened the call-vs-chat window independence from the previous release:
  fixed several edge cases (a reconnect blip mid-call, answering a call from
  a different conversation, a quick call-hang-up-then-redial) that could
  still disrupt a call or leave a conversation's live updates stalled.
* Fixed a potential crash during signaling connect/reconnect when a DNS
  lookup for a server was unusually slow.
* Update download integrity checks are more robust against a background
  update check landing mid-download.

### Changed
* The active-speaker highlight now uses a distinct amber frame, so it is no
  longer easy to confuse with the green border that marks a shared screen.
* The microphone level meter stays green while you speak (it briefly turned
  amber before) — the "who's talking" cue now lives only on the speaker
  frame, and the meter simply shows your level.
* A picture-in-picture call window now docks to the display it is already on
  instead of jumping to the primary monitor.

## v0.57.20 "July Morning" -- BETA (2026-07-03)

### Fixed
* **Browsing chats during a call no longer breaks the call.** Clicking other
  conversations (or re-opening the call's own conversation) while on a call
  could freeze both sides' video within seconds and drop the call to
  "Reconnecting" — the app was silently walking itself out of the call's
  room behind the scenes. Calls now stay fully independent of whatever you
  do in the main chat window. Live message hints for a conversation you
  browse *during* a call resume as soon as the call ends.

### Improved
* The active-speaker highlight is now amber instead of teal — it was too
  easy to confuse with the green screen-sharing border at a glance.

## v0.57.19 "July Morning" -- BETA (2026-07-03)

### Fixed
* Fixed a regression where a peer could stop appearing to properly join a
  call — a race in the signaling backend meant that switching between
  conversations quickly could leave a call session referencing the wrong
  conversation.
* Screen sharing announcements are now sent to everyone on the call, not
  only to peers already tracked as receiving your camera/mic — a peer
  could previously miss the notice that a screen share had started.
* The call window no longer moves or docks itself just because you
  navigated to a chat in the main window — the two windows are fully
  independent now. Previously, browsing conversations while on a call
  could unexpectedly yank the call window into a corner, and on a
  multi-monitor setup could even jump it to the wrong screen.

## v0.57.17 "July Morning" -- BETA (2026-07-03)

### Fixed
* The update downloader now verifies the downloaded installer is intact
  before running it, on every update channel. Previously a download that
  got silently corrupted or truncated partway through (a network hiccup,
  for example) could produce an installer that failed partway through
  setup instead of being caught and safely discarded.

## v0.57.16 "July Morning" -- BETA (2026-07-02)

### Improved
* More reliable call connection setup, especially over longer/slower network
  paths: the app no longer wastes time offering a class of connection
  candidate the server was always going to reject anyway.

## v0.57.15 "July Morning" -- BETA (2026-07-02)

### Improved
* Faster, more reliable connection setup: when supported by the server, the
  app can now automatically pick the best available connection point instead
  of always using a single fixed one. No effect on servers that don't support
  this yet — the app falls back to exactly its previous behaviour.

## v0.57.14 "July Morning" -- BETA (2026-07-02)

### Fixed
* **A screen share that failed to connect to a remote viewer no longer gets
  stuck permanently** ("Starting…" or a black tile forever). The app now
  automatically asks for a fresh connection a few times before giving up,
  instead of requiring the sharer to manually stop and re-share.
* Further server-side reliability work on screen sharing.

## v0.57.13 "July Morning" -- BETA (2026-07-02)

### Fixed
* **A peer's incoming screen share could fail to display if you were already
  sharing your own screen.** The call stage always kept showing your own
  share in that case instead of switching to theirs.
* **Screen-share video now uses the same compatibility-safe H.264 profile as
  camera video**, closing a gap where a strict receiver could fail to decode
  a shared screen (camera video was already covered).
* Server-side reliability work on the regional signaling infrastructure to
  make cross-region screen sharing hold up consistently.

## v0.57.12 "July Morning" -- BETA (2026-07-02)

### Added
* **Per-participant signal-quality indicator.** Each remote participant's
  call tile now shows a small signal-strength bars glyph, backed by real
  packet-loss and jitter measurements for that person's stream — a quick,
  glanceable read on whose connection is struggling, not just a generic
  "connected" state.

## v0.57.11 "July Morning" -- BETA (2026-07-02)

### Added
* **Server latency telemetry now updates live**, instead of only at connect
  time — the home-screen signaling status and the in-call ROUTING panel
  (press **T**) both refresh continuously while connected.
* **Per-topic unread badge.** Unread counts on topic tabs now show as a real
  badge pill, matching the conversation-list badges, instead of small inline
  text.
* **Picture-in-picture on minimize.** Minimizing (or dragging the title bar
  down onto the taskbar) during a live call now docks it to a small
  always-on-top corner window instead of vanishing it to the taskbar.
  Double-click to restore, or drag the dock itself to reposition it.
* **Group calls now show every concurrent screen share.** Previously only one
  participant's share was shown on stage at a time even if several people
  were sharing at once; everyone else's share silently dropped to a plain
  camera tile. All concurrent shares now get their own stage tile.

### Fixed
* **Camera errors on the very first enable of a call now get a short,
  bounded retry** before showing "Camera unavailable" — some hardware needs
  a moment to settle after a previous session releases it. A camera that's
  genuinely unavailable still reports quickly; this only smooths over a
  one-off transient on first connect.
* **Dragging the self-view tile across monitors with different display
  scaling** no longer glitches the tile's position mid-drag.
* Installers now explicitly bundle the D3D shader compiler runtime instead of
  relying on it being deployed implicitly, closing a possible source of a
  missing-runtime-component failure on some machines.

## v0.57.10 "July Morning" -- BETA (2026-07-01)

### Fixed
* **Automatic nearest-server selection now actually connects.** The 0.57.9 picker
  correctly found the closest server, but a URL-formatting bug sent it to a dead
  path so it silently fell back to the far one. It now connects to the nearest.

## v0.57.9 "July Morning" -- BETA (2026-07-01)

### Added
* **Calls connect through the nearest regional server automatically.** TalQ now
  measures which signaling server is closest and connects there, so your call
  takes the shortest, most stable path instead of a long international one — the
  main cause of mid-call disconnects on distant links. If that server stops
  responding, TalQ moves to the next-nearest on the next reconnect.
* **In-call telemetry shows server latency.** Press **T** during a call: the
  ROUTING panel lists the signaling and relay servers in use, each with its
  measured round-trip time.

### Fixed
* **The Settings window opens instantly.** It no longer flashes blank-white or
  stalls for a moment while the microphone and camera start up — the window
  paints first and the devices come alive behind it.

## v0.57.8 "July Morning" -- BETA (2026-07-01)

### Fixed
* **Calls now pick the nearest relay accurately.** The check that measures which
  relay server is closest now uses a fast, reliable probe. TalQ can once again
  confidently route your outgoing audio and video through the relay in your own
  region instead of falling back to keeping all of them. A participant on one
  continent and a relay on another no longer get paired — outbound media stays
  local, which keeps it smooth for everyone on the call.
* **Virtual background is more robust.** On builds where the background library and
  its runtime were out of step, switching on a background could take the app down;
  it now degrades gracefully instead.

### Added
* **Telemetry now shows the call's routing.** The in-call telemetry panel (press
  **T**) lists the signaling server and the relay server this call actually
  selected, so it's clear which region is handling your media.

## v0.57.7 "July Morning" -- BETA (2026-07-01)

### Fixed
* **The active-speaker frame no longer flickers on background noise.** It now needs
  clearer, sustained speech before highlighting someone, so a quiet room or low mic
  hiss won't keep the frame lit.

### Changed
* Steadier relay-server handling on slow or uneven connections — TalQ keeps all
  relays available rather than guessing a "nearest" from an unreliable measurement
  (a more accurate probe is coming).

## v0.57.6 "July Morning" -- BETA (2026-06-30)

### Fixed
* **Calls now relay through the nearest server.** When relay servers are offered in
  more than one region, TalQ measures which is closest and uses only that one.
  Previously a participant's outgoing audio/video could be routed through a relay on
  another continent — a perfect ping, but a long detour that made their stream choppy
  and their audio break up for everyone else, even on a fast connection. Outbound
  media now stays on the local relay.
* **Group calls reliably show every participant.** If you joined a call others were
  already in, you could miss seeing one of them until they toggled their camera.
  TalQ now keeps every in-call participant subscribed, so each tile shows its own
  live video.

### Changed
* **720p is now the recommended send quality** — it fits most connections smoothly.
  1080p is available as an **Ultra** option for strong upload connections.

## v0.57.5 "July Morning" -- BETA (2026-06-30)

### Fixed
* **Group calls now show everyone.** In a call with 3+ people you could end up
  seeing only the first person who joined — others stayed invisible until they
  turned their camera off and back on. TalQ now connects to every participant
  automatically.
* **Smoother incoming video on laptops with switchable graphics.** On machines
  with both Intel and NVIDIA graphics, incoming HD video could freeze at a couple
  of frames per second; TalQ now detects this and falls back to software decoding
  faster so the picture stays smooth.
* **Click a spotlighted speaker to return to the gallery.** Tapping a tile to make
  someone the main view had no way back to multi-view — click their video again.
* **Your own preview now fits the row** in group calls instead of floating, the
  wrong size, in the corner.

### Added
* **Host-overload protection.** If a call ever makes TalQ's memory balloon (e.g. a
  stalled decoder), it now automatically drops incoming video to a lighter quality
  to protect your machine, and restores full quality once it recovers.

## v0.57.4 "July Morning" -- BETA (2026-06-30)

### Added
* **Active-speaker frame.** A clear, gently-pulsing accent border now highlights
  whoever is talking in a call, so it's easy to see who has the floor at a glance.
  It's driven by each person's live audio (with a short hold so it doesn't flicker
  between words), so it works for everyone regardless of their app.

## v0.57.3 "July Morning" -- BETA (2026-06-30)

### Fixed
* **Setting a group picture now works.** Choosing a group picture silently failed
  for most photos because the server only accepts square images — TalQ now
  automatically crops your photo to a centered square, so any picture works. The
  Conversation Info dialog also shows the current picture when you open it, gives
  clear, visible feedback (and a progress bar) while it uploads, and the new
  picture appears immediately in the header and conversation list.
* **Hardware video encoding now recovers after a GPU or version change.** A machine
  that once fell back to software H264 encoding stayed on software permanently —
  even after a graphics-driver update, a GPU change, or an app upgrade that fixed
  the cause — which pinned otherwise-capable machines to CPU-heavy software encoding.
  The hardware encoder is now re-checked whenever the graphics setup or app version
  changes, so it picks the hardware encoder back up automatically.

## v0.57.1 "July Morning" -- BETA (2026-06-30)

### Under the hood
* **Hardened the release build.** The packaging pipeline now verifies the full
  runtime dependency closure of every shipped binary (so a missing library can
  never slip into a release), fails loudly if any required file is absent rather
  than shipping a partial bundle, and trims a few unused libraries from the
  installer. No app behaviour changes from 0.57.0 — same modernized foundation.

## v0.57.0 "July Morning" -- BETA (2026-06-29)

### Under the hood
* **Modernized the app's foundation.** TalQ is now built on an up-to-date
  compiler and refreshed runtime libraries (a new C++ toolchain and a newer Qt),
  bringing the desktop client in step with current components for better
  long-term maintainability and security. No change in features — this beta is
  about proving the new foundation is solid in real calls before it becomes the
  default.

## v0.56.1 "July Morning" -- STABLE (2026-06-29)

### Fixed
* **Sharing a monitor while the call window is on another display** now shows
  your shared screen on the call stage, instead of the "You're sharing this
  screen" placeholder. (The placeholder is only needed when the window is on the
  very monitor you're sharing — otherwise it's safe to show the live view.)
* Removed a stray **"PRE-RELEASE"** tag that was showing in the window title on
  this stable build.
* Restored the **codename description** in its hover tooltip.

## v0.56.0 "July Morning" -- STABLE (2026-06-29)

The "July Morning" stable. Named for the Bulgarian tradition of driving to the
Black Sea cliffs on the night of 30 June to greet the first sunrise of 1 July
together — a fresh start at first light. This release is about making **calls
feel clean and dependable**, gathering everything proven across the 0.53–0.55
betas into one stable.

### Calls
* **Echo cancellation now works with speakers.** If you use speakers instead of
  headphones, the people you talk to no longer hear their own voice echoed back.
  TalQ cancels against the exact audio your speakers actually play, so it holds
  even at high volume.
* **Calls stay up through trouble.** Signalling reconnects and resumes instead of
  dropping; a brief network blip no longer ends the call; a peer leaving no
  longer disturbs the rest.
* **Video adapts instead of failing.** A machine whose GPU video decoder is
  unusable automatically falls back to software decoding (now robust for any
  video, not just the simplest kind); a choppy link lowers quality rather than
  freezing.
* **Cameras and screen sharing are forgiving.** Plug in a camera mid-call and
  TalQ picks it up; rapid camera on/off no longer stalls; screen-share layout
  stays consistent, including your own share on the main stage.

### Conversations
* **Set a group conversation's picture** (as a moderator).
* **Unread topics stand out** in the topic bar at a glance.

### Quality
* **Reliable startup** on every machine, and a hardened install / auto-update path.

## v0.55.3 "July Morning" -- BETA (2026-06-29)

### Fixed / improved
* **Echo cancellation now works with speakers.** If you use speakers instead of
  headphones, the people you talk to no longer hear their own voice echoed back.
  TalQ now cancels the echo against the exact audio your speakers actually play
  (captured from the audio output itself), so it stays clean even when you turn
  the volume up.

## v0.55.2 "July Morning" -- BETA (2026-06-29)

**Codename "July Morning."** On the night of 30 June into 1 July, Bulgarians drive
east to the Black Sea coast, light fires on the cliffs, and stay awake to greet the
first sunrise of the month together — a ritual that grew out of the 1980s
counterculture as a quiet act of freedom and is now a beloved summer fixture. It
takes its name from Uriah Heep's 1971 song "July Morning," which caught on in
Bulgaria a decade late and became the unofficial anthem of the vigil; for years the
band's later singer John Lawton returned to the clifftop at Kamen Bryag, near
Kavarna, to sing it to the rising sun (a monument to him was unveiled there on
1 July 2022). The easternmost headland catches the light first — a fitting name for
a release that meets its own new beginning at first light.

### Added
* **Set a group conversation's picture.** Open a group's info and, as a moderator,
  choose "Change picture" to upload an image as the conversation's avatar.
* **Unread topics now stand out.** A topic with unread messages is highlighted in
  the topic bar (instead of a faint count) so it's easy to spot at a glance.

### Fixed / improved
* **Reliable startup.** Resolves a crash on launch that affected an earlier
  pre-release build of this line; the app now starts cleanly on every machine.
* **Software video decoding is now robust for any video.** Building on the automatic
  hardware→software fallback, TalQ now bundles a full software H.264 decoder, so a
  machine whose GPU decoder is unusable can decode every kind of incoming video, not
  just the simplest profile.
* **Groundwork for smoother video on bad links** (an automatic frame-rate step-down
  under congestion) ships in this build, switched off by default while it's
  validated in the field.

## v0.55.0 "July Morning" -- BETA (2026-06-26)

**Codename "July Morning"** — on the night of 30 June into 1 July, people gather on
Bulgaria's Black Sea coast (Kamen Bryag, Kavarna) to meet the first sunrise of July
together: a 1980s counterculture tradition of renewal and a fresh start, carried by
Uriah Heep's song of the same name. A fitting name for a fresh beta line — a clean
dawn after a reset.

A call-resilience beta, built from a real three-way field session: calls now
recover from far more on their own instead of freezing or going silent.

### Fixed

* **A brief network drop no longer risks dropping you from a call.** The session
  resumes faster after a blip (racing the server's short grace window), and if the
  session has to be rebuilt the call re-establishes your video and audio under the
  new session automatically — instead of leaving you connected but silently unseen
  and unheard by everyone else.
* **No more "Not allowed to request offer" retry storms.** When a peer reconnected
  under a new session, TalQ could chase their old, dead session indefinitely. It now
  drops the dead session cleanly and picks up the new one.
* **Faulty or unsupported hardware video decoding now falls back to software
  automatically.** On a machine whose GPU video decoder is unsupported or
  unreliable, the remote picture used to drop frames or freeze with no way out. TalQ
  now detects this and switches that machine to software decoding (and remembers it
  for next time). The same automatic fallback now also covers the sending side
  mid-call.
* **Choppy remote video recovers on its own.** A frozen or "moving-but-not-updating"
  remote feed now requests a fresh keyframe and recovers, and a peer whose video
  connects but never shows a frame is rebuilt instead of sitting on "Starting…". A
  larger receive buffer smooths jittery links.
* **Your tile stays a proper member of a group call during a screen share.**
  Previously you could be bumped into a small floating self-preview while someone
  shared; you now remain a normal member tile throughout.
* **Your own screen share shows full-size on the main stage in every call.** When you
  share the whole screen TalQ is on, a clean "You're sharing this screen" placeholder
  is shown instead of a hall-of-mirrors feedback loop (this replaces the earlier
  window-only limit).
* **Cameras are harder to wedge.** Rapidly toggling your camera on and off no longer
  parks the device. Plugging or unplugging a camera or microphone during a call is
  now noticed live, and a camera that drops out and comes back resumes on its own.
* **TalQ no longer restarts to update during — or right after — a call.** An update
  that downloaded while you were on a call could restart the app the moment the call
  ended, interrupting back-to-back calls. Updates now wait until you've been off a
  call for a few minutes before installing (and a new call resets the wait). Choosing
  "Update now" still installs immediately.

## v0.53.1 "Bafana Bafana" -- BETA (2026-06-26)

### Fixed

* **Sharing a whole screen that has TalQ on it no longer freezes the picture.** The
  full-size preview of your own share was being shown on the call and then captured
  again by the screen share — over and over, a "hall of mirrors" — leaving the other
  side on a frozen image. The full-size self-view now appears only when you share a
  single window (where there's no feedback); sharing a whole screen keeps just the
  small corner preview.
* **Re-sharing a screen is more reliable still.** Closed the last case where a
  quickly re-shared screen could leave the viewer stuck on "Starting…" — the viewer
  now keeps the connection details it needs even when it rebuilds the connection.
* **Window titles always show in the share picker.** Each window's name now appears
  under its thumbnail at all times, not only when you hover over it.

## v0.53.0 "Bafana Bafana" -- BETA (2026-06-25)

**Codename "Bafana Bafana"** (continued) — South Africa's national football team,
through to a World Cup knockout round for the first time in their history (24 June
2026). This beta opens the 0.53.x line on top of the 0.52.17 stable fixes.

### Added

* **See your own screen share full-size.** While you're sharing your screen, the
  call now shows your shared screen as the main view — the same way the people
  you're sharing with see it — instead of only a small corner preview. Your camera
  stays visible alongside it. If someone else starts sharing too, their share takes
  the main view and yours returns to a corner preview.
* **Full window titles in the share picker.** When picking a window to share, each
  window's full title is now shown under its thumbnail, instead of being cut off and
  readable only on hover.

## v0.52.17 "Bafana Bafana" -- STABLE (2026-06-25)

**Codename "Bafana Bafana"** — "the boys", the nickname of South Africa's national
football team. On 24 June 2026 they won 1–0 to finish their group and reach a World
Cup knockout round for the first time in their history — through to the last 32 in
their fourth finals, having never before made it out of the group. A nod home: the
team and the company that builds TalQ both come from South Africa.

### Fixed

* **Fixed another rare crash when ending a call.** A separate teardown path could
  still crash the app on hang-up while it released the incoming video connections —
  most likely after an unstable call. Ending a call now releases those connections
  safely.
* **Re-sharing a screen is more reliable still.** Stopping a screen share and
  starting a new one could leave the viewer stuck on "Starting remote screen share."
  The viewer now holds onto a share that is actively connecting instead of restarting
  it, and no longer drops the connection details it needs to come up — so a re-shared
  screen appears within a few seconds.
* **Opening Settings during a call no longer interrupts your camera.** With a
  background blur or image enabled, opening Settings mid-call could take over the
  camera and leave your video stuck off for the rest of the call. The live background
  preview now pauses during a call (your chosen background still applies to the call),
  so the call keeps the camera.

## v0.52.16 "Enyov Day" -- STABLE (2026-06-24)

**Codename "Enyov Day"** — Enyovden (Еньовден), the Bulgarian Midsummer, on
24 June (the day this release was cut). It marks the summer solstice — when the
sun reaches its peak and begins its long turn back toward winter — and the feast
of St John the Baptist. Above all it is the herbalists' day: healing herbs
gathered at dawn are believed to hold their greatest power, the legendary "77 and
a half" (77 for 77 ailments, and a half for the one known only to a few healers).
A fitting name for a release about healing what was broken.

### Fixed

* **Fixed a rare crash when ending a call.** Hanging up during a call could, in
  rare cases, crash the app while it tore down the connection. Teardown now
  releases everything safely.
* **Re-sharing a screen is now reliable.** Stopping a screen share and starting a
  new one — especially full-screen — could leave the viewer stuck on "Starting
  remote screen share," churning for many seconds before it settled (or not
  settling at all). The viewer now gives a reconnecting share the time it needs to
  come up instead of repeatedly restarting it, and fully releases the previous
  connection before opening the new one — so a re-shared screen appears within a
  few seconds, which also keeps the sharer's video bitrate from collapsing.

## v0.52.15 "Bafana Bafana" -- STABLE (2026-06-24)

### Fixed

* **A re-shared screen now appears for viewers.** When someone stopped a screen
  share and immediately started a new one, the viewer could get stuck on
  "Starting remote screen share" and never see the new share. The viewer now
  gives a just-started screen a moment to come up — and re-requests its first
  frame — instead of repeatedly restarting it, so the re-shared screen shows
  within a few seconds.

## v0.52.14 "Bafana Bafana" -- STABLE (2026-06-23)

### Added

* **An "Update now" button in Settings → Updates.** Check for a new version and
  install it on demand instead of waiting for the periodic background check. It
  still never restarts the app during a call — if you're on a call, the install
  waits until the call ends.

## v0.52.13 "Bafana Bafana" -- STABLE (2026-06-23)

### Fixed

* **The camera now recovers on its own if it freezes when started during a screen
  share.** On some hardware, turning the camera on while already sharing your
  screen could leave the local preview frozen until you manually switched the
  camera off and back on. The app now detects the stall and re-arms the camera
  automatically, so it comes back without any manual step.

## v0.52.12 "Bafana Bafana" -- STABLE (2026-06-23)

### Fixed

* **An update no longer restarts the app at the end of a call.** The auto-updater
  now waits for the app to be genuinely idle *after* a call finishes, instead of
  installing the instant a call ends -- which previously dropped the call for both
  sides.
* **The "Your camera isn't available" notice clears as soon as the camera
  recovers.** It no longer lingers on screen (or over a screen share) once the
  camera starts working again.

## v0.52.11 "Bafana Bafana" -- STABLE (2026-06-23)

### Changed

* **The hang-up button is now a clear red "leave call" button** with a
  recognizable phone-handset icon, so it reads unmistakably at a glance (it was
  a warm orange with a less obvious symbol).

## v0.52.10 "Bafana Bafana" -- STABLE (2026-06-23)

### Fixed

* **Incoming calls always open on your main screen** instead of occasionally
  appearing in a small bottom-right box.
* **The call window no longer jumps to the corner by itself** during a brief
  reconnect while you have a conversation open.
* **The call timer keeps counting from when the call started** -- a momentary
  reconnect no longer resets it to 00:00.
* **Updates download once, not repeatedly** -- the auto-updater now fetches a new
  version a single time instead of re-pulling the same installer on every check.

## v0.52.9 "Bafana Bafana" -- STABLE (2026-06-23)

### Fixed

* **Screen shares no longer freeze when the presenter switches what they share in
  quick succession.** Rapidly switching between screens/windows could leave the
  viewer stuck on a frozen frame, because the request for a fresh keyframe was
  being discarded before it went out. The keyframe request is now delivered
  correctly, so the view recovers within a moment.

## v0.52.8 "Bafana Bafana" -- STABLE (2026-06-23)

### Fixed

* **Starting a new screen share right after stopping one now works.** Stopping a
  share and immediately starting another could fail with "couldn't start a
  screen-share" for up to a minute on some graphics hardware -- the previous
  share's hardware video encoder was being released only slowly on stop. The app
  now releases it the moment a share ends, so you can re-share right away.

### Changed

* All bundled program components are now digitally signed, for smoother
  installation alongside managed antivirus software.

## v0.52.7 "Bafana Bafana" -- STABLE (2026-06-19)

### Fixed

* **Calls no longer drop after a screen share.** When a peer switched what they
  were sharing (for example from a full screen to a single app window), the call
  could fall into "Reconnecting" and then drop, because the app kept asking the
  server for the peer's camera feed and the server kept refusing while the share
  was reconfiguring. The app now detects that refusal and rebuilds the feed once
  instead of asking forever, so the call recovers on its own.
* **A full-screen call window no longer jumps to a tiny corner.** Opening a
  conversation during a call (or a brief "reconnecting") used to shrink a call you
  had put full-screen on another monitor down to a small box in the corner of your
  main screen. A full-screen call is now left where you put it.
* **The per-topic unread counter on the topic bar updates live** — a reply landing
  in another topic now bumps that topic's count immediately, instead of only after
  you reopened the conversation.

## v0.52.6 "Bafana Bafana" -- STABLE (2026-06-19)

### Fixed

* **Video no longer drops to the lowest quality (or cuts out) when bandwidth
  feedback is missing.** If the connection hadn't yet reported how much bandwidth
  was available, the app mistook "no reading yet" for "no bandwidth" and muted the
  higher-quality video layers — so you could be sending a tiny picture on a
  perfectly good connection. A missing reading is now treated as a healthy
  connection; a genuine low reading still adapts quality down as before.

## v0.52.5 "Bafana Bafana" -- STABLE (2026-06-19)

### Fixed

* **Capable computers stay at full video quality.** On machines with a working
  hardware video encoder, the call picture could drop to the lowest quality a few
  seconds in and stay there — even on a fast, uncongested connection — because the
  automatic load manager was pulling the quality down too aggressively. It no
  longer reduces quality on a capable machine; only a genuinely poor connection
  does.

### Changed

* **Video never silently drops to the lowest setting, and you're told why.** When
  a computer can't keep up at full quality (for example, no hardware video
  encoder), the camera now holds at 480p instead of collapsing to a tiny picture,
  and a small on-screen note explains that quality is limited.

## v0.52.4 "Bafana Bafana" -- STABLE (2026-06-19)

### Fixed

* **You can hear the other person again.** In some calls the incoming audio
  stopped reaching your speakers (the level meter still moved, but there was no
  sound) because all playback was routed through the echo-cancellation stage and
  a single hiccup there silenced the whole call. Incoming audio now plays
  directly, so it can never be held up by that stage.
* **Video climbs back to full quality after a brief network hiccup.** A momentary
  dip could leave the picture stuck at the lowest quality for the rest of the call
  on an otherwise fast connection; it now recovers within about a second once the
  connection is healthy again (while still holding steady on a genuinely
  congested link).
* **The receive-quality dropdown works again** — it could be unclickable once the
  call controls had faded; the click now always registers.

## v0.52.2 "Bafana Bafana" -- STABLE (2026-06-19)

### Fixed

* **Auto-updates now install reliably and don't leave the old version running.**
  In some cases an update would download and say it was ready, but the running
  app didn't fully close, so the new files couldn't be put in place and TalQ kept
  running the old version until it was quit and reopened by hand. The updater now
  guarantees the old process exits, and the installer force-closes any leftover
  instance before replacing files, so the update completes and TalQ restarts on
  its own.

## v0.52.1 "Bafana Bafana" -- STABLE (2026-06-18)

### Fixed

* **Incoming video no longer keeps dropping to low quality on its own.** The
  automatic quality control used the resolution it was *currently receiving* as
  its measure of load — which is the very thing it adjusts — so it fed back on
  itself and oscillated, repeatedly switching a call's video down to the lowest
  layer and back even on machines with plenty of headroom. Automatic receive
  downscaling on that signal is now disabled; per-tile sizing still picks an
  appropriate quality, and send-side adaptation is unchanged.

## v0.52.0 "Bafana Bafana" -- STABLE (2026-06-18)

Promotes the 0.51.x line to stable. Highlights since 0.50.11:

### Screen sharing
* **Sharing a single app window works reliably** (some graphics chips needed an
  even-sized encode; full-screen was unaffected).
* **Re-sharing your screen reliably reaches the other person** — a share started
  right after stopping a previous one no longer gets lost, with no need to wait.
* **A re-shared screen no longer flickers to black on the viewer's side**, while
  a genuinely stuck share still recovers on its own.
* **A received share no longer gets stuck on "Starting…"** if its first frame is
  lost.

### Calls
* **Hanging up a one-to-one call ends it immediately for the other person**
  instead of lingering on "Reconnecting"; a real connection drop still reconnects.
* **A graphics-encoder failure no longer drops the call** — it falls back to a
  working encoder (with a notice when software encoding is in use).
* **Steadier camera quality on busy connections** and a smoother, less twitchy
  automatic video-quality selection; a manually chosen quality sticks.
* The "You're sharing your screen" badge is no longer clipped.

### App
* **Quit and in-app updates always complete** — TalQ can no longer be left
  running after Quit, which previously also blocked an update.
* Closing the window minimizes it (keeping the unread badge on the taskbar);
  a taskbar unread badge and an unobtrusive offline indicator were added.
* **Updating can no longer leave a missing component behind** (a rare failed
  update could remove a library it then couldn't restore).
* Opening a thread no longer shows every message as a reply to the topic.

## v0.51.18 "Bafana Bafana" -- BETA (2026-06-18)

### Fixed

* **A re-shared screen no longer flickers to black on the viewer's side.** The
  reliability re-announce added in the previous build could rebuild a screen
  view that was already working, briefly dropping it to "starting share…"; it
  now only re-announces to a viewer who hasn't connected yet, so a working
  share stays put while a stuck one still recovers.

## v0.51.17 "Bafana Bafana" -- BETA (2026-06-18)

### Fixed

* **Re-sharing your screen right after stopping the previous share now works.**
  If you stopped a screen share and started another one within a few seconds,
  the new share could fail to appear for the other person while looking fine on
  your side. TalQ now re-announces the share so it reliably shows up, with no
  need to wait between shares.
* **Hanging up a one-to-one call now ends it immediately for the other person.**
  Previously the other side could keep showing "Reconnecting" for up to half a
  minute after you hung up. A genuine connection drop still reconnects as before.
* **The "You're sharing your screen" badge is no longer clipped** -- the last
  character of the label was being cut off.

## v0.51.16 "Bafana Bafana" -- BETA (2026-06-18)

### Fixed

* **Sharing a single app window now works reliably.** On some Intel graphics an
  app-window share could fail to appear for the other person (full-screen
  sharing was unaffected). TalQ now always encodes the window at a compatible
  size, so it shows up everywhere.
* **A received screen share no longer gets stuck on "Starting…".** If the first
  frame of an incoming share was lost it could hang on the placeholder; TalQ now
  re-requests the picture until it appears.
* **Messages inside a thread no longer each repeat the topic as a quote.**
* **Choosing a call video quality now sticks.** Manually picking a quality used
  to revert to Auto; your choice is kept until you switch back to Auto.

### Changed

* **TalQ now tells you when it's using software video encoding** (when no
  hardware video encoder is available), so it's clear why a call may use more CPU.

## v0.51.15 "Bafana Bafana" -- BETA (2026-06-18)

### Fixed

* **Quitting TalQ now always closes it completely — and updates install
  reliably.** Previously, choosing Quit could leave TalQ running in the
  background (you'd have to end it manually in Task Manager), and because the
  old copy was still running it could also stop an update from finishing. TalQ
  now guarantees it shuts down when you quit, so Quit works every time and
  updates apply cleanly.

  *Note: this is the fix for the "update won't finish" problem — to get it the
  first time you may need to fully close TalQ (end `talq.exe`) before installing
  this version. After that, future updates install on their own.*

## v0.51.14 "Bafana Bafana" -- BETA (2026-06-18)

### Changed

* **Your camera quality is steadier on a busy network.** Previously it could
  flip up and down every few seconds when bandwidth was tight. It now still
  drops quickly when the connection needs it to, but waits for the connection to
  stay healthy for a few seconds before stepping back up — so the picture stops
  oscillating.

### Fixed

* **Starting a screen share again right after stopping one is more reliable.**
  On some computers, re-sharing within a few seconds could leave the share stuck
  "starting" until it eventually recovered. TalQ now gives the graphics hardware
  more time to release between shares.
* **You're now notified if a screen share has trouble starting.** Instead of
  nothing appearing to happen, TalQ tells you it's still starting your share and,
  if it ultimately can't, says so clearly.

## v0.51.13 "Bafana Bafana" -- BETA (2026-06-18)

### Fixed

* **An interrupted update can no longer leave TalQ unable to start.** A previous
  build's updater could, in rare cases, finish with a runtime file missing (for
  example "libglib-2.0-0.dll was not found"), leaving the app dead until a manual
  reinstall. The installer now overwrites the program files in place instead of
  clearing them out first, so a working copy of every file is always present —
  even if an update is interrupted or a file is briefly in use. Updating is now
  safe to do at any time.

## v0.51.12 "Bafana Bafana" -- BETA (2026-06-17)

### Changed

* **Clicking X now minimizes TalQ to the taskbar instead of hiding it to the
  system tray.** This keeps TalQ's taskbar button — and the unread-message count
  badge on it — visible while TalQ keeps running in the background. You can still
  quit from the tray icon's menu, or switch this off in Settings ("Minimize when
  closing").

## v0.51.11 "Bafana Bafana" -- BETA (2026-06-16)

### Fixed

* **A call no longer drops when the graphics card's video encoder can't start.**
  On some laptops with two graphics chips, the hardware H.264 encoder can fail
  to initialize — previously this ended the whole call with no explanation. Now
  the call keeps going (with audio, and a clear on-screen message), and TalQ
  automatically switches to a working encoder for your next call and remembers
  it, so the problem doesn't recur. A video-encoder fault can no longer end a
  call.

## v0.51.10 "Bafana Bafana" -- BETA (2026-06-16)

### Fixed

* **Call window stability fix** (follow-up to the 0.51.9 multi-monitor video
  fix). Corrected how the call window re-lays-out its video when moved between
  monitors. This had no effect on normal installs, but it could crash the call
  window in development/debug builds; the wiring is now correct everywhere.

## v0.51.9 "Bafana Bafana" -- BETA (2026-06-15)

### Fixed

* **Call video no longer breaks when you move the call between monitors or go
  fullscreen.** Dragging the call window to a second screen with different
  display scaling could leave the video tile stretched/distorted, and
  double-clicking to enter fullscreen could make the video drop off-screen. The
  call now re-lays-out the video correctly on a monitor change and on the
  fullscreen toggle.
* **Messages you've read are now reliably marked as read.** Closed a case where a
  message you had already seen in the open, focused conversation could stay
  marked unread for the other person. The read marker now advances whenever the
  newest message changes while you're looking at the chat.

### Added

* **"Verbose call diagnostics" setting (off by default).** A new option under
  Settings logs detailed per-second call information to help track down call
  freezes or audio issues. It's off for normal use — turn it on only when
  reproducing a problem, and it takes effect on the next call.

## v0.51.8 "Bafana Bafana" -- BETA (2026-06-15)

### Fixed

* **The taskbar button now shows your unread-message count.** A red badge with
  the number of unread messages now appears on TalQ's main taskbar icon — not
  just the small system-tray icon — and updates as messages arrive and as you
  read them. It now reliably reappears after you reopen TalQ from the tray (and
  after the Windows taskbar restarts). The badge shows whenever TalQ is running
  with a window on the taskbar; while TalQ is fully closed, the tray icon keeps
  carrying the count.

## v0.51.7 "Bafana Bafana" -- BETA (2026-06-14)

### Changed

* **The offline indicator is now quiet and unobtrusive.** When the server can't
  be reached, TalQ now shows a thin, muted "Connecting…" strip at the top —
  instead of the previous alarming red banner — and no longer pops a desktop
  notification for a connection blip. The rest of the app stays fully usable
  while offline: your chat list and message history remain readable and
  scrollable from the local cache, nothing is locked.

## v0.51.6 "Bafana Bafana" -- BETA (2026-06-13)

### Added

* **TalQ now tells you when it can't reach the server.** If your connection
  drops or the server becomes unreachable, a clear banner appears across the top
  ("No connection to the server — you're offline. Reconnecting…") and a desktop
  notification is shown, so you know right away — even if TalQ is minimized to the
  tray. As soon as the server is reachable again the banner clears and you get a
  "reconnected" notification. The Home screen's "server" tile now reflects the
  real connection state too, instead of always showing "reachable".

## v0.51.5 "Bafana Bafana" -- BETA (2026-06-12)

### Fixed

* **Camera no longer freezes a few seconds into a video call.** The quality
  adapter that lowers the frame rate under load was changing the framerate in a
  way the encoder couldn't accept on the fly, which stalled the whole video
  pipeline — your camera (and the other side's view of you) would freeze after a
  few seconds. The adapter now reduces quality only by dropping simulcast layers,
  which is safe, so the video keeps flowing. Together with the 0.51.4 memory fix
  this resolves the video-call freezes on 0.51.x.

## v0.51.4 "Bafana Bafana" -- BETA (2026-06-12)

### Fixed

* **Memory leak during video calls — the cause of the freezes.** On a longer
  video call TalQ's memory could climb very fast (about 100 MB per second) until
  it exhausted the machine and froze it. The internal buffer that hands camera
  frames to the encoder was unbounded, so when the new quality-adapter throttled
  the encoder the frames piled up forever instead of being dropped. It is now
  bounded and drops the oldest frame under pressure, so memory stays flat. **This
  is what was freezing machines on 0.51.x — please update.**

## v0.51.3 "Bafana Bafana" -- BETA (2026-06-11)

### Fixed

* **Echo cancellation now actually starts.** On some machines (notably ones
  using their built-in speakers) the echo canceller silently failed to
  initialize, because the reference-audio path it listens to couldn't match the
  speaker's audio format — so echo wasn't removed even with the feature on. The
  audio is now converted to the speaker's format correctly and cancellation
  engages. **Please re-test on speakers, on a normal one-to-one call** (have one
  side call and the other answer — don't both dial at once). It can still be
  turned off under Settings → Audio & Video.

* **No more "ghost" audio after hanging up.** In a rare timing case — most
  likely when both people start the call at the same moment — you could keep
  hearing the other person for a few seconds after you'd hung up, with no call
  window on screen. Call signals that arrive a beat after you leave are now
  ignored, on both the camera and screen-share paths.

* **Settings opens instantly.** Opening Settings no longer briefly freezes while
  it scans your cameras and microphones; the scan runs in the background and the
  device lists fill in a moment later.

### Improved

* **Better diagnostics for hard-to-reproduce freezes.** If the app or the
  machine locks up during a call, the log now records what the call was doing
  right up to the moment it stopped — and forces those lines to disk — so a
  frozen session can finally be diagnosed afterwards. No effect on normal use.

## v0.51.2 "Bafana Bafana" -- BETA (2026-06-11)

### Fixed

* **Echo cancellation actually cancels now (beta).** A long-standing bug meant
  TalQ's echo cancellation *ran* but couldn't remove the echo — so if the person
  you were talking to used speakers, you'd hear your own voice come back a
  fraction of a second later. The decoded audio is now routed so the canceller
  has a correctly-timed reference of exactly what's being played, which is what
  it needs to subtract the echo. **Please test it on speakers** (both sides) and
  let us know. If anything sounds wrong, echo cancellation can still be turned
  off under Settings → Audio & Video.

## v0.51.1 "Bafana Bafana" -- BETA (2026-06-11)

### Fixed

* **The standard build now installs reliably on managed/work machines.** The
  standard (non-123NET) build is now code-signed, so Windows no longer mis-flags
  it as untrusted during install on centrally-managed or cert-trusted computers —
  the same trust the 123NET build already had. (On a personal/unmanaged PC,
  Windows Defender may still need a one-time exclusion until the public signing
  certificate is in place.)

## v0.51.0 "Bafana Bafana" -- BETA (2026-06-11)

Codename **Bafana Bafana** — the nickname ("the boys") of South Africa's
national football team — for the **FIFA 2026 World Cup**, where South Africa
plays the Opening Match. Fitting on two counts: TalQ's call server lives in
South Africa, and so does the company that builds it.

### Added

* **Automatic quality scaling for slower computers (beta).** During a call TalQ
  now measures how hard your computer is actually working to encode and decode
  video, and — if it can't keep up — gently steps the picture down (fewer
  simulcast layers, a lower frame rate, lower incoming resolution from other
  people) to keep the call smooth, then restores quality once there's headroom
  again. It adapts to your *hardware*, separately from how it already adapts to
  your *network*. Powerful machines are unaffected (no change). This is the
  long-promised "Zoom-style" load controller: it keeps weak laptops from
  freezing during camera + screen-share calls. The person you're looking at
  stays sharpest; smaller tiles give way first. Still in beta — if it ever
  misbehaves it can be turned off.

## v0.50.11 "Slartibartfast" -- STABLE (2026-06-10)

### Fixed

* **The remote microphone meter now moves.** On the call screen, each
  participant has a small audio-level meter next to their name. Only your own
  meter ever moved — every remote participant's stayed flat, because TalQ never
  measured their incoming audio. It now measures each remote participant's
  decoded sound and animates their meter, so you can see at a glance who is
  talking.

## v0.50.10 "Slartibartfast" -- STABLE (2026-06-10)

### Fixed

* **Faster, leaner startup.** TalQ was loading its entire media-plugin set into
  the app's own memory on every launch — a ~600 MB spike — because the GStreamer
  plugin scanner wasn't being bundled. It now scans plugins out-of-process and
  caches the result, so startup is quicker and uses far less memory.

## v0.50.9 "Slartibartfast" -- STABLE (2026-06-10)

### Fixed

* **Capable graphics cards are no longer held back.** The protection that caps
  video quality on weak integrated graphics (added to stop screen sharing from
  freezing low-power laptops) was being applied too broadly — capable GPUs such
  as Intel Iris Xe, AMD Radeon and NVIDIA cards were also being limited to a
  480p camera and a 720p screen share. TalQ now identifies the graphics chip and
  only protects genuinely weak integrated GPUs; capable machines send a
  full-quality camera and share their screen at native resolution.

### Added

* **"GPU performance" setting (Settings → Audio & Video).** If TalQ guesses your
  graphics capability wrong, you can override it: "Always full quality" lifts the
  caps, "Always protected" keeps them. Defaults to Auto. Takes effect on your
  next call.

## v0.50.8 "Slartibartfast" -- STABLE (2026-06-08)

### Fixed

* **A second launch no longer disturbs the diagnostic log.** Clicking TalQ's
  taskbar icon while it was already running could corrupt the debug log being
  written for the active session — so a log you sent in to report a call problem
  could come back empty or truncated. A second launch now simply re-opens the
  already-running window and leaves the live log untouched.
* **Smoother calls while someone shares their screen.** When another person in
  the call shares their screen, TalQ now also eases your own camera down to a
  single light stream for the duration — the shared screen is what everyone is
  looking at, and this frees up both your machine and the sharer's, so a screen
  share stays smooth on busy or low-power computers. Your full camera quality
  returns automatically when the share ends, or if that person leaves or briefly
  drops out.

### Changed

* **Camera quality on integrated-graphics laptops.** On machines that rely on
  integrated graphics, your camera is now sent at up to 480p (in line with the
  official web client) so the graphics chip stays comfortably within its limits
  during a call. Machines with a dedicated GPU are unchanged.

## v0.50.7 "Slartibartfast" -- STABLE (2026-06-08)

### Fixed

* **Screen sharing no longer overloads low-power laptops.** On a machine with
  integrated graphics (or no hardware video encoder), sharing your screen on
  top of your camera could gradually bog it down until it froze. TalQ now caps
  the shared screen to 720p (540p when no hardware encoder is present) on those
  machines, and pauses the camera's extra quality layers while you share, so the
  graphics chip is never asked to encode more than it can handle at once.
  Machines with a dedicated GPU are unchanged.
* **The receive-quality menu shows the sender's real resolution.** The "High"
  option was labelled from your OWN maximum-send setting, so it could read
  "High (1080p)" even when the other person was only sending 720p. It now
  reflects the resolution actually arriving from them.

## v0.50.6 "Slartibartfast" -- STABLE (2026-06-08)

### Fixed

* **Avatars now update without a restart.** When someone changed a group's
  picture, or a contact changed their own, TalQ kept showing the old avatar
  until you restarted — every avatar was cached in memory for the whole session
  and never refreshed. Avatars now refresh when you return to TalQ, and at least
  every 15 minutes while it stays open, and a picture that briefly failed to
  load is retried shortly after instead of staying blank until restart.

## v0.50.5 "Slartibartfast" -- STABLE (2026-06-07)

### Fixed

* **New conversations and messages now appear without a restart.** A group
  created on your phone -- or a reply in a chat that wasn't in your list yet --
  could be missing from TalQ's chat list until you restarted the app. Two
  things caused it: the background connection that signals "something changed"
  had no keepalive, so after the laptop slept or sat idle it could go silent
  without TalQ noticing, and the chat list wasn't refreshed when you brought
  TalQ back to the foreground. Now that connection is kept alive and reconnects
  when it drops, the chat list refreshes the moment you return to TalQ, and a
  refresh is never silently skipped or left stuck while another is in progress.
* **Correct release codename note in Settings.** The codename credit on the
  Account tab described every codename except "Deep Thought" as Bulgaria's
  April Uprising; it now shows the right note for each name (Slartibartfast,
  Magrathea, Botev, Margaritka, and the April Uprising cycle), and a neutral
  note for any future codename.

## v0.50.4 "Slartibartfast" -- STABLE (2026-06-05)

### Changed

* **Lighter video on machines without a dedicated graphics card.** On laptops
  with integrated (or no) graphics, TalQ now sends your camera at a resolution
  the machine can comfortably encode -- up to 720p on integrated graphics, 480p
  when no hardware video acceleration is detected -- instead of pushing full HD
  and overloading the GPU (which could stutter audio, choke video, or in the
  worst case lock the machine up). Machines with a dedicated GPU are unchanged,
  and the Home screen now shows the limit that applies to your device.

## v0.50.3 "Slartibartfast" -- STABLE (2026-06-05)

### Fixed

* **Read receipts work reliably again.** A 0.50.2 regression could leave
  messages never marked read after restarting TalQ or restoring it from the
  tray; read-marking now follows the app's focus directly and can't get stuck.
* **A call can no longer freeze your computer.** When the other person left a
  call -- even by hanging up -- their departure could, in rare cases, leave
  your side endlessly retrying to send, pegging the CPU/GPU. TalQ now detects a
  stalled outgoing stream and recovers it automatically.

## v0.50.2 "Slartibartfast" -- STABLE (2026-06-05)

### Changed

* **Read receipts follow your attention, not just an open window.** Messages
  are marked read only while TalQ is the focused window. A conversation left
  open in the background -- or opened from a notification or the tray -- stays
  unread until you actually switch to TalQ, so you keep getting notified about
  messages you haven't seen yet.
* **Smoother history scroll-back.** Older messages start loading a little before
  you reach the top, so scrolling up through a long conversation no longer
  pauses on a spinner.

## v0.50.1 "Slartibartfast" -- STABLE (2026-06-04)

### Changed

* **Faster chat history.** Conversations load and scroll back in larger pages,
  so opening a busy room and paging through its history take far fewer
  round-trips to the server -- noticeably quicker on slower or remote
  connections, and lighter on the server too.

## v0.50.0 "Slartibartfast" -- STABLE (2026-06-04)

The Slartibartfast line, promoted to stable. The headlines since 0.48.9:

### Added

* **Echo cancellation.** Take a call on speakers without the people you're
  talking to hearing their own voice echoed back to them. On by default;
  toggle under Settings -> Echo cancellation.
* **Full-display sharing on laptops with two graphics cards.** Sharing your
  whole screen now works on hybrid-GPU laptops (e.g. Intel + NVIDIA) where it
  could previously fail to start.
* **"Collect diagnostics" button** in Settings -- one click writes a text file
  with your system info and recent log, to send when reporting a problem.

### Changed

* **Sharper screen sharing.** Single windows are captured at native resolution,
  and the default quality is higher, so shared text stays readable.
* The Home screen now lists all of your graphics cards.

### Fixed

* **Your status behaves.** A custom status (like "Working remotely") no longer
  resets to "Online" when you restart, it syncs across your devices, and
  changing it from TalQ applies reliably.
* **Messages don't vanish.** Sending two messages in quick succession no longer
  makes the first one disappear.
* Right-click -> Reply now shows the reply preview above the message box.
* A range of call-screen, Settings, and update-check layout fixes.

## v0.49.7 "Slartibartfast" -- BETA (2026-06-04)

### Changed

* **The Home screen GPU tile now shows all your graphics cards.** On a laptop
  with two GPUs (e.g. NVIDIA + Intel) it lists both, with the hardware-
  acceleration codec on the line below, instead of showing only the one doing
  the work.

## v0.49.6 "Slartibartfast" -- BETA (2026-06-04)

### Changed

* **Sharing a single window now captures at native resolution.** Window app
  shares no longer downscale before sending, so text stays crisp even from a
  high-DPI or large screen -- the bitrate adapts to bandwidth instead of
  throwing away resolution. (Whole-screen shares still follow the quality
  setting.)

### Fixed

* **Clearer message when single-window sharing isn't supported.** On older
  Windows without Windows Graphics Capture, the error now explains that your
  Windows version doesn't support it (and suggests sharing the whole screen),
  instead of wrongly blaming the app.

* **Quieter debug log.** Removed a status-restore trace that fired on every
  click and flooded the log.

## v0.49.5 "Slartibartfast" -- BETA (2026-06-04)

### Fixed

* **Sharing your whole screen now works on laptops with two graphics cards.**
  On hybrid-GPU laptops (e.g. Intel + NVIDIA, with displays driven by different
  cards), full-display sharing could fail to start. TalQ now captures the
  display through Windows Graphics Capture -- the same engine single-window
  sharing already uses -- which works no matter which GPU drives the screen.
  (Older Windows without that support falls back to the previous method.)

* **Right-click → Reply now shows the reply preview.** Choosing Reply from the
  message right-click menu armed the reply but didn't show the preview bar above
  the message box, so it looked like nothing happened. (The reply icon next to a
  message already worked.)

* **Shared screens and windows look sharper by default.** The default sharing
  quality is now 1440p instead of 1080p, so text in a shared window stays
  readable -- a window on a high-DPI or large screen was being downscaled enough
  to soften text. ("High" quality captures most windows at native resolution.)

* **The "check for updates" result text no longer gets cut off** in Settings.

### Added

* **Settings -> "Collect diagnostics".** One click writes a text file with your
  system info (Windows version, graphics cards, displays, audio/video devices,
  installed components) and the recent log -- easy to send when reporting a
  problem.

### Changed

* The call window now has a larger minimum size so the video can't be squeezed
  down to an unreadable sliver.

## v0.49.4 "Slartibartfast" -- BETA (2026-06-04)

### Fixed

* **Changing your status from TalQ now applies and sticks.** A regression in
  0.49.2's cross-device status sync could briefly revert the status you just set
  -- the background refresh read the server a moment before your change had
  propagated, and snapped it back. Your own change now wins for a short window
  after you make it; cross-device sync from your other instances is unchanged.

## v0.49.3 "Slartibartfast" -- BETA (2026-06-04)

### Fixed

* **Call-screen pills now wrap instead of overlapping on narrow windows.** When
  the call window is small -- especially while you're screen-sharing -- the
  status, telemetry, and action pills stack onto their own rows instead of
  colliding, and the "You're sharing your screen" badge sits on its own clear
  row below them instead of being hidden behind the other pills.

## v0.49.2 "Slartibartfast" -- BETA (2026-06-04)

### Fixed

* **Your custom status no longer resets to "Online" when you restart TalQ.** A
  status like "Working remotely" was being cleared on every launch (and after
  every call). TalQ now only undoes the automatic "In a call" status that Talk
  itself sets during a call, and never touches a status you chose.

* **Your status now syncs across your devices.** If you change your status in
  another TalQ or Talk instance signed in to the same account, this one updates
  to match -- within about a minute, or instantly when you switch back to the
  window -- instead of showing a stale status. (It also no longer re-asserts a
  stale "Online" over an Away/Do-not-disturb you set elsewhere.)

## v0.49.1 "Slartibartfast" -- BETA (2026-06-04)

### Fixed

* **A sent message could disappear or jump when you sent a second one right
  after it.** Sending two messages back-to-back could make the first one
  vanish from the open conversation (or reorder so the newer one sat above the
  older). The optimistic "sending" row is now swapped in place for the
  confirmed message instead of being removed and re-added, and pending messages
  always stay at the newest position -- so a just-sent message can no longer be
  dropped or sorted to the wrong spot. (Carries the echo cancellation from
  0.49.0.)

## v0.49.0 "Slartibartfast" -- BETA (2026-06-04)

### Added

* **Echo cancellation (AEC).** When you take a call on speakers instead of a
  headset, the people you are talking to no longer hear their own voice echoed
  back to them. TalQ now removes your speaker output from your microphone in
  real time -- the same acoustic echo cancellation a browser does
  automatically. Switch it on or off under **Settings -> Echo cancellation**
  (on by default; takes effect on your next call). It helps most with
  speakers; a headset has little echo to cancel.

### Notes

* This is a **beta** so echo cancellation can be validated on real hardware
  across different rooms and speaker setups. The feature is strictly optional
  and self-disabling: if anything about it cannot start, the call connects
  exactly as before with echo cancellation simply off -- it can never drop a
  call.

## v0.48.9 "Botev" -- STABLE (2026-06-03)

### Fixed

* **Service messages now show their time.** Missed calls, call started/ended,
  and other system messages were rendering with no timestamp; they now show
  when they happened.

### Changed

* **Outgoing call sound now defaults to "Landline (US)."**

## v0.48.8 "Botev" -- STABLE (2026-06-03)

### Added

* **A separate sound for outgoing calls.** Settings > Audio now has an
  "Outgoing call sound" picker, distinct from your incoming ringtone, so you
  can use one of the bundled tones (or the classic ringback) for when *you're*
  calling someone. Defaults to the ringback tone.

### Fixed

* **Speakers no longer show up in the Microphone list.** Windows exposes
  speakers as loopback capture sources; those are now filtered out, so the
  Microphone picker shows only real microphones.
* **The mic test now works.** It reads the correct device identifier (so the
  level meter actually captures the selected microphone), and falls back to the
  system default if the chosen device can't be opened.

## v0.48.7 "Botev" -- STABLE (2026-06-03)

### Changed

* **A clearer chat-loading indicator.** While a conversation's history is being
  fetched, a thin progress line now sweeps along the bottom of the chat header
  -- the way browsers and other messengers show loading -- replacing the small
  centered spinner that was easy to miss. It only appears when a load actually
  takes a moment, so quick opens stay clean.

## v0.48.6 "Botev" -- STABLE (2026-06-03)

> A pre-1.0 polish pass: theme consistency across all four themes,
> translatable strings, several dialog fixes, and stability/memory hardening
> from a full-codebase audit.

### Changed

* **Consistent theming everywhere.** A pass over menus, dialogs, popups and
  list rows replaced colours that were baked to the dark theme with ones that
  follow the active theme -- so the right-click message menu, search results,
  the reminder / new-topic / participants / new-chat dialogs, the in-app
  notification popup, and assorted badges and status dots now look right on
  every theme, including the light **Paper** theme (where some of them used to
  show up as dark boxes or wrong-coloured accents).
* **More of the interface is translatable.** Many user-facing strings that
  were hard-coded in English (composer placeholder, "Sending…", "Muted",
  "Loading…", thread and share-picker labels, the selection count, and more)
  are now wrapped for translation.

### Added

* **Cancel an upcoming reminder from the reminders list.** The list showed
  your reminders but had no way to cancel one from there; now each row does.
* **Clearer feedback in the screen-share picker and scheduled messages.** The
  Screens/Windows picker shows a message when nothing is shareable instead of
  a blank grid, and editing/cancelling a scheduled message now tells you if
  the server refused instead of silently doing nothing.

### Fixed

* **Stability + memory.** Fixed a crash that could occur during call teardown
  and several small memory leaks that accumulated across calls and over long
  sessions (from a full-codebase pre-1.0 audit), plus a per-frame leak in the
  background-blur path.

## v0.48.5 "Botev" -- STABLE (2026-06-03)

> Call stability + chat polish on the "Botev" line, from continued live testing.

### Added

* **A "Syncing…" indicator while chat history loads.** When you open a
  conversation and the history is still being fetched from the server -- or a
  conversation is being checked for updates -- TalQ now shows a small animated
  indicator instead of a blank panel, so you can tell it's working rather than
  stuck. It only appears when a fetch actually takes a moment, so a fast
  connection never flickers it.
* **A microphone test in Settings.** Settings > Audio now has a live level bar
  under the Microphone picker -- speak, and the bar moves if the selected
  microphone is working. A quick way to confirm your mic before a call.

### Fixed

* **Messages you're looking at are now reliably marked as read.** Under some
  timing, a message that arrived while you had the conversation open could stay
  marked unread; it's now marked read no matter which path delivered it.
* **More stable calls.** Fixed a crash that could happen while a call was being
  torn down (a publisher pipeline freed while it was still finishing its own
  work), and trimmed memory that was being held across calls.

## v0.48.4 "Botev" -- STABLE (2026-06-03)

> A point release on the "Botev" line, polishing call UX from continued live
> testing.

### Fixed

* **Rejoining a call now restores the other person's camera and audio, not
  just their screen.** If you dropped and came back while someone was sharing
  their screen, you'd see the share but get no picture or sound from them.
  Their camera and microphone now come through on rejoin as well.
* **The call info chips no longer overlap the buttons on a narrow window.** The
  codec / mode / quality / resolution chips now wrap onto a second row when the
  call window is too narrow to fit them on one line, instead of sliding under
  the Quality / Background / Share controls.

## v0.48.3 "Botev" -- STABLE (2026-06-03)

> A point release on the "Botev" line, from continued live testing against the
> official Nextcloud Talk (web + Android).

### Added

* **Share a single application window.** You can now share just one app window
  instead of your whole screen, and the other side sees only that window --
  nothing else on your desktop. Picking a window previously fell back to
  sharing the entire monitor; that's fixed. Sharing a whole screen still works
  exactly as before. (Validated live end-to-end, including a 2K window with
  moving video.)

## v0.48.2 "Botev" -- STABLE (2026-06-03)

> A point release on the "Botev" line, from continued live testing against the
> official Nextcloud Talk (web + Android).

### Fixed

* **A microphone that won't start no longer drops the whole call.** If your
  selected mic can't be opened (in use by another app, blocked in Windows
  privacy settings, unplugged, or simply not responding), the call now keeps
  going instead of ending for both sides. TalQ automatically tries the system
  default microphone, and if no microphone can be opened at all it connects
  with your audio muted -- your video and the other person stay fully
  connected -- and shows a clear "Your microphone isn't available" notice
  telling you how to fix it. Previously a single uncooperative microphone
  tore down the entire call, video included.

## v0.48.1 "Botev" -- STABLE (2026-06-03)

> A point release on the "Botev" line, from continued live testing against the
> official Nextcloud Talk (web + Android).

### Fixed

* **Changing screen-share quality mid-share no longer drops the share, and the
  switch is instant.** The quality change now reconfigures the running stream
  in place instead of tearing it down and re-publishing, so you can change the
  resolution as often as you like -- up or down -- and the share keeps running
  smoothly at the new quality, with no "reconnecting" and no dropped share.

## v0.48.0 "Botev" -- STABLE (2026-06-02)

> **The "Botev" line, promoted to stable on Heroes' Day week.** Named for
> **2 June -- the Day of Hristo Botev and those who fell for Bulgaria's
> freedom**, when the sirens sound and the country stands still. This stable
> carries a full day of live two-party testing against the official Nextcloud
> Talk (web + Android): calls that used to die after a few minutes now hold,
> and TalQ interoperates with the official clients in both directions.

### Fixed

* **Calls no longer drop after a few minutes.** TalQ now keeps its signaling
  connection alive and transparently resumes it across a brief network blip, so
  a call survives instead of silently ending with the other side stuck on
  "waiting for others to join."
* **You can join a call that's already in progress.** If someone keeps a
  conversation/call open and you join it, TalQ now connects immediately instead
  of ringing to "no answer."
* **You reliably see the other side's camera and screen.** Fixed an ICE
  candidate timing race that could leave you stuck on "waiting for video," and a
  case where a not-yet-ready stream was accepted empty. Receiving a screen share
  from the official web/Android client now works.

### Added

* **Full interoperability with the official Nextcloud Talk (web + Android).**
  Your camera and screen share now display correctly on the official clients (a
  missing H.264 parameter previously left them black there).
* **More ringtones** -- a classic landline, a UK double-ring, an old telephone,
  and a digital trill, alongside the existing set.
* **Automatic updates are on by default** so everyone stays on current builds.

### Known issues (being worked on)

* Changing screen-share **quality mid-call** can still drop the share -- stop and
  re-share at the new quality for now.
* In a call with **3+ participants**, TalQ currently shows one remote stream.
* Sharing a single **application window** shows your whole screen (a bundled-
  media limitation) -- share a whole screen for now.

## v0.47.0 "Botev" -- BETA (2026-06-02)

> Named for **2 June -- the Day of Hristo Botev and those who fell for Bulgaria's
> freedom** (Heroes' Day), when sirens sound for two minutes at noon and the
> whole country stands still. Botev -- poet, journalist, revolutionary -- was
> killed in the Stara Planina in 1876, the same struggle the earlier "Aprilsko
> Vastanie" / "Panagyurishte" / "Koprivshtitsa" releases honoured. A reliability
> beta fixing the screen-share issues found in live two-party testing on 0.46.0
> "Margaritka".

### Fixed

* **Changing screen-share quality mid-call no longer drops the share.** The
  resolution switch tore down and instantly re-grabbed the Windows screen-
  capture device before the OS had released it, so the new capture stalled after
  one frame and the share died (and, before that, got stuck). The teardown now
  waits for the capture device to actually free, with a brief settle, before
  re-acquiring -- so a quality change blips and resumes at the new resolution
  instead of dropping. A capture error that does slip through now fails fast and
  visibly instead of silently timing out.
* **A failed screen share recovers instead of wedging.** If a share fails to
  start, the share button no longer goes silently dead -- you can start a new
  share immediately (previously it stayed stuck until you restarted the app), and
  ending a call while sharing no longer leaves sharing broken for the next call.

### Known in this beta (being worked on)

* Sharing a single application window currently shows your whole screen instead
  (a limitation of the bundled media components) -- share a whole screen for now.
* Hanging up a 1:1 call can take a few seconds to clear on the other side.

## v0.46.0 "Margaritka" -- STABLE (2026-06-01)

> **Margaritka -- shipped on Children's Day.** *Margaritka* (Bulgarian
> margaritka / маргаритка) is the daisy: the small, sun-following meadow flower
> of early summer, the one children thread into chains and pull petals from. We
> cut this stable on **1 June -- International Children's Day** (Den na deteto,
> marked in Bulgaria every year since 1927), and named it for the season's
> plainest, hardiest flower. It fits the work, too: a daisy shrugs off a gust of
> wind or a passing cloud, and this release taught TalQ to do the same on a call
> -- to quietly ride out a dropped Wi-Fi, a flaky signal, a mid-call reconnect
> and reopen none the worse. It promotes the whole 0.45.x "Margaritka" beta line
> to stable, with three resilience fixes from live two-party testing on top.

### Fixed

* **Your call survives the other side's network blip.** A 1:1 call no longer
  ends the instant the other person's Wi-Fi drops for a few seconds. TalQ holds
  the call in a brief "reconnecting" state and automatically re-joins their
  video when they come back -- even if they reconnect under a new session. Only
  a genuine, lasting departure ends the call.
* **Frozen remote video recovers on its own.** If the other side reconnects and
  their video freezes (their stream changed mid-call while the server kept the
  old, dead connection alive), TalQ now notices the stall and rebuilds the feed
  automatically instead of leaving you on a frozen frame.
* **Changing screen-share quality no longer drops the share.** Switching the
  shared-screen resolution mid-call used to kill the share and refuse to
  restart; it now blips and resumes cleanly at the new quality.
* **A clear message when a call can't start.** If the server rejects a call
  setup (a transient server error), TalQ retries briefly and then tells you
  plainly what happened instead of failing silently -- and a failed outbound
  call no longer rebounds as a phantom "incoming call".

### Also in Margaritka (now on stable)

* **Camera-unavailable notice.** If your camera can't open -- missing, in use by
  another app, or blocked in Windows privacy settings -- TalQ shows a loud,
  plain-language banner telling you exactly what to do, instead of sitting
  silently on "Starting camera...".
* **Tidier call status** (no more misleading "Connecting..." on a call that's
  already up), **send your log from Settings -> Diagnostics**, reliable
  screen-share start with an optional monitor border, and the earlier Margaritka
  chat-sync, large-file upload, and call-audio improvements.

## v0.45.3 "Margaritka" -- BETA (2026-06-01)

> Clearer feedback when your camera can't start, a tidier call status, and an
> easy way to send your log when reporting a problem.

### Added

* **Send your log straight from Settings.** Settings -> General -> Diagnostics now
  has "Save a copy..." and "Open log folder" buttons, so you can grab the
  diagnostic log and send it when reporting an issue without hunting through
  hidden AppData folders.

### Fixed

* **You're now told when your camera can't be used.** If TalQ can't open your
  camera during a call -- because it's missing, already in use by another app, or
  blocked in Windows privacy settings -- it shows a clear "Camera unavailable"
  notice with how to fix it, instead of sitting silently on "Starting camera...".
  The other person sees "Camera off" rather than waiting forever for video that
  will never arrive. Once you've freed the camera up, turn it off and on again to
  retry without leaving the call.

* **No more "Connecting..." on a call that's already connected.** Once a call is
  established, a participant whose video hasn't come through yet now reads
  "Waiting for video..." with the status light staying green, instead of
  misleadingly showing "Connecting".

## v0.45.2 "Margaritka" -- BETA (2026-06-01)

> More dependable screen sharing, and a calmer "you are sharing" indicator.

### Changed

* **The screen-share border is now optional and subtler.** Sharing a full screen
  draws a thin, semi-transparent frame instead of the previous bold one, and you
  can switch it off entirely under Settings -> Audio & Video -> "Show border
  around shared screen". Sharing a single window never draws a border.

### Fixed

* **Screen shares start reliably.** TalQ now confirms a share is actually live on
  the wire (not just negotiated) and automatically retries a fresh attempt if it
  does not come up in time, instead of hanging on "Starting...". Starting a new
  share right after stopping one no longer makes you wait a few seconds first.

## v0.45.1 "Margaritka" — BETA (2026-05-30)

> Same **Margaritka** beta line (the codename runs per minor line). A small fix
> to the experimental direct-call path.

### Fixed

* **Direct (peer-to-peer) one-to-one calls go live faster.** A direct call could
  linger on "Connecting…" for a few seconds before connecting when the media
  link came up before the other side was discovered; it now switches to the live
  call the moment the connection is ready. Direct P2P is still an opt-in
  experiment; calls through the server are unaffected.

## v0.45.0 "Margaritka" — BETA (2026-05-30)

> A new beta line named **Margaritka** (Маргаритка, "the daisy") for **1 June,
> Children's Day in Bulgaria** — after Todor Dinov's beloved 1965 animated short.
> Five long-standing wishlist items land together.

### Added

* **Automatic microphone leveling.** Calls now even out your microphone volume
  so quiet speakers come through clearly without loud ones distorting. It's **on
  by default**; you can turn it off under **Settings → Audio & Video →
  "Automatic gain control"**.
* **Send large files.** The old 100 MB attachment limit is gone. Big files now
  upload in chunks, so they no longer freeze the app or get rejected — the upload
  bar simply fills as the file streams up.
* **Screen-share monitor border.** While you share a whole screen, a green border
  frames the monitor you're sharing so you always know what others can see. The
  border is hidden from the picture your viewers receive.

### Changed

* **Your call audio keeps its volume.** Windows no longer quietens TalQ during a
  call when another communications app (Teams, Skype, a softphone) opens in the
  background.
* **Sharper direct one-to-one calls.** When a 1:1 call connects directly between
  two TalQ apps, video now uses a higher-quality H.264 profile for more detail at
  the same bandwidth. (Direct peer-to-peer is still an opt-in experiment; calls
  through the server are unaffected and keep their broad compatibility.)

## v0.44.0 "Magrathea" — STABLE (2026-05-30)

> Promotes the **Magrathea** beta line (0.43.0–0.43.3) to the stable channel,
> keeping the codename. The headlines are below; the 0.43.x entries have the
> full detail.

### Added

* **Calls reconnect instead of dropping.** When the connection to the call
  server is lost, the call no longer hangs up after a few seconds — it enters a
  **Reconnecting…** state and keeps re-establishing your media with backoff,
  while everyone else stays in the call. A **Leave** button cancels a stuck
  reconnect, and you get a **"Call ended"** notification when a call really ends.
* **Start a call in group conversations.** Audio and video call buttons now
  appear in group and public rooms, not just one-to-one chats.
* **Topics:** unread counts on topic chips; hide or delete a topic from its
  right-click menu; the topic bar scrolls with the mouse wheel, keeps the
  selected topic in view, and lists topics with unread first then most-recent.
* **Edit your last message with ↑**, **clear a conversation's history**
  (server-side — clears on all your devices and the other party's).

### Changed

* **Buttons look like buttons everywhere.** Every dialog and message-box button
  now has a consistent, real shape with proper hover/pressed states across all
  four themes; cramped button rows got breathing room.
* **Photos sit edge-to-edge in chat** — tighter framing (no dead space around
  portrait images) with the timestamp floated over the picture.
* **Deleted messages no longer leave a placeholder** in the thread.
* Debug logs are kept across restarts (last 12 sessions) for easier diagnosis.

### Fixed

* **The callee no longer freezes when a call is hung up** — the hardware-specific
  whole-machine freeze on hang-up. The HW codec teardown was moved off the UI
  thread (applied to every call pipeline).
* A rare crash when quitting the app while a chat was open.

## v0.43.3 "Magrathea" — BETA (2026-05-29)

> Same **Magrathea** beta line — codename is per minor line.

### Added

* **Edit your last message with ↑.** Press the up-arrow in an empty message box
  to jump straight into editing your most recent message — no hovering to hunt
  for the edit action.
* **Clear a conversation's history.** From a conversation's info panel (a
  moderator action, also available in one-to-one chats) you can clear the whole
  chat history. It's done server-side, so it clears on every one of your devices
  and for the other party too, not just locally.
* **Scroll the topic bar with your mouse wheel**, and the selected topic now
  scrolls itself into view — so rooms with many topics stay easy to navigate.

### Changed

* **Every button looks like a button.** Buttons across all dialogs and message
  boxes (delete/leave confirmations, Clear history, New chat, Forward, the file
  and share pickers, scheduled messages, reminders) now have a real, consistent
  shape — proper fill or outline, padding, and hover/pressed states — themed for
  all four themes, with no more buttons jammed against each other or rendering as
  plain text. Message-box buttons are styled once, app-wide, so the accept button
  always reads as the clear primary action.
* **Photos sit edge-to-edge in chat.** An image shared on its own is now framed
  tightly with no wasted padding — portrait photos no longer float in a wide box
  with dead space on either side — and the timestamp floats neatly over the
  bottom-right of the picture.
* **Topics are ordered by what needs attention.** The topic bar lists topics with
  unread messages first, then by most recent activity.
* **Deleted messages no longer leave noise.** Removing a message no longer leaves
  a "deleted message" placeholder cluttering the thread (as in Telegram) — it
  simply disappears.

## v0.43.2 "Magrathea" — BETA (2026-05-29)

> Same **Magrathea** beta line.

### Added

* **Delete a topic.** Right-click a topic chip in the conversation header and
  choose **Delete topic**. Nextcloud Talk has no server-side thread delete, so
  this is best-effort: it removes the topic's messages where the server allows —
  your own (within the edit window) and others' only if you moderate the
  conversation. Anything that can't be deleted is left in place and reported,
  and the chip disappears once the topic has no surviving messages. A
  confirmation makes the scope and limits clear before anything is deleted.

## v0.43.1 "Magrathea" — BETA (2026-05-29)

> Same **Magrathea** beta line as 0.43.0 — codename is per minor line.

### Added

* **Unread counts on topic chips.** Each topic in the conversation header now
  shows a ` · N` badge for messages newer than your read position, updated live;
  opening a topic clears its badge. (Previously the count was never populated,
  so topics gave no hint of new activity.)

### Changed

* **Calmer topic chips.** The selected topic chip used a bold, fully-saturated
  fill that felt too loud. It now uses a soft accent tint with accent-coloured
  text and a thin accent border — clearly "active" but much quieter.
* **Debug logs survive restarts.** The diagnostic log is no longer wiped on
  every launch. Each session's previous log is archived under a timestamped name
  and the most recent 12 sessions are kept — so a crash's log is preserved for
  diagnosis across any number of restarts (the live log still stays lean).

## v0.43.0 "Magrathea" — BETA (2026-05-29)

> Codename **Magrathea** — the legendary planet-building world from *The
> Hitchhiker's Guide to the Galaxy*, continuing the Guide theme opened by 0.42's
> *Deep Thought*. A **pre-release** beta line for call-resilience work; 0.42.x
> remains the stable channel.

### Added

* **Start a call in group conversations.** The audio and video call buttons now
  appear in group and public rooms, not just 1:1 chats — start or join a group
  call straight from the conversation header.

### Changed

* **Calls reconnect instead of dropping (Zoom-style).** When your connection to
  the call server is lost (a flaky long-haul link revoking ICE consent, a
  NAT/Wi-Fi blip), the call no longer hangs up after a few seconds. It enters a
  **Reconnecting…** state and keeps actively re-establishing your media with
  backoff for as long as it takes, while everyone else stays in the call. The
  control bar stays visible with a **Leave** button so you can cancel a stuck
  reconnect, and you now get a **"Call ended"** notification when a call really
  ends (instead of the window silently vanishing).

### Fixed

* **Hanging up no longer freezes the other side.** On some hardware the peer's
  app — and occasionally the whole machine — could lock up when a call was torn
  down, because the media pipeline was shut down synchronously on the UI thread
  and a synchronous hardware-codec teardown could wedge the GPU driver. Every
  call pipeline (1:1 direct, the server send/receive legs, and screen share) now
  releases on a worker thread, off the UI thread.
* **Call events no longer sit under "New messages".** "Started a call" / "Call
  ended" are system events, not unread messages — the *New messages* divider now
  anchors on your first genuine unread message and skips call rows.

## v0.42.1 "Deep Thought" — STABLE (2026-05-29)

> Codename **Deep Thought** — the supercomputer from *The Hitchhiker's Guide
> to the Galaxy* that computed **42**, the Answer to Life, the Universe and
> Everything (a nod to version 0.42). It opens a new naming line after the
> 1876 April-Uprising cycle (*Aprilsko Vastanie* → *Panagyurishte* →
> *Koprivshtitsa*).

### Fixed

* **New messages always appear immediately — on open and while you're in
  the room.** A message that arrived while you were in another room, before
  the app was focused, or even while you were sitting in the conversation
  could be missing until you switched away and back. The message list now
  enforces a single newest-first ordering rule across *every* path that adds
  messages (first load, live updates, scroll-back history), so the newest
  message is always at the bottom the instant it arrives — no switching.
* **"Away" returns to "Online" as soon as you're back.** After an
  automatic idle-away, returning to TalQ (focusing the window or any
  keypress/click) restores your status to online immediately, instead of
  waiting up to ~30 seconds for the next idle check. This now also covers an
  away set automatically by the server or by another device.
* **In-app release notes render fully.** The changelog screen could turn
  unreadable partway down (a stray markup character cut the rest off); it
  now displays correctly all the way through.
* **Settings codename credit** now matches the build (the "Deep Thought"
  release no longer shows the previous April-Uprising blurb).

## v0.42.0 "Deep Thought" — STABLE (2026-05-29)

The chat-room reliability work from the 0.41.x beta series, promoted to
stable. This release makes conversations, unread state, reactions,
replies and topics behave correctly and consistently.

### Fixed — chat reliability

* **Open conversations stay up to date.** New messages always appear in
  the open chat, even after a network blip — the live message connection
  now detects an unexpected drop and reconnects instead of going quiet
  (previously a message could show in the conversation list but not
  inside the open room until you switched away and back).
* **Unread counts are correct and consistent across devices.** The app no
  longer over-counts unread messages or moves your read position
  backwards; the badge clears as you read, and the "New messages" divider
  no longer re-appears above messages you've already read.
* **Reactions from other people now show up** on your messages, and
  survive reopening the conversation and restarting the app.
* **Replies stay in the right place** — a reply no longer jumps to sit
  next to the original message after reopening the conversation; it stays
  in order with a quote of the message it answers, which now fills in
  immediately when you send.
* **New topics appear instantly for everyone** in a group, without having
  to leave and re-open the conversation.
* **Light theme: topic labels are readable** again, following the active
  theme in all four themes.

### Other

* **A contact's TalQ version** is no longer shown as a stale/wrong value —
  it's hidden when it hasn't been confirmed recently and refreshes on a
  call.
* **Experimental "Direct P2P for 1:1 calls"** (opt-in, off by default):
  fixed a crash and the missing self-view camera; when a direct
  connection isn't possible the call falls back to the server-routed path
  automatically. Leave it off unless you're specifically testing it.

### Under the hood

* Core chat-sync rules (forward-only read marker, message-list
  reconciliation, peer-version freshness) are now pure, unit-tested
  invariants so these regressions can't quietly return.

## v0.41.11 "Koprivshtitsa" — BETA (2026-05-29)

More fixes from field testing: topics, the light theme, message reply
ordering, and a crash in the experimental direct-call mode.

### Fixed

* **New topics appear instantly for everyone.** When a member created a
  topic, others in the group didn't see it (and couldn't open it or read
  its messages) until they left the conversation and came back. The
  topic bar now updates live for everyone.
* **Light theme: topic labels are readable again.** The topic-bar labels
  rendered black/low-contrast in the light theme; they now follow the
  theme's text colors like the rest of the app, in all four themes.
* **Replies stay in the right place.** A reply could jump to sit next to
  the original message (near the top of the history) after reopening the
  conversation, instead of staying in order with a quote of the message
  it answers. Message ordering is now consistent between the live view
  and what's restored from the local cache.
* **Fixed a crash in experimental 1:1 "Direct P2P" calls.** If a direct
  call couldn't bring up the camera, the app could die on both ends a few
  seconds in. It no longer crashes and now falls back to the
  server-routed call path. Direct P2P stays opt-in / off by default while
  it's stabilised (the self-view camera in that mode is still being
  worked on).

## v0.41.10 "Koprivshtitsa" — BETA (2026-05-29)

A focused hardening pass on chat synchronisation. Messages, unread
state, reactions, replies and sending are now far more reliable, and a
crash when sending the first message in a brand-new group is fixed.

### Fixed — chat sync

* **New messages now appear in the open conversation reliably.** The
  open chat could silently stop updating after a transient network
  blip (sleep/wake, VPN or proxy reconnect) — new messages showed in
  the conversation list but not inside the room you were looking at,
  until you switched away and back. The live message connection now
  detects an unexpected drop and reconnects instead of going quiet,
  re-syncs the open room when you bring the window back to the
  foreground, and recovers immediately if you re-click the open
  conversation.
* **Unread counts no longer over-count.** The app could send the
  server a read position that moved *backwards* (for example when a
  topic tab was open, or this device was behind another you had
  already read on), which inflated the unread badge — e.g. showing 23
  when you had 4. The read marker is now strictly forward-only, so it
  can never drag your read position backwards or re-inflate the count
  across devices.
* **The unread badge clears as you read**, and the **"New messages"
  divider no longer re-appears above messages you have already read**
  when you return to a conversation.
* **Replies show the quoted message immediately.** A reply you sent
  showed a blank quote until the server echo arrived; the quoted
  message is now filled in the moment you send.
* **Reactions added by other people now show up** (and survive
  re-opening the room and restarting the app). Previously an emoji
  another participant added to your message never appeared.
* **Sending the first message in a brand-new group no longer gets
  stuck on "Sending" or crashes the app.** Sends that do not complete
  now surface a clear failed state you can retry, and a new group
  starts receiving replies without needing to re-open it.
* **A contact's TalQ version is no longer shown as a stale, wrong
  value.** The version a peer reported is now timestamped; if it has
  not been confirmed recently it simply is not shown rather than
  displaying an old number, and a call refreshes it.

### Under the hood

* Core sync rules (forward-only read marker, safe message-list index
  reconciliation that fixes the new-group send crash, and peer-version
  freshness) are now extracted into pure, unit-tested helpers so these
  regressions cannot quietly return.

## v0.41.9 "Koprivshtitsa" — BETA (2026-05-28)

Zoom-style direct peer-to-peer for one-to-one calls, plus a deep
overhaul of the peer-to-peer media engine.

### Added — direct peer-to-peer for 1:1 calls (experimental, opt-in)

* **One-to-one calls can now connect directly between the two
  people** instead of routing audio and video through the server —
  lower latency and bandwidth when you are near each other. Group
  calls (3+) keep using the server, exactly like Zoom. Enable it in
  **Settings → "Direct P2P for 1:1 calls"** (off by default while the
  direct media path is being validated in the field). If the other
  person is on a non-TalQ client, or the direct connection can't be
  established, the call falls back to the server automatically.
* **How it bypasses the media server.** Earlier attempts to force
  direct calls failed because the standalone-signaling server, when
  configured with a media server, intercepts the standard
  offer/answer/ICE-candidate messages and treats them as
  server-publish operations (a relayed 1:1 offer came back answered
  "from self" and ICE candidates were rejected). TalQ now rides a
  private signaling overlay — a custom message type the server relays
  untouched — so the call setup travels straight between the two
  clients and the media goes direct.

### Fixed — peer-to-peer media engine

* **Receiving video now works.** The incoming-media chain was linked
  on the wrong thread, *after* media had already started arriving,
  which permanently stalled the shared transport (and, because send
  and receive share it, killed the outgoing direction too). It is now
  linked synchronously the moment the stream appears.
* **Decoding switched to auto-selection** (the same approach the
  server-receive path uses), which reliably picks a working hardware
  or software H.264 decoder; the previous fixed decoder negotiated but
  produced no frames.
* **Camera no longer emits one frame then freezes.** The capture
  elements were being started in the wrong order relative to the rest
  of the pipeline, so the camera pushed into a not-yet-ready stage and
  paused itself. Now started in dependency order.
* **Hanging up no longer freezes the app.** Tearing the call down while
  the camera was still streaming could deadlock; the source is now
  stopped first.
* **A transient transport hiccup no longer kills your camera** — only
  genuine capture/encoder failures end the video now.
* **Robust camera capture + hardware encoder** carried over: permissive
  capture formats + `decodebin` (fixes `mfvideosrc … not-negotiated`
  on cameras without an exact MJPEG mode), hardware H.264 encoder
  selection, and clean camera-device release on teardown.

### Unchanged & verified — the server (MCU) call path

* All calls that route through the media server (every call today, and
  all 3+ group calls) are **unaffected and re-verified end-to-end**:
  180 H.264 frames flowing publisher → server → subscriber in the
  automated harness, with no regression from any of the above.

### Added — test infrastructure

* **`TALQ_TEST_P2P` harness mode** exercises the private overlay,
  ICE, DTLS and the peer-to-peer media chain between the two bot
  accounts. The overlay relay, ICE connectivity and call teardown are
  verified; full two-way decoded-frame delivery is validated on real
  separate machines (the single-process harness runs two full media
  engines at once, which the direct test cannot fully represent).

---

## v0.41.8 "Koprivshtitsa" — BETA (2026-05-28)

Multi-device self-ring suppression.

### Fixed

* **Sibling-device self-ring on outgoing call.** When you start a
  call from one device (e.g. laptop A) while signed in on another
  (e.g. laptop B at home), laptop B previously rang as if the
  call were incoming — `ConversationListModel`'s
  `hasCall: false → true` transition detector didn't check who
  started the call. **Now**: the same JSON also carries
  `participantInCallFlags` — when non-zero it means OUR user is
  already in the call (from the other device), so we silently
  log + skip the ring. Mirrors upstream Talk's web client
  behaviour and matches the userId-comparison check already in
  `CallManager`'s signaling-based detection path.

### Still open (P2P-for-1:1)

* The experimental "Direct P2P for 1:1 calls" toggle remains
  broken on this build: `PeerPipeline` hard-pins camera caps to
  `image/jpeg,1920x1080@30` (or 1280×720), which fails on
  cameras that don't advertise that exact MJPEG mode (field
  reproduction: `mfvideosrc0 error: streaming stopped, reason
  not-negotiated`). It also doesn't release the camera cleanly
  on stop, so a failed P2P attempt can leave the next MCU call's
  camera in a wedged state until TalQ is restarted. **Workaround
  for now**: keep "Direct P2P for 1:1 calls" OFF in Settings.
  Proper fix queued for the PeerPipeline → PublishPipeline
  unification work.

---

## v0.41.7 "Koprivshtitsa" — BETA (2026-05-28)

One-line fix to the P2P-for-1:1 code path so it can actually
carry video.

### Fixed

* **P2P call camera now auto-enables on video calls.** The MCU
  branch of `CallManager` has always called
  `m_publishPipeline->enableCamera()` immediately after the
  pipeline is set up when `m_cameraOn` is true (mirrors what an
  outgoing or accepted-with-video call expects). The P2P branch
  (added in 0.41.5 behind the experimental
  `Call/preferP2pFor1to1` toggle) was missing the equivalent
  call — `PeerPipeline::start()` only configures audio, leaving
  the camera unattached until the user manually toggled it.
  Symptom: a video call in P2P mode came up audio-only and the
  remote saw nothing. Now mirrors the MCU branch's auto-enable.

### Still open in P2P mode (deferred to 0.41.8 / its own session)

* Encoder parity: `PeerPipeline` uses `openh264enc` while the
  MCU path uses the configurable `makeWebrtcVideoEncoder()`
  factory (nvenc / qsv / mfh264 / x264enc probe with psy-tune).
  Swap is straightforward but needs renegotiation-flow
  verification.
* Larger architectural unification — make P2P-for-1:1 reuse the
  `PublishPipeline` + `SubscribePipeline` pair the MCU path
  uses, with SDP routed peer-to-peer via signaling. Cleaner
  long-term than the parallel `PeerPipeline` codebase.

---

## v0.41.6 "Koprivshtitsa" — BETA (2026-05-28)

Three backlog items + diagnostics for an active field bug.

### Added

* **Reply-chain fallback for thread membership.** When a peer's
  client posted into a thread but didn't tag `threadId` on the
  POST body (older TalQ builds, upstream web Talk, etc.), the
  message previously failed the strict thread filter and the
  topic showed zero messages even though the messages were in
  the room. `passesThreadFilter` now ALSO admits a message if
  its `replyTo.id` equals the current thread's seed id —
  matches upstream Talk's `checkIfBelongsToContext` predicate.
* **Auto-cap on sustained packet loss.** `PublishPipeline`
  captures the original GCC ceiling (`m_originalMaxBitrate`)
  on call start. If the BWE estimate stays below half of that
  for 15 s, `m_maxBitrate` is clamped to `estimate × 1.2` for
  the rest of the call (or until the BWE recovers above 70 %
  of the original). Protects against runaway probing on a
  saturated uplink — the "both video streams froze mid-call at
  22 Mbps" class of bug.

### Added (diagnostics)

* **`refreshLatest` drop tracing** — logs at `qInfo` level when
  a reply is discarded by the generation guard or by a token
  switch. Helps RCA an active field report: new messages visible
  in the conversation list but not in the chat history; the
  hypothesis is a tab-switch race between `setToken` and the
  in-flight `refreshLatest` reply.

### Deferred

* **`PeerPipeline` add-camera-after-audio renegotiation** —
  investigated; the renegotiation IS wired (`enableCamera()`
  calls `createOffer()` after attaching the video chain), but
  testing under HPB-relayed signaling shows the camera-enable
  step failing in the experimental P2P-for-1:1 path. Root cause
  is in the SDP/transceiver state during renegotiation — needs
  two-peer end-to-end debugging, not a quick patch. P2P-for-1:1
  stays opt-in via Settings.

---

## v0.41.5 "Koprivshtitsa" — BETA (2026-05-28)

In-call chrome legibility on 2K panels, a new `MODE` pill, and an
opt-in "direct P2P for 1:1 calls" toggle.

### Changed (in-call UI)

* **Bigger top-row chrome.** Status pill is now 11 px mono on a
  26 px-tall card-style background (was 9 px on a transparent
  outline). Info chips (CODEC / QUALITY / RX) and action buttons
  (QUALITY / BACKGROUND / SHARE) scale to match — 9/11 px mono,
  26 px tall, 11 px radius. Field report: 7/9 px on 1440p was
  unreadable at default Windows scaling.
* **Status pill LED stays readable on light video backgrounds.**
  Added a dark contrast ring around the breathing dot and floored
  the pulse alpha at 0.55 so it never washes out completely.
* **`MODE` pill in the telemetry row.** Reads `P2P` (green LED)
  when the call is direct WebRTC peer-to-peer, `MCU` (amber) when
  it's SFU-forwarded. Decided once per call at signaling-room-join
  time; stable for the call's lifetime.

### Added (Settings → Audio & Video)

* **"Direct P2P for 1:1 calls (experimental)"** checkbox. Default
  OFF. When on, 1:1 calls (room type == 1) bypass the SFU and go
  peer-to-peer for lower latency — useful when both peers are in
  the same region and the SFU is far away (e.g. BG↔BG with an
  SA-hosted HPB). Group calls (3+) always stay on the SFU. The
  decision is per-call: if direct ICE fails, TalQ falls back to
  the MCU automatically.
* Note: the experimental tag is honest — `PeerPipeline`'s audio→
  video renegotiation path needs more verification on HPB-relayed
  signaling. Camera enable was observed failing in a 1:1 P2P call
  during testing; turning the toggle OFF restores the MCU path
  (the 0.40 / 0.41.0–0.41.4 behaviour) until the renegotiation
  flow is hardened.

### Known follow-ups (queued for 0.41.6-beta)

* `PeerPipeline` add-camera-after-audio-handshake hardening so
  P2P-for-1:1 can be the default.
* Auto-cap published resolution on sustained packet loss (GCC
  estimate < threshold for N seconds → clamp HIGH-layer target).
* Reply-chain fallback for thread membership (admit messages
  tagged without `threadId` but with `replyTo` pointing at the
  seed).

---

## v0.41.4 "Koprivshtitsa" — BETA (2026-05-28)

UX polish on the 0.41.0 resolution work + a stickier `revertStuckCall`.

### Added

* **Presentation-mode gate for 2K / 4K.** Settings → Audio & Video →
  "Presentation mode" toggle. Off by default; **the Maximum send
  resolution combo only exposes 720p HD + 1080p Full HD until the
  toggle is on**. Turning it on extends the combo with 1440p 2K and
  2160p 4K. Field-driven by the 22.69 Mbps mid-call freeze where the
  HIGH simulcast layer at 4K saturated both peers' uplinks.
  Auto-downgrades a saved 1440/2160 value to 1080 when the toggle is
  switched OFF, so a casual user can't strand themselves at a tier
  the link can't sustain.

### Changed

* **In-call QUALITY dropdown labels now track the publisher's
  Maximum send resolution.** Was: hard-coded "High (720p)" regardless
  of what the publisher actually sent. Now: `High (720p)` / `High
  (1080p)` / `High (2K)` / `High (4K)` depending on the local
  Settings choice (best-guess symmetric peer config). Applies to the
  button label, the dropdown menu entry, and the hover tooltip.
  Low / Medium / Auto labels unchanged.

### Fixed

* **`revertStuckCall` two-stage clear.** The Talk-specific
  `/user_status/revert/call` endpoint silently no-ops on some NC
  builds (verified on the ZA-hosted instance), so a user-status
  custom message of "In a call" stayed sticky after hangup. The
  revert call now always chains a standard
  `DELETE /user_status/message` after, which is the documented
  endpoint to clear the auto-applied custom-status component. Both
  calls are idempotent — if the first one worked, the second is a
  cheap no-op; if the first didn't, the second unsticks the field
  case. Then `fetchCurrent()` refreshes the local mirror.

### Known follow-ups (queued for 0.41.5-beta)

* P2P-first toggle for 1:1 calls when both peers are in the same
  region (BG/SA HPB latency penalty observed).
* Auto-cap published resolution on sustained packet loss (GCC
  estimate < threshold for N seconds → clamp HIGH-layer target).
* Reply-chain fallback for thread membership (catch messages tagged
  without `threadId` but whose `replyTo` points at the seed).

---

## v0.41.3 "Koprivshtitsa" — BETA (2026-05-28)

Chat history sync hardening, modelled after the upstream Nextcloud
Talk web client. Closes two of the three load-bearing divergences
documented in `docs/superpowers/specs/2026-05-28-upstream-talk-chat-history.md`.

### Fixed

* **`referenceId` dedup on send.** Every outgoing message now
  carries a SHA-256-hex `referenceId` in the POST body (mirrors
  upstream's `prepareTemporaryMessage`). When the long-poll
  catches the same message back — either before OR after the
  POST callback resolves — the receive paths
  (`onMessagesReceived`, `refreshLatest`, `runGapFillStep`) call
  `replaceTempByReferenceId` to remove the optimistic temp by
  referenceId match instead of by numeric tempId. Fixes the field
  bug "first sent message disappears after sending a second" —
  back-to-back sends previously raced the POST callbacks against
  the long-poll and the loser stranded.
* **Long-poll no longer passes `threadId` server-side.** Upstream
  Talk's `pollNewMessages` deliberately omits `threadId` and
  filters by thread in the client. TalQ now matches: dropped the
  `threadId=` query param from `MessagePoller::poll()`, added
  `passesThreadFilter()` in `MessageListModel` for the client-
  side check (used everywhere a message is admitted into the
  model — cache load, `onMessagesReceived`, `refreshLatest`,
  `loadHistory`, gap-fill). Why this matters: when a peer client
  (older TalQ, or upstream web) posted into the room WITHOUT
  attaching a clean `threadId`, our server-side filter excluded
  those messages from the active thread tab's poll → "topic shows
  zero messages" symptom.
* **Client-side filter admits own optimistic temps unconditionally**
  so a just-sent message doesn't vanish if the user switches tabs
  while the POST is still in flight. Upstream does the same
  (`String(id).startsWith('temp-')` exception).

### Changed

* **Playback-volume control removed from Settings → Audio & Video.**
  Telegram, Zoom, Meet do not surface a manual playback gain;
  their receive-side AGC handles it. TalQ's
  `audiodynamic`+`rglimiter` chain (added in 0.41.0) does the
  same job. The 1.8× internal default stays via the
  `QSettings/Audio/playbackGain` key for power users.

### Known follow-ups

* The "Refunds thread shows zero messages" symptom may persist if
  the OTHER side's messages were genuinely never tagged with a
  `threadId` server-side AND don't have a reply-chain to the
  seed. A reply-chain fallback is queued for 0.41.4-beta —
  needs to validate behaviour against the upstream
  `checkIfBelongsToContext` predicate first.
* `revertStuckCall()` returning 404 on this server (per field
  report: status stays "In a call" after hangup) is queued for
  0.41.4-beta — direction is to DELETE the auto-status by id via
  the standard `/apps/user_status/api/v1/user_status` endpoint
  instead of the Talk-specific `/heartbeat/revert/call` path that
  this NC build doesn't expose.

---

## v0.41.2 "Koprivshtitsa" — BETA (2026-05-28)

Multi-device chat history sync fix + diagnostic logging for the
"zero messages inside a topic tab" field report.

### Fixed

* **Multi-device gap-fill on chat open.** When the local cache held
  messages 1–100 but the server's latest 50 returned IDs 251–300
  (because another device sent ~200 messages while this client
  was offline), the model silently left 101–250 unreachable —
  `m_oldestMessageId` was set to the cache's oldest (1), so
  pagination via scroll-up returned empty and `m_hasMoreHistory`
  flipped to false. **Now**: `refreshLatest()` detects when the
  oldest fetched ID is more than one above the newest pre-refresh
  cached ID, then runs a backfill loop (`runGapFillStep`) that
  pages `lookIntoFuture=0 limit=100` from `oldestFetched` backward
  until the gap closes (or hits a 20-page = 2000-message budget
  cap). Applies to both general chats and threads.

### Added (diagnostics)

* `sendMessage` logs `token + threadId + replyTo + length` at
  `qInfo` level so we can verify the sender-side actually
  attached the expected `threadId`.
* `refreshLatest` logs thread-stats: how many fetched messages had
  `threadId > 0` vs were thread-less. Helps tell apart "server has
  the messages but tagged with a different/no threadId" from
  "server has nothing under this token".

### Known follow-ups

* Field reports for "topic tab shows zero messages" and "first
  sent message disappears after sending a second" remain under
  investigation. A research pass against the upstream Nextcloud
  Talk web client's reference chat-sync flow is queued; concrete
  fixes will land in 0.41.3-beta.

---

## v0.41.1 "Koprivshtitsa" — BETA (2026-05-28)

Screen-share UX work that was deferred from 0.41.0. Two changes,
both visible only while screen-sharing is live.

### Added

* **In-call SHARE quality dropdown.** A third action button appears
  in the top-right action row whenever a screen share is active,
  alongside QUALITY and BACKGROUND. Click opens a menu with
  720p / 1080p / 1440p / Native — same options that were previously
  hidden behind a right-click on the bottom share button. The
  current selection is shown directly on the button and a tooltip
  appears on hover.
* **Self-preview tile for the screen being shared.** A second small
  PiP appears in the corner opposite the camera self-PiP while
  sharing, framed in the warm-danger accent so it reads as "this
  is what they see". Fed by an `appsink` tee on the
  `ScreenSharePipeline` AFTER the configured downscale, so the
  preview shows the EXACT post-encode content the peer receives —
  not the raw monitor. A red "SHARING" badge in the corner of the
  tile makes the role obvious even when the frame is briefly
  black during initialisation. Tee construction is best-effort:
  if any of the four extra elements (`tee`, `queue`,
  `videoconvert`, `appsink`) can't be made, the share itself runs
  un-teed exactly as in 0.41.0.

### Notes

* The screen-share UX consensus from
  `docs/superpowers/specs/2026-05-28-screenshare-ux-research.md`:
  the camera self-view is **never** replaced by the share preview
  (Zoom, Teams, Meet, Telegram all keep both visible). TalQ
  matches that — the camera PiP stays put, the share PiP takes
  the opposite corner.
* OS-level coloured monitor borders (Zoom green / Teams red) are
  still deferred behind a future flag — they need per-platform
  transparent click-through overlay windows.

---

## v0.41.0 "Koprivshtitsa" — BETA (2026-05-28)

Opens the 0.41.x beta cycle. Field-driven call-quality work —
remote-peer playback is much quieter than Telegram on the same
hardware (despite healthy VU meters on both ends) and 720p30 looks
softer than Telegram on the same camera. Research briefs in
`docs/superpowers/specs/2026-05-28-*.md`.

### Changed (audio)

* **Receive-side loudness chain.** Vanilla `opusdec → wasapi2sink`
  ran playback at unity gain with no AGC, no compressor, no limiter —
  Telegram/Discord/Zoom/Meet all run libwebrtc's `AudioProcessing`
  (AGC2 + soft-limiter) on playout. TalQ now inserts
  `audiodynamic` (soft-knee compressor, threshold 0.25, ratio 0.4)
  + `volume` + `rglimiter` between `opusdec` and the resampler in
  both `PeerPipeline` (P2P) and `SubscribePipeline` (MCU). Biggest
  perceptible win in the cycle. Default playback gain 1.8×;
  user-tweakable via Settings → Audio & Video → Playback volume.
* **Opus voice-mode + FEC on publish.** `opusenc` was at defaults
  (64 kbps, `audio-type=generic`, `inband-fec=false`, `dtx=false`).
  Now: `bitrate=48000`, `audio-type=voice` (SILK-hybrid voice path),
  `inband-fec=true`, `dtx=false`, `bitrate-type=cbr`, `complexity=10`.
  Matches the tgcalls recipe.
* **Opus SDP fmtp munge.** Local offer now carries
  `useinbandfec=1; usedtx=0; maxaveragebitrate=48000; minptime=10`
  on the Opus payload so the peer also encodes voice-mode + FEC.
  Without this only OUR encode benefits; the peer stays default-mode.

### Added (video resolution)

* **Maximum send resolution selector.** Settings → Audio & Video →
  "Maximum send resolution" picks **720p HD / 1080p Full HD / 1440p
  2K / 2160p 4K**. Default 720 = 0.40 behaviour. The HIGH simulcast
  layer + the shared output caps + the GCC bitrate ceiling all scale
  with the choice: 720p caps at 6 Mbps, 1080p at 9 Mbps, 1440p at
  18 Mbps, 2160p at 30 Mbps. L/M simulcast layers stay 180p/360p so
  bandwidth-constrained subscribers still get a usable stream.
* Pre-0.41 behaviour was: `sharedCaps` pinned to 1280×720@30
  regardless of what the camera advertised, so HD/2K/4K capture
  was wasted via downscale.

### Changed (video quality)

* **`x264enc` psy-tune.** Was `none` (the GStreamer default); now
  `film` for camera and `animation` for screen. Single biggest
  visible-quality delta at the same bitrate — sharper motion edges
  on camera, preserved text edges + better scroll handling on
  screen. Only affects the software-fallback encoder; hardware
  (`nvh264enc`/`qsvh264enc`/`mfh264enc`) already uses its own
  quality preset.
* `x264enc` also gets `qp-min=18 qp-max=42` (camera), `qp-min=14`
  (screen, crisp text), `vbv-buf-capacity=1000`, and the screen
  branch enables `intra-refresh=true`.
* **HIGH simulcast layer bumped 2.5 → 3.5 Mbps nominal at 720p.**
  Scales upward with the resolution selector above.

### Notes

* This is a **beta** — publishes to the ncloud
  `talq-beta-latest.json` channel only; the stable manifest is
  untouched. Stable users on 0.40.16 are unaffected.
* Next on the cycle: 0.41.1-beta adds the screen-share self-preview
  tile and surfaces the existing screen-share quality picker in the
  in-call dropdowns. Will land after field validation of this beta.
* Codename honours the 150th-anniversary thread: 0.39.x betas
  "Aprilsko Vastanie" → 0.40.x stable "Panagyurishte" → 0.41.x
  betas **"Koprivshtitsa"** (the mountain town where Todor
  Kableshkov sent the "bloody letter" that sparked the uprising
  several days before Panagyurishte's Oborishte assembly declared
  it).

---

## v0.40.16 "Panagyurishte" — STABLE (2026-05-27)

Auto-install idle gate now keys off **TalQ-input** rather than
system-wide input, and the last-minute countdown also fires a tray
notification so a backgrounded TalQ can't surprise-install.

### Changed (auto-install gate)

* **Idle is measured against TalQ activity, not desktop activity.**
  The previous `GetLastInputInfo` gate reset the countdown any time
  the user touched the keyboard or mouse anywhere on the desktop —
  even in a browser or IDE. Replaced with an app-level `QApplication`
  event filter that timestamps `m_lastTalqInputMs` on every
  `MouseButtonPress` / `KeyPress` / `Wheel` event reaching our
  process. Events targeted at other apps never enter our event loop,
  so working in another window now correctly DOES let TalQ count up
  toward the install window. Bare `MouseMove` is excluded so a stray
  cursor drift across TalQ's edge doesn't reset the timer.
* **Tray toast when the countdown enters its final minute.** Fires
  once per cycle via `NotificationManager::notify`. The inline
  in-window banner is invisible while TalQ is minimised / in the
  tray, so without this the user only saw the "Installing in M:SS…"
  message after they happened to bring TalQ to focus. The toast
  gives them a real chance to alt-tab over and cancel.

---

## v0.40.15 "Panagyurishte" — STABLE (2026-05-27)

Mission Control visual pass on the in-call surface, dropdown menus
for the QUALITY / BACKGROUND controls, a "Waiting for others to
join" RCA, and a long-overdue ring-only test harness mode.

### Changed (in-call UI)

* **Status pill, info chips, action buttons reskinned** to match the
  empty-state Mission Control vocabulary (bg-surface card, divider
  border, leading LED, monospace KEY + VAL). Sidebar → call now
  speaks one language. Status pill borrows the home's exact pattern:
  border + text both in the LED colour, no fill ("● IN CALL · 00:42"
  with a breathing dot; transient states get an ellipsis).
* **All top-row chrome lives on one line**: status pill, then
  `CODEC H264 · HW`, then `QUALITY HIGH` (the live substream the SFU
  is forwarding — distinct from the QUALITY dropdown, which is what
  you request), then `RX 720p`. Compact 20-px tiles, 7/9-px mono.
* **Click-cycle replaced with dropdown menus.** QUALITY opens a
  proper menu (Auto / Low 180p / Medium 360p / High 720p) with a
  checkmark on the current selection; BACKGROUND opens Off / Blur /
  Image plus an "Open background settings…" entry that jumps to
  Settings → Audio & Video (the home of the blur slider + image
  picker). No more "I need to click three times to go back".
* **Top chrome + control bar fade together on idle** (5 s, smooth
  ~250 ms ease, instant for reduced-motion users). Mouse motion
  brings them back. Status pill stays on always — Mission Control
  "calm glance".
* **Double-click on any chip no longer toggles fullscreen.** Each
  rect (status, codec, quality stat, RX, both dropdown buttons) is
  tracked per-paint and the double-click handler iterates them; only
  the bare video surface still goes fullscreen on double-click.
* **Open dropdown pins the chrome.** Menu can't fade out from under
  your cursor.

### Fixed

* **"Waiting for others to join" after answering an incoming call.**
  Root cause: `SignalingClient::participantJoinedCall` fires exactly
  once per `prevFlags=0 → inCall>0` transition. If the event arrived
  while our state was `Idle` (i.e. while we were ringing), the peer
  was used to drive the ring UI but never added to `m_participants`.
  Once the user accepted, the state moved Outgoing → Connecting →
  Active without any further join event, so `remotes==0`, no tile
  was laid out, and the centered "waiting" fallback rendered even
  though the peer was plainly in the room server-side. Fix: also
  `ensureParticipant` while state==Incoming.
* **Internal status detail no longer leaks into the centered sub-
  line.** "Publisher ICE connected" / "Joining room" / "Fetching
  servers" were useful on the dev console, never on the user-facing
  surface. The sub-line now stays on calm phrasing.
* **Trailing seconds digit in the status pill no longer clipped.**
  Old `text+26` width with `-8` right-margin left a 2-px deficit;
  now `text+34` with `-10`.

### Added (test harness)

* **`TALQ_TEST_RING_ONLY=1` mode for `talq-call-test`.** Peer A
  signaling-joins room + call (bot account, no media, no peer B),
  sits until `--timeout`, hangs up cleanly. Used to ring a real
  human's TalQ — e.g. test-talq calling `u2f3gbu4` rings a real user —
  for manual in-call UI verification without needing a second
  laptop. Default `--timeout 60`; ring-only is allowed up to 300.

### Refactor

* Lazy `MainWindow::ensureSettingsDialog()` helper extracted from
  the inline sidebar-button click body. Both the button and the
  new in-call BACKGROUND→"Open background settings…" entry now go
  through it, so the signal wiring can't drift.
* `m_topChromeRects` consolidates the per-paint hit rects for the
  top row (status, codec, quality stat, RX). Cleared at the top of
  every `paintEvent`; the double-click handler iterates the vector
  instead of `||`-chaining six members.
* Removed dead `paintCodecPill` rename-stub (the legacy entry that
  only forwarded to the new split helpers).

---

## v0.40.14 "Panagyurishte" — STABLE (2026-05-27)

Hotfix for 0.40.13 — the push_sample fix only covered HALF the
BG-bridge code path.

### Fixed

* **PiP works with BG-blur enabled** (the case 0.40.13 missed).
  `onBgSample`'s on-mode path (BG-blur active: engine actually
  processes each frame) still used `gst_app_src_push_buffer` for
  the processed buffer AND for the three error-fallback paths
  (caps shifted off BGRx, buffer map failed, buffer too small).
  Same caps-propagation problem as 0.40.13's off-mode bug: the
  appsrc's static caps with no width/height got forwarded to
  downstream, preview-convert and preview-appsink saw unbounded
  caps, PiP stayed black. Switched all four push sites to
  `gst_app_src_push_sample` — the processed-buffer path builds a
  new GstSample with the input caps (which carry concrete BGRx
  dims + framerate); the fallbacks push the original input sample
  through. Field-verified with BG-blur ON on the affected device.

---

## v0.40.13 "Panagyurishte" — STABLE (2026-05-27)

The actual PiP fix. 0.40.9–0.40.12 were chasing guesses. This one
was RCA'd against live ground-truth via TALQ_DEBUG_PIPELINE state
dumps — the diagnostic instrumentation is kept in the source
(env-gated, zero runtime cost when off) for any future recurrence.

### Fixed

* **Local PiP and remote video both work in release builds.** Two
  separate bugs in the BG-bridge stalling the call's video chain:

  1. **Preroll circular dependency.** BaseSink's default
     `async=TRUE` makes appsinks wait for a preroll buffer in
     PAUSED before the pipeline can reach PLAYING. With the BG
     bridge in the chain (`bg-appsink` → callback → `bg-appsrc`),
     `bg-appsink` only emits `new-preroll` (not `new-sample`) in
     PAUSED, our callback only hooks `new-sample`, so no buffer
     propagates to `bg-appsrc`, `preview-appsink` never gets a
     preroll buffer, and the pipeline stays in `pending=PLAYING`
     forever. Fixed by `async=FALSE` on both appsinks — they
     transition straight to data-flow without waiting on preroll.

  2. **Caps event lost across the BG bridge.** `onBgSample`'s
     off-mode (BG-blur off) path called `gst_app_src_push_buffer`,
     which leaves `bg-appsrc`'s static caps property (`BGRx` with
     no width/height/framerate) in place downstream. The preview
     branch and encoder branch then negotiated against unbounded
     caps and either fixated to garbage (`width=1, height=4095`)
     or never got a usable caps event at all — no PiP, no remote
     video. Fixed by `gst_app_src_push_sample`, which forwards the
     sample's full caps (camera's real BGRx + concrete dims +
     framerate) to downstream.

* **Pipeline-state instrumentation kept in source.** Set
  `TALQ_DEBUG_PIPELINE=1` and the dev/branded binary writes
  enableCamera-state dumps (at +200ms / +1s / +3s) and appsink
  callback / `feedFrame` counters to `C:\temp\talq-pipe.log`. Off
  by default; zero runtime cost when unset. Reusable for any
  future "frames aren't reaching X" investigation without having
  to rebuild for one-shot instrumentation.

---

## v0.40.12 "Panagyurishte" — STABLE (2026-05-27)

Cleanup of 0.40.9–0.40.11. The previous three releases were chasing
the wrong tail.

### Fixed

* **PiP / call camera path restored to the 0.40.8 baseline.** The
  explicit `gst_element_set_state(..., PLAYING)` block introduced in
  0.40.9 was based on a flawed dev test where the BG bridge was
  bypassed. With the bridge present (real release builds), forcing
  an `appsrc` (`m_bgAppsrc`) to PLAYING before any buffer with
  concrete dims is pushed makes downstream fixate caps to garbage
  (width=1, height=4095) — pipeline aborts with `not-negotiated`.
  The explicit forces are removed; the camera chain transitions via
  `sync_state_with_parent` only, matching pre-0.40.9 behavior. The
  other 0.40.x fixes (call-freeze on hangup, topic-mode polish,
  idle-only banner) are preserved.

---

## v0.40.11 "Panagyurishte" — STABLE (2026-05-27)

Hotfix for 0.40.10 — the BG-bridge appsink fix went one element too far.

### Fixed

* **Pipeline no longer aborts with "not-negotiated" on call start.**
  0.40.10 explicitly promoted `m_bgAppsrc` to PLAYING alongside the
  appsinks. An appsrc has a streaming task that wakes the moment the
  element hits PLAYING; if we promote it before `onBgSample` has
  pushed a real buffer with concrete dims, downstream negotiates
  against the appsrc's caps property (BGRx with no width/height) and
  fixates to garbage (width=1, height=4095) — the pipeline then
  aborts with `not-negotiated` and the call fails outright (no PiP,
  no remote video, GST_DEBUG full of `videoconvertscale` and
  `videofilter` errors). This release drops the appsrc force; only
  the two appsinks (`m_bgAppsink`, `m_previewAppsink`) are pinned
  to PLAYING explicitly. The appsrc transitions naturally once the
  bg sink delivers the first real-dim sample.

---

## v0.40.10 "Panagyurishte" — STABLE (2026-05-27)

Hotfix for 0.40.9 — the PiP fix was incomplete.

### Fixed

* **Local PiP works in real (release) builds.** 0.40.9 promoted the
  preview-branch appsink (`m_previewAppsink`) to PLAYING but missed
  the BG-bridge appsink (`m_bgAppsink`) — which sits UPSTREAM of the
  camera-tee. With the bridge appsink stuck in PAUSED, its
  `new-sample` signal never fires, no buffers flow downstream, and
  every consumer (encoder + preview) starves identically. The
  symptom looked exactly like the original 0.40.7 PiP-black bug.
  `enableCamera` now promotes both appsinks AND the appsrc
  explicitly. (The 0.40.9 dev-build appeared to work because that
  build had the BG bridge bypassed entirely — masking the residual
  bug from RCA. Sorry for the bounce.)

---

## v0.40.9 "Panagyurishte" — STABLE (2026-05-27)

Topic-mode polish + call-path recovery. Three RCAs in one release.

### Fixed

* **Local self-view PiP works again during video calls.** Field-RCA'd
  2026-05-27: the preview branch appsink stayed in PAUSED after
  `gst_element_sync_state_with_parent`, even with the parent pipeline
  in PLAYING. An appsink in PAUSED emits `new-preroll` (which nothing
  listens for) instead of `new-sample` (which our callback hooks), so
  hundreds of camera frames per second hit the queue, got dropped at
  `max-buffers=1`, and the PiP stayed empty forever. `enableCamera`
  now promotes the preview-queue, preview-convert and preview-appsink
  to PLAYING with explicit `gst_element_set_state` calls after the
  sync-with-parent batch. The regression was hiding behind the
  Phase 3.3b BG-bridge work — on most setups sync-with-parent
  happens to transition the sink past PAUSED; on the affected
  hardware it didn't, and the symptom was a permanently black
  "Starting camera…" overlay.

* **Hang-up no longer freezes the UI when BG-blur is on.** The
  synchronous `gst_element_set_state(m_cameraSrc, GST_STATE_NULL)`
  inside `disableCamera` and the pipeline-NULL inside `cleanup`
  could park Qt main on the camera pad's stream lock for the entire
  duration of an in-flight BG-engine round-trip (GL + ORT). Both
  state changes now run on a worker thread:
  `gst_element_call_async` for the camera source, a one-shot
  `std::thread` for the cleanup pipeline-NULL. The UI stays
  responsive; the camera LED still releases, just a beat after the
  user clicks hang-up.

* **Messages sent in a topic no longer appear as replies to the seed.**
  The composer used to set `replyTo = m_activeThreadId` whenever a
  topic was open, which the server rendered as "replying to 📌
  `<title>`" on every message. Talk has a proper top-level `threadId`
  parameter for posting INTO a thread; `MessageListModel::sendMessage`
  now wires `m_threadId` (already tracked) onto the POST body and the
  composer-send handler stops overloading `replyTo`. Explicit
  reply-to-a-specific-message still works exactly as before.

* **Topic bar now highlights the active topic.** Opening a topic via
  the bar, the threads list, or the right-click "Thread" action
  switched the chat to topic mode but left "All messages" lit on the
  bar. `openThread` / `closeThread` now drive
  `TopicTabBar::setSelectedThreadId`, so the visible state matches
  what's actually filtered in the message list.

* **Auto-update banner stays hidden while you're using TalQ.** It
  used to sit above the chat permanently from download-complete on,
  swapping between "auto-install when idle…", "paused because you're
  in a call", and the final countdown. Now: while you're in a call,
  typing, mid-upload or holding a mouse button, the banner is fully
  hidden. Even after the gates clear it stays hidden until you've
  been idle 30 s — or until the install is in its final 60-second
  countdown, which is always visible so you can cancel.

---

## v0.40.8 "Panagyurishte" — STABLE (2026-05-27)

0.40.7 fixed the READING side of topics (the chip now appears once the
server has a real thread); this fixes the WRITING side, plus turns down
the auto-install banner so it doesn't sit above the chat all day.

### Fixed

* **"New topic" actually creates a topic now.** `MainWindow::createNewTopic`
  used to send a plain seed message and then call a separate
  `setChatThreadTitle` that tried four different `POST`/`PUT` URL shapes
  against `/chat/{token}/{messageId}/thread` — *none of which exist in
  Talk v23.0.4*. Every variant returned `998 Invalid query` and the
  error was silently swallowed (`bool /*ok2*/`), so what looked like a
  named topic was really just a chat line with a pin emoji. The
  correct Talk API takes a top-level `threadTitle` parameter on the
  original `POST /chat/{token}` send-message call: when non-empty and
  `replyTo == 0`, the server's `ChatController` creates a new thread
  rooted at that message in the same round-trip. `sendChatMessage`
  now accepts an optional `threadTitle`; `createNewTopic` passes it
  directly; the broken `setChatThreadTitle` endpoint chain is removed.

### Changed

* **Auto-install banner is now hidden while you're actively using
  TalQ.** Previously the banner sat above the chat permanently from
  download-complete onward, only changing its label between
  "auto-install when idle...", "paused because you're in a call",
  and the final countdown. That label was useful debug telemetry
  but visually loud during a workday. Now: while you're in a call,
  typing, mid-upload, or holding a mouse button, the banner is
  fully hidden. Even when the gates clear, it stays hidden until
  you've been idle for 30 s — *or* the install is in its final
  60-second countdown, which is always visible so you can cancel.

---

## v0.40.7 "Panagyurishte" — STABLE (2026-05-27)

Follow-up to 0.40.5 (topics in 1:1 chats). Creating a topic worked, but
the new topic chip never appeared in the bar — the bar showed only
"★ All messages" and a stray "# General" chip. Both root causes fixed.

### Fixed

* **Newly-created topic was invisible until the first reply.**
  `ThreadListModel::fetchThreads` filtered messages by `isThread:true`,
  but the Talk API only sets that flag on REPLIES inside a thread —
  the thread root (the seed message) carries `threadTitle` instead.
  A freshly-created topic with zero replies therefore matched nothing
  and never made it into the model. Now the scan also accepts roots
  by recognising a non-empty `threadTitle`; the existing reply count
  still comes exclusively from `isThread:true` messages so it doesn't
  double-count the root.

* **Duplicate "# General" chip in the topic bar.** `TopicTabBar::rebuild`
  iterated every model row, including row 0 — the synthetic "All
  Messages" placeholder the model prepends (titled "General"). That
  produced both the leading "★ All messages" chip AND a second
  "# General" chip for the same logical entry, which looked like
  TalQ had silently auto-created a "General" topic. The loop now
  skips rows tagged `isAllMessages`.

* **Cached thread index was disabled for 1:1 chats.**
  `onCachedThreadsLoaded` bailed when `m_conversationType == 1`,
  left over from the days when topics were group-only. Reopening a
  P2P with an existing topic now paints the bar instantly from
  cache instead of waiting for the API round-trip.

---

## v0.40.6 "Panagyurishte" — STABLE (2026-05-26)

### Fixed

* **Auto-install skipped the countdown when the user was already idle
  during the download.** `GetLastInputInfo` is system-wide; if the
  user was passively watching the download (no mouse/keyboard) for
  longer than the configured idle threshold, the gate passed
  instantly on the first tick and TalQ restarted with no visible
  "Installing in M:SS…" banner. Now the effective idle time is
  clamped to "ms since download completed", so the countdown always
  runs at least once. Tick rate dropped to 1 s for a smooth MM:SS
  display.

---

## v0.40.5 "Panagyurishte" — STABLE (2026-05-26)

### Added

* **Topics + threads now work in 1:1 conversations too.** The right-
  click "💬 Thread" action and the topics side panel (with "+ New
  topic") were previously gated to group/public rooms only. The
  underlying flow — send a seed message, name it as a thread — is
  type-agnostic, and upstream Talk's web client already supports
  threads in 1:1 chats. Useful for separating distinct conversation
  threads with the same contact.

---

## v0.40.4 "Panagyurishte" — STABLE (2026-05-26)

Auto-update is now truly end-to-end automatic. 0.40.2 added auto-install
on idle, but the download itself still required a manual "Install now"
click. With the auto-install setting ON (default), the download now
starts the moment the manifest poll finds a new version; the idle gate
then takes over once the download finishes.

### Changed

* **Auto-download paired with auto-install.** When "Install updates
  when I'm idle" is enabled (Settings → Updates, default ON), the
  download begins as soon as a new version is detected — no click
  needed. The "Install now / Later" buttons stay visible during the
  download in case you want to cancel the auto-flow for this session.

---

## v0.40.3 "Panagyurishte" — STABLE (2026-05-26)

One small fix on top of 0.40.2; first stable to exercise the new
auto-install-on-idle flow end to end.

### Fixed

* **Settings → Background camera preview honours 4:3 vs 16:9.** The
  preview pipeline was forcing a 640×360 (16:9) caps box on every
  camera, so 4:3 webcams looked vertically squished and didn't match
  the actual call output (which never had this box). Caps now fix
  width only — videoscale derives height from the camera's native
  pixel-aspect-ratio. A 4:3 camera lands as 640×480 internally, then
  the 320×180 preview QLabel letterboxes it with `Qt::KeepAspectRatio`.

---

## v0.40.2 "Panagyurishte" — STABLE (2026-05-26)

Rolls 0.40.1's call-freeze + edit-message hotfixes into stable plus one
small feature on top. Stable channel; even-minor parity rule relaxed to
keep everyone on a single line this cycle.

### Added

* **Auto-install on idle (default ON).** After a new version is
  downloaded, TalQ now restarts itself once you've been idle for a
  configurable window (1 / 5 / 15 minutes — default 5). A countdown
  appears in the last minute; touching the keyboard or mouse resets it,
  and the install is hard-gated against active calls, unsent composer
  text, and in-progress uploads. Settings → Updates → "Install updates
  when I'm idle" to disable or change the wait. "Install now" still
  works as before, and the auto-flow can be cancelled per session
  without flipping the global setting.

### Fixed (also in 0.40.1)

* Calls froze when Background = Blur/Image. PublishPipeline's BG bridge
  hopped to Qt main via `Qt::BlockingQueuedConnection` per frame; cold-
  start of GL + ONNX on Qt main parked the GStreamer streaming thread,
  and any Qt-main path that needed the GST stream lock during call
  setup deadlocked the UI. `BackgroundEngine` now owns a dedicated
  worker `QThread` (`BackgroundWorker`); the streaming thread blocks
  on the worker, never on Qt main.
* Edited messages disappeared until chat was reopened. Talk's PUT
  returns the `message_edited` system notification; the updated body
  lives in `parent`. Now reads `parent` and patches the row in place.

---

## v0.40.1 "Panagyurishte" — STABLE hotfix (2026-05-26)

Two fixes against 0.40.0 stable. No new features; no risky surface
beyond the two patched paths.

### Fixed

* **Calls froze when Background = Blur/Image was active.** Symptom:
  outbound video never started (no local PIP either), audio still
  worked, TalQ went non-responding after a few seconds and had to be
  killed from Task Manager. Root cause: the BG bridge in PublishPipeline
  did its per-frame composite via `Qt::BlockingQueuedConnection` from
  the GStreamer streaming thread to Qt main. Cold-start of the GL
  context + ORT session on Qt main parked the streaming thread, and
  any Qt-main code path that needed the GST stream lock during call
  setup deadlocked the UI. Fix: BackgroundEngine now owns a dedicated
  worker QThread (BackgroundWorker) that hosts the compositor + ONNX
  segmenter. The streaming thread blocks briefly on the worker — never
  on Qt main. Settings preview keeps working the same.
* **Edited messages disappeared until you switched chat and back.**
  The PUT response from Talk's edit endpoint is the `message_edited`
  *system notification*, not the edited message itself — the updated
  body lives in the response's `parent` field. We were swapping the
  bubble for the system notification, which the painter rightly hides,
  so the row vanished. Now we read `parent` and patch the original row
  in place.

### Known follow-ups

* "Auto-install on idle" opt-in is on the backlog for 0.41.x (task #34).
* Camera-test harness gate still parked (task #29).

---

## v0.40.0 "Panagyurishte" — STABLE (2026-05-24)

**First stable cut after the 0.39.x "Aprilsko Vastanie" beta cycle.**
Codename continues the 150th-anniversary thread: Panagyurishte is the
town where the April Uprising of 1876 was declared at the Oborishte
assembly. The 0.39.x betas honoured the uprising itself; 0.40 stable
dedicates the cycle to the place that lit the fuse.

This release rolls up everything shipped across 0.39.1–0.39.10 plus
the auto-update self-heal fix below. Highlights — read 0.39.x entries
for full details:

* **Video backgrounds — Blur or Image with real selfie segmentation.**
  Bilateral mask refine, temporal EMA on the segmentation, live Settings
  preview, custom-image grid with right-click remove, BG bridge upstream
  of the camera tee so receivers see your effect too.
* **Auto-away on idle** after 5 min of no keyboard/mouse input.
  *Known limitation:* lock/sleep transitions are caught via the
  same 30 s idle poll, not via a Windows WTS notification yet.
* **Auto-mention in 1:1 with one enabled bot** so messages reach the
  bot without typing `@bot-slug` every time.
* **Periodic `talq.client` re-announce** so peers always see the right
  version.
* **Camera LED actually goes off on mute** instead of staying lit.
  Costs ~1 s of COM-init latency on the next enable.
* **Sidebar settings:** scroll-per-tab on small displays; combo-wheel
  trap closed globally; selected background thumbnail has a clear
  accent border; splitter widths persist across restart; user-status
  dots refresh on window activation.

### Fixed in 0.40.0

* **Auto-update is self-healing now.** Field reports of "Could not
  launch installer" after an interrupted download forced one round of
  manual recovery. 0.40.0 fixes it three ways:
  1. The downloaded temp file uses the real asset name
     (`TalQ-v<ver>-Setup.exe`) instead of the generic
     `talq-update.exe`, so Windows Defender treats it like the signed
     installer it is and doesn't heuristic-quarantine.
  2. On a fresh download cycle, stale `talq-update*.exe` and old
     `TalQ-v*-Setup.exe` blobs in `%TEMP%` are swept away so a
     half-written file from a previous run can't poison the next.
  3. If `QProcess::startDetached` ever does return false on the
     downloaded installer, the file is deleted and re-downloaded
     silently, then retried once. The user never sees the broken
     intermediate state. Only a SECOND failure surfaces a message.
* **Image-mode "transparent person" artifact.** The compose shader's
  `lightWrapping` was applied uniformly across the entire smoothstep-
  saturated person interior, so bright background colour visibly
  bled across the body — not just the silhouette rim the comment
  claimed. Replaced with a true rim-only bell curve
  (`4 * personMask * (1 - personMask)`) so the halo peaks at the
  edge transition and falls to zero in the deep interior. Talk's
  default lightWrapping of 0.30 restored now that the gate works.
  See-through artifact on saturated background images is now
  substantially reduced; report any remaining occurrences with a
  screenshot of the bg image so we can tune further.

---

## v0.39.10 "Aprilsko Vastanie" — BETA (2026-05-24)

### Fixed

* **Blur plate was upside-down (BG-image plate was right-side-up).**
  The Y-flip the 0.39.8 fix added in `passthrough.vert` worked for
  the single-FBO pipeline 0.39.8 had, but 0.39.9's new bilateral
  refine added an intermediate FBO and every FBO-read got the flip
  applied a second time — so the blurred-self background plate
  ended up upside-down while the camera-frame foreground stayed
  upright. Clean fix: identity UV in the shader, vertical mirror on
  the QImage->GL upload, let `fbo->toImage()` flip once on readback.
  All FBOs now stay in GL-native orientation throughout, so any
  number of intermediate passes compose without re-flips.

---

## v0.39.9 "Aprilsko Vastanie" — BETA (2026-05-24)

Three layered improvements to person-segmentation quality + one field nit.

### Improved — virtual background looks like a real cutout now

* **Joint bilateral mask refinement** (was stubbed in 0.39.3-0.39.8).
  The compositor now runs a 7x7 edge-aware bilateral pass that uses
  the camera frame as a colour guide before the blur and compose
  steps. Mask edges snap to actual colour discontinuities in the
  image instead of leaking into the background or eating into hair.
  Ported from Talk's `WebGLCompositor.js` jointBilateralFilter, kernel
  reduced from 9x9 → 7x7 (~40 % less GPU work) without a perceptible
  quality drop at 720p.
* **Temporal EMA on the mask** (alpha 0.4). The bilateral cleans
  spatial noise but can't reduce frame-to-frame shimmer at the
  silhouette. Holding the previous frame's 256×256 mask and blending
  with the new sigmoid output gives a much steadier edge in motion.
  Always 256×256 model-space, decoupled from camera resolution.
* **Compose smoothstep narrowed back to Talk's (0.45, 0.70).** Was
  widened to (0.35, 0.75) in 0.39.5 to hide raw-mask noise while
  the bilateral pass was still a stub. With the bilateral landing
  now, narrowing gives crisper silhouette edges.

### Fixed

* **Mouse-wheel scrolling on Settings combo boxes no longer changes
  the selection silently.** Was scoped to the BG mode combo in
  0.39.6; now blocks wheel on ALL combos in Settings via a single
  dialog-level event filter (covers camera quality, mic, speaker,
  ringtone, theme, etc.).

---

## v0.39.8 "Aprilsko Vastanie" — BETA (2026-05-24)

Two BG-feature nits caught the moment 0.39.7 hit a dev machine.

### Fixed

* **Camera preview was upside-down whenever Blur or Image mode was
  active.** The GL compositor's passthrough vertex shader passed UV
  through unchanged, but GL's texture-coord origin is bottom-left
  while QImage rows are top-down — so the upload + readback round-trip
  vertically flipped every processed frame. Affected the Settings
  live preview, the call-time self-PiP, AND the outgoing video to
  peers. One-line fix in `passthrough.vert`: `v_uv.y = 1.0 - a_uv.y`.
* **Background image picker showed even when irrelevant.** The
  "Background image" header + thumbnail grid + "Choose your own…"
  row are now wrapped in a container that's hidden when the mode is
  Off or Blur. Cleaner Settings on the most common paths; only
  reveals when Image is the active mode.

---

## v0.39.7 "Aprilsko Vastanie" — BETA (2026-05-24)

Two backlog features.

### Added

* **Auto-away on idle.** TalQ now follows the same pattern upstream NC
  Talk web uses: when the user is currently Online and the OS reports
  more than 5 minutes since the last keyboard/mouse input, the status
  flips to Away. The flip undoes itself the moment input resumes. The
  Windows idle source is `GetLastInputInfo`; non-Windows builds skip
  the auto-flip entirely until a native idle source is wired. A
  user-driven status change always wins — picking Online or Dnd
  manually during the auto-away window clears the auto-flag and the
  poll stops trying to "restore" your choice.
  *Known gap:* session-lock / sleep transitions are caught by the same
  30-second idle poll (no input → idle naturally climbs), not by a
  Windows WTS notification. That's a follow-up.
* **Auto-mention in 1:1 with a single enabled bot.** When the active
  conversation is a one-to-one room AND exactly one Talk bot is
  enabled in it (e.g., Aelita in your private chat with her), the
  composer auto-prepends the bot's `@<slug>` so every message reaches
  the bot without you having to type the mention. Replies skip the
  prefix (the reply context already addresses the right recipient),
  and messages that already mention the bot (case-insensitive) are
  sent verbatim. The optimistic message bubble shows the prefixed
  text so what you sent matches what you see.

---

## v0.39.6 "Aprilsko Vastanie" — BETA (2026-05-24)

A bundle of camera/background flow improvements driven by field
feedback during the morning's beta cycle.

### Added

* **Live camera preview in Settings → Backgrounds.** Pick Blur or
  Image and a 320×180 preview of your own camera appears with the
  effect applied in real time. Slider drag updates the blur live;
  picking a different bundled image swaps the background frame on the
  next captured frame. Owns its own BackgroundEngine instance so it
  doesn't contend with an active call's engine for the GL context.
  Camera handle is released the instant the dialog hides so an
  outgoing call can claim it.
* **Custom backgrounds are now visible in the grid.** Picking an
  image via "Choose your own…" adds it as a new thumbnail next to the
  eight bundled ones (previously it was saved but invisible). Custom
  thumbnails persist across restart. Right-click a custom thumbnail
  → "Remove from grid" to drop it; bundled thumbnails are immutable.

### Fixed

* **Camera LED stayed on after muting video mid-call.** Previously
  `disableCamera` set the camera source to GST_STATE_PAUSED, which on
  Windows mfvideosrc keeps the IMFMediaSource handle open and the
  hardware LED lit even though TalQ stopped sending frames - users
  read that as "they can still see me." Now drops to GST_STATE_NULL
  so the device handle is released and the LED goes off. Costs ~1 s
  on the next enableCamera (mfvideosrc COM init) but the visible
  feedback is worth the latency.
* **Background mode dropdown could silently desync from a
  mouse-wheel.** The save handler was wired to QComboBox::activated
  (click-only), so a wheel-scroll while hovering changed the visible
  selection without persisting it - the engine kept running Image
  while the dropdown showed Off. Wheel events on the BG mode combo
  are now swallowed.
* **Currently-selected background thumbnail had no visible state on
  dark themes.** Selection now draws a 2 px accent border + tinted
  background; hover gets a subtle lift.

### Internal (PR-review caught)

* `BgPreviewSource::start()` leaked floating-ref GstElements on the
  early-exit path when any factory returned null.
* `BgPreviewSource::onNewSample` could dereference a destroyed
  BackgroundEngine if the dialog torn down mid-frame. Engine pointer
  is now QPointer-guarded.

---

## v0.39.5 "Aprilsko Vastanie" — BETA (2026-05-24)

Three backlog items.

### Fixed

* **Settings dialog overflowed small laptop displays.** Each tab's
  content is now wrapped in a vertical-scroll QScrollArea so the
  dialog stays at a compact default size and long content
  (Audio & Video tab in particular, with the 8-tile background grid)
  scrolls inside the tab. Initial dialog height is also clamped to
  the primary screen's available height minus chrome.

### Changed

* **Camera-off "mute" dummy now matches upstream geometry.** TalQ has
  been pumping a 16×16 black canvas at 1 fps while video is muted.
  Upstream Nextcloud Talk (spreed v23.0.4 + libwebrtc) sends a
  camera-resolution black canvas at 10 fps for the first 5 s, then
  halts RTP. The 5 s halt timer was already correct from 0.32.0; this
  release fixes the canvas geometry to 640×480 @ 10 fps (upstream's
  documented fallback). Removes the GCC + jitterbuffer
  "convergence-to-dummy-stream" effect that's been suspected as the
  remaining tail in the callee-mid-call-camera-on chop saga
  (#111/#135).
* **Background edge feathering widened.** Phase 2e (0.39.3) shipped
  the real ONNX selfie segmenter but our `bg_mask_refine.frag` is
  still a stub (the joint bilateral filter pass hasn't ported yet).
  Talk's web client coverage (0.45, 0.70) assumes a refined mask;
  feeding the raw sigmoid output through that narrow smoothstep shows
  the model's edge noise more than necessary. Widened to (0.35, 0.75)
  for ~50% more transition pixels. Once the bilateral pass ports for
  real, this will narrow back.

---

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
  field-verified on live calls).** The 16×16 1 fps black dummy that fed
  the funnel forever while the camera was off is gone. We now mirror
  upstream's BlackVideoEnforcer: the dummy runs for a 5-second grace
  window after every "camera off" transition, then closes the dummy
  valve so no RTP reaches webrtcbin — the wire goes silent, exactly
  like Chrome's `track.enabled=false`. When the camera enables, the
  halt timer is stopped and valves flip as before. Autonomous harness
  reproduces the previously-broken scenario at 79–87 % distinct /
  delivered (was ~30 % in the field) and a field tester confirms the chop is
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
  exact field bug, autonomously verifiable, no humans required.

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
  exact field bug, autonomously verifiable, no humans required.

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
- Authenticates a real user + test-talq, joins call via HPB, creates WebRTC pipelines, verifies ICE connection through real STUN/TURN, validates 3s stability, tears down cleanly
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
