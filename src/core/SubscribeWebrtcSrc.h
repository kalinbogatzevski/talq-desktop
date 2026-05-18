#pragma once

// Receive-only video/audio for the Talk MCU, built on gst-plugins-rs
// `webrtcsrc` instead of a hand-driven webrtcbin. webrtcsrc owns
// webrtcbin internally and gets the receive negotiation (RTX/FEC,
// transceivers, jitterbuffer, decode, threading) right — the exact
// things the old SubscribePipeline kept failing at. We feed it the
// Nextcloud-spreed↔Janus signalling through a custom GObject that
// implements gst-plugins-rs' `Signallable` interface; this class is the
// Qt bridge for that signaller and exposes the SAME surface CallManager
// already used for SubscribePipeline (drop-in).

#include <QObject>
#include <QString>
#include <QList>
#include <QPair>
#include <QMutex>
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/app/gstappsink.h>
#include "SignalingClient.h"

class VideoFrameProvider;

class SubscribeWebrtcSrc : public QObject
{
    Q_OBJECT
public:
    explicit SubscribeWebrtcSrc(const QString &remoteSessionId, QObject *parent = nullptr);
    ~SubscribeWebrtcSrc() override;

    bool start(const QString &stunServer, const QList<TurnServer> &turnServers,
               const QString &audioOutputDeviceId = {});
    void stop();
    void setRemoteOffer(const QString &sdp);
    void addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    bool isRunning() const { return m_running; }
    VideoFrameProvider *videoProvider() const { return m_videoProvider; }
    QString videoCodec() const { return m_videoCodec; }
    QString videoDecoder() const { return QStringLiteral("webrtcsrc"); }

signals:
    void localAnswerReady(const QString &sdp);
    void iceCandidateReady(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void iceGatheringComplete();
    void iceStateChanged(const QString &state);
    void mediaStateReceived(const QString &type);     // unused via webrtcsrc (signaling drives mute)
    void peerClientInfo(const QString &client, const QString &version);
    void error(const QString &message);

private:
    void cleanup();
    // GstRSWebRTCSignallableIface implementor (registered once at runtime).
    GObject *makeSignaller();
    void onVideoSample(GstSample *sample);
    // Emit session-started + session-description(offer) into the signaller
    // once BOTH webrtcsrc has emitted "start" and we hold the Janus offer.
    // Drives the answerer handshake; safe to call from either thread.
    void feedOfferToSignaller();

    // webrtcsrc→signaller signal handlers (it emits these ON the signaller;
    // we connect, run before the Rust no-op default, forward to spreed).
    static gboolean sigStart(GObject *signaller, gpointer self);
    static gboolean sigStop(GObject *signaller, gpointer self);
    static gboolean sigSendSdp(GObject *signaller, const gchar *sessionId,
                               GstWebRTCSessionDescription *desc, gpointer self);
    static gboolean sigSendIce(GObject *signaller, const gchar *sessionId,
                               const gchar *candidate, guint mlineIndex,
                               const gchar *mid, gpointer self);
    static gboolean sigEndSession(GObject *signaller, const gchar *sessionId, gpointer self);
    // Signaller emits this with webrtcsrc's internal webrtcbin. Lets us
    // drive ICE directly on webrtcbin if the send-ice signal path is
    // unreliable from C (it lacks .run_last() unlike send-sdp).
    static void onWebrtcbinReady(GObject *signaller, const gchar *peerId,
                                 GstElement *webrtcbin, gpointer self);
    static void onWebrtcbinLocalIce(GstElement *webrtcbin, guint mlineIndex,
                                    gchar *candidate, gpointer self);
    // webrtcsrc's internal webrtcbin owns the real ICE connection state.
    // Without this, iceStateChanged is never emitted and CallManager never
    // leaves "Connecting" even though media is flowing.
    static void onWebrtcbinIceConnState(GstElement *webrtcbin,
                                        GParamSpec *pspec, gpointer self);
    static void onPadAdded(GstElement *src, GstPad *pad, gpointer self);
    static GstFlowReturn onVideoNewSample(GstAppSink *sink, gpointer self);

    QString m_remoteSessionId;
    QString m_sessionId = QStringLiteral("talq");  // our id within the signaller
    GstElement *m_pipeline = nullptr;
    GstElement *m_webrtcsrc = nullptr;
    GstElement *m_webrtcbin = nullptr;  // weak: webrtcsrc-owned, via webrtcbin-ready
    GObject *m_signaller = nullptr;
    GstElement *m_videoConvert = nullptr;
    GstElement *m_videoAppsink = nullptr;
    VideoFrameProvider *m_videoProvider = nullptr;
    QString m_videoCodec;
    QString m_audioOutputDeviceId;
    bool m_running = false;
    bool m_offerDelivered = false;     // session-started+session-description emitted
    bool m_webrtcStarted = false;      // webrtcsrc emitted "start" on the signaller
    int m_lastIceConnState = -1;       // de-dup webrtcbin ice-connection-state notify
    QString m_pendingOffer;            // Janus offer SDP, held until both ready
    QList<QPair<int, QString>> m_pendingRemoteIce;  // mline, candidate
    QMutex m_feedMutex;                // serialises feedOfferToSignaller (2 threads)
    guint m_busWatchId = 0;
};
