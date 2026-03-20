#include "core/CallManager.h"
#include <QJsonObject>
#include <QDebug>

CallManager::CallManager(ApiClient *api, SignalingClient *signaling, QObject *parent)
    : QObject(parent)
    , m_api(api)
    , m_signaling(signaling)
{
    // Call state signals from signaling
    connect(m_signaling, &SignalingClient::participantJoinedCall,
            this, &CallManager::onParticipantJoinedCall);
    connect(m_signaling, &SignalingClient::participantLeftCall,
            this, &CallManager::onParticipantLeftCall);
    connect(m_signaling, &SignalingClient::offerReceived,
            this, &CallManager::onOfferReceived);
    connect(m_signaling, &SignalingClient::answerReceived,
            this, &CallManager::onAnswerReceived);
    connect(m_signaling, &SignalingClient::candidateReceived,
            this, [this](const QString &fromSessionId, const QJsonObject &candidate) {
        if (fromSessionId == m_remoteSessionId || m_remoteSessionId.isEmpty()) {
            m_pipeline.addIceCandidate(
                candidate["candidate"].toString(),
                candidate["sdpMLineIndex"].toInt(),
                candidate["sdpMid"].toString());
        }
    });

    // Pipeline signals → signaling
    connect(&m_pipeline, &CallPipeline::localSdpReady,
            this, [this](const QString &type, const QString &sdp) {
        if (m_remoteSessionId.isEmpty()) return;
        if (type == "offer")
            m_signaling->sendOffer(m_remoteSessionId, sdp);
        else
            m_signaling->sendAnswer(m_remoteSessionId, sdp);
    });
    connect(&m_pipeline, &CallPipeline::iceCandidateReady,
            this, [this](const QString &candidate, int mlineIndex, const QString &sdpMid) {
        if (m_remoteSessionId.isEmpty()) return;
        QJsonObject candidateObj;
        candidateObj["candidate"] = candidate;
        candidateObj["sdpMLineIndex"] = mlineIndex;
        candidateObj["sdpMid"] = sdpMid;
        m_signaling->sendCandidate(m_remoteSessionId, candidateObj);
    });

    // Duration timer — ticks every second during Active state
    m_durationTimer.setInterval(1000);
    connect(&m_durationTimer, &QTimer::timeout, this, [this]() {
        m_callDuration++;
        emit durationChanged();
    });

    // Ring timeout — auto-decline/cancel after 30 seconds
    m_ringTimeout.setSingleShot(true);
    m_ringTimeout.setInterval(30000);
    connect(&m_ringTimeout, &QTimer::timeout, this, [this]() {
        if (m_state == Incoming) {
            qDebug() << "CallManager: incoming call timed out";
            declineCall();
        } else if (m_state == Outgoing) {
            qDebug() << "CallManager: outgoing call timed out (no answer)";
            teardown("No answer");
        }
    });
}

void CallManager::setState(CallState newState)
{
    if (m_state == newState) return;
    m_state = newState;
    qDebug() << "CallManager: state ->" << newState;
    emit stateChanged();
}

void CallManager::startCall(const QString &token, bool withVideo)
{
    if (m_state != Idle) {
        qWarning() << "CallManager: can't start call, state =" << m_state;
        return;
    }

    m_callToken = token;
    m_withVideo = withVideo;
    m_cameraOn = withVideo;
    m_muted = false;
    m_callDuration = 0;

    setState(Outgoing);
    joinCallOnServer(withVideo);
    m_ringTimeout.start();
}

void CallManager::acceptCall(bool withVideo)
{
    if (m_state != Incoming) return;

    m_withVideo = withVideo;
    m_cameraOn = withVideo;
    m_muted = false;
    m_callDuration = 0;

    m_ringTimeout.stop();
    setState(Connecting);
    joinCallOnServer(withVideo);
}

void CallManager::declineCall()
{
    if (m_state != Incoming) return;
    m_ringTimeout.stop();
    setState(Idle);
    emit callEnded("Declined");
}

void CallManager::hangUp()
{
    if (m_state == Idle) return;
    teardown("Hung up");
}

void CallManager::toggleMute()
{
    m_muted = !m_muted;
    m_pipeline.setMuted(m_muted);
    emit muteChanged();
}

void CallManager::toggleCamera()
{
    m_cameraOn = !m_cameraOn;
    emit cameraChanged();
    // TODO: enable/disable video track in pipeline
}

void CallManager::joinCallOnServer(bool withVideo)
{
    // flags: 1 = in-call, 2 = audio, 4 = video
    int flags = 1 | 2;
    if (withVideo) flags |= 4;

    QJsonObject body;
    body["flags"] = flags;

    m_api->post("apps/spreed/api/v4/call/" + m_callToken, body,
        [this](bool ok, const QJsonObject &data, int statusCode) {
            if (!ok) {
                qWarning() << "CallManager: failed to join call, status=" << statusCode;
                teardown("Failed to join call");
                return;
            }
            qDebug() << "CallManager: joined call on server, starting pipeline";
            // TODO: get STUN/TURN from signaling settings
            m_pipeline.startCall(QString(), QString());
        });
}

void CallManager::leaveCallOnServer()
{
    if (m_callToken.isEmpty()) return;

    QJsonObject body;
    m_api->del("apps/spreed/api/v4/call/" + m_callToken,
        [](bool, const QJsonObject &, int) {});
}

void CallManager::teardown(const QString &reason)
{
    m_ringTimeout.stop();
    m_durationTimer.stop();
    leaveCallOnServer();
    m_pipeline.stop();

    m_callToken.clear();
    m_remoteSessionId.clear();
    m_remotePeerName.clear();
    m_callDuration = 0;

    setState(Idle);
    emit callEnded(reason);
}

void CallManager::onParticipantJoinedCall(const QString &sessionId, int flags, const QString &displayName)
{
    if (m_state == Outgoing && m_remoteSessionId.isEmpty()) {
        // Remote peer answered our call (first-writer guard)
        m_remoteSessionId = sessionId;
        m_remotePeerName = displayName;
        qDebug() << "CallManager: remote peer joined, session=" << sessionId.left(20) << "name=" << displayName;
        m_ringTimeout.stop();
        setState(Connecting);
        emit callInfoChanged();
    }
    else if (m_state == Idle) {
        // Incoming call — remote peer started a call in our current room
        m_remoteSessionId = sessionId;
        m_remotePeerName = displayName;
        m_callToken = m_signaling->currentRoom();
        m_withVideo = (flags & 4) != 0;

        setState(Incoming);
        m_ringTimeout.start();
        emit callInfoChanged();
        emit incomingCall(m_remotePeerName, m_callToken, m_withVideo);
    }
}

void CallManager::onParticipantLeftCall(const QString &sessionId)
{
    if (sessionId != m_remoteSessionId) return;

    // 1:1 specific: remote left → call is over
    qDebug() << "CallManager: remote peer left call";
    teardown("Call ended");
}

void CallManager::onOfferReceived(const QString &fromSessionId, const QString &sdp)
{
    if (m_state != Connecting && m_state != Active) return;
    m_remoteSessionId = fromSessionId;
    qDebug() << "CallManager: received SDP offer, setting remote description";
    m_pipeline.setRemoteDescription("offer", sdp);
}

void CallManager::onAnswerReceived(const QString &fromSessionId, const QString &sdp)
{
    if (m_state != Connecting) return;
    if (fromSessionId != m_remoteSessionId) return;
    qDebug() << "CallManager: received SDP answer, setting remote description";
    m_pipeline.setRemoteDescription("answer", sdp);
    setState(Active);
    m_durationTimer.start();
}
