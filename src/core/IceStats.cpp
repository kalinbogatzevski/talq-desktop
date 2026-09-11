#include "core/IceStats.h"

#include <QDebug>
#include <QHash>
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>

namespace talq {
namespace {

struct CandidateInfo {
    QString type;       // "host", "srflx", "prflx", "relay"
    QString protocol;   // "udp" / "tcp"
    QString address;
    int port = 0;
};

QString str(const GstStructure *s, const char *field)
{
    const gchar *v = gst_structure_get_string(s, field);
    return v ? QString::fromUtf8(v) : QString();
}

// The reply is a flat structure whose every field is itself a structure keyed
// by stats id. Two passes: collect the candidates, then find the pair that
// succeeded and look its local candidate back up.
void reportStats(const GstStructure *reply, const QString &tag)
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
                guint p = 0;
                if (gst_structure_get_uint(entry, "port", &p)) c.port = int(p);
                candidates.insert(QString::fromUtf8(name), c);
            } else if (pass == 1 && type == GST_WEBRTC_STATS_CANDIDATE_PAIR) {
                // Only the pair actually carrying media. Field naming has
                // varied across GStreamer versions, so accept either spelling
                // rather than silently reporting nothing.
                gboolean nominated = FALSE;
                gst_structure_get_boolean(entry, "nominated", &nominated);
                const QString state = str(entry, "state");
                const bool succeeded = (state.compare(QStringLiteral("succeeded"),
                                                      Qt::CaseInsensitive) == 0);
                if (!succeeded && !nominated)
                    continue;
                const QString localId = str(entry, "local-candidate-id");
                if (!localId.isEmpty())
                    winningLocalId = localId;
            }
        }
    }

    if (winningLocalId.isEmpty()) {
        qDebug() << tag << "selected candidate pair: none reported yet";
        return;
    }
    const CandidateInfo c = candidates.value(winningLocalId);
    if (c.type.isEmpty()) {
        qDebug() << tag << "selected candidate pair: local id" << winningLocalId
                 << "(no candidate detail)";
        return;
    }

    // The whole point of the line. "relay" means the media is going through a
    // TURN server -- and if this machine has a media proxy configured, through
    // that proxy. "host"/"srflx" means it found a direct path and the proxy
    // work was not needed here.
    qDebug().nospace() << tag << " selected candidate pair: LOCAL type="
                       << c.type << " proto=" << c.protocol
                       << " " << c.address << ":" << c.port
                       << (c.type == QStringLiteral("relay")
                               ? "  <-- RELAYED (TURN)" : "  <-- direct");
}

} // namespace

void logSelectedCandidatePair(GstElement *webrtcbin, const QString &tag)
{
    if (!webrtcbin) return;

    // Heap copy: the reply lands on a GStreamer thread after this returns.
    auto *tagCopy = new QString(tag);
    GstPromise *promise = gst_promise_new_with_change_func(
        [](GstPromise *p, gpointer data) {
            QString *t = static_cast<QString *>(data);
            const GstStructure *reply = gst_promise_get_reply(p);
            if (reply)
                reportStats(reply, *t);
            delete t;
            gst_promise_unref(p);
        },
        tagCopy, nullptr);

    g_signal_emit_by_name(webrtcbin, "get-stats", nullptr, promise);
}

} // namespace talq
