<div align="center">

<img src="resources/logo.png" alt="TalQ" width="128" height="128" />

# TalQ

**A native desktop client for Nextcloud Talk — drawn, not wrapped.**

Your team chat as a real desktop app: instant open, near-zero idle, built to be left running all day.

</div>

> ⚠️ **Status:** Chat is the solid, daily-driver core. **Audio and video
> calls are under active development and not fully tested** — expect rough
> edges, especially across varied network/NAT setups. Windows only for now.

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
  a tray that stays out of the way.
- **Calls** — one-to-one and group audio/video over WebRTC, with optional
  noise suppression. *(Beta — see Status.)*
- **Four themes** — Ember, Warm, Vivid, Paper. Calm, warm, fast.
- **Built to idle** — QPainter-on-QWidget rendering, no web engine; near-
  zero CPU at rest.

## Requirements

- **Windows 10/11, 64-bit.** TalQ currently uses Windows-specific media
  and compositor paths; Linux/macOS are not supported yet.
- A **Nextcloud server with the Talk app** enabled, and an account on it.

Prebuilt installers are published on the
[Releases](https://github.com/kalinbogatzevski/talq-desktop/releases) page.
To build from source, read on.

## Build from source (Windows)

### Prerequisites

- **Python 3.10+** (for `aqtinstall`)
- **Git**
- **GStreamer** (MSYS2 `mingw-w64-x86_64` packages) for calls

### 1. Install Qt 6.8.2 + build tools

```bash
pip install aqtinstall

# Qt 6.8.2 with MinGW + WebSockets module
python -m aqt install-qt windows desktop 6.8.2 win64_mingw --outputdir C:\Qt -m qtwebsockets

# MinGW 13.1 compiler, CMake, Ninja
python -m aqt install-tool windows desktop tools_mingw1310 --outputdir C:\Qt
python -m aqt install-tool windows desktop tools_cmake --outputdir C:\Qt
python -m aqt install-tool windows desktop tools_ninja --outputdir C:\Qt
```

You should now have `C:\Qt\6.8.2\mingw_64\`, `C:\Qt\Tools\mingw1310_64\`,
`C:\Qt\Tools\CMake_64\`, `C:\Qt\Tools\Ninja\`.

### 2. Clone

```bash
git clone https://github.com/kalinbogatzevski/talq-desktop.git
cd talq-desktop
```

> Use a path without non-ASCII characters — Qt's tooling breaks on
> Cyrillic/OneDrive paths.

### 3. Configure and build

```bash
# Git Bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/c/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build build
```

### 4. Run

```bash
export PATH="/c/Qt/6.8.2/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH"
./build/talq.exe
```

For full runtime DLL deployment (Qt + GStreamer) into a run directory,
see `scripts/deploy-dev.sh`.

## Usage

Launch TalQ, sign in through your Nextcloud server's browser login
(Login Flow v2 — no embedded browser, no password stored), and your
conversations appear. The funnel control in the search row sorts and
filters the list; Settings → Audio & Video selects devices and toggles
noise suppression.

## Status

TalQ is what I use every day, but it's honest about where it is:

- **Solid:** chat, threads, reactions, file sharing, search,
  notifications, the conversation list — the things I rely on for hours a
  day.
- **Under development / not fully tested:** **audio and video calls.** The
  WebRTC pipeline works in my setup, but it hasn't been validated across
  the range of NAT, firewall, and device configurations real users have.
  Treat calls as beta: usable, not guaranteed. Subscriber-side ICE on
  certain network topologies is a known open issue.
- **Windows only.** Linux/macOS would need the media/compositor paths
  reworked.
- **Help especially welcome here:** call reliability across networks is
  where outside testing and bug reports move the needle most.

I'd rather you know that going in than discover it on a customer call.

## Architecture

- **Qt 6.8.2 Widgets**, **C++20**, MinGW 13.1, CMake + Ninja.
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

## License

TalQ is licensed under the **Apache License 2.0** — see [`LICENSE`](LICENSE).

It links and bundles third-party components (Qt and GStreamer under LGPL,
the Twemoji emoji set under CC-BY 4.0, the Inter typeface under the SIL
OFL) under their own terms; see [`NOTICE`](NOTICE) and
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

"Nextcloud" is a registered trademark of Nextcloud GmbH. TalQ is an
independent, unofficial client and is not affiliated with or endorsed by
Nextcloud GmbH.

---

Built by **Kalin Bogatzevski**. By day I work on the commercial side —
**ISPCQ**, the multi-tenant ISP/ERP platform. TalQ is the same engineering
DNA applied to a desktop client.

Open-sourced because the tool you live inside all day shouldn't be a black
box — and because good software is the best advertising I know.
