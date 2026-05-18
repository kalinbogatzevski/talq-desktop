#pragma once

#include <atomic>
#include <QObject>
#include <QString>
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
 * Video architecture (funnel + valve flips):
 * Both dummy (16x16 black) and camera sources are PERMANENTLY linked to a
 * funnel element via valves. Only one valve is open at a time. Switching
 * between dummy and camera is a pair of g_object_set("drop") calls — no
 * unlinking, no relinking, no pad swaps. The encoder chain downstream of
 * the funnel (vp8enc → rtpvp8pay → ssrcFilter → webrtcbin) is built once
 * and never torn down. RTP sequence number continuity is maintained across
 * all switches.
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
    bool isCameraOn() const { return m_cameraEnabled; }
    bool usesH264() const { return m_useH264; }
    // e.g. "H264 · nvh264enc · hw" — for the call codec/quality telemetry.
    QString encoderDescription() const { return m_encoderDesc; }
    // Live send bitrate (bits/s): the value GCC last pushed to the
    // encoder, or the conservative start until the estimate kicks in.
    int currentVideoBitrate() const {
        return m_lastAppliedBitrate > 0 ? m_lastAppliedBitrate : m_initBitrate;
    }

    void enableCamera(int deviceIndex, bool hd1080 = true);
    void disableCamera();

    VideoFrameProvider *localVideoProvider() const { return m_localVideoProvider; }

    void sendStatusMessage(const QByteArray &json);

signals:
    void localOfferReady(const QString &sdp);
    void iceCandidateReady(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void iceStateChanged(const QString &state);
    void iceGatheringComplete();           // ICE gathering done → endOfCandidates
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

    // Shared encoder chain (built once, never torn down):
    // funnel → vp8enc → rtpvp8pay → videoSsrcFilter → webrtcbin
    GstElement *m_funnel = nullptr;
    GstElement *m_sharedScale = nullptr;  // funnel→...→sharedScale→sharedCaps→enc
    GstElement *m_sharedCaps = nullptr;   // pins a CONSTANT enc resolution so the
                                          // encoder never reconfigures on the
                                          // 16x16-dummy↔camera source switch
    GstElement *m_videoEncoder = nullptr;
    GstElement *m_videoPayloader = nullptr;
    GstElement *m_videoSsrcFilter = nullptr;
    GstPad *m_videoSinkPad = nullptr;     // webrtcbin's video sink pad
    guint32 m_videoSsrc = 0;
    // Start bitrate (bits/s) handed to the encoder; rtpgccbwe drives the
    // live rate up to the server ceiling once TWCC feedback flows.
    int m_initBitrate = 2500000;
    // Server video ceiling (HPB signaling [mcu] maxstreambitrate). GCC is
    // clamped here so it never probes past what Janus will forward.
    int m_maxBitrate = 4000000;
    GstElement *m_gccbwe = nullptr;  // rtpgccbwe, owned by webrtcbin once returned
    // Set before the pipeline goes to NULL in cleanup(). webrtcbin can fire
    // request-aux-sender / notify::estimated-bitrate on a streaming thread
    // concurrently with Qt-thread teardown; these callbacks bail on it.
    std::atomic<bool> m_shuttingDown{false};
    // Last bitrate actually pushed to the encoder. GCC notifies every
    // ~200 ms with tiny deltas; hardware encoders (qsvh264enc/oneVPL)
    // reject most live reconfigures (MFX_ERR_INCOMPATIBLE_VIDEO_PARAM) and
    // a per-tick g_object_set storms that path. Only re-apply on a
    // meaningful change.
    int m_lastAppliedBitrate = 0;
    GstElement *m_videoParser = nullptr;  // h264parse (present iff m_useH264)
    bool m_cameraEnabled = false;
    bool m_useH264 = false;
    QString m_encoderDesc;   // human codec/encoder/hw-sw, for telemetry/pill
    int m_lvlDbg = 0;
    GstWebRTCDataChannel *m_statusDataChannel = nullptr;

    // Dummy source (16x16 black, 1fps — feeds funnel when camera is off)
    GstElement *m_dummySrc = nullptr;
    GstElement *m_dummyCaps = nullptr;
    GstElement *m_dummyConv = nullptr;
    GstElement *m_dummyValve = nullptr;

    // Camera branch elements (built lazily in buildCameraChain(), stay alive)
    GstElement *m_cameraSrc = nullptr;
    GstElement *m_videoConvert = nullptr;
    GstElement *m_videoCapsFilter = nullptr;
    GstElement *m_tee = nullptr;
    GstElement *m_encQueue = nullptr;
    GstElement *m_cameraValve = nullptr;

    // Local preview (tee branch off camera)
    GstElement *m_previewQueue = nullptr;
    GstElement *m_previewConvert = nullptr;
    GstElement *m_previewAppsink = nullptr;
    VideoFrameProvider *m_localVideoProvider = nullptr;

    static void onNegotiationNeeded(GstElement *webrtc, gpointer userData);
    static void onIceCandidate(GstElement *webrtc, guint mlineIndex, gchar *candidate, gpointer userData);
    static void onOfferCreated(GstPromise *promise, gpointer userData);
    static void onIceStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    static void onIceGatheringStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    static GstFlowReturn onPreviewSample(GstAppSink *sink, gpointer userData);
    // Send-side adaptive bitrate: webrtcbin asks for an aux sender; we hand
    // back rtpgccbwe and follow its estimate live onto the encoder.
    static GstElement *onRequestAuxSender(GstElement *webrtc, GObject *transport,
                                          gpointer userData);
    static void onGccBitrate(GObject *gcc, GParamSpec *pspec, gpointer userData);
};
