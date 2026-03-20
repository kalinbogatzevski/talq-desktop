#pragma once

#include <QObject>
#include <QTimer>
#include <QByteArray>
#include "core/ApiClient.h"
#include "core/SignalingClient.h"
#include "core/CallPipeline.h"
#include "core/CallSignaling.h"

class CallManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(CallState state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool isMuted READ isMuted NOTIFY muteChanged)
    Q_PROPERTY(bool isCameraOn READ isCameraOn NOTIFY cameraChanged)
    Q_PROPERTY(int callDuration READ callDuration NOTIFY durationChanged)
    Q_PROPERTY(QString remotePeerName READ remotePeerName NOTIFY callInfoChanged)
    Q_PROPERTY(QString remotePeerId READ remotePeerId NOTIFY callInfoChanged)

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
    QString remotePeerId() const { return m_remotePeerId; }

    Q_INVOKABLE void startCall(const QString &token, bool withVideo);
    Q_INVOKABLE void setRemotePeerInfo(const QString &name, const QString &peerId);
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
    void connectPipeline();

    ApiClient *m_api;
    SignalingClient *m_signaling;
    CallPipeline m_pipeline;
    CallSignaling m_callSignaling;  // internal signaling for WebRTC messages

    CallState m_state = Idle;
    QString m_callToken;
    QString m_callSid;
    QString m_ncSessionId;         // Nextcloud session for internal signaling
    QString m_remoteSessionId;
    QString m_remotePeerName;
    QString m_remotePeerId;
    bool m_muted = false;
    bool m_cameraOn = false;
    bool m_withVideo = false;
    bool m_isOfferer = false;      // true if our sessionId > remote (we create offer)
    int m_callDuration = 0;

    void startRingtone();
    void stopRingtone();

    QTimer m_durationTimer;
    QTimer m_ringTimeout;

    // Queued SDP/ICE
    QString m_pendingSdpType;
    QString m_pendingSdp;
    QString m_pendingSdpTarget;  // who to send the SDP to
    QList<QJsonObject> m_pendingCandidates;
};
