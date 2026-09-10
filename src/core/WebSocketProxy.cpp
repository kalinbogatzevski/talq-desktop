#include "core/WebSocketProxy.h"
#include "core/WebSocketProxyPolicy.h"

#include <QNetworkProxy>
#include <QUrl>
#include <QWebSocket>
#include <vector>

namespace talq {
namespace {

int defaultPortFor(const QUrl &url)
{
    // A WebSocket URL usually carries no explicit port, but the proxy query
    // needs one -- and asking about port 0 is how you get a nonsense answer.
    const QString scheme = url.scheme().toLower();
    return url.port(scheme == QStringLiteral("wss") ? 443 : 80);
}

QList<QNetworkProxy> resolveCandidates(const QUrl &url)
{
    // TcpSocket, NOT the default UrlRequest query. This is the whole point:
    // for a URL request Qt will happily return a caching proxy, which is
    // correct for HTTP and fatal for a socket. Asking as a TCP socket makes
    // Qt answer the question we are actually asking.
    QNetworkProxyQuery query(url.host(), defaultPortFor(url), QString(),
                             QNetworkProxyQuery::TcpSocket);
    return QNetworkProxyFactory::proxyForQuery(query);
}

ProxyCandidate toPolicyInput(const QNetworkProxy &p)
{
    ProxyCandidate c;
    c.isDirect    = (p.type() == QNetworkProxy::NoProxy);
    c.canTunnel   = p.capabilities().testFlag(QNetworkProxy::TunnelingCapability);
    c.hasEndpoint = !p.hostName().isEmpty() && p.port() > 0;
    return c;
}

// Host and port only -- never user()/password(). This ends up in a log file.
QString describeCandidates(const QList<QNetworkProxy> &proxies)
{
    QString out;
    bool sawDirect = false;
    for (const QNetworkProxy &p : proxies) {
        if (p.type() == QNetworkProxy::NoProxy) {
            // Qt commonly returns several equivalent NoProxy entries; listing
            // "direct, direct, direct" reads like a bug in the log reader.
            if (sawDirect)
                continue;
            sawDirect = true;
        }
        if (!out.isEmpty())
            out += QStringLiteral(", ");
        if (p.type() == QNetworkProxy::NoProxy) {
            out += QStringLiteral("direct");
            continue;
        }
        out += QStringLiteral("%1:%2 type=%3 tunneling=%4")
                   .arg(p.hostName())
                   .arg(p.port())
                   .arg(int(p.type()))
                   .arg(p.capabilities().testFlag(QNetworkProxy::TunnelingCapability)
                            ? QStringLiteral("yes") : QStringLiteral("NO"));
    }
    return out.isEmpty() ? QStringLiteral("no proxy candidates") : out;
}

} // namespace

QString applyWebSocketProxy(QWebSocket &socket, const QUrl &url)
{
    // ONE resolution. Everything below -- the installed proxy and the line that
    // gets logged about it -- is derived from this single answer, so the two can
    // never disagree.
    const QList<QNetworkProxy> proxies = resolveCandidates(url);

    std::vector<ProxyCandidate> inputs;
    inputs.reserve(static_cast<size_t>(proxies.size()));
    for (const QNetworkProxy &p : proxies)
        inputs.push_back(toPolicyInput(p));

    const ProxyVerdict verdict =
        decideWebSocketProxy(inputs.data(), static_cast<int>(inputs.size()));

    QString action;
    switch (verdict.action) {
    case ProxyAction::Direct:
        socket.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        action = QStringLiteral("connecting direct");
        break;
    case ProxyAction::UseCandidate:
        socket.setProxy(proxies.at(verdict.index));
        action = QStringLiteral("using it as-is");
        break;
    case ProxyAction::RecastAsHttp: {
        // Same box, same port, same credentials -- only the type changes, so
        // that Qt will CONNECT-tunnel through it instead of refusing.
        const QNetworkProxy &src = proxies.at(verdict.index);
        socket.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, src.hostName(),
                                      src.port(), src.user(), src.password()));
        action = QStringLiteral("RE-CAST as HTTP CONNECT tunnel (it could not tunnel as given)");
        break;
    }
    }

    return QStringLiteral("%1 -> %2").arg(describeCandidates(proxies), action);
}

} // namespace talq
