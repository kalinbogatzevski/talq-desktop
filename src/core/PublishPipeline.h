#pragma once

#include <QObject>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include "SignalingClient.h"
#include "VideoFrameProvider.h"

/**
 * Send-only GStreamer webrtcbin pipeline for MCU publishing.
 * Captures local audio, encodes as Opus, sends via RTP to the MCU.
 * Creates an offer; MCU answers back.
 *
 * Video architecture: dummy and camera branches share the same webrtcbin
 * sink pad (m_videoSinkPad). Camera toggle unlinks one SSRC filter and
 * links the other — direct pad swap, no input-selector, no renegotiation.
 */
class PublishPipeline : public QObject
{
    Q_OBJECT

public:
    explicit PublishPipeline(QObject *parent = nullptr);
    ~PublishPipeline() override;

    bool start(const QString &stunServer, const QList<TurnServer> &turnServers = {},
               const QString &audioDeviceId = {}, bool withVideo = false,
               int videoDeviceIndex = 0, bool hd1080 = true);
    void stop();
    void setRemoteAnswer(const QString &sdp);
    void addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void setMuted(bool muted);
    bool isRunning() const { return m_running; }

    void enableCamera(int deviceIndex, bool hd1080 = true);
    void disableCamera();

    VideoFrameProvider *localVideoProvider() const { return m_localVideoProvider; }

signals:
    void localOfferReady(const QString &sdp);
    void iceCandidateReady(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void iceStateChanged(const QString &state);
    void audioLevelUpdated(double level);  // 0.0 to 1.0
    void error(const QString &message);
    void cameraError(const QString &reason);

public slots:
    void pollBus();  // called from CallManager's GLib timer

private:
    void cleanup();
    bool buildCameraChain(int deviceIndex, bool hd1080);

    GstElement *m_pipeline = nullptr;
    GstElement *m_webrtcbin = nullptr;
    bool m_running = false;
    bool m_remoteDescSet = false;
    QList<QPair<int, QString>> m_pendingCandidates;
    guint m_busWatchId = 0;

    // Video: direct pad swap between dummy and camera SSRC filters
    GstPad *m_videoSinkPad = nullptr;
    guint32 m_videoSsrc = 0;
    bool m_cameraEnabled = false;
    int m_lvlDbg = 0;

    // Dummy branch elements (16x16 black, 1fps VP8)
    GstElement *m_dummySrc = nullptr;
    GstElement *m_dummyConv = nullptr;
    GstElement *m_dummyEnc = nullptr;
    GstElement *m_dummyPay = nullptr;
    GstElement *m_dummySsrcFilter = nullptr;

    // Camera branch elements (built once in buildCameraChain(), stay alive)
    GstElement *m_cameraSrc = nullptr;
    GstElement *m_videoConvert = nullptr;
    GstElement *m_videoCapsFilter = nullptr;
    GstElement *m_videoEncoder = nullptr;
    GstElement *m_jpegDec = nullptr;
    GstElement *m_videoPayloader = nullptr;
    GstElement *m_camSsrcFilter = nullptr;
    GstElement *m_cameraValve = nullptr;  // drops frames when camera is off (saves CPU)

    // Local preview (tee branch off camera)
    GstElement *m_tee = nullptr;
    GstElement *m_encQueue = nullptr;
    GstElement *m_previewQueue = nullptr;
    GstElement *m_previewConvert = nullptr;
    GstElement *m_previewAppsink = nullptr;
    VideoFrameProvider *m_localVideoProvider = nullptr;

    static void onNegotiationNeeded(GstElement *webrtc, gpointer userData);
    static void onIceCandidate(GstElement *webrtc, guint mlineIndex, gchar *candidate, gpointer userData);
    static void onOfferCreated(GstPromise *promise, gpointer userData);
    static void onIceStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    static GstFlowReturn onPreviewSample(GstAppSink *sink, gpointer userData);
};
