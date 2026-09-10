#include "core/WebSocketProxy.h"
#include "core/SystemProxy.h"
#include "core/WebSocketProxyPolicy.h"

#include <QNetworkProxy>
#include <QUrl>
#include <QWebSocket>
#include <QAuthenticator>
#include <QDebug>
#include <vector>

namespace talq {
namespace {

// Set by a 407, cleared by any WebSocket that actually connects.
//
// It must be clearable, and the reason is a laptop. One challenge on hotel or
// guest Wi-Fi would otherwise pin TalQ into a terminal "your proxy needs a
// sign-in" for the rest of the session -- red tray, undismissable banner --
// long after the user got home to a network with no proxy at all, where
// everything works. Worse, because that fault is terminal it is evaluated
// ahead of every other rule, so it would also BLIND the health report to a
// real outage that happened later the same evening.
//
// A successful connection is proof the proxy is not refusing us, so it is the
// right thing to clear on. Flicker is not a concern: the health policy already
// requires a fault to outlive a threshold before anyone is told about it.
bool g_proxyAuthFailed = false;


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
    const QList<QNetworkProxy> proxies = systemTcpProxiesFor(url);
    const std::vector<ProxyCandidate> inputs = toProxyCandidates(proxies);
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

void watchProxyAuthentication(QWebSocket &socket)
{
    // Any successful connection retires the flag: whatever the proxy did
    // before, it is not blocking us now.
    QObject::connect(&socket, &QWebSocket::connected, [&socket]() {
        if (g_proxyAuthFailed) {
            g_proxyAuthFailed = false;
            qInfo() << "Proxy authentication no longer blocking:"
                    << "a WebSocket connected successfully.";
        }
    });

    QObject::connect(&socket, &QWebSocket::proxyAuthenticationRequired,
                     [](const QNetworkProxy &proxy, QAuthenticator *) {
        // We deliberately do NOT fill the authenticator in. TalQ holds no proxy
        // credentials, and guessing the logged-in user's domain password would
        // be both wrong and a good way to lock an account out. Leaving it empty
        // fails the CONNECT, which is the honest outcome -- and now a reported
        // one instead of a silent reconnect loop.
        if (!g_proxyAuthFailed) {
            g_proxyAuthFailed = true;
            qWarning() << "Proxy at" << proxy.hostName()
                       << "requires authentication that TalQ cannot supply;"
                       << "live connections and call media will not work"
                       << "until it is exempted or made open to this machine.";
        }
    });
}

bool proxyAuthenticationFailed()
{
    return g_proxyAuthFailed;
}

} // namespace talq
