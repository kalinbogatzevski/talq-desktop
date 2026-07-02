# Running TalQ against your own Nextcloud Talk HPB

TalQ uses the **Nextcloud Talk High Performance Backend (HPB)** —
[`nextcloud-spreed-signaling`](https://github.com/strukturag/nextcloud-spreed-signaling)
plus a [Janus](https://janus.conf.meetecho.com/) WebRTC gateway — exactly
like the official Talk web/mobile clients. On a Nextcloud *without* the
HPB (internal signaling only) TalQ calls do **not** work; the HPB is
required.

If your calls connect but **video and screen share look soft / low
resolution**, the cause is almost always **server-side bitrate limits**,
not the client. This is the single most common self-hosting gotcha, so
it is documented here.

## The bitrate ceiling is set by the server, not the client

The capacity is **not** advertised in the Talk/signaling protocol. The
client never receives a "your max bitrate is X" field. Janus enforces
the limit on the media plane via RTCP **REMB**, and the publisher is
expected to adapt to it. So whatever the server allows is the hard
ceiling — a client cannot exceed it no matter what it encodes at.

Two layers cap it:

### 1. Janus videoroom (`janus.plugin.videoroom.jcfg`)

The plugin default per-publisher cap is **`bitrate = 512000`** (512
kbps). Any statically-defined demo room (e.g. `room-1234`) also carries
its own `bitrate`. **Nextcloud Talk does not use static rooms** — it
creates rooms dynamically — so for Talk this static value is mostly
irrelevant; the dynamic-room bitrate is governed by the signaling
server (next section). Still, set the videoroom codec sensibly:

```
videocodec = "vp8,h264"   # VP8 preferred, H264 fallback (Talk/Janus default)
```

TalQ publishes **VP8** (matching the browser client and the Janus
default). H264 is only used as a fallback; do not configure an
H264-only room or browser interop breaks.

### 2. Signaling server (`nextcloud-spreed-signaling` `server.conf`)

This is the **authoritative ceiling for Talk's dynamic rooms.** In the
`[mcu]` section:

```ini
[mcu]
type = janus
url  = ws://janus:8188
# Maximum bitrate per publishing (camera) stream, bits/second.
# Commented-out / unset => code default ~1 Mbit/s.
maxstreambitrate = 4000000
# Maximum bitrate per screen-sharing stream, bits/second.
# Commented-out / unset => code default ~2 Mbit/s.
maxscreenbitrate = 12000000
```

If these are left unset you get the conservative code defaults
(~1 Mbit/s camera, ~2 Mbit/s screen), which is why an out-of-the-box
HPB makes TalQ (and the official clients) look mediocre. Screen share
has its **own, higher** cap (`maxscreenbitrate`) — raise it well above
the camera cap for sharp shared screens, since screen content benefits
far more from bitrate than camera does.

Pick values for *your* bandwidth budget. The numbers above are a
"high quality, ample bandwidth" profile (camera ≈ 4 Mbit/s ≈ Zoom/Teams
1080p; screen ≈ 12 Mbit/s ≈ near-lossless). For constrained links,
3 Mbit/s / 6 Mbit/s is a safe HD profile.

### Apply

The HPB config is normally a bind mount, so edit the host file and
restart the signaling container (this **drops active calls**):

```sh
cd /path/to/your/talk-hpb
cp server.conf server.conf.bak           # always back up first
$EDITOR server.conf                      # add the two [mcu] lines
docker compose restart signaling         # janus needs no restart for this
```

Verify in the signaling startup log that it comes back up
(`Using janus MCU`, `Listening on …`) with no config parse errors.

## Codec / encoding notes

- TalQ encodes **VP8** for both camera and screen (separate pipelines).
  Camera is captured within device-reported capabilities and pinned to
  a constant ≤1280×720 @ ≤30 fps for the encoder (forcing 1080p caused
  an encoder-reconfiguration storm that collapsed calls; see CHANGELOG
  v0.29.2). Screen share is sent at the **full native resolution** of
  the shared display — give it the bandwidth.
- The client currently uses a fixed encoder target bitrate rather than
  REMB-driven congestion control, so set the server caps to the quality
  you actually want; the client will fill them but not exceed them.

## TURN/STUN

Calls that fail to connect (rather than connecting but looking soft)
are usually a TURN/STUN problem, not bitrate. Ensure the HPB's
configured STUN/TURN servers are reachable from the clients; symmetric
NAT / firewalls require a working TURN relay.
