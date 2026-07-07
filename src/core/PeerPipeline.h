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
    // 0.41.9-beta — video transceiver direction. Default SENDONLY (the
    // harness MCU-publisher path: Janus receives our video on a publisher
    // handle). For TRUE P2P set SENDRECV BEFORE enableCamera so the single
    // video m-line carries both directions — otherwise both peers offer
    // sendonly and neither receives the other's camera. CallManager's P2P
    // branch + the TALQ_TEST_P2P harness set this true.
    void setVideoSendRecv(bool v) { m_videoSendRecv = v; }
    // forceTestSource: use videotestsrc regardless of env (test harness
    // only — lets one in-process peer use the real camera while another
    // uses synthetic, since two mfvideosrc consumers of one device race).
    // triggerOffer: whether to create a (re)negotiation offer once the
    // video chain is attached. Default true = mid-call "turn camera on"
    // renegotiation. Pass false for the answerer (it will answer the
    // peer's offer instead) and for the start-time auto-enable (the
    // single offer is driven by participant-joined), avoiding offer
    // glare in true P2P.
    void enableCamera(int deviceIndex, bool hd1080 = true,
                      bool forceTestSource = false, bool triggerOffer = true);
    void disableCamera();

    VideoFrameProvider *localVideoProvider() const { return m_localVideoProvider; }
    VideoFrameProvider *remoteVideoProvider() const { return m_remoteVideoProvider; }

signals:
    void localOfferReady(const QString &sdp);
    void localAnswerReady(const QString &sdp);
    void iceCandidateReady(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void iceStateChanged(const QString &state);
    void iceGatheringComplete();           // ICE gathering done → endOfCandidates
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
    bool m_muted = false;   // sender mute state; applied to peer-volume on (re)build
    bool m_remoteDescSet = false;
    QList<QPair<int, QString>> m_pendingCandidates;
    QString m_audioOutputDeviceId;

    // Video send elements
    GstElement *m_cameraSrc = nullptr;
    // 0.41.9-beta — robust camera capture, ported from PublishPipeline:
    // mfvideosrc → camSrcCaps (permissive menu / user setting) →
    // decodebin →(dynamic)→ videoConvert → videoCapsFilter(I420) → tee.
    // Replaces the previous hard-pinned image/jpeg caps + fixed jpegdec
    // which failed `not-negotiated (-4)` on cameras that don't advertise
    // that exact MJPEG mode.
    GstElement *m_camSrcCaps = nullptr;     // capsfilter on mfvideosrc output
    GstElement *m_camDecode = nullptr;      // decodebin (MJPEG or raw)
    GstElement *m_videoConvert = nullptr;
    GstElement *m_videoCapsFilter = nullptr;
    GstElement *m_videoEncoder = nullptr;
    GstElement *m_videoParser = nullptr;    // h264parse when the factory picks H264
    GstElement *m_videoProfileCaps = nullptr; // #69 capsfilter forcing H264 High profile (P2P only)
    GstElement *m_videoPayloader = nullptr;
    GstElement *m_videoSsrcFilter = nullptr;
    guint32 m_videoSsrc = 0;
    GstElement *m_audioSsrcFilter = nullptr;
    guint32 m_audioSsrc = 0;
    GstElement *m_jpegDec = nullptr;        // legacy, unused since 0.41.9
    GstElement *m_tee = nullptr;
    GstElement *m_encQueue = nullptr;
    GstElement *m_previewQueue = nullptr;
    GstElement *m_previewConvert = nullptr;
    GstElement *m_previewAppsink = nullptr;
    GstPad *m_videoSinkPad = nullptr;
    bool m_cameraEnabled = false;
    bool m_videoSendRecv = false;   // 0.41.9 — SENDRECV for P2P, SENDONLY for MCU
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
    static void onIceGatheringStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    static GstFlowReturn onPreviewSample(GstAppSink *sink, gpointer userData);
    static GstFlowReturn onRemoteVideoSample(GstAppSink *sink, gpointer userData);
};
