#include "core/SystemProxy.h"

#include <QNetworkProxyQuery>
#include <QUrl>

namespace talq {

QList<QNetworkProxy> systemTcpProxiesFor(const QString &host, int port)
{
    QNetworkProxyQuery query(host, port, QString(), QNetworkProxyQuery::TcpSocket);
    return QNetworkProxyFactory::proxyForQuery(query);
}

QList<QNetworkProxy> systemTcpProxiesFor(const QUrl &url)
{
    // A ws/wss URL usually carries no explicit port, and querying about port 0
    // gets a nonsense answer back.
    const QString scheme = url.scheme().toLower();
    const int port = url.port(scheme == QStringLiteral("wss") ? 443 : 80);
    return systemTcpProxiesFor(url.host(), port);
}

std::vector<ProxyCandidate> toProxyCandidates(const QList<QNetworkProxy> &proxies)
{
    std::vector<ProxyCandidate> out;
    out.reserve(static_cast<size_t>(proxies.size()));
    for (const QNetworkProxy &p : proxies) {
        ProxyCandidate c;
        c.isDirect    = (p.type() == QNetworkProxy::NoProxy);
        c.canTunnel   = p.capabilities().testFlag(QNetworkProxy::TunnelingCapability);
        c.hasEndpoint = !p.hostName().isEmpty() && p.port() > 0;
        out.push_back(c);
    }
    return out;
}

} // namespace talq
