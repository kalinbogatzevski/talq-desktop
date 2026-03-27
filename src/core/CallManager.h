#pragma once

#include <QObject>
#include <QTimer>
#include <QHash>
#include "core/ApiClient.h"
#include "core/SignalingClient.h"
#include "core/PublishPipeline.h"
#include "core/SubscribePipeline.h"
#include "core/PeerPipeline.h"
#include "core/MediaDeviceManager.h"
#include "core/VideoFrameProvider.h"

class CallManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(CallState state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool isMuted READ isMuted NOTIFY muteChanged)
    Q_PROPERTY(bool isCameraOn READ isCameraOn NOTIFY cameraChanged)
    Q_PROPERTY(int callDuration READ callDuration NOTIFY durationChanged)
    Q_PROPERTY(QString remotePeerName READ remotePeerName NOTIFY callInfoChanged)
    Q_PROPERTY(QString remotePeerId READ remotePeerId NOTIFY callInfoChanged)
    Q_PROPERTY(double audioLevel READ audioLevel NOTIFY audioLevelChanged)
    Q_PROPERTY(QString callStats READ callStats NOTIFY callStatsChanged)
    Q_PROPERTY(VideoFrameProvider* remoteVideoProvider READ remoteVideoProvider NOTIFY remoteVideoProviderChanged)
    Q_PROPERTY(VideoFrameProvider* localVideoProvider READ localVideoProvider NOTIFY localVideoProviderChanged)
    Q_PROPERTY(QString statusDetail READ statusDetail NOTIFY statusDetailChanged)
    Q_PROPERTY(bool callsAvailable READ callsAvailable CONSTANT)
    Q_PROPERTY(QString callsUnavailableReason READ callsUnavailableReason CONSTANT)

public:
    enum CallState { Idle, Outgoing, Incoming, Connecting, Active, Ending };
    Q_ENUM(CallState)

    explicit CallManager(ApiClient *api, SignalingClient *signaling, MediaDeviceManager *deviceMgr, QObject *parent = nullptr);

    CallState state() const { return m_state; }
    bool isMuted() const { return m_muted; }
    bool isCameraOn() const { return m_cameraOn; }
    int callDuration() const { return m_callDuration; }
    QString remotePeerName() const { return m_remotePeerName; }
    QString remotePeerId() const { return m_remotePeerId; }
    double audioLevel() const { return m_audioLevel; }
    QString callStats() const { return m_callStats; }
    VideoFrameProvider *remoteVideoProvider() const { return m_remoteVideoProvider; }
    VideoFrameProvider *localVideoProvider() const { return m_localVideoProvider; }
    QString statusDetail() const { return m_statusDetail; }
    bool callsAvailable() const { return m_callsAvailable; }
    QString callsUnavailableReason() const { return m_callsUnavailableReason; }

    Q_INVOKABLE void startCall(const QString &token, bool withVideo);
    Q_INVOKABLE void setRemotePeerInfo(const QString &name, const QString &peerId);
    Q_INVOKABLE void acceptCall(bool withVideo);
    Q_INVOKABLE void declineCall();
    Q_INVOKABLE void setUserActionReady();
    Q_INVOKABLE void hangUp();
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void toggleCamera();
    void onIncomingCallDetected(const QString &callerName, const QString &token, int callFlag);

signals:
    void stateChanged();
    void muteChanged();
    void cameraChanged();
    void durationChanged();
    void callInfoChanged();
    void incomingCall(const QString &callerName, const QString &token, bool withVideo);
    void callEnded(const QString &reason);
    void audioLevelChanged();
    void callStatsChanged();
    void remoteVideoProviderChanged();
    void localVideoProviderChanged();
    void statusDetailChanged();

private slots:
    void onParticipantJoinedCall(const QString &sessionId, int flags, const QString &displayName);
    void onParticipantLeftCall(const QString &sessionId);
    void onOfferReceived(const QString &fromSessionId, const QString &sdp, const QString &sid);
    void onAnswerReceived(const QString &fromSessionId, const QString &sdp);

private:
    void setState(CallState newState);
    void joinCallOnServer(bool withVideo);
    void leaveCallOnServer();
    void teardown(const QString &reason);
    void stopAllPipelines();
    void startRingtone();
    void stopRingtone();
    int videoDeviceIndex() const;
    bool preferHd1080() const;
    void broadcastMediaState(const QString &media, bool enabled);
    void forceReconnectPublisher();
    void updateCallFlags();

    ApiClient *m_api;
    SignalingClient *m_signaling;
    MediaDeviceManager *m_deviceManager = nullptr;
    // MCU dual pipelines
    PublishPipeline *m_publishPipeline = nullptr;
    QHash<QString, SubscribePipeline*> m_subscribePipelines;
    // P2P single pipeline
    PeerPipeline *m_peerPipeline = nullptr;
    bool m_useP2P = false;
    QTimer m_glibTimer;  // shared GLib main context pump

    CallState m_state = Idle;
    QString m_callToken;
    QString m_stunServer;
    QList<TurnServer> m_turnServers;
    QString m_remoteSessionId;
    QString m_remotePeerName;
    QString m_remotePeerId;
    bool m_muted = false;
    bool m_cameraOn = false;
    bool m_cameraFallbackTried = false;
    bool m_withVideo = false;
    int m_callDuration = 0;
    double m_audioLevel = 0.0;
    QString m_callStats;
    QString m_statusDetail;
    bool m_callsAvailable = true;
    QString m_callsUnavailableReason;
    void checkGStreamerPlugins();
    void updateCallStats();
    void setStatusDetail(const QString &detail) {
        if (m_statusDetail != detail) { m_statusDetail = detail; emit statusDetailChanged(); }
    }

    QTimer m_durationTimer;
    QTimer m_ringTimeout;
    QTimer m_statsTimer;

    bool m_joinedCall = false;
    bool m_userActionReady = false;

    QString m_lastDeclinedToken;
    QDateTime m_lastDeclinedTime;
    QDateTime m_incomingTime;  // when incoming call was detected

    VideoFrameProvider *m_remoteVideoProvider = nullptr;
    VideoFrameProvider *m_localVideoProvider = nullptr;
};
