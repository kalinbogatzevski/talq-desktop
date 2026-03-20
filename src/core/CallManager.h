#pragma once

#include <QObject>
#include <QTimer>
#include "core/ApiClient.h"
#include "core/SignalingClient.h"
#include "core/CallPipeline.h"

class CallManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(CallState state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool isMuted READ isMuted NOTIFY muteChanged)
    Q_PROPERTY(bool isCameraOn READ isCameraOn NOTIFY cameraChanged)
    Q_PROPERTY(int callDuration READ callDuration NOTIFY durationChanged)
    Q_PROPERTY(QString remotePeerName READ remotePeerName NOTIFY callInfoChanged)

public:
    enum CallState {
        Idle,
        Outgoing,
        Incoming,
        Connecting,
        Active,
        Ending
    };
    Q_ENUM(CallState)

    explicit CallManager(ApiClient *api, SignalingClient *signaling, QObject *parent = nullptr);

    CallState state() const { return m_state; }
    bool isMuted() const { return m_muted; }
    bool isCameraOn() const { return m_cameraOn; }
    int callDuration() const { return m_callDuration; }
    QString remotePeerName() const { return m_remotePeerName; }

    Q_INVOKABLE void startCall(const QString &token, bool withVideo);
    Q_INVOKABLE void acceptCall(bool withVideo);
    Q_INVOKABLE void declineCall();
    Q_INVOKABLE void hangUp();
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void toggleCamera();

signals:
    void stateChanged();
    void muteChanged();
    void cameraChanged();
    void durationChanged();
    void callInfoChanged();
    void incomingCall(const QString &callerName, const QString &token, bool withVideo);
    void callEnded(const QString &reason);

private slots:
    void onParticipantJoinedCall(const QString &sessionId, int flags, const QString &displayName);
    void onParticipantLeftCall(const QString &sessionId);
    void onOfferReceived(const QString &fromSessionId, const QString &sdp);
    void onAnswerReceived(const QString &fromSessionId, const QString &sdp);

private:
    void setState(CallState newState);
    void joinCallOnServer(bool withVideo);
    void leaveCallOnServer();
    void teardown(const QString &reason);

    ApiClient *m_api;
    SignalingClient *m_signaling;
    CallPipeline m_pipeline;

    CallState m_state = Idle;
    QString m_callToken;
    QString m_remoteSessionId;
    QString m_remotePeerName;
    bool m_muted = false;
    bool m_cameraOn = false;
    bool m_withVideo = false;
    int m_callDuration = 0;

    QTimer m_durationTimer;
    QTimer m_ringTimeout;
};
