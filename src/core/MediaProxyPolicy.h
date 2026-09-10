#pragma once

// Whether call MEDIA should be pointed at the machine's HTTP proxy, and which
// one. Companion to WebSocketProxyPolicy.h, which answers the same question for
// the signalling sockets; this file reuses its ProxyCandidate deliberately, so
// the two paths cannot drift into disagreeing about what the system proxy is.
//
// Established from libnice 0.1.23 and gst-plugins-bad 1.28 source, because
// every one of these is easy to assume wrongly:
//
//   * webrtcbin's "http-proxy" reaches libnice as the agent's proxy-ip /
//     proxy-port / proxy-type / proxy-username / proxy-password, and those have
//     exactly ONE consumer: agent_create_tcp_turn_socket(). That is called only
//     from the TURN_TCP / TURN_TLS branch. So the proxy applies to TURN over
//     TCP and nothing else -- never to UDP, never to STUN, never to a host or
//     server-reflexive candidate.
//
//   * Setting the ICE agent's ice-tcp to FALSE (which TalQ does, to keep
//     useless ICE-TCP host candidates out of Janus's budget) does NOT disable
//     this. The one use_ice_tcp guard on the TURN path requires reliable_tcp,
//     which libnice sets only under OC2007 compatibility; webrtcbin builds its
//     agent with NICE_COMPATIBILITY_RFC5245, so that guard is dead code here.
//
//   * A TURN/TCP relay candidate is emitted with transport UDP
//     (conncheck.c's discovery_add_relay_candidate). It is not an ICE-TCP
//     candidate, so the far end -- Janus, with ICE-TCP off cluster-wide --
//     accepts it like any other relay candidate. Adding this costs the
//     cross-region pairing nothing.
//
// The policy is ADDITIVE ON PURPOSE. Pointing webrtcbin at the proxy only makes
// a TURN/TCP relay candidate reachable; it does not remove the UDP ones. ICE
// then picks whichever actually connects, and prefers the cheaper UDP path when
// it works. The tempting companion change -- forcing ice-transport-policy=relay
// so we "commit" to the proxy -- is NOT made here, and must not be added
// casually: libnice's force-relay is fail-closed, so on a machine where plain
// UDP was working fine it would discard every working candidate and leave the
// call with none if the proxy refused the CONNECT.

#include "core/WebSocketProxyPolicy.h"

namespace talq {

// Which candidate, if any, media should be routed through. -1 means "no proxy":
// either none is configured, or the system positively said this destination
// goes direct.
inline int decideMediaProxy(const ProxyCandidate *candidates, int count)
{
    // Qt's own order is preference order. A leading NoProxy is a bypass entry
    // -- the machine saying this host is reached directly -- and honouring it
    // is what keeps internal traffic off the corporate box.
    for (int i = 0; i < count; ++i) {
        if (candidates[i].isDirect)
            return -1;
        if (candidates[i].canTunnel && candidates[i].hasEndpoint)
            return i;
    }

    // Nothing Qt labelled as tunnelling. Unlike the WebSocket path, that label
    // does not decide anything here: libnice performs its own HTTP CONNECT and
    // never consults Qt's classification. All we need is somewhere to aim.
    for (int i = 0; i < count; ++i) {
        if (candidates[i].hasEndpoint)
            return i;
    }
    return -1;
}

} // namespace talq
