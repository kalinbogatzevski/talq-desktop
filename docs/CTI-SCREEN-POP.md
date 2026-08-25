# Caller screen-pop — integrating your phone system

TalQ can show a card about the caller while the phone is still ringing, and open
that customer in your browser. It works with whatever telephony and whatever
customer system you already run, because TalQ implements only the *client* half
and knows nothing about either.

This document is the contract. Implement it and TalQ will work against your
setup with no changes to TalQ and no custom build.

If you would rather start from something that runs,
[CTI-INTEGRATION-EXAMPLES.md](CTI-INTEGRATION-EXAMPLES.md) is a walkthrough with
a complete working integration in about a hundred lines, plus notes on wiring it
to Asterisk, FreeSWITCH or a webhook-based cloud PBX.

## What you provide

Two things, and they can be the same service or two:

1. **An event source** — a WebSocket server that tells a desktop when its
   extension is ringing.
2. **A card source** — an HTTP endpoint that turns a phone number into
   something worth showing.

TalQ never talks to your PBX. It never places, answers, transfers or ends a
call. It watches, and it draws what you send it.

## Configuration

Under **Settings → Phone** the user supplies:

| Field | Meaning |
| --- | --- |
| Call service address | `wss://your-host:port` — your event source |
| Customer system address | `https://your-erp.example` — base for the card and pairing endpoints |

A distribution may ship defaults for both so nobody types a URL.

### Transport requirements

`wss://` and `https://` are **required** for anything but `localhost`. TalQ
refuses plaintext elsewhere and says so in Settings. This is not
configurable, and the reason is below under Security.

## 1. Pairing — how a desktop gets a credential

TalQ never asks the user for their password to your system. Instead it uses a
browser approval flow, so approval happens in a session the user already
trusts.

```
POST {customer_system}/api/v1/cti/pair/start
     {"device": "<hostname>"}
  -> 200 {"pair_token": "...", "approve_url": "https://...", "poll_interval": 3}
```

TalQ opens `approve_url` in the user's browser. Your page authenticates them
however you normally do, shows what is being authorised, and records approval.

```
POST {customer_system}/api/v1/cti/pair/poll
     {"pair_token": "..."}
  -> 202  still waiting for the human
  -> 200  {"api_key": "...", "extension": "131", "display_name": "..."}
  -> 410  denied, expired, or already claimed
```

TalQ stores `api_key` and uses it for everything afterwards. Return it exactly
once.

**`extension` matters.** It is how you tell TalQ which extension this user
owns. If you return it empty or null, TalQ will tell the user they are paired
but no extension is linked to their account — because otherwise they would get
a device that pairs successfully and then silently never pops anything.

**Implementer notes, learned the hard way:**

- Put CSRF protection on the approval POST. Without it, an attacker can start a
  pairing themselves, lure a signed-in user into approving it cross-site, then
  poll and collect that user's credential.
- Scope any unauthenticated allowance for `pair/start` and `pair/poll` to the
  request **path**, not the raw URI. A URI includes the query string, so an
  unanchored match lets `?x=/api/v1/cti/pair/start` open the gate for a request
  routed somewhere else entirely.
- `approve_url` must be `http`/`https`. TalQ refuses to open anything else.

## 2. The event source — WebSocket

TalQ connects to your `wss://` endpoint and sends one frame:

```json
{"token": "<the api_key from pairing>"}
```

Validate it. Then either accept:

```json
{"type": "ready", "extension": "131", "display_name": "Sam Patel"}
```

or reject:

```json
{"type": "error", "error": "unauthorised"}
```

⚠ **Only send `error` when the credential is genuinely bad.** TalQ treats
`unauthorised` and `no-extension` as terminal and stops retrying for the
session — which is correct for a revoked key and disastrous for a five-minute
outage of your own backend. If *you* are broken, close the socket without a
frame; TalQ treats that as retryable and reconnects with backoff. Any error
code TalQ does not recognise is also treated as retryable.

### Events

```json
{"type":"ring","call_id":"<opaque>","extension":"131",
 "caller":"0824445555","direction":"inbound","ts":1787602524}

{"type":"end","call_id":"<opaque>",
 "reason":"answered_by_me|answered_elsewhere|missed|busy|cancelled"}
```

- `call_id` is opaque to TalQ but **must be stable across the whole call** and
  shared by every extension ringing for it. That is what lets a hunt-group call
  dismiss the other agents' cards when one person answers.
- `caller` may be empty for a withheld number. TalQ still shows a card.
- Send `ring` **once per extension per call**. If your telephony emits several
  events for one ringing phone, collapse them; TalQ will ignore an exact
  duplicate `call_id`, but it cannot merge two different ids for one call.
- `answered_elsewhere` is what dismisses a losing agent's card. If you cannot
  distinguish it from `cancelled`, prefer `cancelled` — a card that lingers is
  better than one that claims the wrong thing.

TalQ answers WebSocket pings and sends its own roughly every 25 seconds; if
nothing arrives for about 70 seconds it assumes the connection is dead and
reconnects. Keep the socket alive or expect reconnects.

## 3. The card source — HTTP

When a `ring` arrives, TalQ asks your system who is calling, using **the
agent's own credential**:

```
GET {customer_system}/api/v1/pbx/screen-pop/{phone}
X-API-Key: <the api_key from pairing>
```

Because the request carries that user's credential, you can and should return
different content to different people. A field someone may not see should be
**absent**, not blanked — TalQ shows nothing and cannot tell the difference.

> Use `X-API-Key`. `Authorization: Bearer` is not used, because it is commonly
> stripped by web servers before the application ever sees it.

### The response

TalQ renders whatever you send. It does not know what a "balance" or a
"contract" is, and it does no formatting whatsoever.

```json
{
  "known": true,
  "title": "Ravishkar Singh",
  "subtitle": "Account 01906007156  ·  0814461266",
  "badges": [ {"text": "OUTAGE", "style": "danger"} ],
  "fields": [
    {"label": "Balance due", "value": "R 1 234.56", "style": "warning"},
    {"label": "CT11782",     "value": "expires 2027-03-01"}
  ],
  "actions": [
    {"label": "Open customer", "url": "/customers/01906007156"}
  ],
  "max_fields": 8
}
```

An unrecognised caller is simply:

```json
{"known": false}
```

| Key | Notes |
| --- | --- |
| `known` | required; everything else is optional |
| `title` | the headline. Falls back to the phone number |
| `subtitle` | one line under it |
| `badges[]` | short chips. `text` + optional `style` |
| `fields[]` | **ordered**, most important first — the tail is what gets cut |
| `actions[]` | rendered as buttons, in order |
| `max_fields` | how many rows to show. Omit for the default (6) |

**Everything is a display-ready string.** Currency symbols, separators, date
formats, translation and time zones are yours. TalQ prints what it is given.

### `style`

A closed set: `normal`, `muted`, `warning`, `danger`. Anything else — including
a colour value — is drawn as `normal`.

This is deliberate. TalQ guarantees readable contrast in all its themes, and it
cannot do that for arbitrary colours. Note that unknown values *degrade* rather
than fail, so a newer server cannot blank an older client's card.

### `max_fields`

You choose how many rows your card is worth, so showing one more contract never
requires a new TalQ release. TalQ enforces an upper bound of **14** — a card
taller than the screen cannot be read or dismissed. Rows beyond the limit are
not silently dropped; the card says how many were left out.

### `actions[].url`

Absolute, or relative to the customer system address. **Only `http` and
`https` are opened** — the card is an unprompted pop-up, so anything that could
launch a local handler is refused.

## Security

Worth stating plainly, because the failure modes are not obvious:

- **The pairing credential is the same one used for card lookups.** Anyone who
  can read the WebSocket frame can read customer data as that user. This is why
  plaintext is refused off `localhost`.
- **Scope the credential narrowly.** It should do nothing but read screen-pop
  cards. Give it an expiry and make it revocable per device.
- **Prefer a read-only event source.** If your daemon holds telephony
  credentials, they should not be able to originate or redirect calls. A bug in
  a screen-pop must never be able to disturb the phones.
- **Return only what that user may see.** TalQ has no permission model and
  cannot filter for you.

## Behaviour you can rely on

- The card never takes keyboard focus. A call arriving mid-sentence will not
  eat what the user is typing.
- It is dismissed by call state, never a timer, so it cannot vanish while the
  caller is still holding.
- If the agent answers, the card stays — that is when its contents matter most.
- If your card source is slow, fails, or does not recognise the number, the
  card still appears with the phone number on it. Something is better than
  nothing at the moment the user knows least.
- If the socket drops, cards are cleared rather than left showing stale calls.
