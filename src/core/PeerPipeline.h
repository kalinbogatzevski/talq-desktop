#pragma once

#include <QObject>
#include <QPointer>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <gst/app/gstappsink.h>
#include "SignalingClient.h"
#include "VideoFrameProvider.h"

/**
 * Single-webrtcbin pipeline for P2P 1:1 WebRTC calls.
 * Sends local audio (and optionally camera video) while simultaneously
 * receiving remote audio/video via pad-added callbacks.
 *
 * Thread safety: all GStreamer callbacks marshal signals to the Qt main
 * thread via QMetaObject::invokeMethod(Qt::QueuedConnection).
 */
class PeerPipeline : public QObject
{
    Q_OBJECT
public:
    explicit PeerPipeline(QObject *parent = nullptr);
    ~PeerPipeline() override;

    bool start(const QString &stunServer, const QList<TurnServer> &turnServers = {},
               const QString &audioInputDeviceId = {},
               const QString &audioOutputDeviceId = {});
    void stop();
    bool isRunning() const { return m_running; }

    void createOffer();
    void setRemoteOffer(const QString &sdp);
    void setRemoteAnswer(const QString &sdp);
    void addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);

    void setMuted(bool muted);
    void enableCamera(int deviceIndex, bool hd1080 = true);
    void disableCamera();

    VideoFrameProvider *localVideoProvider() const { return m_localVideoProvider; }
    VideoFrameProvider *remoteVideoProvider() const { return m_remoteVideoProvider; }

signals:
    void localOfferReady(const QString &sdp);
    void localAnswerReady(const QString &sdp);
    void iceCandidateReady(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void iceStateChanged(const QString &state);
    void audioLevelUpdated(double level);
    void error(const QString &message);
    void cameraError(const QString &reason);

public slots:
    void pollBus();

private:
    void cleanup();
    void createAudioReceiveChain(GstPad *pad);
    void createVideoReceiveChain(GstPad *pad, const gchar *encoding);

    GstElement *m_pipeline = nullptr;
    GstElement *m_webrtcbin = nullptr;
    bool m_running = false;
    bool m_remoteDescSet = false;
    QList<QPair<int, QString>> m_pendingCandidates;
    QString m_audioOutputDeviceId;

    // Video send elements
    GstElement *m_cameraSrc = nullptr;
    GstElement *m_videoConvert = nullptr;
    GstElement *m_videoCapsFilter = nullptr;
    GstElement *m_videoEncoder = nullptr;
    GstElement *m_videoPayloader = nullptr;
    GstElement *m_jpegDec = nullptr;
    GstElement *m_tee = nullptr;
    GstElement *m_encQueue = nullptr;
    GstElement *m_previewQueue = nullptr;
    GstElement *m_previewConvert = nullptr;
    GstElement *m_previewAppsink = nullptr;
    GstPad *m_videoSinkPad = nullptr;
    bool m_cameraEnabled = false;
    bool m_audioChainCreated = false;
    bool m_videoChainCreated = false;
    int m_lvlDbg = 0;

    // Video providers
    VideoFrameProvider *m_localVideoProvider = nullptr;
    VideoFrameProvider *m_remoteVideoProvider = nullptr;

    // GStreamer callbacks
    static void onNegotiationNeeded(GstElement *webrtc, gpointer userData);
    static void onIceCandidate(GstElement *webrtc, guint mlineIndex, gchar *candidate, gpointer userData);
    static void onOfferCreated(GstPromise *promise, gpointer userData);
    static void onAnswerCreated(GstPromise *promise, gpointer userData);
    static void onPadAdded(GstElement *webrtc, GstPad *pad, gpointer userData);
    static void onIceStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    static GstFlowReturn onPreviewSample(GstAppSink *sink, gpointer userData);
    static GstFlowReturn onRemoteVideoSample(GstAppSink *sink, gpointer userData);
};
