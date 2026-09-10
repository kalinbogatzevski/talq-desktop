#pragma once

// The one place TalQ asks Windows what proxy stands between it and a host.
//
// Shared by the signalling path (WebSocketProxy) and the media path
// (MediaProxy) on purpose. Both must reach the same verdict about the same
// machine: a build where the chat socket honoured a bypass entry and the call
// did not would be diagnosed as "calls are broken on some PCs", which is
// exactly the class of bug that cost a week in 0.70.3.

#include <QList>
#include <QNetworkProxy>
#include <vector>

#include "core/WebSocketProxyPolicy.h"

class QUrl;

namespace talq {

// Ask for a proxy suitable for a TCP SOCKET to this destination.
//
// The query type is the load-bearing detail. Qt's default query is
// UrlRequest, and for a URL request it will happily return a caching proxy --
// correct for fetching a page, useless for a socket, and the direct cause of
// the 0.70.3 outage where every WebSocket died in zero milliseconds.
QList<QNetworkProxy> systemTcpProxiesFor(const QUrl &url);

// Same, for callers that have a bare host and port rather than a URL.
QList<QNetworkProxy> systemTcpProxiesFor(const QString &host, int port);

// Reduce Qt's answer to the three booleans the policy headers decide on.
std::vector<ProxyCandidate> toProxyCandidates(const QList<QNetworkProxy> &proxies);

} // namespace talq
