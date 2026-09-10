#pragma once

// Which of the proxies Qt offered us can actually carry a WebSocket.
//
// This exists because of a field failure that cost a week of looking in the
// wrong place. On a domain PC with a Windows system proxy, all three of TalQ's
// WebSockets -- signaling, push and CTI -- died like this:
//
//   13:04:55.233 Signaling: connecting to "wss://turn-ru.../spreed"
//   13:04:55.233 Signaling: disconnected
//
// Same millisecond. No TCP handshake, no TLS, no packet ever left the machine.
// Qt had resolved the system proxy, found it had no TunnelingCapability, and
// refused locally with UnsupportedSocketOperationError -- "The proxy type is
// invalid for this operation". Plain HTTP kept working the whole time, because
// a caching proxy is perfectly good for a URL request and useless for a raw
// socket. That asymmetry -- chat fine, WebSockets dead -- is the signature.
//
// The trap is that it LOOKS like a firewall, and a firewall is what everyone
// blames. It cannot be one: a blocked port fails after a round trip, not in
// zero milliseconds. Whenever these two facts appear together -- instant
// failure and working HTTP -- suspect proxy CLASSIFICATION, not reachability.
//
// The insight that makes the fix small: the proxy HOST is fine. The corporate
// proxy is right there and will happily CONNECT-tunnel to port 443. Only Qt's
// classification of it is unusable for a socket. So rather than give up, we
// re-cast the same host:port as an HttpProxy, which tunnels.
//
// Kept Qt-free on purpose, like the other policy headers: this is a decision
// over three booleans, and a decision buried in socket setup is one nobody can
// test. The caller maps Qt's QNetworkProxy list onto Candidate and applies the
// verdict.

namespace talq {

// One proxy as Qt resolved it, reduced to the three facts the choice turns on.
struct ProxyCandidate {
    // Qt's NoProxy. Not an absence of an answer -- it is Qt positively saying
    // "this destination goes straight out", which is what a bypass list or a
    // PAC returning DIRECT looks like. It must be honoured, not overridden.
    bool isDirect = false;
    // TunnelingCapability: can carry an opaque TCP stream via CONNECT. This is
    // the single capability a WebSocket needs and a caching proxy lacks.
    bool canTunnel = false;
    // Whether there is a host:port we could aim at. A candidate without one
    // cannot be re-cast into anything useful.
    bool hasEndpoint = false;
};

enum class ProxyAction {
    Direct,        // connect with no proxy at all
    UseCandidate,  // use this candidate exactly as Qt handed it over
    RecastAsHttp,  // same host:port, but as an HTTP CONNECT proxy
};

struct ProxyVerdict {
    ProxyAction action = ProxyAction::Direct;
    int index = -1;   // which candidate; -1 when the action needs none
};

// Qt returns candidates in ITS order of preference, so the first usable one
// wins rather than the "best" one by some ranking of our own -- second-guessing
// that order is how a bypass entry gets skipped and internal traffic starts
// taking a detour through the proxy.
inline ProxyVerdict decideWebSocketProxy(const ProxyCandidate *candidates, int count)
{
    // Pass 1: anything Qt gave us that already works, in Qt's own order.
    for (int i = 0; i < count; ++i) {
        if (candidates[i].isDirect)
            return { ProxyAction::Direct, i };
        if (candidates[i].canTunnel)
            return { ProxyAction::UseCandidate, i };
    }

    // Pass 2: nothing could tunnel. Before 0.70.3 this was the silent death --
    // Qt took the first entry, the socket refused it, and the user saw an
    // endless reconnect with no explanation. The proxy itself is reachable, so
    // aim at the same host:port as an HTTP CONNECT proxy instead of giving up.
    for (int i = 0; i < count; ++i) {
        if (candidates[i].hasEndpoint)
            return { ProxyAction::RecastAsHttp, i };
    }

    // No candidates, or none with an endpoint. Direct is the honest default:
    // on a machine with no proxy this is simply correct, and on one where a
    // proxy is mandatory it fails at the network layer with a real error
    // instead of a local refusal nobody can interpret.
    return { ProxyAction::Direct, -1 };
}

} // namespace talq
