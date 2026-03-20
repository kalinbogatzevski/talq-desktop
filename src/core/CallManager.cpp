#include "core/CallManager.h"
#include <QJsonObject>
#include <QDebug>
#include <QUuid>
#include <QtMath>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

// --- Ringtone ---

static QByteArray generateRingtoneWav()
{
    const int sampleRate = 22050;
    const int totalSamples = sampleRate * 3;
    const int tone1Start = 0, tone1End = sampleRate * 3 / 10;
    const int tone2Start = sampleRate * 4 / 10, tone2End = sampleRate * 7 / 10;
    QByteArray pcm(totalSamples * 2, 0);
    auto *samples = reinterpret_cast<qint16*>(pcm.data());
    for (int i = 0; i < totalSamples; i++) {
        bool inTone = (i >= tone1Start && i < tone1End) || (i >= tone2Start && i < tone2End);
        samples[i] = inTone ? static_cast<qint16>(16000 * qSin(2.0 * M_PI * 440.0 * i / sampleRate)) : 0;
    }
    QByteArray wav;
    int dataSize = pcm.size(), fileSize = 36 + dataSize;
    wav.append("RIFF", 4); wav.append(reinterpret_cast<const char*>(&fileSize), 4);
    wav.append("WAVE", 4); wav.append("fmt ", 4);
    qint32 fmtSize = 16; wav.append(reinterpret_cast<const char*>(&fmtSize), 4);
    qint16 audioFmt = 1; wav.append(reinterpret_cast<const char*>(&audioFmt), 2);
    qint16 channels = 1; wav.append(reinterpret_cast<const char*>(&channels), 2);
    qint32 sr = sampleRate; wav.append(reinterpret_cast<const char*>(&sr), 4);
    qint32 byteRate = sampleRate * 2; wav.append(reinterpret_cast<const char*>(&byteRate), 4);
    qint16 blockAlign = 2; wav.append(reinterpret_cast<const char*>(&blockAlign), 2);
    qint16 bps = 16; wav.append(reinterpret_cast<const char*>(&bps), 2);
    wav.append("data", 4); wav.append(reinterpret_cast<const char*>(&dataSize), 4);
    wav.append(pcm);
    return wav;
}

void CallManager::startRingtone() {
#ifdef Q_OS_WIN
    static QByteArray wavData = generateRingtoneWav();
    PlaySoundA(wavData.constData(), nullptr, SND_MEMORY | SND_ASYNC | SND_LOOP);
#endif
}
void CallManager::stopRingtone() {
#ifdef Q_OS_WIN
    PlaySoundA(nullptr, nullptr, 0);
#endif
}

// --- CallManager ---

CallManager::CallManager(ApiClient *api, SignalingClient *signaling, QObject *parent)
    : QObject(parent)
    , m_api(api)
    , m_signaling(signaling)
    , m_callSignaling(api, this)
{
    // HPB participant events
    connect(m_signaling, &SignalingClient::participantJoinedCall,
            this, &CallManager::onParticipantJoinedCall);
    connect(m_signaling, &SignalingClient::participantLeftCall,
            this, &CallManager::onParticipantLeftCall);

    // HPB WebSocket signaling for WebRTC messages
    connect(m_signaling, &SignalingClient::offerReceived,
            this, &CallManager::onOfferReceived);
    connect(m_signaling, &SignalingClient::answerReceived,
            this, [this](const QString &from, const QString &sdp) {
        onAnswerReceived(from, sdp);
    });
    connect(m_signaling, &SignalingClient::candidateReceived,
            this, [this](const QString &fromSessionId, const QJsonObject &candidate) {
        // Candidates may be nested: payload.candidate.{candidate, sdpMLineIndex, sdpMid}
        QJsonObject c = candidate.contains("candidate") && candidate["candidate"].isObject()
            ? candidate["candidate"].toObject() : candidate;
        m_pipeline.addIceCandidate(
            c["candidate"].toString(),
            c["sdpMLineIndex"].toInt(),
            c["sdpMid"].toString());
    });

    connectPipeline();

    m_durationTimer.setInterval(1000);
    connect(&m_durationTimer, &QTimer::timeout, this, [this]() {
        m_callDuration++;
        emit durationChanged();
    });

    m_ringTimeout.setSingleShot(true);
    m_ringTimeout.setInterval(30000);
    connect(&m_ringTimeout, &QTimer::timeout, this, [this]() {
        if (m_state == Incoming) declineCall();
        else if (m_state == Outgoing) teardown("No answer");
    });
}

void CallManager::connectPipeline()
{
    // Pipeline SDP → send via HPB WebSocket
    connect(&m_pipeline, &CallPipeline::localSdpReady,
            this, [this](const QString &type, const QString &sdp) {
        // For MCU ownPeer: send to our own session ID
        // For P2P or MCU subscriber answer: send to remote session
        QString target = m_pendingSdpTarget.isEmpty() ? m_remoteSessionId : m_pendingSdpTarget;
        m_pendingSdpTarget.clear();

        if (target.isEmpty()) {
            m_pendingSdpType = type;
            m_pendingSdp = sdp;
            qDebug() << "CallManager: queued" << type << "SDP (no target yet)";
            return;
        }
        if (type == "offer")
            m_signaling->sendOffer(target, sdp);
        else
            m_signaling->sendAnswer(target, sdp);
    });

    connect(&m_pipeline, &CallPipeline::iceCandidateReady,
            this, [this](const QString &candidate, int mlineIndex, const QString &sdpMid) {
        QJsonObject candidateObj;
        candidateObj["candidate"] = candidate;
        candidateObj["sdpMLineIndex"] = mlineIndex;
        candidateObj["sdpMid"] = sdpMid.isEmpty() ? QString("0") : sdpMid;

        // For MCU: candidates go to our own session (the MCU intercepts)
        QString target = m_signaling->hasMcu() ? m_signaling->sessionId() : m_remoteSessionId;
        if (target.isEmpty()) {
            m_pendingCandidates.append(candidateObj);
            return;
        }
        m_signaling->sendCandidate(target, candidateObj);
    });
}

void CallManager::setState(CallState newState)
{
    if (m_state == newState) return;
    m_state = newState;
    qDebug() << "CallManager: state ->" << newState;
    if (newState == Outgoing || newState == Incoming) startRingtone();
    else stopRingtone();
    emit stateChanged();
}

void CallManager::startCall(const QString &token, bool withVideo)
{
    if (m_state != Idle) return;
    m_callToken = token;
    m_withVideo = withVideo;
    m_cameraOn = withVideo;
    m_muted = false;
    m_callDuration = 0;
    m_callSid = QUuid::createUuid().toString(QUuid::WithoutBraces).left(16);

    setState(Outgoing);
    joinCallOnServer(withVideo);
    m_ringTimeout.start();
}

void CallManager::setRemotePeerInfo(const QString &name, const QString &peerId)
{
    m_remotePeerName = name;
    m_remotePeerId = peerId;
    emit callInfoChanged();
}

void CallManager::acceptCall(bool withVideo) {
    if (m_state != Incoming) return;
    m_withVideo = withVideo; m_cameraOn = withVideo; m_muted = false; m_callDuration = 0;
    m_ringTimeout.stop();
    setState(Connecting);
    joinCallOnServer(withVideo);
}

void CallManager::declineCall() {
    if (m_state != Incoming) return;
    m_ringTimeout.stop(); setState(Idle); emit callEnded("Declined");
}

void CallManager::hangUp() {
    if (m_state == Idle) return;
    teardown("Hung up");
}

void CallManager::toggleMute() {
    m_muted = !m_muted; m_pipeline.setMuted(m_muted); emit muteChanged();
}

void CallManager::toggleCamera() {
    m_cameraOn = !m_cameraOn; emit cameraChanged();
}

void CallManager::joinCallOnServer(bool withVideo)
{
    int flags = 1 | 2;
    if (withVideo) flags |= 4;

    QJsonObject body;
    body["flags"] = flags;

    m_api->post("apps/spreed/api/v4/call/" + m_callToken, body,
        [this](bool ok, const QJsonObject &, int statusCode) {
            if (!ok) {
                qWarning() << "CallManager: failed to join call, status=" << statusCode;
                teardown("Failed to join call");
                return;
            }
            qDebug() << "CallManager: joined call on server, MCU=" << m_signaling->hasMcu();

            // Fetch ICE servers
            m_api->get("apps/spreed/api/v3/signaling/settings",
                [this](bool ok2, const QJsonObject &settings, int) {
                    QString stunUrl = "stun:stun.nextcloud.com:443";
                    if (ok2) {
                        auto stunArr = settings["stunservers"].toArray();
                        if (!stunArr.isEmpty()) {
                            auto urls = stunArr[0].toObject()["urls"].toArray();
                            if (!urls.isEmpty()) stunUrl = urls[0].toString();
                        }
                    }
                    qDebug() << "CallManager: STUN:" << stunUrl;

                    if (m_signaling->hasMcu()) {
                        // MCU flow: publish ownPeer (offer to our own session)
                        // The MCU will answer back
                        m_pendingSdpTarget = m_signaling->sessionId();
                        m_pipeline.startCall(stunUrl, QString());
                        qDebug() << "CallManager: MCU mode — publishing ownPeer to" << m_signaling->sessionId().left(20);
                    } else {
                        // P2P flow: start pipeline, offer will be sent when remote peer joins
                        m_pipeline.startCall(stunUrl, QString());
                    }
                });
        });
}

void CallManager::leaveCallOnServer()
{
    if (m_callToken.isEmpty()) return;
    m_api->del("apps/spreed/api/v4/call/" + m_callToken,
        [](bool, const QJsonObject &, int) {});
}

void CallManager::teardown(const QString &reason)
{
    m_ringTimeout.stop();
    m_durationTimer.stop();
    m_callSignaling.stop();
    leaveCallOnServer();
    m_pipeline.stop();

    m_callToken.clear();
    m_remoteSessionId.clear();
    m_remotePeerName.clear();
    m_remotePeerId.clear();
    m_callDuration = 0;
    m_pendingSdp.clear();
    m_pendingSdpType.clear();
    m_pendingSdpTarget.clear();
    m_pendingCandidates.clear();

    setState(Idle);
    emit callEnded(reason);
}

// --- Participant events ---

void CallManager::onParticipantJoinedCall(const QString &sessionId, int flags, const QString &displayName)
{
    if (sessionId == m_signaling->sessionId()) {
        qDebug() << "CallManager: ignoring own session join";
        return;
    }

    if ((m_state == Outgoing || m_state == Connecting) && m_remoteSessionId.isEmpty()) {
        m_remoteSessionId = sessionId;
        if (!displayName.isEmpty()) m_remotePeerName = displayName;
        qDebug() << "CallManager: remote peer joined:" << sessionId.left(20) << "name=" << m_remotePeerName;
        m_ringTimeout.stop();
        setState(Connecting);
        emit callInfoChanged();

        if (m_signaling->hasMcu()) {
            // MCU: request the remote peer's stream from MCU
            m_signaling->requestOffer(sessionId, "video");
            qDebug() << "CallManager: MCU — requesting offer for remote peer";
        } else {
            // P2P: flush queued offer
            if (!m_pendingSdp.isEmpty()) {
                m_signaling->sendOffer(m_remoteSessionId, m_pendingSdp);
                m_pendingSdp.clear(); m_pendingSdpType.clear();
            }
            for (const auto &c : m_pendingCandidates)
                m_signaling->sendCandidate(m_remoteSessionId, c);
            m_pendingCandidates.clear();
        }
    }
    else if (m_state == Idle) {
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
    if (sessionId == m_signaling->sessionId()) return;
    if (sessionId != m_remoteSessionId) return;
    qDebug() << "CallManager: remote peer left call";
    teardown("Call ended");
}

// --- SDP events (from HPB WebSocket) ---

void CallManager::onOfferReceived(const QString &fromSessionId, const QString &sdp)
{
    qDebug() << "CallManager: received offer from" << fromSessionId.left(20) << "state=" << m_state;

    if (m_signaling->hasMcu()) {
        // MCU: this is either our ownPeer answer-back or a subscriber offer
        if (fromSessionId == m_signaling->sessionId()) {
            // This is the MCU's answer to our ownPeer — but it comes as "offer" type
            // Actually with MCU, the MCU sends us offers for subscriber streams
            // Just set remote description and create answer
            qDebug() << "CallManager: MCU offer for own stream, creating answer";
        }
        m_pipeline.setRemoteDescription("offer", sdp);
        return;
    }

    // P2P: standard offer from remote peer
    if (m_state != Connecting && m_state != Active && m_state != Outgoing) return;
    if (m_remoteSessionId.isEmpty()) m_remoteSessionId = fromSessionId;
    m_pipeline.setRemoteDescription("offer", sdp);
}

void CallManager::onAnswerReceived(const QString &fromSessionId, const QString &sdp)
{
    qDebug() << "CallManager: received answer from" << fromSessionId.left(20) << "state=" << m_state;

    if (m_signaling->hasMcu()) {
        // MCU: answer to our ownPeer offer
        qDebug() << "CallManager: MCU answer for ownPeer, setting remote description";
        m_pipeline.setRemoteDescription("answer", sdp);

        // ownPeer is connected to MCU — now we can receive remote streams
        if (m_state == Connecting || m_state == Outgoing) {
            setState(Active);
            m_durationTimer.start();
        }
        return;
    }

    // P2P flow
    if (m_state != Connecting && m_state != Outgoing) return;
    if (m_remoteSessionId.isEmpty()) m_remoteSessionId = fromSessionId;
    m_ringTimeout.stop();
    m_pipeline.setRemoteDescription("answer", sdp);
    setState(Active);
    m_durationTimer.start();
}
