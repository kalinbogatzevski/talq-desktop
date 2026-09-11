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

typedef struct _GstElement GstElement;

namespace talq {

// Asynchronous: the reply arrives on a GStreamer thread and is logged there.
// Safe to call on a webrtcbin that is already connected; a null element is a
// no-op. `tag` prefixes the log line so the pipeline is identifiable.
void logSelectedCandidatePair(GstElement *webrtcbin, const QString &tag);

} // namespace talq
