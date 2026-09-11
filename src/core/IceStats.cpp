#include "core/IceStats.h"

#include <QDebug>
#include <QHash>
#include <gst/gst.h>
#include <memory>
#include <gst/webrtc/webrtc.h>

namespace talq {
namespace {

struct CandidateInfo {
    QString type;           // "host", "srflx", "prflx", "relay"
    QString protocol;       // the candidate's own transport
    QString relayProtocol;  // how we reach the TURN server: udp / tcp / tls
    QString address;
    int port = 0;
};

QString str(const GstStructure *s, const char *field)
{
    const gchar *v = gst_structure_get_string(s, field);
    return v ? QString::fromUtf8(v) : QString();
}

} // namespace

// Pull the selected pair out of a stats reply. Returns an invalid SelectedPair
// when nothing has settled yet, which is a normal transient state rather than
// an error.
SelectedPair parseSelectedPair(const GstStructure *reply)
{
    QHash<QString, CandidateInfo> candidates;
    QString winningLocalId;

    const int n = gst_structure_n_fields(reply);
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < n; ++i) {
            const gchar *name = gst_structure_nth_field_name(reply, i);
            const GValue *val = gst_structure_get_value(reply, name);
            if (!val || !GST_VALUE_HOLDS_STRUCTURE(val))
                continue;
            const GstStructure *entry = gst_value_get_structure(val);
            if (!entry) continue;

            GstWebRTCStatsType type;
            if (!gst_structure_get(entry, "type", GST_TYPE_WEBRTC_STATS_TYPE, &type, nullptr))
                continue;

            if (pass == 0 && (type == GST_WEBRTC_STATS_LOCAL_CANDIDATE
                           || type == GST_WEBRTC_STATS_REMOTE_CANDIDATE)) {
                CandidateInfo c;
                c.type     = str(entry, "candidate-type");
                c.protocol = str(entry, "protocol");
                c.address  = str(entry, "address");
                // Present only on relay candidates, and the only field that
                // distinguishes a proxied TURN/TCP path from a plain TURN/UDP
                // one. Verified present in this GStreamer build.
                c.relayProtocol = str(entry, "relay-protocol");
                // gstwebrtcnice's relay-type table carries a "none" nick
                // alongside udp/tcp/tls. It means "not a relay", not "over
                // UDP" -- and it would otherwise render as "relayed over none".
                if (c.relayProtocol.compare(QStringLiteral("none"),
                                            Qt::CaseInsensitive) == 0)
                    c.relayProtocol.clear();
                guint p = 0;
                if (gst_structure_get_uint(entry, "port", &p)) c.port = int(p);
                candidates.insert(QString::fromUtf8(name), c);
            } else if (pass == 1 && type == GST_WEBRTC_STATS_CANDIDATE_PAIR) {
                // NO state/nominated gate. Two releases shipped one keyed on a
                // field this build does not emit -- first "nominated", then
                // "state" -- and each time the parser returned nothing at all.
                // The candidate-pair structure here carries exactly
                // local-candidate-id and remote-candidate-id; GStreamer only
                // ever emits the SELECTED pair (its other branch logs "No
                // selected ICE candidate pair was found"), so the entry's
                // existence is the signal. Verified by dumping the literal run
                // around "ice-candidate-pair_%s" in libgstwebrtc.dll, and
                // pinned by ice_stats_parse_test against a synthetic reply.
                const QString localId = str(entry, "local-candidate-id");
                if (!localId.isEmpty())
                    winningLocalId = localId;
            }
        }
    }

    SelectedPair out;
    if (winningLocalId.isEmpty()) return out;
    const CandidateInfo c = candidates.value(winningLocalId);
    if (c.type.isEmpty()) return out;
    out.valid     = true;
    out.localType = c.type;
    out.protocol  = c.protocol;
    out.relayProtocol = c.relayProtocol;
    out.address   = c.address;
    out.port      = c.port;
    return out;
}

namespace {

void logPair(const SelectedPair &p, const QString &tag)
{
    if (!p.valid) {
        qDebug() << tag << "selected candidate pair: none reported yet";
        return;
    }
    // The whole point of the line. "relay" means the media is going through a
    // TURN server -- and if this machine has a media proxy configured, through
    // that proxy. "host"/"srflx" means it found a direct path and the proxy
    // work was not needed here.
    qDebug().nospace() << tag << " selected candidate pair: LOCAL type="
                       << p.localType << " proto=" << p.protocol
                       << (p.relayProtocol.isEmpty()
                               ? QString() : QStringLiteral(" relay-proto=") + p.relayProtocol)
                       << " " << p.address << ":" << p.port
                       << (!p.relayed()
                               ? "  <-- direct"
                               : p.relayCouldUseProxy()
                                     ? "  <-- RELAYED over TCP/TLS (can be proxied)"
                                     : p.relayProtocol.isEmpty()
                                           ? "  <-- RELAYED, transport to TURN unreported"
                                           : "  <-- RELAYED over UDP (never proxied)");
}

struct StatsCtx {
    QString tag;
    std::function<void(const SelectedPair &)> cb;
};

} // namespace

void querySelectedCandidatePair(GstElement *webrtcbin, const QString &tag,
                                std::function<void(const SelectedPair &)> onResult)
{
    if (!webrtcbin) return;

    // Heap-owned: the reply lands on a GStreamer thread after this returns.
    auto *ctx = new StatsCtx{tag, std::move(onResult)};
    GstPromise *promise = gst_promise_new_with_change_func(
        [](GstPromise *p, gpointer data) {
            std::unique_ptr<StatsCtx> c(static_cast<StatsCtx *>(data));
            SelectedPair sp;
            const GstStructure *reply = gst_promise_get_reply(p);
            if (reply)
                sp = parseSelectedPair(reply);
            logPair(sp, c->tag);
            if (c->cb)
                c->cb(sp);
            gst_promise_unref(p);
        },
        ctx, nullptr);

    g_signal_emit_by_name(webrtcbin, "get-stats", nullptr, promise);
}

void logSelectedCandidatePair(GstElement *webrtcbin, const QString &tag)
{
    querySelectedCandidatePair(webrtcbin, tag, nullptr);
}

} // namespace talq
