#pragma once

#include <QObject>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include "SignalingClient.h"
#include "VideoFrameProvider.h"

/**
 * Receive-only GStreamer webrtcbin pipeline for MCU subscription.
 * Receives audio from the MCU for one remote participant.
 * MCU sends an offer; we create an answer.
 *
 * Thread safety: all GStreamer callbacks marshal to Qt main thread.
 */
class SubscribePipeline : public QObject
{
    Q_OBJECT

public:
    explicit SubscribePipeline(const QString &remoteSessionId, QObject *parent = nullptr);
    ~SubscribePipeline() override;

    bool start(const QString &stunServer, const QList<TurnServer> &turnServers = {},
               const QString &audioOutputDeviceId = {});
    void stop();
    void setRemoteOffer(const QString &sdp);
    void addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    QString remoteSessionId() const { return m_remoteSessionId; }
    bool isRunning() const { return m_running; }
    VideoFrameProvider *videoProvider() const { return m_videoProvider; }
    QString videoCodec() const { return m_videoCodec; }
    QString videoDecoder() const { return m_videoDecoder; }
    void requestKeyframe();   // 0.52.15 — on-demand RTCP PLI (screen-sub startup-grace re-PLI)

signals:
    void localAnswerReady(const QString &sdp);
    void iceCandidateReady(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void iceStateChanged(const QString &state);
    void iceGatheringComplete();           // ICE gathering done → endOfCandidates
    void error(const QString &message);
    void mediaStateReceived(const QString &type);
    // Emitted when peer sends a `{"type":"talq.client",...}` data channel
    // message. Used to display "TalQ/X.Y.Z" in the call UI.
    void peerClientInfo(const QString &client, const QString &version);

private:
    void cleanup();

    GstElement *m_pipeline = nullptr;
    GstElement *m_webrtcbin = nullptr;
    QString m_remoteSessionId;
    QString m_audioOutputDeviceId;
    bool m_running = false;
    bool m_remoteDescSet = false;
    QList<QPair<int, QString>> m_pendingCandidates;
    guint m_busWatchId = 0;

    VideoFrameProvider *m_videoProvider = nullptr;
    GstElement *m_videoAppsink = nullptr;
    GstElement *m_videoConvert = nullptr;  // decodebin pad-added links here
    GstElement *m_videoDecode = nullptr;   // decodebin; PLI on its sink pad
    QTimer *m_pliTimer = nullptr;
    bool m_audioChainCreated = false;
    QString m_videoCodec;    // "VP8", "H264"
    QString m_videoDecoder;  // "NVDEC", "DXVA", "Software"

    void createAudioChain(GstPad *pad);
    void createVideoChain(GstPad *pad, const gchar *encoding);

    static void onIceCandidate(GstElement *webrtc, guint mlineIndex, gchar *candidate, gpointer userData);
    static void onPadAdded(GstElement *webrtc, GstPad *pad, gpointer userData);
    static void onDecodebinPad(GstElement *decodebin, GstPad *pad, gpointer userData);
    static void onAnswerCreated(GstPromise *promise, gpointer userData);
    static void onIceStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    static void onIceGatheringStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    static GstFlowReturn onNewVideoSample(GstAppSink *sink, gpointer userData);
    static void onDataChannel(GstElement *webrtc, GstWebRTCDataChannel *channel, gpointer userData);
    static void onDataChannelMessage(GstWebRTCDataChannel *channel, gchar *str, gpointer userData);
};
