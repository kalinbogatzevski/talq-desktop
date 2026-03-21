#pragma once

#include <QObject>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

/**
 * Receive-only GStreamer webrtcbin pipeline for MCU subscription.
 * Receives audio from the MCU for one remote participant.
 * MCU sends an offer; we create an answer.
 */
class SubscribePipeline : public QObject
{
    Q_OBJECT

public:
    explicit SubscribePipeline(const QString &remoteSessionId, QObject *parent = nullptr);
    ~SubscribePipeline() override;

    bool start(const QString &stunServer);
    void stop();
    void setRemoteOffer(const QString &sdp);
    void addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    QString remoteSessionId() const { return m_remoteSessionId; }
    bool isRunning() const { return m_running; }

signals:
    void localAnswerReady(const QString &sdp);
    void iceCandidateReady(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void iceStateChanged(const QString &state);
    void error(const QString &message);

private:
    void cleanup();

    GstElement *m_pipeline = nullptr;
    GstElement *m_webrtcbin = nullptr;
    QString m_remoteSessionId;
    bool m_running = false;

    static void onIceCandidate(GstElement *webrtc, guint mlineIndex, gchar *candidate, gpointer userData);
    static void onPadAdded(GstElement *webrtc, GstPad *pad, gpointer userData);
    static void onAnswerCreated(GstPromise *promise, gpointer userData);
    static void onIceStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
};
