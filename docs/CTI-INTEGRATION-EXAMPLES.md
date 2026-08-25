# Building your own caller screen-pop integration

TalQ shows a card about the caller while the phone rings. It implements only the
client half, so it works with whatever phone system and whatever customer system
you already run — you write the bit in the middle.

This is the practical walkthrough. [CTI-SCREEN-POP.md](CTI-SCREEN-POP.md) is the
exact contract; come here first, go there when you need the detail.

## What you are building

```
   your phone system ──▶  your bridge  ──WebSocket──▶  TalQ
                                                        │
   your CRM / ERP    ◀──────────── HTTPS ───────────────┘
```

Two responsibilities, and they can live in one process or two:

- **Tell TalQ an extension is ringing.** A WebSocket server. TalQ connects,
  proves who it is, and waits.
- **Say who is calling.** An HTTP endpoint that turns a phone number into a
  card.

TalQ never talks to your PBX and never touches a call. It watches and draws.

## Start here: the smallest thing that works

Standard library only, no dependencies. It fakes a ringing phone every 20
seconds so you can see a card without wiring up telephony yet.

Save as `demo_cti.py`, run `python3 demo_cti.py`, then in TalQ set
**Settings → Phone** to `ws://127.0.0.1:8790` and
`http://127.0.0.1:8791`, and press **Pair**.

> Loopback is the only place TalQ accepts unencrypted `ws://`/`http://`. That
> exemption exists exactly for this. Anywhere else needs `wss://`/`https://` —
> see *Going to production* below.

```python
import asyncio, base64, hashlib, json, struct, threading, time
from http.server import BaseHTTPRequestHandler, HTTPServer

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
AGENT_EXTENSION = "131"          # who this demo pretends you are

# ── 1. The card source (and pairing) ────────────────────────────────────────
class Api(BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def _json(self, payload, code=200):
        body = json.dumps({"success": True, "data": payload}).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path.endswith("/cti/pair/start"):
            # A real implementation shows approve_url in the user's browser and
            # only issues a key once a human clicks Approve.
            return self._json({"pair_token": "demo",
                               "approve_url": "http://127.0.0.1:8791/approved",
                               "poll_interval": 1})
        if self.path.endswith("/cti/pair/poll"):
            return self._json({"api_key": "demo-key",
                               "extension": AGENT_EXTENSION,
                               "display_name": "Demo Agent"})
        self.send_error(404)

    def do_GET(self):
        if "/screen-pop/" in self.path:
            phone = self.path.rsplit("/", 1)[-1]
            # Look this up in YOUR system. Everything is a display-ready
            # string: you format currency and dates, not TalQ.
            return self._json({
                "known": True,
                "title": "Acme Logistics",
                "subtitle": f"Account 88123  ·  {phone}",
                "badges": [{"text": "VIP", "style": "warning"}],
                "fields": [
                    {"label": "Balance due",  "value": "£1,240.00", "style": "warning"},
                    {"label": "Open tickets", "value": "2"},
                ],
                "actions": [{"label": "Open customer",
                             "url": "https://crm.example.com/c/88123"}],
                "max_fields": 6,
            })
        if self.path.startswith("/approved"):
            self.send_response(200); self.end_headers()
            return self.wfile.write(b"<h2>Approved. You can close this tab.</h2>")
        self.send_error(404)

threading.Thread(target=lambda: HTTPServer(("127.0.0.1", 8791), Api).serve_forever(),
                 daemon=True).start()

# ── 2. The event source ─────────────────────────────────────────────────────
def frame(text):                      # server frames are never masked
    data = text.encode(); head = bytearray([0x81])
    if len(data) < 126:      head.append(len(data))
    elif len(data) < 1 << 16: head += bytes([126]) + struct.pack("!H", len(data))
    else:                     head += bytes([127]) + struct.pack("!Q", len(data))
    return bytes(head) + data

async def read_frame(reader):
    head = await reader.readexactly(2)
    n, masked = head[1] & 0x7F, head[1] & 0x80
    if n == 126: n = struct.unpack("!H", await reader.readexactly(2))[0]
    mask = await reader.readexactly(4) if masked else b""
    data = await reader.readexactly(n)
    return bytes(b ^ mask[i % 4] for i, b in enumerate(data)) if masked else data

async def serve(reader, writer):
    req = (await reader.readuntil(b"\r\n\r\n")).decode("latin-1")
    key = next(l.split(": ", 1)[1].strip() for l in req.split("\r\n")
               if l.lower().startswith("sec-websocket-key"))
    accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                  "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept +
                  "\r\n\r\n").encode())
    await writer.drain()

    hello = json.loads(await read_frame(reader))
    # VALIDATE hello["token"] against whatever you issued at pairing.
    # Only send {"type":"error"} for a genuinely bad credential — see below.
    writer.write(frame(json.dumps({"type": "ready", "extension": AGENT_EXTENSION})))
    await writer.drain()
    print("agent connected")

    while True:                        # pretend a call arrives every 20s
        await asyncio.sleep(20)
        call_id = str(time.time())
        writer.write(frame(json.dumps({"type": "ring", "call_id": call_id,
                                       "extension": AGENT_EXTENSION,
                                       "caller": "02079460000",
                                       "direction": "inbound"})))
        await writer.drain()
        await asyncio.sleep(8)
        writer.write(frame(json.dumps({"type": "end", "call_id": call_id,
                                       "reason": "missed"})))
        await writer.drain()

async def main():
    server = await asyncio.start_server(serve, "127.0.0.1", 8790)
    print("ws://127.0.0.1:8790  +  http://127.0.0.1:8791")
    async with server:
        await server.serve_forever()

asyncio.run(main())
```

That is the whole integration. Everything below is replacing the fake parts
with real ones.

## Wiring it to a real phone system

You need one fact: **which extension is ringing, and from what number.** How you
get it depends on your system.

**Asterisk / FreePBX** — connect to AMI (or ARI) and watch `DialBegin`. Two
things will bite you:

- One ringing phone can emit *several* `DialBegin` events. A call routed through
  a queue produces an intermediate `Local/...` channel *and* the real device
  channel. Act only on the leg that is an actual device, or you will pop twice.
- Use the call-wide identifier (`Linkedid`), not the per-leg one (`Uniqueid`),
  as `call_id`. The per-leg id differs between the ring and the end, so nothing
  correlates and cards never dismiss.

**FreeSWITCH** — subscribe to `CHANNEL_CALLSTATE` / `CHANNEL_CREATE` over ESL
and filter to `RINGING` on the destination leg.

**A cloud PBX with webhooks** — often the easiest: your HTTP handler receives
the ring event and pushes it to the connected socket. No polling.

**Anything else** — if you can learn "extension X is ringing from Y" within a
second or two, you can drive this.

Whatever the source, send **one `ring` per extension per call**. TalQ ignores a
duplicate `call_id`, but it cannot merge two different ids for one call.

## Wiring it to a real CRM

Replace the `do_GET` body with your lookup. Three things worth getting right:

**Normalise the number.** The same customer is `020 7946 0000`,
`+442079460000` and `442079460000`. Strip non-digits and try the variants your
region actually uses.

**Return what *this* user may see.** The request carries the agent's own
credential, so you can vary the card per person. A field they may not see should
be **absent**, not blank — TalQ shows nothing and cannot tell the difference.

A useful principle: **show what that agent could already see if they clicked
through.** Hiding a figure on the card that they can read one click later in
your CRM is not a security boundary, just an inconsistency that makes the card
less useful than the button next to it.

**Be quick.** This runs while a phone is ringing. A couple of indexed queries.
If something is expensive, leave it off the card and let the button fetch it.

## Testing without a PBX

Keep the fake ring loop from the demo and point it at your real CRM lookup
first. Then swap in real telephony events. Debugging both halves at once is
how afternoons disappear.

To exercise a specific caller, just change the `caller` field to a number that
exists in your system.

## Going to production

- **TLS is required.** TalQ refuses plain `ws://`/`http://` anywhere but
  loopback, because the token on that socket is the same credential used for
  card lookups — anyone who can read the frame can read customer data as that
  user.
- **Issue a narrow, revocable credential per device.** It should do nothing but
  read screen-pop cards, and have an expiry.
- **Prefer a read-only connection to your phone system.** If your bridge holds
  telephony credentials, they should not be able to originate or redirect
  calls. A bug in a screen-pop must never disturb the phones.
- **Do not send `{"type":"error"}` when *you* are broken.** TalQ treats
  `unauthorised` as terminal and stops retrying for the session — correct for a
  revoked key, disastrous for a five-minute outage of your backend. If your
  own dependencies are down, close the socket without a frame; TalQ treats that
  as retryable and reconnects with backoff.
- **CSRF-protect the approval page.** Otherwise an attacker starts a pairing,
  lures a signed-in user into approving it cross-site, and collects their
  credential.
- **Watch what sits in front of you.** Reverse proxies and bot filters cause the
  strangest failures here: a stripped `Authorization` header or a rejected
  user-agent arrives looking like an application-level "no", not a transport
  problem. If something is refused for no reason you can see, test the same
  request with `curl` before believing the error.

## Checklist

- [ ] `ring` fires once per extension per call, within a second or two
- [ ] `call_id` is stable across the whole call and shared by every ringing extension
- [ ] `end` distinguishes *answered by this agent* from *answered elsewhere*
- [ ] Bad token → `{"type":"error","error":"unauthorised"}`; your outage → close, no frame
- [ ] Card lookup returns in well under a second
- [ ] Absent rather than blank for anything the agent may not see
- [ ] All values pre-formatted; `style` only ever `normal`/`muted`/`warning`/`danger`
- [ ] `actions[].url` is http/https
- [ ] `wss://` and `https://` in production
