#include "core/CallManager.h"
#include <QJsonObject>
#include <QDebug>
#include <QDateTime>
#include <QtMath>
#include <QSet>
#include <QSettings>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

// NC Talk call flags (bitmask)
static constexpr int CALL_FLAG_IN_CALL    = 1;
static constexpr int CALL_FLAG_WITH_AUDIO = 2;
static constexpr int CALL_FLAG_WITH_VIDEO = 4;

// --- Ringtone ---

static QByteArray buildWavHeader(int dataSize, int sampleRate)
{
    QByteArray wav;
    int fs = 36 + dataSize;
    wav.append("RIFF", 4); wav.append(reinterpret_cast<const char*>(&fs), 4);
    wav.append("WAVE", 4); wav.append("fmt ", 4);
    qint32 fmtSz = 16; wav.append(reinterpret_cast<const char*>(&fmtSz), 4);
    qint16 af = 1; wav.append(reinterpret_cast<const char*>(&af), 2);
    qint16 ch = 1; wav.append(reinterpret_cast<const char*>(&ch), 2);
    qint32 sr = sampleRate; wav.append(reinterpret_cast<const char*>(&sr), 4);
    qint32 br = sampleRate * 2; wav.append(reinterpret_cast<const char*>(&br), 4);
    qint16 ba = 2; wav.append(reinterpret_cast<const char*>(&ba), 2);
    qint16 bps = 16; wav.append(reinterpret_cast<const char*>(&bps), 2);
    wav.append("data", 4); wav.append(reinterpret_cast<const char*>(&dataSize), 4);
    return wav;
}

// Outgoing: simple ringback tone (brrr-brrr)
static QByteArray generateOutgoingTone()
{
    const int sampleRate = 22050;
    const int totalSamples = sampleRate * 3;
    const int t1s = 0, t1e = sampleRate * 3 / 10;
    const int t2s = sampleRate * 4 / 10, t2e = sampleRate * 7 / 10;
    QByteArray pcm(totalSamples * 2, 0);
    auto *samples = reinterpret_cast<qint16*>(pcm.data());
    for (int i = 0; i < totalSamples; i++) {
        bool inTone = (i >= t1s && i < t1e) || (i >= t2s && i < t2e);
        samples[i] = inTone ? static_cast<qint16>(12000 * qSin(2.0 * M_PI * 440.0 * i / sampleRate)) : 0;
    }
    return buildWavHeader(pcm.size(), sampleRate) + pcm;
}

// Incoming: classic two-tone ring (ding-dong, pause, ding-dong)
static QByteArray generateIncomingRingtone()
{
    const int sampleRate = 22050;
    // Two-tone chime: high-low, pause, high-low, longer pause
    struct Tone { double freq; double duration; double gap; };
    Tone pattern[] = {
        {880, 0.15, 0.05},   // ding (A5)
        {659, 0.25, 0.40},   // dong (E5) + pause
        {880, 0.15, 0.05},   // ding
        {659, 0.25, 1.20},   // dong + long pause before loop
    };
    const int count = sizeof(pattern) / sizeof(pattern[0]);

    double totalDuration = 0;
    for (int n = 0; n < count; n++) totalDuration += pattern[n].duration + pattern[n].gap;

    const int totalSamples = static_cast<int>(sampleRate * totalDuration);
    QByteArray pcm(totalSamples * 2, 0);
    auto *samples = reinterpret_cast<qint16*>(pcm.data());

    int pos = 0;
    for (int n = 0; n < count; n++) {
        int toneSamples = static_cast<int>(sampleRate * pattern[n].duration);
        int gapSamples = static_cast<int>(sampleRate * pattern[n].gap);
        for (int i = 0; i < toneSamples && pos < totalSamples; i++, pos++) {
            double t = static_cast<double>(i) / sampleRate;
            // Smooth envelope: fade in 10ms, fade out last 30%
            double env = 1.0;
            if (i < sampleRate / 100) env = static_cast<double>(i) / (sampleRate / 100);
            int fadeOut = toneSamples * 3 / 10;
            if (i > toneSamples - fadeOut) env = static_cast<double>(toneSamples - i) / fadeOut;
            // Clean sine with soft second harmonic
            double wave = 0.85 * qSin(2.0 * M_PI * pattern[n].freq * t)
                        + 0.15 * qSin(2.0 * M_PI * pattern[n].freq * 2.0 * t);
            samples[pos] = static_cast<qint16>(14000 * env * wave);
        }
        pos += gapSamples;  // silence gap
    }

    return buildWavHeader(pcm.size(), sampleRate) + pcm;
}

void CallManager::startRingtone() {
#ifdef Q_OS_WIN
    static QByteArray outgoing = generateOutgoingTone();
    static QByteArray incoming = generateIncomingRingtone();
    const QByteArray &wav = (m_state == Incoming) ? incoming : outgoing;
    PlaySoundA(wav.constData(), nullptr, SND_MEMORY | SND_ASYNC | SND_LOOP);
#endif
}
void CallManager::stopRingtone() {
#ifdef Q_OS_WIN
    PlaySoundA(nullptr, nullptr, 0);
#endif
}

// --- CallManager ---

CallManager::CallManager(ApiClient *api, SignalingClient *signaling, MediaDeviceManager *deviceMgr, QObject *parent)
    : QObject(parent)
    , m_api(api)
    , m_signaling(signaling)
    , m_deviceManager(deviceMgr)
{
    // HPB participant events
    connect(m_signaling, &SignalingClient::participantJoinedCall,
            this, &CallManager::onParticipantJoinedCall);
    connect(m_signaling, &SignalingClient::participantLeftCall,
            this, &CallManager::onParticipantLeftCall);

    // Re-request subscriber stream when remote peer enables their camera
    connect(m_signaling, &SignalingClient::participantFlagsChanged,
            this, [this](const QString &sessionId, int oldFlags, int newFlags) {
        if (m_state != Connecting && m_state != Active) return;
        bool hadVideo = (oldFlags & CALL_FLAG_WITH_VIDEO) != 0;
        bool hasVideo = (newFlags & CALL_FLAG_WITH_VIDEO) != 0;
        if (!hadVideo && hasVideo && m_subscribePipelines.contains(sessionId)) {
            qDebug() << "CallManager: peer" << sessionId.left(20) << "enabled video, re-requesting stream";
            m_signaling->requestOffer(sessionId, "video");
        }
    });

    // Room peer joined — request their stream if we're in a call
    connect(m_signaling, &SignalingClient::roomPeerJoined,
            this, [this](const QString &sessionId) {
        if ((m_state == Connecting || m_state == Active)
            && !m_subscribePipelines.contains(sessionId)) {
            if (m_remoteSessionId.isEmpty()) {
                m_remoteSessionId = sessionId;
                emit callInfoChanged();
            }
            m_signaling->requestOffer(sessionId, "video");
            qDebug() << "CallManager: requestOffer for room peer" << sessionId.left(20);
        }
    });

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

        if (m_useP2P && m_peerPipeline) {
            m_peerPipeline->addIceCandidate(cStr, mline, mid);
            return;
        }
        if (fromSessionId == m_signaling->sessionId() && m_publishPipeline) {
            m_publishPipeline->addIceCandidate(cStr, mline, mid);
        } else if (m_subscribePipelines.contains(fromSessionId)) {
            m_subscribePipelines[fromSessionId]->addIceCandidate(cStr, mline, mid);
        }
    });

    // GLib main context pump + bus polling
    connect(&m_glibTimer, &QTimer::timeout, this, [this]() {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        if (m_publishPipeline) m_publishPipeline->pollBus();
        if (m_peerPipeline) m_peerPipeline->pollBus();
    });

    // Duration timer
    m_durationTimer.setInterval(1000);
    connect(&m_durationTimer, &QTimer::timeout, this, [this]() {
        m_callDuration++;
        emit durationChanged();
    });

    // Ring timeout
    m_ringTimeout.setSingleShot(true);
    m_ringTimeout.setInterval(60000);  // 60s — incoming call detection can take up to 30s
    connect(&m_ringTimeout, &QTimer::timeout, this, [this]() {
        if (m_state == Incoming) declineCall();
        else if (m_state == Outgoing) teardown("No answer");
    });

    // Stats timer — update call info every 2 seconds
    m_statsTimer.setInterval(2000);
    connect(&m_statsTimer, &QTimer::timeout, this, &CallManager::updateCallStats);
}

void CallManager::updateCallStats()
{
    QStringList lines;

    // Signaling
    lines << "Signaling: HPB (MCU)";
    lines << "STUN: " + m_stunServer;
    lines << "Session: " + m_signaling->sessionId().left(16) + "...";

    // Publisher
    if (m_publishPipeline && m_publishPipeline->isRunning())
        lines << "Publisher: connected (Opus 48kHz)";
    else
        lines << "Publisher: -";

    // Subscribers
    lines << "Subscribers: " + QString::number(m_subscribePipelines.size());
    for (auto it = m_subscribePipelines.begin(); it != m_subscribePipelines.end(); ++it) {
        QString state = it.value()->isRunning() ? "active" : "stopped";
        lines << "  " + it.key().left(12) + "... " + state;
    }

    // Remote peer
    if (!m_remoteSessionId.isEmpty())
        lines << "Remote: " + m_remoteSessionId.left(16) + "...";

    // Codec info
    lines << "Codec: Opus (WebRTC)";
    lines << "Transport: DTLS-SRTP";

    m_callStats = lines.join("\n");
    emit callStatsChanged();
}

void CallManager::setState(CallState newState)
{
    if (m_state == newState) return;
    m_state = newState;
    qDebug() << "CallManager: state ->" << newState;
    if (newState == Outgoing || newState == Incoming) startRingtone();
    else stopRingtone();
    if (newState == Active) { updateCallStats(); m_statsTimer.start(); }
    else m_statsTimer.stop();
    emit stateChanged();
}

void CallManager::startCall(const QString &token, bool withVideo)
{
    if (m_state != Idle) return;
    m_callToken = token;
    m_withVideo = withVideo;
    m_cameraOn = withVideo;
    emit cameraChanged();
    m_muted = false;
    m_callDuration = 0;
    setState(Outgoing);
    setStatusDetail("Joining call");
    joinCallOnServer(withVideo);
    m_ringTimeout.start();
}

void CallManager::onIncomingCallDetected(const QString &callerName, const QString &token, int callFlag)
{
    if (m_state != Idle) return;

    // Cooldown: don't re-detect the same call we just declined
    if (token == m_lastDeclinedToken
        && m_lastDeclinedTime.isValid()
        && m_lastDeclinedTime.msecsTo(QDateTime::currentDateTime()) < 10000) {
        qDebug() << "CallManager: ignoring re-detection of recently declined call" << token;
        return;
    }

    qDebug() << "CallManager: incoming call detected:" << callerName << "token=" << token;
    m_callToken = token;
    m_remotePeerName = callerName;
    m_withVideo = (callFlag & CALL_FLAG_WITH_VIDEO) != 0;
    m_incomingTime = QDateTime::currentDateTime();
    m_userActionReady = false;
    setState(Incoming);
    m_ringTimeout.start();
    emit callInfoChanged();
    emit incomingCall(callerName, token, m_withVideo);
}

void CallManager::setRemotePeerInfo(const QString &name, const QString &peerId) {
    m_remotePeerName = name;
    m_remotePeerId = peerId;
    emit callInfoChanged();
}

void CallManager::setUserActionReady()
{
    m_userActionReady = true;
    qDebug() << "CallManager: user action ready (popup loaded)";
}

void CallManager::acceptCall(bool withVideo) {
    if (m_state != Incoming) return;
    if (!m_userActionReady) {
        qDebug() << "CallManager: ignoring accept — UI not ready";
        return;
    }
    m_withVideo = withVideo; m_cameraOn = withVideo; m_muted = false; m_callDuration = 0;
    emit cameraChanged();
    m_ringTimeout.stop();
    setStatusDetail("Joining room");
    setState(Connecting);

    // Sequence: join room (REST) → join signaling room (WS) → wait for room joined → join call
    QJsonObject empty;
    m_api->post("apps/spreed/api/v4/room/" + m_callToken + "/participants/active", empty,
        [this, withVideo](bool, const QJsonObject &, int) {
            // Wait for HPB room join confirmation before calling the API
            auto *conn = new QMetaObject::Connection;
            *conn = connect(m_signaling, &SignalingClient::roomJoined,
                this, [this, withVideo, conn]() {
                    disconnect(*conn);
                    delete conn;
                    qDebug() << "CallManager: signaling room joined, now joining call";
                    joinCallOnServer(withVideo);
                });
            m_signaling->joinRoom(m_callToken);
        });
}

void CallManager::declineCall()
{
    if (m_state != Incoming) return;
    if (!m_userActionReady) {
        qDebug() << "CallManager: ignoring decline — UI not ready";
        return;
    }

    qDebug() << "CallManager: declining call for token" << m_callToken;
    setState(Ending);

    if (m_joinedCall) {
        m_api->del(QString("apps/spreed/api/v4/call/%1").arg(m_callToken),
            [this](bool ok, const QJsonObject &, int) {
                qDebug() << "CallManager: leave call API" << (ok ? "succeeded" : "failed");
            });
        m_joinedCall = false;
    }

    // Leave the room — triggers participantLeftRoom event for caller
    m_api->del(QString("apps/spreed/api/v4/room/%1/participants/active").arg(m_callToken),
        [this](bool ok, const QJsonObject &, int) {
            qDebug() << "CallManager: leave room API" << (ok ? "succeeded" : "failed");
            teardown("declined");
        });

    // Preserve cooldown tracking to prevent re-detection
    m_lastDeclinedToken = m_callToken;
    m_lastDeclinedTime = QDateTime::currentDateTime();
}

void CallManager::hangUp() {
    if (m_state == Idle) return;
    qDebug() << "CallManager: hangUp from state" << m_state;
    teardown("Hung up");
}

void CallManager::toggleMute() {
    m_muted = !m_muted;
    if (m_useP2P && m_peerPipeline) m_peerPipeline->setMuted(m_muted);
    else if (m_publishPipeline) m_publishPipeline->setMuted(m_muted);
    emit muteChanged();

    // Broadcast mute/unmute state to peers via signaling (NC Talk compatibility)
    broadcastMediaState("audio", !m_muted);
}

void CallManager::toggleCamera() {
    m_cameraOn = !m_cameraOn;
    emit cameraChanged();

    auto enableCam = [this](auto *pipeline) {
        pipeline->enableCamera(videoDeviceIndex(), preferHd1080());
    };

    if (m_useP2P && m_peerPipeline) {
        m_cameraOn ? enableCam(m_peerPipeline) : m_peerPipeline->disableCamera();
    } else if (m_publishPipeline) {
        m_cameraOn ? enableCam(m_publishPipeline) : m_publishPipeline->disableCamera();
    }

    // Broadcast video state + update call flags on server
    broadcastMediaState("video", m_cameraOn);
    updateCallFlags();
}

int CallManager::videoDeviceIndex() const
{
    return m_deviceManager ? qMax(0, m_deviceManager->selectedVideoInput()) : 0;
}

bool CallManager::preferHd1080() const
{
    return QSettings().value("video/resolution", 0).toInt() == 0;
}

void CallManager::broadcastMediaState(const QString &media, bool enabled)
{
    // NC Talk sends mute/unmute messages via signaling to all peers
    QString type = enabled ? "unmute" : "mute";
    QJsonObject payload;
    payload["name"] = media;

    // Collect unique peer session IDs (remote + all subscribers)
    QSet<QString> peers(m_subscribePipelines.keyBegin(), m_subscribePipelines.keyEnd());
    if (!m_remoteSessionId.isEmpty())
        peers.insert(m_remoteSessionId);

    for (const QString &peerId : peers)
        m_signaling->sendSessionMessage(peerId, type, payload, QString());
    qDebug() << "CallManager: broadcast" << type << media << "to" << peers.size() << "peer(s)";
}

void CallManager::updateCallFlags()
{
    if (m_callToken.isEmpty() || (m_state != Connecting && m_state != Active))
        return;

    int flags = CALL_FLAG_IN_CALL | CALL_FLAG_WITH_AUDIO;
    if (m_cameraOn) flags |= CALL_FLAG_WITH_VIDEO;

    QJsonObject body;
    body["flags"] = flags;
    m_api->put("apps/spreed/api/v4/call/" + m_callToken, body,
        [](bool ok, const QJsonObject &, int statusCode) {
            if (!ok) qWarning() << "CallManager: failed to update call flags, status=" << statusCode;
        });
}

void CallManager::joinCallOnServer(bool withVideo)
{
    int flags = CALL_FLAG_IN_CALL | CALL_FLAG_WITH_AUDIO;
    if (withVideo) flags |= CALL_FLAG_WITH_VIDEO;

    QJsonObject body;
    body["flags"] = flags;

    m_api->post("apps/spreed/api/v4/call/" + m_callToken, body,
        [this](bool ok, const QJsonObject &, int statusCode) {
            if (!ok) {
                qWarning() << "CallManager: failed to join call, status=" << statusCode;
                teardown("Failed to join call");
                return;
            }

            m_joinedCall = true;
            setStatusDetail("Fetching servers");
            qDebug() << "CallManager: joined call, MCU=" << m_signaling->hasMcu();

            // Fetch STUN server
            m_api->get("apps/spreed/api/v3/signaling/settings",
                [this](bool ok2, const QJsonObject &settings, int) {
                    m_stunServer = "stun:stun.nextcloud.com:443";
                    if (ok2) {
                        // Pick STUN server — prefer non-nextcloud.com (own server)
                        auto stunArr = settings["stunservers"].toArray();
                        for (const auto &s : stunArr) {
                            auto urls = s.toObject()["urls"].toArray();
                            for (const auto &u : urls) {
                                QString url = u.toString();
                                if (!url.contains("nextcloud.com")) {
                                    m_stunServer = url;
                                    break;
                                }
                                if (m_stunServer.contains("nextcloud.com"))
                                    m_stunServer = url;
                            }
                            if (!m_stunServer.contains("nextcloud.com")) break;
                        }
                    }
                    qDebug() << "CallManager: STUN:" << m_stunServer;

                    QList<TurnServer> turnServers;
                    auto turnArr = settings["turnservers"].toArray();
                    for (const auto &ts : turnArr) {
                        auto obj = ts.toObject();
                        TurnServer turn;
                        auto urls = obj["urls"].toArray();
                        for (const auto &u : urls)
                            turn.urls.append(u.toString());
                        turn.username = obj["username"].toString();
                        turn.credential = obj["credential"].toString();
                        if (!turn.urls.isEmpty())
                            turnServers.append(turn);
                    }
                    qDebug() << "CallManager: found" << turnServers.size() << "TURN servers";
                    m_turnServers = turnServers;

                    // P2P for 1:1 calls when no MCU, MCU when server has it
                    m_useP2P = !m_signaling->hasMcu();
                    setStatusDetail("Starting pipeline");
                    qDebug() << "CallManager: call mode =" << (m_useP2P ? "P2P" : "MCU");

                    if (m_useP2P) {
                        // --- P2P mode: single PeerPipeline for 1:1 calls ---
                        QString p2pSid = QString::number(QDateTime::currentMSecsSinceEpoch());

                        m_peerPipeline = new PeerPipeline(this);
                        m_localVideoProvider = m_peerPipeline->localVideoProvider();
                        emit localVideoProviderChanged();
                        m_remoteVideoProvider = m_peerPipeline->remoteVideoProvider();
                        emit remoteVideoProviderChanged();

                        connect(m_peerPipeline, &PeerPipeline::localOfferReady,
                                this, [this, p2pSid](const QString &sdp) {
                            setStatusDetail("Sending offer");
                            m_signaling->sendOffer(m_remoteSessionId, sdp, p2pSid);
                            qDebug() << "CallManager: sent P2P offer to" << m_remoteSessionId.left(20);
                        });

                        connect(m_peerPipeline, &PeerPipeline::localAnswerReady,
                                this, [this, p2pSid](const QString &sdp) {
                            m_signaling->sendAnswer(m_remoteSessionId, sdp, p2pSid);
                            qDebug() << "CallManager: sent P2P answer to" << m_remoteSessionId.left(20);
                        });

                        connect(m_peerPipeline, &PeerPipeline::iceCandidateReady,
                                this, [this, p2pSid](const QString &candidate, int mline, const QString &mid) {
                            QJsonObject c;
                            c["candidate"] = candidate;
                            c["sdpMLineIndex"] = mline;
                            c["sdpMid"] = mid;
                            m_signaling->sendCandidate(m_remoteSessionId, c, p2pSid);
                        });

                        connect(m_peerPipeline, &PeerPipeline::iceStateChanged,
                                this, [this](const QString &state) {
                            qDebug() << "CallManager: P2P ICE:" << state;
                            setStatusDetail("ICE " + state);
                            if (state == "connected" || state == "completed") {
                                setStatusDetail("Connected");
                                if (m_state == Connecting) {
                                    setState(Active);
                                    m_durationTimer.start();
                                }
                            } else if (state == "failed" && m_signaling->hasMcu()) {
                                // P2P failed, fall back to MCU
                                qWarning() << "CallManager: P2P ICE failed, falling back to MCU";
                                m_peerPipeline->stop();
                                m_peerPipeline->deleteLater();
                                m_peerPipeline = nullptr;
                                m_useP2P = false;
                                m_localVideoProvider = nullptr;
                                emit localVideoProviderChanged();
                                m_remoteVideoProvider = nullptr;
                                emit remoteVideoProviderChanged();
                                // Re-enter joinCallOnServer which will now use MCU
                                joinCallOnServer(m_withVideo);
                            }
                        });

                        connect(m_peerPipeline, &PeerPipeline::audioLevelUpdated,
                                this, [this](double level) {
                            if (qAbs(m_audioLevel - level) > 0.02) {
                                m_audioLevel = level;
                                emit audioLevelChanged();
                            }
                        });

                        connect(m_peerPipeline, &PeerPipeline::cameraError, this, [this](const QString &reason) {
                            qWarning() << "CallManager: P2P camera error:" << reason;
                            m_cameraOn = false;
                            emit cameraChanged();
                        });

                        connect(m_peerPipeline, &PeerPipeline::error, this, [this](const QString &msg) {
                            qWarning() << "CallManager: peer pipeline error:" << msg;
                            teardown(msg);
                        });

                        if (!m_peerPipeline->start(m_stunServer, turnServers,
                            m_deviceManager ? m_deviceManager->selectedInputDeviceId() : QString(),
                            m_deviceManager ? m_deviceManager->selectedOutputDeviceId() : QString())) {
                            qWarning() << "CallManager: failed to start peer pipeline";
                            teardown("Failed to start audio pipeline");
                            return;
                        }
                        m_glibTimer.start(20);

                        // If outgoing call and remote peer already known, create offer
                        if (m_state == Outgoing && !m_remoteSessionId.isEmpty()) {
                            m_peerPipeline->createOffer();
                            qDebug() << "CallManager: creating initial P2P offer";
                        }
                    } else {
                        // --- MCU mode: dual PublishPipeline + SubscribePipeline ---
                        // Generate publisher SID (matches NC Talk: Date.now().toString())
                        QString pubSid = QString::number(QDateTime::currentMSecsSinceEpoch());

                        // Start publisher (send our audio to MCU)
                        m_publishPipeline = new PublishPipeline(this);
                        m_localVideoProvider = m_publishPipeline->localVideoProvider();
                        emit localVideoProviderChanged();

                        connect(m_publishPipeline, &PublishPipeline::localOfferReady,
                                this, [this, pubSid](const QString &sdp) {
                            // Send offer to OUR OWN session ID (MCU intercepts)
                            setStatusDetail("Sending offer to MCU");
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
                            setStatusDetail("Publisher ICE " + state);
                        });

                        connect(m_publishPipeline, &PublishPipeline::audioLevelUpdated,
                                this, [this](double level) {
                            if (qAbs(m_audioLevel - level) > 0.02) {
                                m_audioLevel = level;
                                emit audioLevelChanged();
                            }
                        });

                        connect(m_publishPipeline, &PublishPipeline::error, this, [this](const QString &msg) {
                            qWarning() << "CallManager: publish pipeline error:" << msg;
                            teardown(msg);
                        });

                        connect(m_publishPipeline, &PublishPipeline::cameraError, this, [this](const QString &reason) {
                            qWarning() << "CallManager: camera error:" << reason;
                            // Try 720p fallback if 1080p failed
                            if (m_cameraOn && !m_cameraFallbackTried) {
                                m_cameraFallbackTried = true;
                                qDebug() << "CallManager: retrying camera at 720p";
                                m_publishPipeline->enableCamera(videoDeviceIndex(), false);
                            } else {
                                m_cameraOn = false;
                                m_cameraFallbackTried = false;
                                emit cameraChanged();
                            }
                        });

                        if (!m_publishPipeline->start(m_stunServer, turnServers, m_deviceManager ? m_deviceManager->selectedInputDeviceId() : QString())) {
                            qWarning() << "CallManager: failed to start publish pipeline";
                            teardown("Failed to start audio pipeline");
                            return;
                        }
                        m_glibTimer.start(20);

                        // If video call, enable camera immediately (local preview shows right away)
                        if (m_withVideo) {
                            m_publishPipeline->enableCamera(videoDeviceIndex(), preferHd1080());
                            qDebug() << "CallManager: auto-enabled camera for video call";
                        }

                        // If remote peer already joined (incoming call), request their stream
                        if (!m_remoteSessionId.isEmpty() && !m_subscribePipelines.contains(m_remoteSessionId)) {
                            setStatusDetail("Requesting peer stream");
                            m_signaling->requestOffer(m_remoteSessionId, "video");
                            qDebug() << "CallManager: sent requestOffer for already-joined remote peer";
                        } else {
                            // Discover who's already in the call and request their streams
                            m_api->getArray("apps/spreed/api/v4/call/" + m_callToken,
                                [this](bool ok, const QJsonArray &data, int) {
                                    if (!ok) { qWarning() << "CallManager: failed to discover call participants"; return; }
                                    if (m_state == Idle) return;  // call ended during request
                                    for (const auto &val : data) {
                                        QJsonObject p = val.toObject();
                                        QString sid = p["sessionId"].toString();
                                        int inCall = p["inCall"].toInt();
                                        if (sid.isEmpty() || sid == m_signaling->sessionId() || inCall == 0)
                                            continue;
                                        if (m_remoteSessionId.isEmpty()) {
                                            m_remoteSessionId = sid;
                                            emit callInfoChanged();
                                        }
                                        if (!m_subscribePipelines.contains(sid)) {
                                            m_signaling->requestOffer(sid, "video");
                                            qDebug() << "CallManager: sent requestOffer for discovered peer" << sid.left(20);
                                        }
                                    }
                                });
                        }
                    }
                });
        });
}

void CallManager::leaveCallOnServer()
{
    if (m_callToken.isEmpty()) return;
    m_api->del("apps/spreed/api/v4/call/" + m_callToken,
        [](bool ok, const QJsonObject &, int statusCode) {
            if (!ok) qWarning() << "CallManager: failed to leave call on server, status=" << statusCode;
        });
}

void CallManager::stopAllPipelines()
{
    m_glibTimer.stop();
    if (m_peerPipeline) {
        m_peerPipeline->stop();
        m_peerPipeline->deleteLater();
        m_peerPipeline = nullptr;
    }
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

    m_remoteVideoProvider = nullptr;
    emit remoteVideoProviderChanged();

    m_localVideoProvider = nullptr;
    emit localVideoProviderChanged();
}

void CallManager::teardown(const QString &reason)
{
    setStatusDetail("");
    m_ringTimeout.stop();
    m_durationTimer.stop();
    stopAllPipelines();
    leaveCallOnServer();

    m_callToken.clear();
    m_remoteSessionId.clear();
    m_remotePeerName.clear();
    m_remotePeerId.clear();
    m_callDuration = 0;
    m_joinedCall = false;
    m_userActionReady = false;

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

        if (m_useP2P && m_peerPipeline) {
            m_peerPipeline->createOffer();
            qDebug() << "CallManager: creating P2P offer for joined peer";
        } else {
            m_signaling->requestOffer(sessionId, "video");
            qDebug() << "CallManager: sent requestOffer for remote peer";
        }
    }
    else if (m_state == Idle) {
        m_remoteSessionId = sessionId;
        m_remotePeerName = displayName;
        m_callToken = m_signaling->currentRoom();
        m_withVideo = (flags & CALL_FLAG_WITH_VIDEO) != 0;
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
    setStatusDetail("Received offer");
    qDebug() << "CallManager: received offer from" << fromSessionId.left(20) << "sid=" << sid;

    if (m_useP2P && m_peerPipeline) {
        m_peerPipeline->setRemoteOffer(sdp);
        return;
    }

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
            setStatusDetail("Subscriber ICE " + state);
            if (state == "connected" || state == "completed") {
                setStatusDetail("Connected");
                if (m_state == Connecting) {
                    setState(Active);
                    m_durationTimer.start();
                }
            }
        });

        connect(sub, &SubscribePipeline::error, this, [this, fromSessionId](const QString &msg) {
            qWarning() << "CallManager: subscriber pipeline error for" << fromSessionId.left(20) << ":" << msg;
        });

        m_subscribePipelines[fromSessionId] = sub;
        if (!sub->start(m_stunServer, m_turnServers, m_deviceManager ? m_deviceManager->selectedOutputDeviceId() : QString())) {
            qWarning() << "CallManager: failed to start subscriber pipeline for" << fromSessionId.left(20);
            m_subscribePipelines.remove(fromSessionId);
            sub->deleteLater();
            return;
        }

        m_remoteVideoProvider = sub->videoProvider();
        emit remoteVideoProviderChanged();
    }

    m_subscribePipelines[fromSessionId]->setRemoteOffer(sdp);
}

void CallManager::onAnswerReceived(const QString &fromSessionId, const QString &sdp)
{
    setStatusDetail("Received answer");
    qDebug() << "CallManager: received answer from" << fromSessionId.left(20);

    if (m_useP2P && m_peerPipeline) {
        m_peerPipeline->setRemoteAnswer(sdp);
        qDebug() << "CallManager: set P2P remote answer";
        return;
    }

    // MCU answer to our publisher offer (from our own session ID)
    if (m_publishPipeline) {
        m_publishPipeline->setRemoteAnswer(sdp);
        qDebug() << "CallManager: set publisher remote answer";

        // If this answer includes video (renegotiation after enableCamera),
        // re-request all existing subscriber streams so MCU sends video too.
        // Don't tear down existing subscribers — just request new offers.
        // onOfferReceived handles re-offers for existing subscribers.
        // Check for active video line (port > 0; "m=video 0" means rejected)
        bool hasActiveVideo = sdp.contains(QRegularExpression("m=video [1-9]"));
        if (m_cameraOn && hasActiveVideo && !m_subscribePipelines.isEmpty()) {
            QStringList peerIds = m_subscribePipelines.keys();
            qDebug() << "CallManager: video renegotiation accepted, re-requesting"
                     << peerIds.size() << "subscriber stream(s)";
            for (const QString &peerId : peerIds) {
                m_signaling->requestOffer(peerId, "video");
                qDebug() << "CallManager: re-requested stream for" << peerId.left(20);
            }
        }
    }
}
