#pragma once

// The http-proxy string to hand webrtcbin, if media should use one at all.
// See MediaProxyPolicy.h for why this only ever affects TURN over TCP.

#include <QString>
#include <QStringList>

namespace talq {

// Returns "http://[user:pass@]host:port" for webrtcbin's "http-proxy"
// property, or an empty string when media must not be proxied.
//
// probeHost/probePort name the TURN server we are about to reach, because a
// proxy configuration can be per-destination (a bypass list, or a PAC script
// that answers DIRECT for some hosts). Asking about the actual destination is
// what makes those answers correct rather than approximately correct.
QString mediaHttpProxyUrl(const QString &probeHost, int probePort);

// Convenience for the pipelines, which hold TURN URLs rather than a host:port.
// Uses the first URL it can parse -- the pool is one deployment behind one
// proxy policy, so probing the first is representative, and webrtcbin takes a
// single http-proxy for all TURN servers anyway.
QString mediaHttpProxyUrlForTurn(const QStringList &turnUrls);

// The same string with any credentials replaced by ***, for logging.
QString maskProxyCredentials(const QString &proxyUrl);

} // namespace talq
