# Multi-server signaling discovery

If you run more than one Nextcloud Talk High Performance Backend (HPB) behind
a single Nextcloud, this doc explains a small, backward-compatible way to let
clients pick the best one themselves — instead of hardcoding a server list
into every client release.

## The starting point

Nextcloud Talk already has real multi-HPB support, entirely server-side:

- `occ config:app:set spreed signaling_servers` takes a JSON array of HPB
  URLs (plus a shared secret used by all of them).
- `occ config:app:set spreed signaling_mode` picks how a conversation gets
  assigned one: `external` picks one at random per request;
  `conversation_cluster` assigns one HPB per conversation and sticks with it
  for the conversation's lifetime.

This already works with **zero client changes** — every existing Talk client,
including old ones already in the field, benefits the moment you configure
it.

## The gap

The REST endpoint a client calls to get its signaling settings
(`/ocs/v2.php/apps/spreed/api/v3/signaling/settings`) only ever returns the
**one** HPB Nextcloud already picked for it — as a single `server` string.
It never exposes the rest of the configured list. That's fine if you're
happy with whatever Nextcloud assigned, but it rules out a client doing its
own nearest/fastest selection, and it rules out fast client-side failover
between HPBs — the client only ever knows about one at a time.

## The fix: an additive field, not a new protocol

Rather than build a second, parallel discovery mechanism, we patched the
existing endpoint to *optionally* include the full list alongside the field
it already returns:

```php
// in SignalingController::getSettings(), right after $data is built:
$signalingServers = $this->talkConfig->getSignalingServers();
if (count($signalingServers) > 1) {
    $data['servers'] = array_map(static function (array $server): array {
        return ['server' => $server['server']];
    }, $signalingServers);
}
```

Two things make this safe to ship:

1. **It's additive.** Any client that doesn't know about `servers` — every
   existing Talk client, web or mobile — just ignores the unknown JSON key.
   Nothing about the existing `server` field changes.
2. **It never leaks the shared secret**, only the server URLs the client
   already implicitly trusts (it's already talking to one of them).

It's gated on there being more than one configured server, so a single-HPB
install sees no change in the response at all.

On the client, we do the mirror image: read `servers` if present, probe each
candidate with a cheap TCP connect to measure round-trip time, and connect to
the fastest one that answers. If the field is absent — a stock, unpatched
Nextcloud — the client falls back to exactly its previous single-server
behaviour. No feature flag, no version negotiation: presence of the field
*is* the capability check.

```cpp
// SignalingClient::fetchSettings(), parsing the response:
m_discoveredHpbPool.clear();
for (const auto &v : data["servers"].toArray()) {
    const QString u = v.toObject()["server"].toString().trimmed();
    if (!u.isEmpty()) m_discoveredHpbPool << u;
}
```

The result: adding, removing, or replacing an HPB is purely a server-side
config change (`occ config:app:set spreed signaling_servers ...`). No app
update, no rebuild, on either side — old clients keep working via the
existing single-`server` field, patched clients automatically start using
the full list and pick the nearest one.

## Patching core app code — the tradeoff

This lives in Nextcloud's own `apps/spreed`, which we don't control the
release cadence of. An app update overwrites the whole directory from the
upstream release, silently reverting a direct edit. If you do this, track it
as a standalone patch file (`diff -u` against the unmodified source) with a
short README next to it, and check whether it still applies after every Talk
app update — a clean `patch --dry-run` tells you immediately whether you're
done or need to reapply it.

## A gotcha we hit turning on `conversation_cluster`

Worth calling out since it cost us a few minutes of "why is signaling
offline": `conversation_cluster` mode requires a real conversation to assign
an HPB to. Any call to the signaling-settings endpoint *without* one — a
general capability/status check, for instance — throws rather than falling
back to a default. `external` mode has no such requirement; it picks
randomly regardless of context. If you want sticky per-conversation
assignment, make sure every caller of that endpoint always has a room in
scope first — otherwise `external` is the safer default, and you still get
load spread across every configured server, just without the stickiness.
