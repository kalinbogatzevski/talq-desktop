#pragma once

#include <QObject>
#include <QString>
#include <QHash>
#include <QMutex>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

// AEC far-end reference bus — see docs/aec-design.md for the full rationale.
//
// One always-on GStreamer pipeline that carries a COMPOSITE of every remote
// peer's decoded playback audio past a single webrtcechoprobe:
//
//   (silent keepalive) ─┐
//   appsrc(peerA) ──────┤
//   appsrc(peerB) ──────┼─→ audiomixer(talq-far-mix) → caps(48k/S16LE/mono)
//   appsrc(peerN) ──────┘       → webrtcechoprobe(talq-aec-probe) → queue → fakesink
//
// The probe registers its NAME in webrtcdsp's process-global registry the
// instant it is created, so the PUBLISHER's webrtcdsp(echo-cancel=true,
// probe="talq-aec-probe") can acquire it at its READY→PAUSED transition. That
// is why the bus MUST be brought to PLAYING *before* the publisher starts —
// otherwise gst_webrtc_dsp_start fails ("No echo probe found") and the call
// drops (the exact original bug this design fixes).
//
// The probe tapping the mixer OUTPUT means it sees the sum of all peers — the
// correct composite far-end reference. A silent keepalive input keeps the
// mixer producing a steady stream even with zero peers, so the probe is never
// starved. Real playback still happens on the per-subscriber wasapi2sink; this
// bus is a parallel TAP (fakesink) so the proven playback path is untouched.
//
// Lifecycle: created at call-start before the publisher, survives subscriber
// join/leave churn (mixer/probe/sink never restart — only per-peer appsrc
// branches are added/removed), torn down last at call-end. Everything is
// strictly non-fatal: if a required plugin is missing or the bus fails to
// build, start() returns false and AEC simply stays off.
class SharedFarEndBus : public QObject
{
    Q_OBJECT
public:
    // The name webrtcdsp(probe=...) looks up in the process-global registry.
    static constexpr const char *kProbeName = "talq-aec-probe";

    explicit SharedFarEndBus(QObject *parent = nullptr);
    ~SharedFarEndBus() override;

    // Build the bus and bring it to PLAYING. Returns false (and leaves the
    // object inert) if webrtcechoprobe/audiomixer are unavailable or the
    // pipeline can't start — the caller must then keep AEC OFF.
    bool start();
    void stop();
    bool isReady() const { return m_ready; }

    // Add / remove a peer's input branch. Call on the MAIN (Qt) thread.
    void attach(const QString &peerId);
    void detach(const QString &peerId);

    // Push one decoded far-end audio sample for an attached peer. Safe to call
    // from a GStreamer streaming thread; a no-op if the peer isn't attached.
    void pushSample(const QString &peerId, GstSample *sample);

private:
    GstElement *m_pipeline = nullptr;
    GstElement *m_mixer    = nullptr;   // audiomixer (request pads)
    GstElement *m_probe    = nullptr;   // webrtcechoprobe (named kProbeName)
    GstElement *m_sink     = nullptr;   // fakesink
    bool m_ready = false;

    struct Peer {
        GstElement *appsrc   = nullptr;
        GstElement *queue    = nullptr;
        GstPad     *mixerPad = nullptr; // mixer's request sink_%u (owned ref)
    };
    QHash<QString, Peer> m_peers;
    // Guards m_peers: attach/detach run on the Qt thread, pushSample on a
    // streaming thread. Held across pushSample so a concurrent detach can't
    // pull the appsrc out from under an in-flight push.
    QMutex m_mutex;
};
