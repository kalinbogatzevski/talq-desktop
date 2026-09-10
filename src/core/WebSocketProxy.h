#pragma once

// Qt-side glue for WebSocketProxyPolicy.h. See that header for WHY this
// exists; in short, Qt can hand a socket a proxy the socket cannot use, and
// the failure is instant, local, and looks exactly like a firewall.

#include <QString>

class QWebSocket;
class QUrl;

namespace talq {

// Resolve a proxy this WebSocket can actually connect through, install it, and
// return a log-safe one-line account of what was chosen and why.
//
// MUST be called before every open(): the proxy is per-socket state, and a
// reconnect after a network change can resolve differently than the first
// attempt did.
//
// It returns the description rather than offering a separate describe()
// function on purpose. Resolving the system proxy is the expensive step -- on
// a PAC/WPAD domain machine, the very environment this code exists for, it can
// mean a WinHTTP autoproxy lookup -- and a separate describe() meant doing it
// TWICE per connect and once more per backoff retry, with the second result
// thrown away whenever detailed logging was off (the default). Worse, the line
// in the log was then computed by a different resolution than the one actually
// installed, so the diagnostic could disagree with reality. Returning it here
// makes the logged verdict the applied verdict by construction, and costs one
// resolution instead of two.
//
// The returned string never contains proxy credentials: it goes into a log
// file users are asked to email in.
QString applyWebSocketProxy(QWebSocket &socket, const QUrl &url);

} // namespace talq
