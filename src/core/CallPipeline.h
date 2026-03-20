#pragma once

#include <QObject>
#include <QJsonObject>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

class CallPipeline : public QObject
{
    Q_OBJECT

public:
    explicit CallPipeline(QObject *parent = nullptr);
    ~CallPipeline() override;

    bool startCall(const QString &stunServer, const QString &turnServer);
    void stop();
    void setRemoteDescription(const QString &type, const QString &sdp);
    void addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void setMuted(bool muted);

    bool isRunning() const { return m_running; }

signals:
    void localSdpReady(const QString &type, const QString &sdp);
    void iceCandidateReady(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void iceConnectionStateChanged(const QString &state);
    void error(const QString &message);

private:
    GstElement *m_pipeline = nullptr;
    GstElement *m_webrtcbin = nullptr;
    bool m_running = false;

    void cleanup();

    // GStreamer callbacks (static, forward to instance via user_data)
    static void onNegotiationNeeded(GstElement *webrtc, gpointer userData);
    static void onIceCandidate(GstElement *webrtc, guint mlineIndex, gchar *candidate, gpointer userData);
    static void onPadAdded(GstElement *webrtc, GstPad *pad, gpointer userData);
    static void onOfferCreated(GstPromise *promise, gpointer userData);
    static void onAnswerCreated(GstPromise *promise, gpointer userData);
};
