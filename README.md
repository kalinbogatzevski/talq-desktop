<div align="center">

<img src="resources/logo.png" alt="TalQ" width="128" height="128" />

# TalQ

**A native desktop client for Nextcloud Talk — drawn, not wrapped.**

Your team chat as a real desktop app: instant open, near-zero idle, built to be left running all day.

<sub>Current line: **Saedinenie** — Bulgarian for *unification*, for the line that brings the pieces of a person into one place: whether a colleague is online, whether they are actually on shift, and whatever else your own systems know about them, on one card. Stable is **0.68.2**; **0.69.x** is the open beta line (odd minor = beta, even = stable). It follows **Blue Fiesta** (0.68 stable, 0.60), **July Morning** (0.56, clean calls), **Bafana Bafana** (0.52), **Slartibartfast** (0.50), **Botev** (0.48), **Margaritka** (0.46), *Magrathea* (0.44) and *Deep Thought* (0.42) 🔵</sub>

</div>

> ⚠️ **Status:** Chat is the solid, daily-driver core. **Audio/video calls,
> simulcast quality switching, and screen sharing work end to end** and are
> verified in real two-party use (1:1 and MCU/conference). The remaining work
> is breadth of cross-network/NAT hardening and multi-party validation beyond
> two peers.
>
> **Group calls need a Talk High Performance Backend** on the server — that is
> a Nextcloud Talk requirement, not a TalQ one, but it is the thing most often
> missing when calls do not work. Windows only for now.

---

## Why I built this

I run an ISP. Nextcloud Talk is how my team actually talks to each other
all day — support, on-call, the lot. The official Talk desktop app is an
Electron shell around the web client, and it works: it's the reference,
it's maintained by people who know the protocol far better than I do, and
TalQ wouldn't exist without it to learn from.

But "works" and "I want this open for nine hours a day" are different
bars. On the machines my staff actually use, a Chromium instance per chat
window is real RAM and real latency. I wanted something that opened
instantly, idled at near-zero CPU, survived being left running for a week,
and felt like a *desktop app* — native notifications, a tray that behaves,
a window that doesn't hitch when a message arrives.

So TalQ is a native Qt client. No web view. The conversation list and
messages are drawn directly with QPainter; the whole thing is built around
three words I kept coming back to — *calm, warm, fast*. It speaks the same
Nextcloud Talk HTTP API the official client does; it just renders it the
way I wanted to look at it forty times a day.

It's an unofficial client. Nextcloud® is their trademark and their
protocol, and I'm grateful for both. This is just what happens when
someone who ships software for a living has to live inside a tool:
eventually you rebuild the part that's between you and the work.

## What it is

- **Conversation list** — unread badges, mentions, favorites grouped on
  top, plus sort (recent / unread / name) and filter (all / unread /
  favorites / direct / groups).
- **Messaging** — message bubbles, threads, replies with quoted context,
  reactions, read receipts, date separators, in-conversation search.
- **Files** — share from disk or Nextcloud, image previews, and a
  per-conversation "shared files" view.
- **Live updates** — long-poll for new messages; native notifications and
  a tray that stays out of the way. If the server drops off the network, a
  quiet "Connecting…" strip says so while you keep reading and scrolling
  your cached conversations — nothing locks up.
- **Calls** — one-to-one and group audio/video over WebRTC, screen
  sharing with live thumbnail picker, optional noise suppression.
  **Simulcast publishing** sends three layers (180p / 360p / 720p) so a
  receiver on a weak network can drop down without dragging everyone
  with them; a **manual Quality chip** on the call screen lets you
  override the per-tile auto-select. *(Working end to end; multi-party
  hardening — see Status.)*
- **Mission Control** — a live telemetry panel on the call screen
  (outbound bandwidth sparkline, codec / encoder / TX-RX resolution
  cards, per-participant subsystem chips) and a matching strip on the
  Settings dialog. One diagnostic surface, one design language.
- **Know who you are talking to** — click the picture beside a message,
  or a name in a room's member list, and a card opens: their picture and
  name, whether they are online and what their status says, and — where
  your workplace provides it — whether they are **on shift right now**.
  Presence and shift answer different questions, and the interesting case
  is when they disagree: an app left open at 22:00 is not someone who is
  going to reply. **Every other row on that card is defined by your own
  server**, so a job title, a team or a desk extension appears without
  anyone shipping a new TalQ build. See *Cards from your own systems*.
- **Four themes** — Ember, Warm, Vivid, Paper. Calm, warm, fast.
- **Built to idle** — QPainter-on-QWidget rendering, no web engine; near-
  zero CPU at rest.
- **Self-tested** — a suite of headless logic tests runs on every build,
  and `talq-call-test` drives the real publish + subscribe pipelines
  against the MCU to check signalling, negotiation and the substream
  ladder. It is honest about its limits: on a single headless box media
  frames do not route, so it proves the call is *set up* correctly, not
  that video arrived. See [`docs/TESTING.md`](docs/TESTING.md), which
  states for each level what it proves and what it cannot.

## Download

**[⬇ Download the latest TalQ installer](https://github.com/kalinbogatzevski/talq-desktop/releases/latest)** — Windows 10/11, 64-bit.

Grab the `TalQ-v<version>-Setup.exe` asset from the latest release and run it,
then point TalQ at your Nextcloud server. The same installer performs in-place
upgrades (it reuses your existing install location and shortcuts).

### Code signing

The Windows builds (`TalQ-…-Setup.exe` and the `talq.exe` inside) are
Authenticode-signed with 123 NET CPT (PTY) LTD's code-signing certificate.

Every release is a brand-new binary, so Microsoft Defender's cloud reputation
may still flag a fresh build on a standalone PC until it has been seen enough
times. If that happens, allow it once (restore it from quarantine, or add a
per-machine Defender exclusion for the install folder) — the signature confirms
the publisher.

## Requirements

- **Windows 10/11, 64-bit.** TalQ currently uses Windows-specific media
  and compositor paths; Linux/macOS are not supported yet.
- A **Nextcloud server with the Talk app** enabled, and an account on it.
- For **group calls**, that server also needs a **Talk High Performance
  Backend**. This is a Nextcloud Talk requirement rather than a TalQ one, but
  it is the single most common reason calls do not work.

Prebuilt installers are on the [Releases](https://github.com/kalinbogatzevski/talq-desktop/releases)
page (see [Download](#download) above). To build from source, read on.

## Build from source (Windows)

TalQ is built with the **MSYS2 mingw-w64 toolchain** — GCC 15, Qt 6.10 and
GStreamer 1.28 all come from the same MSYS2 prefix. Only CMake and Ninja come
from the Qt installer.

> ⚠️ **Use one toolchain, not two.** Do not build against a Qt installed by the
> Qt online installer (`C:\Qt.x\mingw_64`) with the MSYS2 compiler, or
> against MSYS2's Qt with the Qt installer's MinGW. Mixing them links two C++
> runtimes into one binary; it compiles and links cleanly and then dies at
> launch with `0xC00000FD`. That shipped once, as 0.55.1, and the release was
> withdrawn. If `C:\Qt\Tools\mingw1310_64` is on your `PATH`, take it off.

### Prerequisites

- **MSYS2** — <https://www.msys2.org>, default location `C:\msys64`. This
  provides the compiler, Qt and GStreamer.
- **Git** (Git Bash provides the shell the commands below assume)
- **Python 3.10+** — CMake runs `scripts/gen-emoji-data.py` at configure time
- *(optional)* **Visual Studio 2022 Build Tools** with the C++ workload and a
  Windows 11 SDK — needed only to *rebuild* the single-window capture helper.
  The prebuilt `native/wgc/talq_wgc.dll` is committed, so a plain clone already
  has window capture; see step 5.

### 1. Install the toolchain (MSYS2)

In an **MSYS2 MinGW64** shell:

```bash
pacman -Syu     # update first (re-open the shell if it asks you to)
pacman -S --needed   mingw-w64-x86_64-gcc   mingw-w64-x86_64-qt6-base   mingw-w64-x86_64-qt6-websockets   mingw-w64-x86_64-qt6-multimedia   mingw-w64-x86_64-qt6-svg   mingw-w64-x86_64-gstreamer   mingw-w64-x86_64-gst-plugins-base   mingw-w64-x86_64-gst-plugins-good   mingw-w64-x86_64-gst-plugins-bad   mingw-w64-x86_64-gst-plugins-ugly   mingw-w64-x86_64-gst-plugins-rs   mingw-w64-x86_64-gst-libav   mingw-w64-x86_64-libnice   mingw-w64-x86_64-libsrtp
```

> ⚠️ Be conservative with `pacman -S` in this prefix afterwards. A package set
> that pulls a mismatched runtime is the other route to `0xC00000FD`.

CMake looks for GStreamer under `C:/msys64/mingw64` by default (the
`GSTREAMER_ROOT` cache variable). If your MSYS2 is elsewhere, pass
`-DGSTREAMER_ROOT=/path/to/mingw64` at configure time.

### 2. Install CMake + Ninja

These are the only pieces that come from the Qt installer. Any recent CMake and
Ninja will do — if you already have them on `PATH`, skip this.

```bash
pip install aqtinstall
python -m aqt install-tool windows desktop tools_cmake --outputdir C:\Qt
python -m aqt install-tool windows desktop tools_ninja --outputdir C:\Qt
```

**Do not** install `tools_mingw1310` or a `win64_mingw` Qt — see the warning
above.

### 3. Clone

```bash
git clone https://github.com/kalinbogatzevski/talq-desktop.git
cd talq-desktop
```

> Use a path without non-ASCII characters — Qt's tooling breaks on
> Cyrillic/OneDrive paths.

### 4. Configure and build

```bash
# Git Bash — MSYS2 first on PATH, then CMake + Ninja
export PATH="/c/msys64/mingw64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -Wno-dev
cmake --build build --target talq
```

MSYS2 must come **first** on `PATH`: its `g++`, `qmake6` and Qt CMake package
all have to be the ones CMake finds.

### 5. *(optional)* Rebuild the window-capture helper

**Skip this unless you are changing it.** Sharing a *single application window*
(rather than a whole monitor) uses Windows Graphics Capture, whose headers ship
only with MSVC and the Windows 11 SDK — not with the MinGW toolchain TalQ
otherwise uses. So it lives in a tiny, self-contained C-ABI DLL loaded at
runtime, and **the built `native/wgc/talq_wgc.dll` is committed to the
repository**: a plain clone already has working window capture.

You only need this step to rebuild the DLL after editing
`native/wgc/talq_wgc.cpp`, and only then do you need VS2022 + the Win11 SDK.

```bash
# From a shell where cmd is available (needs VS2022 Build Tools + Win11 SDK):
cmd //c native/wgc/build.bat     # produces native/wgc/talq_wgc.dll
```

### 6. Deploy the runtime + run

The app needs the Qt and GStreamer DLLs (and GStreamer plugins) beside the
executable. `scripts/deploy-dev.sh` copies them all into the build dir and
launches:

```bash
bash scripts/deploy-dev.sh        # deploy Qt + GStreamer DLLs/plugins (+ talq_wgc.dll) and run
# or, to deploy without launching:
bash scripts/deploy-dev.sh --no-run
```

To produce a packaged Windows installer (Inno Setup required, `ISCC` on
PATH), see `scripts/build-release.sh`.

## Usage

Launch TalQ, sign in through your Nextcloud server's browser login
(Login Flow v2 — no embedded browser, no password stored), and your
conversations appear. The funnel control in the search row sorts and
filters the list; Settings → Audio & Video selects devices and toggles
noise suppression.

## Cards from your own systems (optional)

TalQ can show a card about a **person** — either the customer whose call is
ringing, or the colleague whose picture you just clicked. Both are optional,
both work with whatever systems you already run, and both are **described
entirely by your server**.

That last part is the whole design. TalQ does not know what a "balance", a
"contract" or a "job title" is. Your endpoint answers with a title, a subtitle,
some badges, an ordered list of label/value rows and some buttons; TalQ draws
what it is given, in the order it is given. **Adding a field, rewording one, or
showing different detail to different people is a change on your side that
takes effect the next time someone opens a card** — no new TalQ build, no
redeploy to anyone's desktop.

```
   your phone system ──▶  your bridge  ──WebSocket──▶  TalQ
                                                        │
   your CRM / ERP / HR  ◀────────── HTTPS ──────────────┘
```

There are four endpoints, and **every one of them is optional**. Implement the
ones that are worth it to you; the parts you skip simply do not appear.

| What you implement | What the user gets |
| --- | --- |
| Pairing | A desktop can obtain a credential of its own |
| A ring event source (WebSocket) | A card appears while the phone is still ringing |
| `GET …/pbx/screen-pop/{number}` | That card says who is calling |
| `GET …/people/card/{username}` | Clicking a colleague's picture shows what your systems know about them |
| `POST …/hr/shift-status` | Colleagues show as on shift, on break or off shift |

The last two have nothing to do with telephony — a site with no phone system at
all can implement them on their own. And a site that implements *none* of this
still gets the person card: it falls back to the picture, the name and the
Nextcloud presence, which need no configuration.

TalQ never places, answers or ends a call. It watches and draws.

- **[docs/CTI-INTEGRATION-EXAMPLES.md](docs/CTI-INTEGRATION-EXAMPLES.md)** — the
  practical walkthrough, with a complete working integration in about a hundred
  lines of standard-library Python, plus notes for Asterisk, FreeSWITCH and
  webhook-based cloud PBXs.
- **[docs/CTI-SCREEN-POP.md](docs/CTI-SCREEN-POP.md)** — the exact contract:
  pairing, the event stream, both card payloads and shift status.

Three things are deliberately **not** yours to choose, because they are
promises TalQ makes to the person reading the screen: the colour vocabulary is
a closed set (so a card can never break the contrast guarantees in any theme),
only `http`/`https` links are opened (a card is an unprompted pop-up), and the
row count is capped (a card taller than the screen cannot be read or
dismissed). Rows past the cap are announced, never silently dropped.

Turn it on under **Settings → Phone**.

## Status

TalQ is what I use every day, but it's honest about where it is:

- **Solid:** chat, threads, reactions, file sharing, search,
  notifications, the conversation list — the things I rely on for hours a
  day.
- **Solid in two-party use:** **audio/video calls, simulcast quality
  switching, and screen sharing.** The WebRTC pipeline is verified working
  end to end in real 1:1 and MCU/conference use —
  hardware H264, three-layer simulcast publishing with auto + manual
  substream selection, screen sharing with live-thumbnail picker — with
  crash barriers so a media failure can't take the app down. A headless
  self-test suite (`talq-call-test`) asserts the entire publish +
  subscribe path against the MCU on every release.
- **Multi-party (3+ peers):** works, but hasn't been validated as
  exhaustively as 1:1; field reports from larger meetings are the most
  useful contribution right now.
- **Newest, least proven:** the **person card** and the **shift status** it
  can show. Both shipped in the 0.69.x beta line and have far less field
  mileage than anything above. The card falls back to name, picture and
  presence when a site provides nothing, so the failure mode is a quieter
  card rather than a broken one — but if you wire an endpoint up, I would
  like to hear how it went.
- **Windows only.** Linux/macOS would need the media/compositor paths
  reworked.
- **Help especially welcome here:** call reliability across the range of
  NAT, firewall, and device configurations real users have is where
  outside testing and bug reports move the needle most.

I'd rather you know that going in than discover it on a customer call.

## Architecture

- **Qt 6.10 Widgets**, **C++20**, mingw-w64 GCC 15 (from MSYS2), CMake + Ninja.
- The conversation list and message view are rendered with **QPainter on
  QWidget** — no QML, no web engine. Earlier versions used QML; it was
  replaced for rendering control and idle cost.
- **GStreamer** WebRTC pipelines for calls.
- Talks to Nextcloud over the **OCS v2 / Talk Chat / Login Flow v2** HTTP
  APIs — no Nextcloud code is linked.

## Contributing

Issues and PRs welcome. The single highest-value area is **call
reliability across real networks** — if you can reproduce a call failure
with details about your NAT/firewall/devices, that's gold. Please keep the
*calm, warm, fast* design intent in mind for UI changes.

## Privacy

TalQ collects **no** telemetry, analytics, or crash reports. It talks only to
the Nextcloud server you configure (your data, your server) and checks GitHub
for new versions. Everything else stays on your machine. See
[`PRIVACY.md`](PRIVACY.md).

## License

TalQ is licensed under the **Apache License 2.0** — see [`LICENSE`](LICENSE).

It links and bundles third-party components under their own terms — Qt and
GStreamer (LGPL), the Twemoji emoji set (CC-BY 4.0), the Inter typeface (SIL
OFL), Feather Icons (MIT), ONNX Runtime (MIT), the MediaPipe Selfie Segmenter
model (Apache-2.0), and the bundled call-background images (**AGPL-3.0**).
If you redistribute TalQ, read [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md)
and [`NOTICE`](NOTICE) in full — the terms are not uniform.

"Nextcloud" is a registered trademark of Nextcloud GmbH. TalQ is an
independent, unofficial client and is not affiliated with or endorsed by
Nextcloud GmbH.

---

Built by **Kalin Bogatzevski**. By day I work on the commercial side —
**[ISPCQ](https://ispcq.com)**, the multi-tenant ISP/ERP platform. TalQ is
the same engineering DNA applied to a desktop client.

Open-sourced because the tool you live inside all day shouldn't be a black
box — and because good software is the best advertising I know.
