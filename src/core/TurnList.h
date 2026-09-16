#pragma once

// Qt side of TurnListPolicy.h. Every call pipeline turns its TURN entries into
// add-turn-server URIs here, and CallManager orders the entries by probe RTT
// here, so there is exactly one TURN URI transform in the tree. Header-only.

#include "core/SignalingClient.h"   // TurnServer
#include "core/TurnListPolicy.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace talq {

inline std::vector<TurnEntryIn> toTurnEntries(const QList<TurnServer> &servers)
{
    std::vector<TurnEntryIn> out;
    out.reserve(size_t(servers.size()));
    for (const TurnServer &ts : servers) {
        TurnEntryIn e;
        for (const QString &u : ts.urls) e.urls.push_back(u.toStdString());
        e.username = ts.username.toStdString();
        e.credential = ts.credential.toStdString();
        out.push_back(std::move(e));
    }
    return out;
}

struct GstTurnList {
    QList<QByteArray> uris;   // one libnice relay each; they carry the credential, never log them
    QStringList logUris;      // same order, credential replaced by ***
};

// The URIs a pipeline hands webrtcbin (or webrtcsrc's turn-servers): at most 8
// relays, every host kept, `servers`' host order deciding what is cut.
inline GstTurnList gstTurnList(const QList<TurnServer> &servers)
{
    GstTurnList out;
    for (const TurnRelay &r : planTurnRelays(toTurnEntries(servers))) {
        out.uris << QByteArray::fromStdString(r.uri());
        out.logUris << QString::fromStdString(r.logUri());
    }
    return out;
}

inline TurnRttByHost toTurnRtt(const QHash<QString, int> &rttByHost)
{
    TurnRttByHost rtt;
    for (auto it = rttByHost.cbegin(); it != rttByHost.cend(); ++it)
        rtt[it.key().toStdString()] = it.value();
    return rtt;
}

// The TURN entries a pipeline is built with: `servers` regrouped by host in
// plan order (reached hosts by RTT, then unprobed, then unreachable,
// `preferredHost` first within the last two), limited to the nearby hosts when
// `narrow` (TurnListPolicy.h turnEntriesForBuild). URLs that do not parse are
// dropped; every part keeps its entry's credentials.
inline QList<TurnServer> turnServersForBuild(const QList<TurnServer> &servers,
                                             const QHash<QString, int> &rttByHost,
                                             const QString &preferredHost,
                                             bool narrow)
{
    QList<TurnServer> out;
    for (const TurnEntryIn &e : turnEntriesForBuild(toTurnEntries(servers), toTurnRtt(rttByHost),
                                                    preferredHost.toStdString(), narrow)) {
        TurnServer ts;
        for (const std::string &u : e.urls) ts.urls << QString::fromStdString(u);
        ts.username = QString::fromStdString(e.username);
        ts.credential = QString::fromStdString(e.credential);
        out << ts;
    }
    return out;
}

// The distinct TURN hosts (lower-case) turnServersForBuild would keep, in plan
// order. With no RTT, no preferred host and no narrowing that is the order
// they appear in `servers`.
inline QStringList turnHosts(const QList<TurnServer> &servers,
                             const QHash<QString, int> &rttByHost = {},
                             const QString &preferredHost = {},
                             bool narrow = false)
{
    QStringList out;
    for (const std::string &h : turnHostsForBuild(toTurnEntries(servers), toTurnRtt(rttByHost),
                                                  preferredHost.toStdString(), narrow))
        out << QString::fromStdString(h);
    return out;
}

// The port to RTT-probe per host (TurnListPolicy.h turnProbePortByHost).
inline QHash<QString, quint16> turnProbePorts(const QList<TurnServer> &servers)
{
    QHash<QString, quint16> out;
    for (const auto &kv : turnProbePortByHost(toTurnEntries(servers)))
        out.insert(QString::fromStdString(kv.first), quint16(kv.second));
    return out;
}

} // namespace talq
