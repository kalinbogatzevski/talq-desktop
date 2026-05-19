#pragma once

#include <atomic>
#include <QObject>
#include <QString>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include "SignalingClient.h"

/**
 * Send-only screen capture pipeline for MCU screen sharing.
 * Captures the screen via d3d11screencapturesrc at full native resolution
 * and encodes with hardware H264 (preferred) so a shared screen stays
 * sharp at the high screen bitrate the HPB allows. Sends via RTP to the
 * MCU on a separate webrtcbin (roomType "screen"). No audio. No data
 * channels needed beyond the required "status" channel.
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
    // e.g. "H264 · nvh264enc · hw" — for the call codec/quality telemetry.
    QString encoderDescription() const { return m_encoderDesc; }
    // Screen-capture downscale cap applied before encode. Default 1080p:
    // a native 4K raw frame is ~38 MB and an unbounded native-res pool
    // ballooned RAM ~400 MB on share start. The quality switch sets a
    // higher cap then stop()->start(). Must be called before start().
    void setQualityCap(int maxW, int maxH) { m_capW = maxW; m_capH = maxH; }

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
    GstElement *m_videoEncoder = nullptr;  // kept for proper adaptive (todo)
    GstElement *m_videoParser = nullptr;   // h264parse, present iff H264
    bool m_useH264 = false;
    bool m_running = false;
    bool m_remoteDescSet = false;
    QList<QPair<int, QString>> m_pendingCandidates;
    // Screen content is mostly static, so a VBR encoder costs little when
    // nothing changes and spends bits (up to the server ~12 Mbps screen
    // cap) only on detail/motion — content-adaptive without TWCC.
    int m_initBitrate = 8000000;
    // Server screen ceiling (HPB signaling [mcu] maxscreenbitrate). GCC is
    // clamped here; it drives the VBR average up/down with the content.
    int m_maxBitrate = 12000000;
    int m_capW = 1920;   // screen-capture downscale cap (default 1080p,
    int m_capH = 1080;   // ~4.7x less per-frame RAM than native 4K)
    GstElement *m_gccbwe = nullptr;  // rtpgccbwe, owned by webrtcbin once returned
    // Set before pipeline→NULL in cleanup(); aux-sender/GCC callbacks
    // (streaming thread) bail on it to avoid a teardown-race UAF.
    std::atomic<bool> m_shuttingDown{false};
    // Last bitrate pushed to the encoder — GCC notifies far more often
    // than a hardware encoder can honor a live reconfigure; deadband it.
    int m_lastAppliedBitrate = 0;
    QString m_encoderDesc;   // human codec/encoder/hw-sw, for telemetry/pill

    static void onNegotiationNeeded(GstElement *webrtc, gpointer userData);
    static void onIceCandidate(GstElement *webrtc, guint mlineIndex, gchar *candidate, gpointer userData);
    static void onOfferCreated(GstPromise *promise, gpointer userData);
    static void onIceStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    static void onIceGatheringStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    // Send-side adaptive bitrate (rtpgccbwe). Screen needs this most:
    // mostly-static content lets GCC idle near-zero and burst on motion.
    static GstElement *onRequestAuxSender(GstElement *webrtc, GObject *transport,
                                          gpointer userData);
    static void onGccBitrate(GObject *gcc, GParamSpec *pspec, gpointer userData);
};
