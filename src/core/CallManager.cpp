#include "core/CallManager.h"
#include <QJsonObject>
#include <QDebug>
#include <QDateTime>
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
    const int t1s = 0, t1e = sampleRate * 3 / 10;
    const int t2s = sampleRate * 4 / 10, t2e = sampleRate * 7 / 10;
    QByteArray pcm(totalSamples * 2, 0);
    auto *samples = reinterpret_cast<qint16*>(pcm.data());
    for (int i = 0; i < totalSamples; i++) {
        bool inTone = (i >= t1s && i < t1e) || (i >= t2s && i < t2e);
        samples[i] = inTone ? static_cast<qint16>(16000 * qSin(2.0 * M_PI * 440.0 * i / sampleRate)) : 0;
    }
    QByteArray wav;
    int ds = pcm.size(), fs = 36 + ds;
    wav.append("RIFF", 4); wav.append(reinterpret_cast<const char*>(&fs), 4);
    wav.append("WAVE", 4); wav.append("fmt ", 4);
    qint32 fmtSz = 16; wav.append(reinterpret_cast<const char*>(&fmtSz), 4);
    qint16 af = 1; wav.append(reinterpret_cast<const char*>(&af), 2);
    qint16 ch = 1; wav.append(reinterpret_cast<const char*>(&ch), 2);
    qint32 sr = sampleRate; wav.append(reinterpret_cast<const char*>(&sr), 4);
    qint32 br = sampleRate * 2; wav.append(reinterpret_cast<const char*>(&br), 4);
    qint16 ba = 2; wav.append(reinterpret_cast<const char*>(&ba), 2);
    qint16 bps = 16; wav.append(reinterpret_cast<const char*>(&bps), 2);
    wav.append("data", 4); wav.append(reinterpret_cast<const char*>(&ds), 4);
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
{
    // HPB participant events
    connect(m_signaling, &SignalingClient::participantJoinedCall,
            this, &CallManager::onParticipantJoinedCall);
    connect(m_signaling, &SignalingClient::participantLeftCall,
            this, &CallManager::onParticipantLeftCall);

    // HPB WebSocket signaling messages
    connect(m_signaling, &SignalingClient::offerReceived,
            this, &CallManager::onOfferReceived);
    connect(m_signaling, &SignalingClient::answerReceived,
            this, &CallManager::onAnswerReceived);
    connect(m_signaling, &SignalingClient::candidateReceived,
            this, [this](const QString &fromSessionId, const QJsonObject &candidate) {
        // Unwrap: payload may be {candidate: {candidate, sdpMLineIndex, sdpMid}}
        QJsonObject c = candidate.contains("candidate") && candidate["candidate"].isObject()
            ? candidate["candidate"].toObject() : candidate;
        QString cStr = c["candidate"].toString();
        int mline = c["sdpMLineIndex"].toInt();
        QString mid = c["sdpMid"].toString();

        if (fromSessionId == m_signaling->sessionId() && m_publishPipeline) {
            m_publishPipeline->addIceCandidate(cStr, mline, mid);
        } else if (m_subscribePipelines.contains(fromSessionId)) {
            m_subscribePipelines[fromSessionId]->addIceCandidate(cStr, mline, mid);
        }
    });

    // GLib main context pump (shared for all GStreamer pipelines)
    connect(&m_glibTimer, &QTimer::timeout, this, []() {
        while (g_main_context_iteration(nullptr, FALSE)) {}
    });

    // Duration timer
    m_durationTimer.setInterval(1000);
    connect(&m_durationTimer, &QTimer::timeout, this, [this]() {
        m_callDuration++;
        emit durationChanged();
    });

    // Ring timeout
    m_ringTimeout.setSingleShot(true);
    m_ringTimeout.setInterval(30000);
    connect(&m_ringTimeout, &QTimer::timeout, this, [this]() {
        if (m_state == Incoming) declineCall();
        else if (m_state == Outgoing) teardown("No answer");
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
    setState(Outgoing);
    joinCallOnServer(withVideo);
    m_ringTimeout.start();
}

void CallManager::setRemotePeerInfo(const QString &name, const QString &peerId) {
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
    m_muted = !m_muted;
    if (m_publishPipeline) m_publishPipeline->setMuted(m_muted);
    emit muteChanged();
}

void CallManager::toggleCamera() {
    m_cameraOn = !m_cameraOn; emit cameraChanged();
}

void CallManager::joinCallOnServer(bool withVideo)
{
    int flags = 1 | 2;  // IN_CALL | WITH_AUDIO
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

            qDebug() << "CallManager: joined call, MCU=" << m_signaling->hasMcu();

            // Fetch STUN server
            m_api->get("apps/spreed/api/v3/signaling/settings",
                [this](bool ok2, const QJsonObject &settings, int) {
                    m_stunServer = "stun:stun.nextcloud.com:443";
                    if (ok2) {
                        auto stunArr = settings["stunservers"].toArray();
                        if (!stunArr.isEmpty()) {
                            auto urls = stunArr[0].toObject()["urls"].toArray();
                            if (!urls.isEmpty()) m_stunServer = urls[0].toString();
                        }
                    }
                    qDebug() << "CallManager: STUN:" << m_stunServer;

                    // Generate publisher SID (matches NC Talk: Date.now().toString())
                    QString pubSid = QString::number(QDateTime::currentMSecsSinceEpoch());

                    // Start publisher (send our audio to MCU)
                    m_publishPipeline = new PublishPipeline(this);

                    connect(m_publishPipeline, &PublishPipeline::localOfferReady,
                            this, [this, pubSid](const QString &sdp) {
                        // Send offer to OUR OWN session ID (MCU intercepts)
                        m_signaling->sendOffer(m_signaling->sessionId(), sdp, pubSid);
                        qDebug() << "CallManager: sent publish offer to own session, sid=" << pubSid;
                    });

                    connect(m_publishPipeline, &PublishPipeline::iceCandidateReady,
                            this, [this, pubSid](const QString &candidate, int mline, const QString &mid) {
                        QJsonObject c;
                        c["candidate"] = candidate;
                        c["sdpMLineIndex"] = mline;
                        c["sdpMid"] = mid;
                        m_signaling->sendCandidate(m_signaling->sessionId(), c, pubSid);
                    });

                    connect(m_publishPipeline, &PublishPipeline::iceStateChanged,
                            this, [this](const QString &state) {
                        qDebug() << "CallManager: publisher ICE:" << state;
                    });

                    m_publishPipeline->start(m_stunServer);
                    m_glibTimer.start(20);
                });
        });
}

void CallManager::leaveCallOnServer()
{
    if (m_callToken.isEmpty()) return;
    m_api->del("apps/spreed/api/v4/call/" + m_callToken,
        [](bool, const QJsonObject &, int) {});
}

void CallManager::stopAllPipelines()
{
    m_glibTimer.stop();
    if (m_publishPipeline) {
        m_publishPipeline->stop();
        m_publishPipeline->deleteLater();
        m_publishPipeline = nullptr;
    }
    for (auto *sub : m_subscribePipelines) {
        sub->stop();
        sub->deleteLater();
    }
    m_subscribePipelines.clear();
}

void CallManager::teardown(const QString &reason)
{
    m_ringTimeout.stop();
    m_durationTimer.stop();
    stopAllPipelines();
    leaveCallOnServer();

    m_callToken.clear();
    m_remoteSessionId.clear();
    m_remotePeerName.clear();
    m_remotePeerId.clear();
    m_callDuration = 0;

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

        // MCU: request the remote peer's audio stream
        m_signaling->requestOffer(sessionId, "video");
        qDebug() << "CallManager: sent requestOffer for remote peer";
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

    // Remove subscriber pipeline for this peer
    if (m_subscribePipelines.contains(sessionId)) {
        m_subscribePipelines[sessionId]->stop();
        m_subscribePipelines[sessionId]->deleteLater();
        m_subscribePipelines.remove(sessionId);
        qDebug() << "CallManager: removed subscriber for" << sessionId.left(20);
    }

    if (sessionId == m_remoteSessionId) {
        qDebug() << "CallManager: remote peer left call";
        teardown("Call ended");
    }
}

// --- SDP events ---

void CallManager::onOfferReceived(const QString &fromSessionId, const QString &sdp, const QString &sid)
{
    qDebug() << "CallManager: received offer from" << fromSessionId.left(20) << "sid=" << sid;

    // Use the MCU's sid for all subscriber messages (answer + candidates)
    QString mcuSid = sid;

    // MCU sends offer for subscriber stream (from the remote session ID)
    if (!m_subscribePipelines.contains(fromSessionId)) {
        auto *sub = new SubscribePipeline(fromSessionId, this);

        connect(sub, &SubscribePipeline::localAnswerReady,
                this, [this, fromSessionId, mcuSid](const QString &sdp) {
            m_signaling->sendAnswer(fromSessionId, sdp, mcuSid);
            qDebug() << "CallManager: sent subscriber answer to" << fromSessionId.left(20) << "sid=" << mcuSid;
        });

        connect(sub, &SubscribePipeline::iceCandidateReady,
                this, [this, fromSessionId, mcuSid](const QString &candidate, int mline, const QString &mid) {
            QJsonObject c;
            c["candidate"] = candidate;
            c["sdpMLineIndex"] = mline;
            c["sdpMid"] = mid;
            m_signaling->sendCandidate(fromSessionId, c, mcuSid);
        });

        connect(sub, &SubscribePipeline::iceStateChanged,
                this, [this](const QString &state) {
            qDebug() << "CallManager: subscriber ICE:" << state;
            if (state == "connected" || state == "completed") {
                if (m_state == Connecting) {
                    setState(Active);
                    m_durationTimer.start();
                }
            }
        });

        m_subscribePipelines[fromSessionId] = sub;
        sub->start(m_stunServer);
    }

    m_subscribePipelines[fromSessionId]->setRemoteOffer(sdp);
}

void CallManager::onAnswerReceived(const QString &fromSessionId, const QString &sdp)
{
    qDebug() << "CallManager: received answer from" << fromSessionId.left(20);

    // MCU answer to our publisher offer (from our own session ID)
    if (m_publishPipeline) {
        m_publishPipeline->setRemoteAnswer(sdp);
        qDebug() << "CallManager: set publisher remote answer";
    }
}
