#pragma once

#include <QObject>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include "SignalingClient.h"

/**
 * Send-only screen capture pipeline for MCU screen sharing.
 * Captures the primary monitor via d3d11screencapturesrc, encodes as VP8,
 * sends via RTP to the MCU on a separate webrtcbin (roomType "screen").
 * No audio. No data channels needed beyond the required "status" channel.
 */
class ScreenSharePipeline : public QObject
{
    Q_OBJECT

public:
    explicit ScreenSharePipeline(QObject *parent = nullptr);
    ~ScreenSharePipeline() override;

    bool start(const QString &stunServer, const QList<TurnServer> &turnServers = {},
                int monitorIndex = 0, quintptr windowHandle = 0);
    void stop();
    void setRemoteAnswer(const QString &sdp);
    void addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    bool isRunning() const { return m_running; }

signals:
    void localOfferReady(const QString &sdp);
    void iceCandidateReady(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void iceStateChanged(const QString &state);
    void iceGatheringComplete();           // ICE gathering done → endOfCandidates
    void error(const QString &message);

public slots:
    void pollBus();

private:
    void cleanup();

    GstElement *m_pipeline = nullptr;
    GstElement *m_webrtcbin = nullptr;
    bool m_running = false;
    bool m_remoteDescSet = false;
    QList<QPair<int, QString>> m_pendingCandidates;

    static void onNegotiationNeeded(GstElement *webrtc, gpointer userData);
    static void onIceCandidate(GstElement *webrtc, guint mlineIndex, gchar *candidate, gpointer userData);
    static void onOfferCreated(GstPromise *promise, gpointer userData);
    static void onIceStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    static void onIceGatheringStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
};
