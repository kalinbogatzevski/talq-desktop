#include "core/MediaProxy.h"
#include "core/MediaProxyPolicy.h"
#include "core/SystemProxy.h"

#include <QNetworkProxy>
#include <QRegularExpression>
#include <QUrl>

namespace talq {

QString mediaHttpProxyUrl(const QString &probeHost, int probePort)
{
    if (probeHost.isEmpty() || probePort <= 0)
        return QString();

    const QList<QNetworkProxy> proxies = systemTcpProxiesFor(probeHost, probePort);
    const std::vector<ProxyCandidate> inputs = toProxyCandidates(proxies);
    const int chosen = decideMediaProxy(inputs.data(), static_cast<int>(inputs.size()));
    if (chosen < 0)
        return QString();

    const QNetworkProxy &p = proxies.at(chosen);

    // Percent-encode the credentials. A corporate password routinely contains
    // '@' or ':', and libnice parses this string as a URI -- an unescaped '@'
    // silently relocates the host and the CONNECT goes somewhere absurd.
    QString auth;
    if (!p.user().isEmpty()) {
        auth = QString::fromLatin1(QUrl::toPercentEncoding(p.user()));
        if (!p.password().isEmpty())
            auth += QLatin1Char(':')
                  + QString::fromLatin1(QUrl::toPercentEncoding(p.password()));
        auth += QLatin1Char('@');
    }
    // Built by concatenation, NOT QString::arg. arg() re-scans the string it
    // just produced, so a percent-encoded credential is a live minefield: a
    // password containing ':' encodes to "%3A", and a following .arg(port) then
    // substitutes into that "%3", turning "p%40ss%3Aword" into
    // "p%40ss8080Aword". Silent, and only visible once a proxy rejects the
    // credentials for no apparent reason.
    return QStringLiteral("http://") + auth + p.hostName()
         + QLatin1Char(':') + QString::number(p.port());
}

QString mediaHttpProxyUrlForTurn(const QStringList &turnUrls)
{
    for (const QString &raw : turnUrls) {
        // "turn:host:3478?transport=udp" / "turns://user@host:5349" and the
        // variants in between. Peel off scheme, credentials and query rather
        // than trusting QUrl, which does not treat turn: as hierarchical.
        QString s = raw.trimmed();
        const int q = s.indexOf(QLatin1Char('?'));
        if (q >= 0) s.truncate(q);
        const bool secure = s.startsWith(QStringLiteral("turns"), Qt::CaseInsensitive);
        const int colon = s.indexOf(QLatin1Char(':'));
        if (colon >= 0) s = s.mid(colon + 1);
        if (s.startsWith(QStringLiteral("//"))) s = s.mid(2);
        const int at = s.lastIndexOf(QLatin1Char('@'));
        if (at >= 0) s = s.mid(at + 1);
        if (s.isEmpty()) continue;

        QString host = s;
        int port = secure ? 5349 : 3478;
        const int hostPortSep = s.lastIndexOf(QLatin1Char(':'));
        if (hostPortSep > 0) {
            bool ok = false;
            const int parsed = s.mid(hostPortSep + 1).toInt(&ok);
            if (ok && parsed > 0) {
                host = s.left(hostPortSep);
                port = parsed;
            }
        }
        // Reject anything that is not plausibly a host. A malformed TURN URL
        // must not silently become a proxy lookup for a nonsense destination:
        // against a PAC script or a bypass list, the wrong host gets the wrong
        // answer, and the failure looks like "media is proxied on this machine
        // and not that one" rather than like a parse bug.
        if (host.isEmpty() || host.contains(QLatin1Char(':'))
            || host.contains(QLatin1Char('/')))
            continue;
        const QString url = mediaHttpProxyUrl(host, port);
        if (!url.isEmpty())
            return url;
        // A parseable host that resolves to "no proxy" is a real answer, not a
        // parse failure: stop rather than hunting for a URL that says yes.
        return QString();
    }
    return QString();
}

QString maskProxyCredentials(const QString &proxyUrl)
{
    QString masked = proxyUrl;
    masked.replace(QRegularExpression(QStringLiteral("://[^@/]+@")),
                   QStringLiteral("://***@"));
    return masked;
}

} // namespace talq
