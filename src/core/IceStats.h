#pragma once

// Ask webrtcbin which candidate pair actually won, and write it to the log.
//
// This exists because a working call proves almost nothing about HOW it
// connected. TalQ offers a proxied TURN/TCP relay candidate alongside the
// ordinary UDP ones and deliberately lets ICE choose, so a successful call on a
// restricted network is ambiguous between "the proxy path carried it" and "UDP
// was open all along and the new path was never exercised". Without this line
// the only way to tell them apart is a packet capture on someone else's PC.
//
// Emits the standard "get-stats" signal and reports the succeeded pair's LOCAL
// candidate type: host, srflx or relay.

#include <QString>
#include <functional>

typedef struct _GstElement GstElement;
typedef struct _GstStructure GstStructure;

namespace talq {

// What ICE actually settled on, as reported by webrtcbin's own stats.
struct SelectedPair {
    bool valid = false;      // false when no succeeded pair was reported yet
    QString localType;       // "host", "srflx", "prflx", "relay"
    QString protocol;        // the candidate's own transport: effectively always "udp"
    QString address;
    int port = 0;

    // How we reach the TURN SERVER, which is a different question from the
    // candidate's own protocol and the only one that identifies a proxied path.
    // Empty for a non-relay candidate. "udp" / "tcp" / "tls".
    //
    // This field is why `protocol` cannot be used for the same purpose: a
    // TURN/TCP relay candidate is still emitted with candidate transport UDP
    // (verified in libnice: conncheck.c hands discovery_add_relay_candidate
    // NICE_CANDIDATE_TRANSPORT_UDP whenever the socket is not "reliable"), so
    // a genuinely proxied relay and a plain TURN/UDP relay look identical
    // there.
    QString relayProtocol;

    // Is the media going through a TURN server rather than straight out?
    bool relayed() const { return localType == QStringLiteral("relay"); }

    // Could this relay have gone through an HTTP proxy? Only TURN over TCP or
    // TLS ever touches one -- libnice consults the proxy in exactly one place,
    // agent_create_tcp_turn_socket, which the UDP branch never reaches.
    bool relayCouldUseProxy() const
    {
        return relayed() && (relayProtocol == QStringLiteral("tcp")
                          || relayProtocol == QStringLiteral("tls"));
    }
};

// Asynchronous: the reply arrives on a GStreamer thread and is logged there.
// Safe to call on a webrtcbin that is already connected; a null element is a
// no-op. `tag` prefixes the log line so the pipeline is identifiable.
void logSelectedCandidatePair(GstElement *webrtcbin, const QString &tag);

// Same query, but hands the result back as well as logging it.
//
// ⚠ `onResult` is invoked on a GSTREAMER thread, not the caller's, and the
// object that asked may be destroyed before the answer arrives.
//
// A QPointer captured in the callback is NOT sufficient and must not be copied
// as a recipe: checking it on the GStreamer thread and then letting
// invokeMethod dereference it is a time-of-check/time-of-use race against a
// deleteLater() running on the Qt thread. Marshal through an object that is
// guaranteed to outlive the call (qApp) and gate on an ownership token that is
// destroyed with the receiver, so the liveness test happens on the receiver's
// own thread. See PublishPipeline::onIceStateChanged for the pattern.
void querySelectedCandidatePair(GstElement *webrtcbin, const QString &tag,
                                std::function<void(const SelectedPair &)> onResult);

// Parse a webrtcbin "get-stats" reply. Exposed so the parsing can be tested
// against a synthetic reply: two successive releases shipped a gate keyed on a
// field this GStreamer build does not emit ("nominated", then "state"), and
// both times the result was a feature that silently never produced anything.
// A byte-grep proves a literal exists SOMEWHERE in a DLL; it cannot prove the
// structure you are reading actually carries it. A test can.
SelectedPair parseSelectedPair(const GstStructure *reply);

} // namespace talq
