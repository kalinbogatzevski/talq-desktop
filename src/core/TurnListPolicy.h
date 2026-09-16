#pragma once

// The TURN relay plan: which TURN URIs every call pipeline hands webrtcbin.
// One function for all of them (publisher, camera subscriber, screen share,
// screen subscriber, P2P), because four hand-copied loops and one uncapped one
// is how the 2026-09-16 incident happened.
//
// Established from gst-plugins-bad 1.28.1 (gst-libs/gst/webrtc/nice/nice.c,
// gst/gsturi.c) and libnice 0.1.23 (agent/agent.c, agent/candidate.c,
// socket/), because each of these is easy to assume wrongly:
//
//   * RELAYS PER URI. _add_turn_server makes one libnice relay per transport:
//     turns://  -> 1 TURN_TLS, whatever ?transport= says;
//     turn://?transport=udp -> 1 TURN_UDP;  turn://?transport=tcp -> 1 TURN_TCP;
//     turn://   with no transport -> 2 (UDP and TCP).
//     _validate_turn_server rejects any other transport value, any other query
//     key, a scheme other than turn/turns, and userinfo without both a user and
//     a password. GstUri does not lower-case the scheme, and without "//" it
//     parses no host, so "turn:host" yields nothing.
//
//   * THE LIMIT. nice_agent_set_relay_info refuses a 9th relay per component
//     with a g_warning. webrtcbin uses one component per bundled stream, so a
//     plan of at most 8 relays never warns. Concurrent warnings from many
//     agents are what reached the fatal GLib log race on 2026-09-16.
//
//   * DE-DUPLICATION. add-turn-server inserts into a GHashTable keyed on the
//     exact URI string. Only byte-identical strings collapse: Nextcloud's
//     "turns:h:5349?transport=udp" and "...?transport=tcp" are two keys and
//     become two identical TLS relays. So the plan de-duplicates on
//     (host, port, relay kind), and every URI it emits is exactly one relay.
//
//   * ORDER DOES NOT REACH LIBNICE. The servers are applied to the ICE stream
//     when webrtcbin creates it (transport_stream_new -> add_stream), which is
//     after every pipeline's add-turn-server loop, by g_hash_table_foreach:
//     hash order, not call order. libnice's relay priority is its position in
//     that list (turn->preference). So this plan decides WHICH relays exist,
//     never which one ICE prefers. Its order only decides what is cut when the
//     budget runs out, and is what the logs show. Keeping a relay local means
//     leaving far hosts out: see NEAREST-POP NARROWING below.
//
//   * TLS IS NOT TLS. libnice 0.1.23 has no TLS socket (socket/ holds only
//     pseudossl, a fake record header for Google/MSOC compatibility). Under
//     webrtcbin's NICE_COMPATIBILITY_RFC5245 a TURN_TLS relay is plain TURN
//     over TCP sent to the turns: port. Both TCP and TLS relays go through
//     agent_create_tcp_turn_socket, the http-proxy path. TLS ranks last.
//
// The plan GUARANTEES every host one relay before any host gets a second, so a
// POP is never silently dropped by the cap (the old publisher loop capped at 8
// URL strings and so always kept one dead POP and never offered the third).
//
// Host order: hosts the RTT probe reached, fastest first; then hosts never
// probed (the preferred host first, then config order); then hosts the probe
// could not reach (preferred first, then config order). An unreachable host is
// ranked last, NOT removed: the probe is UDP STUN only, and on a network that
// blocks UDP every host reads unreachable while TURN over TCP still works.
//
// Pure C++, no Qt; see TurnList.h for the adapter the pipelines use.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace talq {

// NICE_CANDIDATE_MAX_TURN_SERVERS (libnice 0.1.23 candidate.h).
constexpr int kLibniceMaxTurnServersPerComponent = 8;

// Declared in preference order within one host: UDP, then TCP (which also
// carries the http-proxy path), then TLS (plain TCP to the turns: port here).
enum class TurnRelayKind { Udp = 0, Tcp = 1, Tls = 2 };

struct TurnUrlParts {
    std::string host;               // lower-case; IPv6 without brackets
    int port = 0;                   // defaulted to 3478 / 5349 when absent
    bool secure = false;            // turns:
    bool hasTransport = false;
    TurnRelayKind transport = TurnRelayKind::Udp;   // meaningful when hasTransport
    bool hasAuthority = false;      // "//" present -- GstUri needs it for a host
    bool hasUserinfo = false;
    bool userinfoHasPassword = false;
};

// One Nextcloud "turnservers" entry.
struct TurnEntryIn {
    std::vector<std::string> urls;
    std::string username;
    std::string credential;
};

// Probe result per host (lower-case, as parseTurnUrl reports it): RTT in ms
// when the host answered, -1 when it was probed and did not. A host missing
// from the map was never probed.
using TurnRttByHost = std::map<std::string, int>;

namespace turn_detail {

inline std::string lower(std::string s)
{
    for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string trim(const std::string &s)
{
    const auto b = s.find_first_not_of(" \t\r\n\v\f");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n\v\f");
    return s.substr(b, e - b + 1);
}

inline bool parsePort(const std::string &s, int &port)
{
    if (s.empty() || s.size() > 5) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v < 1 || v > 65535) return false;
    port = v;
    return true;
}

inline bool validHostChars(const std::string &host, bool ipv6)
{
    for (char ch : host) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (ipv6) {
            if (!(std::isxdigit(c) || c == ':' || c == '.')) return false;
        } else if (!(std::isalnum(c) || c == '-' || c == '.' || c == '_')) {
            return false;
        }
    }
    return true;
}

} // namespace turn_detail

// Parse a Nextcloud TURN URL: "turn:host:3478?transport=udp", "turns://host",
// "turn://user:pass@[::1]:3478". Userinfo is noted but ignored -- credentials
// come from the entry. Returns false for anything webrtcnice would reject.
inline bool parseTurnUrl(const std::string &url, TurnUrlParts &out)
{
    using namespace turn_detail;
    out = TurnUrlParts{};
    std::string s = trim(url);

    const auto colon = s.find(':');
    if (colon == std::string::npos) return false;
    const std::string scheme = lower(s.substr(0, colon));
    if (scheme == "turn") out.secure = false;
    else if (scheme == "turns") out.secure = true;
    else return false;
    std::string rest = s.substr(colon + 1);
    if (rest.compare(0, 2, "//") == 0) {
        out.hasAuthority = true;
        rest.erase(0, 2);
    }

    std::string query;
    const auto q = rest.find('?');
    if (q != std::string::npos) {
        query = rest.substr(q + 1);
        rest.erase(q);
    }
    if (!query.empty()) {
        // RFC 7065 defines exactly one parameter; webrtcnice rejects any other.
        const auto eq = query.find('=');
        if (eq == std::string::npos || query.find('&') != std::string::npos
            || lower(query.substr(0, eq)) != "transport")
            return false;
        const std::string value = lower(query.substr(eq + 1));
        if (value == "udp") out.transport = TurnRelayKind::Udp;
        else if (value == "tcp") out.transport = TurnRelayKind::Tcp;
        else return false;
        out.hasTransport = true;
    }

    const auto at = rest.rfind('@');
    if (at != std::string::npos) {
        out.hasUserinfo = true;
        out.userinfoHasPassword = rest.substr(0, at).find(':') != std::string::npos;
        rest.erase(0, at + 1);
    }

    std::string host;
    std::string portText;
    bool hasPort = false;
    bool ipv6 = false;
    if (!rest.empty() && rest[0] == '[') {
        const auto rb = rest.find(']');
        if (rb == std::string::npos) return false;
        host = rest.substr(1, rb - 1);
        ipv6 = true;
        const std::string after = rest.substr(rb + 1);
        if (!after.empty()) {
            if (after[0] != ':') return false;
            hasPort = true;
            portText = after.substr(1);
        }
    } else {
        const auto c = rest.find(':');
        if (c != std::string::npos) {
            host = rest.substr(0, c);
            hasPort = true;
            portText = rest.substr(c + 1);
        } else {
            host = rest;
        }
    }
    if (host.empty() || !validHostChars(host, ipv6)) return false;
    if (hasPort) {
        if (!parsePort(portText, out.port)) return false;
    } else {
        out.port = out.secure ? 5349 : 3478;
    }
    out.host = lower(host);
    return true;
}

// Relays webrtcnice creates for one add-turn-server URI (0 = rejected).
inline int webrtcniceRelaysForUri(const std::string &uri)
{
    TurnUrlParts p;
    if (!parseTurnUrl(uri, p) || !p.hasAuthority || !p.hasUserinfo
        || !p.userinfoHasPassword)
        return 0;
    // GstUri keeps the scheme exactly as written and webrtcnice compares it
    // case-sensitively.
    if (uri.compare(uri.find_first_not_of(" \t\r\n\v\f"), p.secure ? 6 : 5,
                    p.secure ? "turns:" : "turn:") != 0)
        return 0;
    if (p.secure || p.hasTransport) return 1;
    return 2;
}

// Relays one webrtcbin ends up with for a list of add-turn-server URIs,
// before libnice's cap: identical strings collapse, nothing else does.
inline int webrtcniceRelayCount(const std::vector<std::string> &uris)
{
    std::set<std::string> unique(uris.begin(), uris.end());
    int n = 0;
    for (const auto &u : unique) n += webrtcniceRelaysForUri(u);
    return n;
}

// RFC 3986 unreserved characters pass; every other byte becomes %XX, as
// QUrl::toPercentEncoding does. Talk's TURN usernames are "<expiry>:<user>",
// and a raw ':' would move the user/password split in _parse_userinfo.
inline std::string percentEncodeUriComponent(const std::string &in)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (char ch : in) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

struct TurnRelay {
    std::string host;
    int port = 0;
    TurnRelayKind kind = TurnRelayKind::Udp;
    std::string username;
    std::string credential;

    std::string hostPort() const
    {
        const bool v6 = host.find(':') != std::string::npos;
        return (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
    }
    std::string suffix() const
    {
        switch (kind) {
        case TurnRelayKind::Udp: return "?transport=udp";
        case TurnRelayKind::Tcp: return "?transport=tcp";
        case TurnRelayKind::Tls: return {};
        }
        return {};
    }
    std::string scheme() const { return kind == TurnRelayKind::Tls ? "turns://" : "turn://"; }

    // Exactly one webrtcnice relay. Carries the credential: never log it.
    std::string uri() const
    {
        return scheme() + percentEncodeUriComponent(username) + ":"
             + percentEncodeUriComponent(credential) + "@" + hostPort() + suffix();
    }
    std::string logUri() const { return scheme() + "***@" + hostPort() + suffix(); }
};

// Distinct hosts of the valid URLs, in plan order (see the header comment).
inline std::vector<std::string> orderTurnHosts(const std::vector<TurnEntryIn> &entries,
                                               const TurnRttByHost &rttByHost,
                                               const std::string &preferredHost)
{
    std::vector<std::string> hosts;
    for (const auto &e : entries)
        for (const auto &url : e.urls) {
            TurnUrlParts p;
            if (parseTurnUrl(url, p)
                && std::find(hosts.begin(), hosts.end(), p.host) == hosts.end())
                hosts.push_back(p.host);
        }

    std::map<std::string, int> rtt;
    for (const auto &kv : rttByHost) rtt[turn_detail::lower(kv.first)] = kv.second;
    const std::string preferred = turn_detail::lower(turn_detail::trim(preferredHost));

    // (class, rtt, not-preferred, config index); class 0 reached, 1 unprobed,
    // 2 unreachable.
    using Key = std::tuple<int, int, int, std::size_t>;
    std::vector<std::pair<Key, std::string>> keyed;
    for (std::size_t i = 0; i < hosts.size(); ++i) {
        const auto it = rtt.find(hosts[i]);
        const int cls = it == rtt.end() ? 1 : (it->second >= 0 ? 0 : 2);
        const int ms = cls == 0 ? it->second : 0;
        keyed.push_back({Key{cls, ms, hosts[i] == preferred ? 0 : 1, i}, hosts[i]});
    }
    std::sort(keyed.begin(), keyed.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    std::vector<std::string> ordered;
    for (const auto &k : keyed) ordered.push_back(k.second);
    return ordered;
}

namespace turn_detail {

// The entries regrouped by `hosts`, in that order, each part keeping its
// entry's credentials. URLs that do not parse or name another host are dropped.
inline std::vector<TurnEntryIn> entriesForHosts(const std::vector<TurnEntryIn> &entries,
                                                const std::vector<std::string> &hosts)
{
    std::vector<TurnEntryIn> out;
    for (const auto &host : hosts)
        for (const auto &e : entries) {
            TurnEntryIn part;
            part.username = e.username;
            part.credential = e.credential;
            for (const auto &url : e.urls) {
                TurnUrlParts p;
                if (parseTurnUrl(url, p) && p.host == host) part.urls.push_back(url);
            }
            if (!part.urls.empty()) out.push_back(std::move(part));
        }
    return out;
}

} // namespace turn_detail

// The entries regrouped by host in plan order, each keeping its credentials and
// dropping URLs that do not parse. planTurnRelays on the result, with no probe,
// equals planTurnRelays on the original with the probe.
inline std::vector<TurnEntryIn> reorderTurnEntries(const std::vector<TurnEntryIn> &entries,
                                                   const TurnRttByHost &rttByHost,
                                                   const std::string &preferredHost)
{
    return turn_detail::entriesForHosts(entries, orderTurnHosts(entries, rttByHost, preferredHost));
}

// NEAREST-POP NARROWING (0.57.6: a client's media was relayed through another region's POP).
// Since relay priority among POPs is hash order (see above), the only way to
// keep a relay local is to leave the far POPs out of a pipeline's plan. That
// narrowed plan is also a trap (failure model B4): if its POP dies, every
// rebuild offers only the dead relay, and CallManager re-probes only while the
// call is Active. So a build narrows only when all of these hold:
//   * the call is Active, where the probe re-runs every ~8 s;
//   * the probe result is younger than kTurnNarrowFreshMs; an older one means
//     the probe stopped re-running, so a POP that died since is not measured;
//   * it is not a recovery rebuild, because a relay can fail while its host
//     still answers the STUN probe.
// Everything else (the publisher, which is built in Connecting and rebuilt in
// Reconnecting, and every recovery) gets the full budgeted plan.
constexpr int kTurnNarrowMaxBestRttMs = 120;   // best reached host must be closer than this
constexpr int kTurnNarrowMarginMs = 40;        // hosts within best + margin stay
constexpr long long kTurnNarrowFreshMs = 20000;

// Hosts a narrowed build keeps: every host that answered within best + margin,
// when the best one answered under maxBestRttMs. UDP-STUN RTT reads ~5-40 ms
// in-region and ~150+ ms cross-continent. Empty = no narrowing: nothing
// answered, or every host is far and redundancy is worth more.
inline std::set<std::string> nearbyTurnHosts(const TurnRttByHost &rttByHost,
                                             int maxBestRttMs = kTurnNarrowMaxBestRttMs,
                                             int marginMs = kTurnNarrowMarginMs)
{
    int best = -1;
    for (const auto &kv : rttByHost)
        if (kv.second >= 0 && (best < 0 || kv.second < best)) best = kv.second;
    std::set<std::string> hosts;
    if (best < 0 || best >= maxBestRttMs) return hosts;
    for (const auto &kv : rttByHost)
        if (kv.second >= 0 && kv.second <= best + marginMs)
            hosts.insert(turn_detail::lower(kv.first));
    return hosts;
}

// Whether a pipeline build may narrow (see NEAREST-POP NARROWING).
// probeAgeMs: age of the probe result in force, -1 when there is none.
inline bool mayNarrowTurnHosts(bool callActive, long long probeAgeMs, bool recoveryBuild,
                               long long freshMs = kTurnNarrowFreshMs)
{
    return callActive && !recoveryBuild && probeAgeMs >= 0 && probeAgeMs < freshMs;
}

// The hosts a build uses, in plan order: all of them, or with `narrow` only the
// nearby ones. Falls back to all when no nearby host is in this list.
inline std::vector<std::string> turnHostsForBuild(const std::vector<TurnEntryIn> &entries,
                                                  const TurnRttByHost &rttByHost,
                                                  const std::string &preferredHost,
                                                  bool narrow)
{
    std::vector<std::string> hosts = orderTurnHosts(entries, rttByHost, preferredHost);
    if (!narrow) return hosts;
    const std::set<std::string> keep = nearbyTurnHosts(rttByHost);
    std::vector<std::string> kept;
    for (const auto &h : hosts)
        if (keep.count(h)) kept.push_back(h);
    return kept.empty() ? hosts : kept;
}

// The entries a pipeline is built with: turnHostsForBuild's hosts, regrouped
// as reorderTurnEntries does. `narrow` is mayNarrowTurnHosts(...).
inline std::vector<TurnEntryIn> turnEntriesForBuild(const std::vector<TurnEntryIn> &entries,
                                                    const TurnRttByHost &rttByHost,
                                                    const std::string &preferredHost,
                                                    bool narrow)
{
    return turn_detail::entriesForHosts(
        entries, turnHostsForBuild(entries, rttByHost, preferredHost, narrow));
}

// The budgeted plan: round-robin over hosts in plan order, each host's relays
// in UDP, TCP, TLS order, one (host, port, kind) once, at most maxRelays.
inline std::vector<TurnRelay> planTurnRelays(const std::vector<TurnEntryIn> &entries,
                                             const TurnRttByHost &rttByHost = {},
                                             const std::string &preferredHost = {},
                                             int maxRelays = kLibniceMaxTurnServersPerComponent)
{
    std::vector<TurnRelay> plan;
    if (maxRelays <= 0) return plan;

    const std::vector<std::string> hosts = orderTurnHosts(entries, rttByHost, preferredHost);
    std::map<std::string, std::vector<TurnRelay>> byHost;
    std::set<std::tuple<std::string, int, int>> seen;
    auto add = [&](const TurnUrlParts &p, TurnRelayKind kind, const TurnEntryIn &e) {
        if (!seen.insert({p.host, p.port, int(kind)}).second) return;   // first entry wins
        TurnRelay r;
        r.host = p.host;
        r.port = p.port;
        r.kind = kind;
        r.username = e.username;
        r.credential = e.credential;
        byHost[p.host].push_back(std::move(r));
    };
    for (const auto &e : entries)
        for (const auto &url : e.urls) {
            TurnUrlParts p;
            if (!parseTurnUrl(url, p)) continue;
            if (p.secure) {
                add(p, TurnRelayKind::Tls, e);
            } else if (p.hasTransport) {
                add(p, p.transport, e);
            } else {
                add(p, TurnRelayKind::Udp, e);
                add(p, TurnRelayKind::Tcp, e);
            }
        }
    for (auto &kv : byHost)
        std::stable_sort(kv.second.begin(), kv.second.end(),
                         [](const TurnRelay &a, const TurnRelay &b) { return a.kind < b.kind; });

    for (std::size_t round = 0;; ++round) {
        bool any = false;
        for (const auto &host : hosts) {
            const auto &relays = byHost[host];
            if (round >= relays.size()) continue;
            any = true;
            plan.push_back(relays[round]);
            if (int(plan.size()) >= maxRelays) return plan;
        }
        if (!any) return plan;
    }
}

// The port CallManager's RTT probe sends its UDP STUN Binding to, per host
// (lower-case): the host's first plain turn: port, because that listener takes
// UDP, else its first turns: port. Hosts with no valid URL are not probed.
inline std::map<std::string, int> turnProbePortByHost(const std::vector<TurnEntryIn> &entries)
{
    std::map<std::string, int> ports;
    std::set<std::string> fromPlainTurn;
    for (const auto &e : entries)
        for (const auto &url : e.urls) {
            TurnUrlParts p;
            if (!parseTurnUrl(url, p) || fromPlainTurn.count(p.host)) continue;
            if (!p.secure) {
                ports[p.host] = p.port;
                fromPlainTurn.insert(p.host);
            } else if (!ports.count(p.host)) {
                ports[p.host] = p.port;
            }
        }
    return ports;
}

} // namespace talq
