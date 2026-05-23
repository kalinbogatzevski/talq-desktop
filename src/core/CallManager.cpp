#include "core/CallManager.h"
#include "core/BackgroundEngine.h"
#include "core/TalqLog.h"
#include <QJsonObject>
#include <QDateTime>
#include <QtMath>
#include <QSet>
#include <QSettings>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QEventLoop>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

// NC Talk call flags (bitmask)
static constexpr int CALL_FLAG_IN_CALL    = 1;
static constexpr int CALL_FLAG_WITH_AUDIO = 2;
static constexpr int CALL_FLAG_WITH_VIDEO = 4;

static QJsonObject makeCandidateJson(const QString &candidate, int mline, const QString &mid)
{
    QJsonObject c;
    c["candidate"] = candidate;
    c["sdpMLineIndex"] = mline;
    c["sdpMid"] = mid;
    return c;
}

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

QVector<QPair<QString, QString>> CallManager::ringtones()
{
    // id -> label. "default" = the synthesized TalQ ring; classic/bright/
    // soft are the bundled CC0 ring WAVs at :/sounds/ring_<id>.wav. Order
    // here is the order shown in the Settings combo.
    return {
        { QStringLiteral("classic"), QStringLiteral("Classic bell") },
        { QStringLiteral("bright"),  QStringLiteral("Bright bell")  },
        { QStringLiteral("soft"),    QStringLiteral("Soft bell")    },
        { QStringLiteral("default"), QStringLiteral("TalQ tone")    },
        { QStringLiteral("none"),    QStringLiteral("None (silent)")},
    };
}

void CallManager::startRingtone() {
#ifdef Q_OS_WIN
    static QByteArray outgoing = generateOutgoingTone();
    static QByteArray incoming = generateIncomingRingtone();
    if (m_state == Incoming) {
        // Incoming ring is user-selectable (Calls/incomingRingtone). A
        // bundled CC0 ring loads from :/sounds/ring_<id>.wav; "default"
        // uses the synthesized tone; "none" stays silent. The outgoing
        // (calling…) tone is not customizable.
        QSettings s("TalQ", "TalQ");
        s.beginGroup("Calls");
        const QString id = s.value("incomingRingtone", "classic").toString();
        s.endGroup();
        if (id == "none") return;
        if (id != "default") {
            QFile f(QStringLiteral(":/sounds/ring_%1.wav").arg(id));
            if (f.open(QIODevice::ReadOnly)) {
                // Keep the buffer alive: SND_ASYNC reads from it after we return.
                m_ringtoneData = f.readAll();
                PlaySoundA(m_ringtoneData.constData(), nullptr,
                           SND_MEMORY | SND_ASYNC | SND_LOOP);
                return;
            }
            // fall through to the synthesized ring if the file is missing
        }
        PlaySoundA(incoming.constData(), nullptr, SND_MEMORY | SND_ASYNC | SND_LOOP);
        return;
    }
    PlaySoundA(outgoing.constData(), nullptr, SND_MEMORY | SND_ASYNC | SND_LOOP);
#endif
}

void CallManager::auditionRingtone(const QString &id)
{
#ifdef Q_OS_WIN
    // Static one-shot preview (no SND_LOOP). The buffer is function-static so
    // it stays alive for the async playback; UI-thread-only, so overwriting
    // it on the next audition is safe.
    static QByteArray buf;
    if (id == "none") return;
    if (id == "default") {
        buf = generateIncomingRingtone();
    } else {
        QFile f(QStringLiteral(":/sounds/ring_%1.wav").arg(id));
        if (!f.open(QIODevice::ReadOnly)) return;
        buf = f.readAll();
    }
    PlaySoundA(buf.constData(), nullptr, SND_MEMORY | SND_ASYNC);
#else
    Q_UNUSED(id);
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
    // #20 — long-lived BackgroundEngine. Constructed even when the
    // feature is Off so the publisher can ask for it later without a
    // null check; the engine itself is the gate (Mode::None → no-op).
    m_backgroundEngine = new BackgroundEngine(this);
    applyBackgroundSettings();

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
        if (auto *p = m_participants.value(sessionId)) {
            p->setAudioMuted(!(newFlags & CALL_FLAG_WITH_AUDIO));
            p->setVideoMuted(!(newFlags & CALL_FLAG_WITH_VIDEO));
        }
        if (!hadVideo && hasVideo) {
            qInfo() << "CallManager: peer" << sessionId.left(20)
                    << "enabled video, re-requesting stream";
            // MUST go through requestPeerStream, not a bare one-shot
            // requestOffer: when a peer toggles video on, the MCU often
            // hasn't registered their new publish yet and the server
            // rejects with "Not allowed to request offer." A single send
            // is then lost forever (the new video never streams). This
            // path resets the attempt budget and re-arms the 8s retry
            // timer; onOfferReceived rebuilds the subscriber for the new
            // session when the offer finally arrives.
            requestPeerStream(sessionId);
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
            requestPeerStream(sessionId);
            qDebug() << "CallManager: requestOffer for room peer" << sessionId.left(20);

            // Upstream: while a screen share is active the publisher must
            // sendoffer to every NEW joiner (peers can't discover an
            // ongoing screen share any other way — no flag/event for it).
            if (m_screenSharing && m_screenSharePipeline) {
                QJsonObject data;
                data["type"] = QString("sendoffer");
                data["roomType"] = QString("screen");
                m_signaling->sendMinimalMessage(sessionId, data);
                qDebug() << "CallManager: sent screen sendoffer to new peer"
                         << sessionId.left(20);
            }
        }
    });

    // Remote mute/unmute tracking
    connect(m_signaling, &SignalingClient::remoteMuteChanged,
            this, [this](const QString &sessionId, const QString &media, bool muted) {
        if (media == "video") { m_remoteVideoMuted = muted; emit remoteMediaChanged(); }
        if (media == "audio") { m_remoteAudioMuted = muted; emit remoteMediaChanged(); }
        if (auto *p = m_participants.value(sessionId)) {
            if (media == "video") p->setVideoMuted(muted);
            if (media == "audio") p->setAudioMuted(muted);
        }
        qDebug() << "CallManager: remote" << media << (muted ? "muted" : "unmuted");
    });

    // Remote screen share stopped
    connect(m_signaling, &SignalingClient::screenShareStopped,
            this, [this](const QString &sessionId) {
        qDebug() << "CallManager: remote screen share stopped from" << sessionId.left(20);
        // Disconnect video provider BEFORE deleting subscriber
        if (m_remoteScreenProvider) {
            m_remoteScreenProvider->disconnect();
            m_remoteScreenProvider = nullptr;
        }
        emit remoteScreenProviderChanged();
        if (m_screenSubscribers.contains(sessionId)) {
            m_screenSubscribers[sessionId]->stop();
            m_screenSubscribers[sessionId]->deleteLater();
            m_screenSubscribers.remove(sessionId);
        }
        if (auto *p = m_participants.value(sessionId)) {
            p->setScreen(nullptr);
            p->setScreenSharing(false);
        }
    });

    // Keep the self participant mirrored to our own media state.
    for (auto sig : { &CallManager::muteChanged, &CallManager::cameraChanged,
                      &CallManager::screenShareChanged,
                      &CallManager::localVideoProviderChanged })
        connect(this, sig, this, [this]{ syncSelfParticipant(); });

    // HPB WebSocket signaling messages
    connect(m_signaling, &SignalingClient::offerReceived,
            this, [this](const QString &from, const QString &sdp, const QString &sid, const QString &roomType) {
        if (roomType == "screen") {
            // Incoming screen share — create a subscriber for it
            qDebug() << "CallManager: received screen share offer from" << from.left(20);
            // A re-share (stop → share again) sends a fresh offer for a
            // session we may still hold a screen subscriber for. Feeding
            // the new offer into the OLD SubscribePipeline leaves its
            // already-negotiated decode wired to the dead stream — the
            // viewer is stuck on a frozen last frame of the previous
            // share. Same class as the camera re-attach bug: tear the
            // stale subscriber down and fall through to build a fresh one.
            if (auto *stale = m_screenSubscribers.take(from)) {
                qDebug() << "CallManager: screen re-offer for" << from.left(20)
                         << "— rebuilding screen subscriber (avoid frozen frame)";
                stale->stop();
                stale->deleteLater();
                if (m_remoteScreenProvider) {
                    m_remoteScreenProvider = nullptr;
                    emit remoteScreenProviderChanged();
                }
                if (auto *p = m_participants.value(from)) p->setScreen(nullptr);
            }
            auto *sub = new SubscribePipeline(from, this);
            connect(sub, &SubscribePipeline::localAnswerReady,
                    this, [this, from, sid](const QString &answerSdp) {
                m_signaling->sendAnswer(from, answerSdp, sid, {}, "screen");
            });
            connect(sub, &SubscribePipeline::iceCandidateReady,
                    this, [this, from, sid](const QString &candidate, int mline, const QString &mid) {
                QJsonObject c;
                c["candidate"] = candidate;
                c["sdpMLineIndex"] = mline;
                c["sdpMid"] = mid;
                m_signaling->sendCandidate(from, c, sid, "screen");
            });
            connect(sub, &SubscribePipeline::iceStateChanged,
                    this, [this](const QString &state) {
                qDebug() << "CallManager: screen subscriber ICE:" << state;
            });
            connect(sub, &SubscribePipeline::iceGatheringComplete,
                    this, [this, from, sid]() {
                m_signaling->sendEndOfCandidates(from, sid, "screen");
            });
            m_screenSubscribers[from] = sub;
            qDebug() << "CallManager: starting screen subscriber, STUN:" << m_stunServer;
            if (!sub->start(m_stunServer, m_turnServers)) {
                qWarning() << "CallManager: failed to start screen subscriber pipeline";
                m_screenSubscribers.remove(from);
                sub->deleteLater();
                return;
            }
            qDebug() << "CallManager: screen subscriber started, setting offer...";
            m_remoteScreenProvider = sub->videoProvider();
            emit remoteScreenProviderChanged();
            if (auto *p = ensureParticipant(from, {})) {
                p->setScreen(sub->videoProvider());
                p->setScreenSharing(true);
            }
            sub->setRemoteOffer(sdp);
            qDebug() << "CallManager: screen subscriber offer set";
            return;
        }
        onOfferReceived(from, sdp, sid);
    });
    connect(m_signaling, &SignalingClient::answerReceived,
            this, [this](const QString &from, const QString &sdp, const QString &roomType) {
        if (roomType == "screen" && m_screenSharePipeline) {
            m_screenSharePipeline->setRemoteAnswer(sdp);
            qDebug() << "CallManager: set screen share answer";
            return;
        }
        onAnswerReceived(from, sdp);
    });
    connect(m_signaling, &SignalingClient::candidateReceived,
            this, [this](const QString &fromSessionId, const QJsonObject &candidate, const QString &roomType) {
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

        // Route by roomType: screen candidates go to screen pipelines
        if (roomType == "screen") {
            if (m_screenSubscribers.contains(fromSessionId)) {
                m_screenSubscribers[fromSessionId]->addIceCandidate(cStr, mline, mid);
            } else if (m_screenSharePipeline) {
                m_screenSharePipeline->addIceCandidate(cStr, mline, mid);
            }
            return;
        }

        // Video candidates
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
        // Bypass userActionReady — timeout is the safety net for when UI never shows
        if (m_state == Incoming) teardown("Ring timeout");
        else if (m_state == Outgoing) teardown("No answer");
    });

    // Stats timer — update call info every 2 seconds
    m_statsTimer.setInterval(2000);
    connect(&m_statsTimer, &QTimer::timeout, this, &CallManager::updateCallStats);

    // Publisher ICE-failed grace timer. Fires only if publisher ICE was
    // still "failed" 8 s after it first failed (any "connected"/
    // "completed" in between stops it). This is the publisher-side
    // analogue of recoverSubscriber()'s "never suicide on a transient
    // blip" rule — a momentary network hiccup must not kill the call.
    m_pubIceGrace.setSingleShot(true);
    m_pubIceGrace.setInterval(8000);
    connect(&m_pubIceGrace, &QTimer::timeout, this, [this]() {
        if (m_state != Connecting && m_state != Active) return;
        qWarning() << "CallManager: publisher ICE still failed after grace "
                      "window — tearing down call";
        hangUp();
    });

    m_speakingGrace.setSingleShot(true);
    m_speakingGrace.setInterval(500);
    connect(&m_speakingGrace, &QTimer::timeout, this, [this]() {
        if (m_speaking && m_audioLevel <= 0.05) {
            m_speaking = false;
            if (m_publishPipeline && m_publishPipeline->isRunning())
                m_publishPipeline->sendStatusMessage(R"({"type":"stoppedSpeaking"})");
        }
    });

    // requestoffer retry — upstream resends ~every 8s until the MCU
    // delivers the offer (a single send races MCU room-creation and
    // leaves that peer silent). Stops once nothing is outstanding.
    m_requestOfferRetry.setInterval(8000);
    connect(&m_requestOfferRetry, &QTimer::timeout, this, [this]() {
        if (m_state != Connecting && m_state != Active) {
            m_pendingRequestOffers.clear();
            m_requestOfferAttempts.clear();
            m_requestOfferRetry.stop();
            return;
        }
        const auto sids = m_pendingRequestOffers;
        for (const QString &sid : sids) {
            if (m_subscribePipelines.contains(sid)) {
                m_pendingRequestOffers.remove(sid);
                m_requestOfferAttempts.remove(sid);
                continue;
            }
            int &n = m_requestOfferAttempts[sid];
            // 6 attempts (~48s) was too short: if the MCU keeps replying
            // "Not allowed to request offer" while a peer's publish or
            // media permission is still settling, giving up here loses
            // that peer's video for the WHOLE call (a likely cause of
            // permanent one-direction black). The subscribePipelines
            // check above already stops retrying the moment an offer
            // actually lands, so a generous cap only bounds the genuine
            // never-publishes case. ~8 min headroom.
            if (n >= 60) {
                qWarning() << "CallManager: requestoffer gave up for" << sid.left(20)
                           << "after" << n << "attempts";
                m_pendingRequestOffers.remove(sid);
                m_requestOfferAttempts.remove(sid);
                continue;
            }
            ++n;
            qInfo() << "CallManager: re-requesting offer from" << sid.left(20) << "attempt" << n;
            m_signaling->requestOffer(sid, "video");
        }
        if (m_pendingRequestOffers.isEmpty())
            m_requestOfferRetry.stop();
    });

    // Check GStreamer plugins on startup
    checkGStreamerPlugins();
}

void CallManager::checkGStreamerPlugins()
{
    static const char *requiredPlugins[] = {
        "coreelements", "audioconvert", "audioresample", "opus",
        "rtp", "rtpmanager", "srtp", "dtls", "nice", "webrtc",
        "wasapi2", "app", "autodetect", nullptr
    };

    QStringList missing;
    for (int i = 0; requiredPlugins[i]; ++i) {
        GstPlugin *plugin = gst_registry_find_plugin(gst_registry_get(), requiredPlugins[i]);
        if (!plugin) {
            missing << requiredPlugins[i];
        } else {
            gst_object_unref(plugin);
        }
    }

    if (!missing.isEmpty()) {
        m_callsAvailable = false;
        m_callsUnavailableReason = "Missing GStreamer plugins: " + missing.join(", ");
        qWarning() << "CallManager:" << m_callsUnavailableReason;
        qWarning() << "CallManager: copy plugins to gst-plugins/ directory next to talq.exe";
    } else {
        qDebug() << "CallManager: all required GStreamer plugins found";
    }

    // Detect GPU hardware acceleration
    GstElementFactory *nvvp8 = gst_element_factory_find("nvvp8dec");
    GstElementFactory *dxvaVp8 = gst_element_factory_find("d3d11vp8dec");
    GstElementFactory *dxvaH264 = gst_element_factory_find("d3d11h264dec");
    if (nvvp8) {
        m_gpuAccelStatus = "NVIDIA NVDEC";
        gst_object_unref(nvvp8);
    } else if (dxvaVp8) {
        m_gpuAccelStatus = "Intel DXVA";
        gst_object_unref(dxvaVp8);
    } else if (dxvaH264) {
        m_gpuAccelStatus = "DXVA (H264 only)";
        gst_object_unref(dxvaH264);
    } else {
        m_gpuAccelStatus = "Software only";
    }
    if (dxvaVp8 && !nvvp8) gst_object_unref(dxvaVp8);
    if (dxvaH264 && !nvvp8 && !dxvaVp8) gst_object_unref(dxvaH264);
    qDebug() << "CallManager: GPU accel:" << m_gpuAccelStatus;
}

QString CallManager::activeVideoCodec() const
{
    // Prefer the codec from an active subscriber (what we're decoding).
    for (auto *sub : m_subscribePipelines) {
        if (sub->isRunning() && !sub->videoCodec().isEmpty())
            return sub->videoCodec();
    }
    // Fall back to our own publish codec — the Janus room runs ONE codec
    // for everyone, so the codec we send is the call's codec even before a
    // subscriber feed exists (or if the subscriber didn't surface it). This
    // is why the telemetry CODEC row used to read "—" for the whole call.
    if (m_publishPipeline && m_publishPipeline->isRunning())
        return m_publishPipeline->usesH264() ? QStringLiteral("H264")
                                             : QStringLiteral("VP8");
    return {};
}

QString CallManager::activeVideoDecoder() const
{
    for (auto *sub : m_subscribePipelines) {
        if (sub->isRunning() && !sub->videoDecoder().isEmpty())
            return sub->videoDecoder();
    }
    return {};
}

QString CallManager::activeVideoEncoder() const
{
    if (m_screenSharing && m_screenSharePipeline)
        return m_screenSharePipeline->encoderDescription();
    if (m_publishPipeline && m_publishPipeline->isRunning() &&
        m_publishPipeline->isCameraOn())
        return m_publishPipeline->encoderDescription();
    return {};
}

bool CallManager::activeVideoEncoderIsHw() const
{
    // encoderDescription tags the backend, e.g. "H264 · nvh264enc · hw".
    return activeVideoEncoder().endsWith(QStringLiteral("hw"));
}

QString CallManager::activeRxResolution() const
{
    // Decoded resolution of the substream we're receiving — from the first
    // running subscriber with a known frame size. Reflects the active
    // simulcast layer (changes live as the SFU switches). Empty until the
    // first frame decodes.
    for (auto *sub : m_subscribePipelines) {
        if (!sub->isRunning()) continue;
        const int w = sub->rxWidth(), h = sub->rxHeight();
        if (w > 0 && h > 0)
            return QStringLiteral("%1×%2").arg(w).arg(h);
    }
    return {};
}

QString CallManager::videoTxLabel() const
{
    if (m_screenSharing && m_screenSharePipeline)
        return QStringLiteral("screen share");
    if (m_publishPipeline && m_publishPipeline->isRunning() &&
        m_publishPipeline->isCameraOn()) {
        const double mbps = m_publishPipeline->currentVideoBitrate() / 1e6;
        // Camera publish is pinned to 1280×720 (native-client target).
        return QStringLiteral("1280×720 · %1 Mbps").arg(mbps, 0, 'f', 1);
    }
    return {};
}

double CallManager::txBitrateMbps() const
{
    // Numeric counterpart of streamBandwidthLabel (outbound rate we
    // control/measure) for the telemetry bandwidth gauge/sparkline.
    double bps = 0.0;
    if (m_screenSharing && m_screenSharePipeline)
        bps += 2.5e6;
    if (m_publishPipeline && m_publishPipeline->isRunning()) {
        if (m_publishPipeline->isCameraOn())
            bps += m_publishPipeline->currentVideoBitrate();
        bps += 40000.0;
    }
    return bps / 1e6;
}

QString CallManager::streamBandwidthLabel() const
{
    // Honest: this is the OUTBOUND rate we control/measure. Video = the
    // GCC-applied send bitrate (real, congestion-controlled), audio = the
    // fixed Opus rate (~40 kbps). RX has no per-stream accessor on the MCU
    // subscribers, so we don't fabricate one.
    double bps = 0.0;
    if (m_screenSharing && m_screenSharePipeline)
        bps += 2.5e6;  // screen-share target (no live accessor); indicative
    if (m_publishPipeline && m_publishPipeline->isRunning()) {
        if (m_publishPipeline->isCameraOn())
            bps += m_publishPipeline->currentVideoBitrate();
        bps += 40000.0;  // Opus
    }
    if (bps <= 0.0) return {};
    return bps >= 1e6
        ? QStringLiteral("↑ %1 Mbps").arg(bps / 1e6, 0, 'f', 2)
        : QStringLiteral("↑ %1 kbps").arg(bps / 1e3, 0, 'f', 0);
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
        SubscribeWebrtcSrc *s = it.value();
        const bool run = s && s->isRunning();
        QString line = "  " + it.key().left(12) + "... "
                     + (run ? QStringLiteral("active") : QStringLiteral("stopped"));
        if (run) {
            // Live receive stats for this peer. RX fps≈1 with ptsΔ≈1000 ms
            // ⇒ peer encoding ~1 fps; ptsΔ≈33 ms with low fps ⇒ frames
            // dropped on receive. The resolution comes from the last
            // actually-decoded frame so it reflects what's being painted,
            // not what was negotiated.
            QString res = QStringLiteral("?×?");
            if (auto *p = s->videoProvider()) {
                QSize sz = p->lastFrameSize();
                if (!sz.isEmpty())
                    res = QString::number(sz.width()) + QChar(0x00D7)
                        + QString::number(sz.height());
            }
            line += QStringLiteral("  RX %1 fps %2 (%3 distinct, ptsΔ %4 ms)")
                        .arg(s->rxVideoFps())
                        .arg(res)
                        .arg(s->rxDistinctVideoFps())
                        .arg(s->rxPtsGapMs());
        }
        lines << line;
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
    qInfo() << "CallManager: state ->" << newState;
    if (newState == Outgoing || newState == Incoming) startRingtone();
    else stopRingtone();
    if (newState == Active) {
        updateCallStats();
        m_statsTimer.start();
        // Late fallback only: the camera is now enabled immediately when the
        // publish pipeline starts (see joinCallOnServer). This re-checks in
        // case it wasn't on yet; isCameraOn() prevents a double-enable.
        if (m_cameraOn && m_publishPipeline && !m_publishPipeline->isCameraOn()) {
            m_publishPipeline->enableCamera(videoDeviceIndex(), preferHd1080());
            m_localVideoProvider = m_publishPipeline->localVideoProvider();
            emit localVideoProviderChanged();
        }
        // Broadcast initial media state so remote peers show correct mute/video status
        broadcastMediaState("audio", !m_muted);
        broadcastMediaState("video", m_cameraOn);
    } else {
        m_statsTimer.stop();
    }
    if (newState == Idle)
        clearParticipants();
    else if (!m_selfParticipant)
        ensureSelfParticipant();
    emit stateChanged();
}

void CallManager::startCall(const QString &token, bool withVideo)
{
    if (m_state != Idle) return;
    TLOG_CALL("startCall token=" << token << "withVideo=" << withVideo
              << "sigRoom=" << m_signaling->currentRoom()
              << "sigSession=" << m_signaling->sessionId().left(20));
    m_callToken = token;
    m_withVideo = withVideo;
    m_cameraOn = withVideo;
    emit cameraChanged();
    m_muted = false;
    m_callDuration = 0;
    setState(Outgoing);
    setStatusDetail("Joining room");
    m_ringTimeout.start();

    // Join call — but only once the signaling room is actually joined.
    // Posting POST call/{token} before the HPB hello+room handshake
    // completes loses the participant/offer events and yields a silent
    // call. (The incoming path already did this; now both share it.)
    setStatusDetail("Joining call");
    ensureSignalingRoomJoined([this, withVideo]() {
        if (m_state != Outgoing && m_state != Connecting) return;
        joinCallOnServer(withVideo);
    });
}

void CallManager::ensureSignalingRoomJoined(std::function<void()> next)
{
    // Already in the right signaling room and authenticated → proceed.
    if (m_signaling->isConnected()
        && m_signaling->currentRoom() == m_callToken
        && !m_signaling->sessionId().isEmpty()) {
        next();
        return;
    }
    auto fired = std::make_shared<bool>(false);
    auto conn  = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_signaling, &SignalingClient::roomJoined, this,
        [this, next, conn, fired]() {
            if (*fired) return;
            *fired = true;
            QObject::disconnect(*conn);
            qDebug() << "CallManager: signaling room joined, proceeding";
            next();
        });
    QTimer::singleShot(15000, this, [conn, fired]() {
        if (!*fired) {
            QObject::disconnect(*conn);
            qWarning() << "CallManager: signaling roomJoined timeout";
        }
    });
    // joinRoom() POSTs participants/active (yielding the NC sessionId) and
    // then sends the WS `room` message.
    m_signaling->joinRoom(m_callToken);
}

void CallManager::requestPeerStream(const QString &sessionId)
{
    if (sessionId.isEmpty() || sessionId == m_signaling->sessionId()) return;
    if (m_subscribePipelines.contains(sessionId)) return;   // already subscribed
    // Asymmetric-chop fix: the CALLER reaches this twice for the same
    // peer (once when its publisher comes up + the remote is already
    // joined, again on participantJoinedCall / roomPeerJoined). Without
    // this guard, two requestOffer messages go to the MCU; the second
    // offer triggers the stale-subscriber rebuild path in
    // onOfferReceived, which costs a frame-loss spike → chop. The
    // callee never double-requests (it gets one event for the existing
    // caller). Skipping in-flight duplicates makes both directions
    // equivalent. Retries still happen via m_requestOfferRetry, which
    // calls signaling->requestOffer directly (not this function).
    if (m_pendingRequestOffers.contains(sessionId)) return;
    m_pendingRequestOffers.insert(sessionId);
    m_requestOfferAttempts[sessionId] = 0;
    m_signaling->requestOffer(sessionId, "video");
    if (!m_requestOfferRetry.isActive())
        m_requestOfferRetry.start();
}

// A subscriber feed died mid-call: the SFU sent end-session, or its
// webrtcbin ICE went "failed". On this HPB both happen routinely when the
// publisher renegotiates (peer toggles camera, server recycles the
// publish session). The old behaviour — hangUp() the WHOLE call — made
// TalQ commit suicide ~9 min into calls. Instead: tear down ONLY this
// peer's subscriber and re-subscribe to its current feed. Audio, our own
// media, the publisher, and every other peer stay up. Bounded so a feed
// that flaps forever can't spin the retry machinery indefinitely; the
// budget resets whenever the subscriber successfully (re)connects. This
// function NEVER ends the call — only a publisher failure / user hangup
// does that.
void CallManager::recoverSubscriber(const QString &sessionId, const QString &reason)
{
    if (sessionId.isEmpty()) return;
    if (m_state != Connecting && m_state != Active) return;  // teardown owns cleanup

    const bool hadSub = m_subscribePipelines.contains(sessionId);
    if (!hadSub && m_pendingRequestOffers.contains(sessionId))
        return;  // already recovering (end-session + ICE-failed both fired for one death)

    const int n = ++m_subscriberRecoveries[sessionId];

    VideoFrameProvider *deadProv = nullptr;
    if (auto *dead = m_subscribePipelines.take(sessionId)) {
        deadProv = dead->videoProvider();
        dead->stop();
        dead->deleteLater();   // we may be inside this sub's own queued signal
    }
    m_subscriberSids.remove(sessionId);
    m_pendingRequestOffers.remove(sessionId);
    m_requestOfferAttempts.remove(sessionId);

    // Unbind the now-dangling provider so the UI stops painting a dead
    // feed; a fresh SubscribeWebrtcSrc + provider is built on the re-offer.
    if (m_remoteVideoProvider && m_remoteVideoProvider == deadProv) {
        m_remoteVideoProvider = nullptr;
        emit remoteVideoProviderChanged();
    }
    if (auto *p = m_participants.value(sessionId)) {
        p->setCamera(nullptr);
        p->setConnState(CallParticipant::Reconnecting);
    }

    constexpr int kMaxSubRecoveries = 8;
    if (n > kMaxSubRecoveries) {
        qWarning() << "CallManager: subscriber" << sessionId.left(20)
                   << "unrecoverable after" << n << "attempts (" << reason
                   << ") — peer video stays down, call continues";
        setStatusDetail("Peer video unavailable");
        if (auto *p = m_participants.value(sessionId))
            p->setConnState(CallParticipant::Failed);
        return;  // never hangUp() from a subscriber problem
    }

    qInfo() << "CallManager: recovering subscriber for" << sessionId.left(20)
            << "(" << reason << ") attempt" << n << "— re-subscribing";
    setStatusDetail("Reconnecting peer video…");
    requestPeerStream(sessionId);
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
    // #13: pre-answer self-preview for video calls. Camera turns on while
    // the call rings so the callee can check framing before answering.
    if (m_withVideo) startIncomingCameraPreview();
}

// #13 — Pre-answer self-preview pipeline (standalone, not coupled to
// PublishPipeline). Starts on an incoming VIDEO call so the callee can
// frame themselves before answering; releases the camera SYNCHRONOUSLY on
// accept/decline/timeout/teardown so the publish pipeline can grab the
// device cleanly (Windows MF only lets one source own a camera).
void CallManager::startIncomingCameraPreview()
{
    if (m_previewPipeline) return;             // already running
    if (!m_withVideo) return;                  // video calls only
    const int deviceIndex = videoDeviceIndex();

    // Mirrors PublishPipeline's source choice (mfvideosrc → ksvideosrc →
    // autovideosrc). No forced-exact source caps (per memory
    // project_talq_camera_fps_rca: never default to exact mfvideosrc caps).
    GstElement *pipe = gst_pipeline_new("incoming-preview");
    GstElement *src  = gst_element_factory_make("mfvideosrc", nullptr);
    if (!src) src = gst_element_factory_make("ksvideosrc", nullptr);
    if (!src) src = gst_element_factory_make("autovideosrc", nullptr);
    GstElement *dec  = gst_element_factory_make("decodebin", "preview-decode");
    GstElement *conv = gst_element_factory_make("videoconvert", nullptr);
    GstElement *sink = gst_element_factory_make("appsink", "preview-sink");
    if (!pipe || !src || !dec || !conv || !sink) {
        qWarning() << "CallManager: incoming preview — element creation failed";
        for (GstElement *e : { pipe, src, dec, conv, sink })
            if (e && !GST_OBJECT_PARENT(e)) gst_object_unref(e);
        return;
    }
    g_object_set(src, "device-index", deviceIndex, nullptr);

    // appsink: same shape as PublishPipeline's preview (BGRx, drop, depth 1).
    {
        GstCaps *caps = gst_caps_from_string("video/x-raw,format=BGRx");
        g_object_set(sink, "emit-signals", TRUE, "caps", caps,
                     "drop", TRUE, "max-buffers", 1, nullptr);
        gst_caps_unref(caps);
    }
    gst_bin_add_many(GST_BIN(pipe), src, dec, conv, sink, nullptr);
    if (!gst_element_link(src, dec) || !gst_element_link(conv, sink)) {
        qWarning() << "CallManager: incoming preview — static linking failed";
        gst_element_set_state(pipe, GST_STATE_NULL);
        gst_object_unref(pipe);
        return;
    }
    // decodebin's src pad appears dynamically — connect to videoconvert.
    g_signal_connect(dec, "pad-added",
        G_CALLBACK(+[](GstElement *, GstPad *p, gpointer ud) {
            GstPad *cs = gst_element_get_static_pad(GST_ELEMENT(ud), "sink");
            if (cs && !gst_pad_is_linked(cs)) gst_pad_link(p, cs);
            if (cs) gst_object_unref(cs);
        }), conv);

    m_previewPipeline = pipe;
    m_previewAppsink  = sink;
    m_previewProvider = new VideoFrameProvider(this);
    g_signal_connect(sink, "new-sample",
                     G_CALLBACK(&CallManager::onPreviewSample), this);

    if (gst_element_set_state(pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        qWarning() << "CallManager: incoming preview — pipeline failed to start";
        stopIncomingCameraPreview();
        return;
    }
    if (m_selfParticipant) m_selfParticipant->setCamera(m_previewProvider);
    m_localVideoProvider = m_previewProvider;
    emit localVideoProviderChanged();
    qInfo() << "CallManager: incoming-call self-preview started (camera"
            << deviceIndex << ")";
}

void CallManager::stopIncomingCameraPreview()
{
    if (!m_previewPipeline) return;
    // Detach the participant's camera FIRST so the UI stops requesting frames.
    if (m_selfParticipant) m_selfParticipant->setCamera(nullptr);
    if (m_localVideoProvider == m_previewProvider) {
        m_localVideoProvider = nullptr;
        emit localVideoProviderChanged();
    }
    if (m_previewAppsink)
        g_signal_handlers_disconnect_by_data(m_previewAppsink, this);
    // SYNCHRONOUS teardown: set state NULL blocks until the state change
    // completes, guaranteeing mfvideosrc has released the camera before we
    // return — so a subsequent enableCamera on the publish pipeline can
    // open the device. THIS IS WHY THE TEARDOWN IS A BLOCKING CALL.
    gst_element_set_state(m_previewPipeline, GST_STATE_NULL);
    gst_object_unref(m_previewPipeline);
    m_previewPipeline = nullptr;
    m_previewAppsink  = nullptr;
    if (m_previewProvider) {
        m_previewProvider->deleteLater();
        m_previewProvider = nullptr;
    }
    qInfo() << "CallManager: incoming-call self-preview stopped";
}

GstFlowReturn CallManager::onPreviewSample(GstAppSink *sink, gpointer userData)
{
    auto *self = static_cast<CallManager *>(userData);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    QPointer<CallManager> guard(self);
    QMetaObject::invokeMethod(self, [guard, sample]() {
        if (guard && guard->m_previewProvider)
            guard->m_previewProvider->feedFrame(sample);
        gst_sample_unref(sample);
    }, Qt::QueuedConnection);
    return GST_FLOW_OK;
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
    // #13: release the camera BEFORE anything else — the publish pipeline
    // will grab it via enableCamera below, and Windows Media Foundation
    // only lets one source open a camera at a time. Synchronous teardown
    // (set state NULL waits for state-change) guarantees the device is
    // free by the time we return here.
    stopIncomingCameraPreview();
    m_withVideo = withVideo; m_cameraOn = withVideo; m_muted = false; m_callDuration = 0;
    emit cameraChanged();
    m_ringTimeout.stop();
    setStatusDetail("Joining room");
    setState(Connecting);

    // Same ordering as the outgoing path: signaling room first, then the
    // call API. ensureSignalingRoomJoined() handles participants/active
    // (NC sessionId) + the WS room join + the await.
    ensureSignalingRoomJoined([this, withVideo]() {
        // Guard: call may have been hung up while waiting for room join.
        if (m_state != Incoming && m_state != Connecting) return;
        joinCallOnServer(withVideo);
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
    stopIncomingCameraPreview();   // #13: release the camera
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

    // Stop speaking broadcast immediately on mute
    if (m_muted && m_speaking) {
        m_speakingGrace.stop();
        m_speaking = false;
        if (m_publishPipeline && m_publishPipeline->isRunning())
            m_publishPipeline->sendStatusMessage(R"({"type":"stoppedSpeaking"})");
    }

    // Broadcast mute/unmute state to peers via signaling (NC Talk compatibility)
    broadcastMediaState("audio", !m_muted);

    // Upstream keeps the in-call flags in sync with mic state (clears the
    // WITH_AUDIO bit on mute) so the participant list / other clients show
    // the correct mic status.
    updateCallFlags();
}

void CallManager::toggleCamera() {
    m_cameraOn = !m_cameraOn;

    // Do the actual swap BEFORE emitting signals
    if (m_useP2P && m_peerPipeline) {
        m_cameraOn ? m_peerPipeline->enableCamera(videoDeviceIndex(), preferHd1080())
                   : m_peerPipeline->disableCamera();
    } else if (m_publishPipeline) {
        if (m_cameraOn) {
            m_publishPipeline->enableCamera(videoDeviceIndex(), preferHd1080());
            m_localVideoProvider = m_publishPipeline->localVideoProvider();
        } else {
            m_publishPipeline->disableCamera();
            m_localVideoProvider = nullptr;
        }
        emit localVideoProviderChanged();
    }
    emit cameraChanged();

    // Broadcast video state + update call flags on server
    broadcastMediaState("video", m_cameraOn);
    updateCallFlags();
}

void CallManager::startScreenShare(int monitorIndex, quintptr windowHandle)
{
    if (m_state != Active && m_state != Connecting) return;
    if (m_screenSharing) return;

    // Remember target + quality so setScreenShareQuality() can re-share
    // the SAME screen/window at a new cap without re-prompting.
    m_ssMonitorIndex = monitorIndex;
    m_ssWindowHandle = windowHandle;
    m_ssQuality = QSettings("TalQ", "TalQ")
                      .value("Video/screenShareQuality", 1).toInt();

    m_screenSharing = true;
    m_screenSharePipeline = new ScreenSharePipeline(this);
    {
        // Level → pre-encode downscale cap (set before start(), which the
        // pipeline reads once). Native uses an 8K cap so the scaleCaps
        // range passes the real resolution through untouched — the user
        // opted in; #119's 4K+ RAM caveat applies there.
        int cw = 1920, ch = 1080;
        switch (m_ssQuality) {
            case 0: cw = 1280; ch = 720;  break;
            case 1: cw = 1920; ch = 1080; break;
            case 2: cw = 2560; ch = 1440; break;
            case 3: cw = 7680; ch = 4320; break;
            default: break;
        }
        m_screenSharePipeline->setQualityCap(cw, ch);
    }

    connect(m_screenSharePipeline, &ScreenSharePipeline::localOfferReady,
            this, [this](const QString &sdp) {
        m_screenShareSid = QString::number(qHash(sdp)).left(10);
        // Screen publisher offer carries broadcaster = own session id
        // (upstream Peer.send) so Janus can associate the screen stream.
        m_signaling->sendOffer(m_signaling->sessionId(), sdp, m_screenShareSid,
                               {}, "screen", m_signaling->sessionId());
        qDebug() << "CallManager: sent screen share offer, sid=" << m_screenShareSid;
    });

    connect(m_screenSharePipeline, &ScreenSharePipeline::iceCandidateReady,
            this, [this](const QString &candidate, int mline, const QString &mid) {
        QJsonObject c;
        c["candidate"] = candidate;
        c["sdpMLineIndex"] = mline;
        c["sdpMid"] = mid;
        m_signaling->sendCandidate(m_signaling->sessionId(), c, m_screenShareSid, "screen");
    });

    connect(m_screenSharePipeline, &ScreenSharePipeline::iceGatheringComplete,
            this, [this]() {
        m_signaling->sendEndOfCandidates(m_signaling->sessionId(), m_screenShareSid, "screen");
    });

    connect(m_screenSharePipeline, &ScreenSharePipeline::iceStateChanged,
            this, [this](const QString &state) {
        // qInfo so the screen-share start sequence is visible in the
        // production field log without verbose mode (#134 diag).
        qInfo() << "CallManager: screen share ICE:" << state;
        if (state == "connected") {
            // Send sendoffer once — HPB creates subscriber for each peer
            const auto peers = m_subscribePipelines.keys();
            qInfo() << "CallManager: screen pub connected — dispatching "
                       "sendoffer to" << peers.size() << "peer(s)";
            for (const QString &peerId : peers) {
                QJsonObject data;
                data["type"] = QString("sendoffer");
                data["roomType"] = QString("screen");
                m_signaling->sendMinimalMessage(peerId, data);
                qInfo() << "CallManager: sent sendoffer screen to" << peerId.left(20);
            }
        }
        if (state == "failed") {
            qWarning() << "CallManager: screen share ICE failed";
            stopScreenShare();
        }
    });

    connect(m_screenSharePipeline, &ScreenSharePipeline::error,
            this, [this](const QString &msg) {
        qWarning() << "CallManager: screen share error:" << msg;
        stopScreenShare();
    });

    connect(&m_glibTimer, &QTimer::timeout, m_screenSharePipeline, &ScreenSharePipeline::pollBus);

    if (!m_screenSharePipeline->start(m_stunServer, m_turnServers, monitorIndex, windowHandle)) {
        qWarning() << "CallManager: failed to start screen share pipeline";
        m_screenSharing = false;
        m_screenSharePipeline->deleteLater();
        m_screenSharePipeline = nullptr;
    }

    emit screenShareChanged();
}

void CallManager::requestPeerVideoQuality(const QString &sessionId, int substream)
{
    if (substream < 0 || substream > 2) return;
    if (m_desiredSubstream.value(sessionId, -1) == substream) return;  // dedupe
    m_desiredSubstream[sessionId] = substream;
    // Only send now if we actually have a live subscriber for this peer;
    // otherwise the value is stored and sent when the subscriber connects.
    if (m_subscribePipelines.contains(sessionId)
        && m_subscriberSids.contains(sessionId)) {
        m_signaling->sendSelectStream(sessionId,
                                      m_subscriberSids.value(sessionId), substream);
    }
}

void CallManager::stopScreenShare()
{
    if (!m_screenSharing) return;

    // Mark the screen-share teardown window so the publisher-ICE-failed
    // handler short-circuits its recovery counter / hangUp during this
    // span. The screen pipeline's webrtcbin teardown can transiently
    // perturb the shared signaling agent (50-iteration GLib flush
    // below), and any spurious "failed" edge on the audio/video
    // publisher must NOT drop the whole call — only the main audio
    // stream truly failing should do that. Cleared at end of function.
    m_screenShareTearingDown = true;

    if (m_screenSharePipeline) {
        m_screenSharePipeline->stop();
        m_screenSharePipeline->deleteLater();
        m_screenSharePipeline = nullptr;
    }
    m_screenSharing = false;
    // Defensive: clear the per-share sid so a subsequent re-share starts
    // from a fresh signaling identity (any straggler messages bound to
    // the old sid get dropped instead of clobbering the new session).
    m_screenShareSid.clear();
    // Flush pending GStreamer/GLib callbacks (libnice agents, DTLS timers,
    // bus messages) from the just-deleteLater'd pipeline before a possible
    // immediate re-share creates a new webrtcbin. Same pattern as
    // stopAllPipelines uses for the camera publish path — prevents
    // stale-callback ↔ fresh-resource collisions in an enable→disable→
    // enable cycle.
    for (int i = 0; i < 50; ++i) g_main_context_iteration(nullptr, FALSE);

    // Send unshareScreen as room message (browser compatibility) AND
    // to ourselves (triggers HPB to close the screen publisher in Janus).
    QJsonObject data;
    data["roomType"] = QString("screen");
    data["type"] = QString("unshareScreen");
    m_signaling->sendBroadcastMessage(data);
    // Also send to own session — HPB only cleans up publisher when recipient == self
    m_signaling->sendMinimalMessage(m_signaling->sessionId(), data);
    qInfo() << "CallManager: stopped screen sharing";

    m_screenShareTearingDown = false;
    emit screenShareChanged();
}

void CallManager::setScreenShareQuality(int level)
{
    if (level < 0 || level > 3) return;
    m_ssQuality = level;
    QSettings("TalQ", "TalQ").setValue("Video/screenShareQuality", level);
    emit screenShareQualityChanged();
    if (!m_screenSharing) return;   // applied on next share

    // Live change: quick managed re-share at the new cap, SAME target.
    // stop+start reuses all the existing signaling/ICE/offer wiring;
    // peers re-receive the screen at the new resolution after a brief
    // (~sub-second) blip. startScreenShare() re-reads the persisted
    // quality and applies the new downscale cap.
    const int mi = m_ssMonitorIndex;
    const quintptr wh = m_ssWindowHandle;
    qInfo() << "CallManager: screen-share quality ->" << level
            << "(re-sharing same target)";
    stopScreenShare();
    startScreenShare(mi, wh);
}

int CallManager::videoDeviceIndex() const
{
    return m_deviceManager ? qMax(0, m_deviceManager->selectedVideoInput()) : 0;
}

bool CallManager::preferHd1080() const
{
    return QSettings().value("video/resolution", 0).toInt() == 0;
}

void CallManager::applyBackgroundSettings()
{
    if (!m_backgroundEngine) return;

    QSettings s("TalQ", "TalQ");
    s.beginGroup("Talk/Backgrounds");
    const bool enabled = s.value("virtualBackgroundEnabled", false).toBool();
    const QString type = s.value("virtualBackgroundType", "blur").toString();
    const int strength = s.value("virtualBackgroundBlurStrength", 10).toInt();
    const QString url  = s.value("virtualBackgroundUrl", QString()).toString();
    s.endGroup();

    BackgroundEngine::Mode mode = BackgroundEngine::Mode::None;
    if (enabled) {
        if (type == QStringLiteral("image")) mode = BackgroundEngine::Mode::Image;
        else                                  mode = BackgroundEngine::Mode::Blur;
    }

    m_backgroundEngine->setMode(mode);
    m_backgroundEngine->setBlurStrength(strength);
    m_backgroundEngine->setImagePath(url);
    qInfo() << "CallManager: background mode applied —"
            << (mode == BackgroundEngine::Mode::None  ? "Off"
              : mode == BackgroundEngine::Mode::Blur  ? "Blur"
              : mode == BackgroundEngine::Mode::Image ? "Image" : "?")
            << "strength=" << strength
            << "url=" << QFileInfo(url).fileName();
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

    // Send via data channel (matches browser Talk protocol)
    if (m_publishPipeline && m_publishPipeline->isRunning()) {
        QByteArray dcType;
        if (media == "audio") dcType = enabled ? R"({"type":"audioOn"})" : R"({"type":"audioOff"})";
        else if (media == "video") dcType = enabled ? R"({"type":"videoOn"})" : R"({"type":"videoOff"})";
        if (!dcType.isEmpty())
            m_publishPipeline->sendStatusMessage(dcType);
    }
}

static int callFlags(bool withVideo, bool withAudio)
{
    int flags = CALL_FLAG_IN_CALL;
    if (withAudio) flags |= CALL_FLAG_WITH_AUDIO;
    if (withVideo) flags |= CALL_FLAG_WITH_VIDEO;
    return flags;
}

void CallManager::updateCallFlags()
{
    if (m_callToken.isEmpty() || (m_state != Connecting && m_state != Active))
        return;

    QJsonObject body;
    body["flags"] = callFlags(m_cameraOn, !m_muted);
    m_api->put("apps/spreed/api/v4/call/" + m_callToken, body,
        [](bool ok, const QJsonObject &, int statusCode) {
            if (!ok) qWarning() << "CallManager: failed to update call flags, status=" << statusCode;
        });
}

void CallManager::joinCallOnServer(bool withVideo)
{
    QJsonObject body;
    body["flags"] = callFlags(withVideo, !m_muted);
    // Match the official client's POST call/{token} parameter shape.
    body["silent"] = false;            // ring participants normally
    body["recordingConsent"] = false;  // no consent UI; server enforces only if required
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
                    // Use the server's configured STUN order (what the
                    // official client does); only fall back to the public
                    // default if the server provided none.
                    m_stunServer.clear();
                    if (ok2) {
                        const auto stunArr = settings["stunservers"].toArray();
                        for (const auto &s : stunArr) {
                            const auto urls = s.toObject()["urls"].toArray();
                            if (!urls.isEmpty()) {
                                m_stunServer = urls.first().toString();
                                break;
                            }
                        }
                    }
                    if (m_stunServer.isEmpty())
                        m_stunServer = "stun:stun.nextcloud.com:443";
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

                    // Process any offers that arrived before ICE servers were available
                    processPendingOffers();

                    // Use MCU when HPB has it, P2P otherwise
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
                            m_signaling->sendCandidate(m_remoteSessionId, makeCandidateJson(candidate, mline, mid), p2pSid);
                        });

                        connect(m_peerPipeline, &PeerPipeline::iceGatheringComplete,
                                this, [this, p2pSid]() {
                            m_signaling->sendEndOfCandidates(m_remoteSessionId, p2pSid);
                        });

                        connect(m_peerPipeline, &PeerPipeline::iceStateChanged,
                                this, [this](const QString &state) {
                            qDebug() << "CallManager: P2P ICE:" << state;
                            setStatusDetail("ICE " + state);
                            if (state == "connected") {
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
                                // Skip the server join POST if already joined (avoid double-join)
                                if (m_joinedCall) {
                                    // Already joined on server; just create MCU pipelines.
                                    // Re-fetch ICE servers and create publisher directly.
                                    setStatusDetail("Falling back to MCU");
                                }
                                joinCallOnServer(m_withVideo);
                            } else if (state == "failed") {
                                qWarning() << "CallManager: P2P ICE failed, no MCU available, tearing down";
                                hangUp();
                            }
                        });

                        connect(m_peerPipeline, &PeerPipeline::audioLevelUpdated,
                                this, &CallManager::onAudioLevelUpdated);

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
                        qDebug() << "CallManager: creating PublishPipeline...";
                        m_publishPipeline = new PublishPipeline(this);
                        qDebug() << "CallManager: PublishPipeline created, connecting signals...";
                        m_localVideoProvider = m_publishPipeline->localVideoProvider();
                        emit localVideoProviderChanged();

                        connect(m_publishPipeline, &PublishPipeline::localOfferReady,
                                this, [this, pubSid](const QString &sdp) {
                            // Send offer to OUR OWN session ID (MCU intercepts).
                            // (PublishPipeline already logs the compact
                            // a=simulcast line for field diag.)
                            setStatusDetail("Sending offer to MCU");
                            m_signaling->sendOffer(m_signaling->sessionId(), sdp, pubSid);
                            qDebug() << "CallManager: sent publish offer to own session, sid=" << pubSid;
                        });

                        // Self-heal: a forced exact camera caps that
                        // mfvideosrc can't deliver triggers no frames →
                        // PublishPipeline already reset the pick to Auto;
                        // we just re-arm the camera so it comes up via
                        // permissive negotiation (guaranteed-working).
                        connect(m_publishPipeline, &PublishPipeline::cameraNegotiationFailed,
                                this, [this]() {
                            qWarning() << "CallManager: camera negotiation failed — "
                                          "re-arming via Auto/permissive";
                            if (!m_publishPipeline) return;
                            m_publishPipeline->disableCamera();
                            QTimer::singleShot(150, this, [this]() {
                                if (!m_publishPipeline) return;
                                m_publishPipeline->enableCamera(
                                    videoDeviceIndex(), preferHd1080());
                            });
                        });

                        connect(m_publishPipeline, &PublishPipeline::iceCandidateReady,
                                this, [this, pubSid](const QString &candidate, int mline, const QString &mid) {
                            m_signaling->sendCandidate(m_signaling->sessionId(), makeCandidateJson(candidate, mline, mid), pubSid);
                        });

                        connect(m_publishPipeline, &PublishPipeline::iceGatheringComplete,
                                this, [this, pubSid]() {
                            // Upstream terminates trickle with endOfCandidates
                            // (publisher candidates go to our own session).
                            m_signaling->sendEndOfCandidates(m_signaling->sessionId(), pubSid);
                        });

                        connect(m_publishPipeline, &PublishPipeline::iceStateChanged,
                                this, [this](const QString &state) {
                            qDebug() << "CallManager: publisher ICE:" << state;
                            if (m_state != Active)
                                setStatusDetail("Publisher ICE " + state);
                            if (state == "connected" || state == "completed") {
                                // Upstream conformance: the call is "live"
                                // the moment the publisher PC is up — peer
                                // video subscribers come and go independently
                                // and shouldn't gate the top-level status.
                                // Without this, the call stays pinned on
                                // "Connecting" until a peer enables their
                                // camera (callee-mid-call scenario), even
                                // though audio is flowing both ways.
                                if (m_state == Connecting) {
                                    setState(Active);
                                    m_durationTimer.start();
                                }
                                if (m_pubIceGrace.isActive()) {
                                    qInfo() << "CallManager: publisher ICE "
                                               "recovered (" << state
                                            << ") — call stays up";
                                    m_pubIceGrace.stop();
                                    setStatusDetail("Connected");
                                }
                                m_pubIceRecoveries = 0;
                            } else if (state == "failed") {
                                // Suppress recovery-counter bumps when the
                                // failed edge is collateral from a screen-
                                // share teardown — a non-main-stream failure
                                // must never drop the call (#138).
                                if (m_screenShareTearingDown) {
                                    qInfo() << "CallManager: publisher ICE "
                                               "transient 'failed' during "
                                               "screen-share teardown — "
                                               "ignored (call stays up)";
                                    return;
                                }
                                constexpr int kMaxPubRecoveries = 6;
                                if (++m_pubIceRecoveries > kMaxPubRecoveries) {
                                    qWarning() << "CallManager: publisher ICE "
                                                  "failed" << m_pubIceRecoveries
                                               << "times — tearing down call";
                                    hangUp();
                                } else if (!m_pubIceGrace.isActive()) {
                                    qWarning() << "CallManager: publisher ICE "
                                                  "failed — grace window, "
                                                  "awaiting self-heal (attempt"
                                               << m_pubIceRecoveries << ")";
                                    setStatusDetail("Reconnecting…");
                                    m_pubIceGrace.start();
                                }
                            }
                        });

                        connect(m_publishPipeline, &PublishPipeline::audioLevelUpdated,
                                this, &CallManager::onAudioLevelUpdated);

                        connect(m_publishPipeline, &PublishPipeline::error, this, [this](const QString &msg) {
                            qWarning() << "CallManager: publish pipeline error:" << msg;
                            teardown(msg);
                        });

                        connect(m_publishPipeline, &PublishPipeline::cameraError, this, [this](const QString &reason) {
                            qWarning() << "CallManager: camera error:" << reason;
                            // Give up on camera — don't retry, as it floods signaling with offers
                            m_cameraOn = false;
                            m_cameraFallbackTried = false;
                            emit cameraChanged();
                            qDebug() << "CallManager: camera disabled, continuing audio-only";
                        });

                        qDebug() << "CallManager: calling PublishPipeline::start()...";
                        if (!m_publishPipeline->start(m_stunServer, turnServers,
                            m_deviceManager ? m_deviceManager->selectedInputDeviceId() : QString(),
                            m_withVideo, videoDeviceIndex(), preferHd1080())) {
                            qWarning() << "CallManager: failed to start publish pipeline";
                            teardown("Failed to start audio pipeline");
                            return;
                        }
                        m_glibTimer.start(20);

                        // Camera must come up IMMEDIATELY for a video call —
                        // the local preview should be live the moment the call
                        // starts, not deferred until it reaches Active (a call
                        // that stalls in Connecting must still show your own
                        // camera). mfvideosrc starts async so this does not
                        // block the UI. The setState(Active) block remains
                        // only as a late fallback (guarded by isCameraOn()).
                        if (m_cameraOn && m_publishPipeline && !m_publishPipeline->isCameraOn()) {
                            qDebug() << "CallManager: enabling camera immediately (video call)";
                            m_publishPipeline->enableCamera(videoDeviceIndex(), preferHd1080());
                            m_localVideoProvider = m_publishPipeline->localVideoProvider();
                            emit localVideoProviderChanged();
                        }

                        // If remote peer already joined (incoming call), request their stream
                        if (!m_remoteSessionId.isEmpty() && !m_subscribePipelines.contains(m_remoteSessionId)) {
                            setStatusDetail("Requesting peer stream");
                            requestPeerStream(m_remoteSessionId);
                            qDebug() << "CallManager: sent requestOffer for already-joined remote peer";
                        } else {
                            // Discover who's already in the call and request their streams.
                            // Poll periodically since HPB participant events may not arrive
                            // for mobile clients using internal signaling.
                            auto pollParticipants = [this]() {
                                if (m_state == Idle || !m_remoteSessionId.isEmpty()) return;
                                TLOG_CALL("polling call participants for" << m_callToken);
                                m_api->getArray("apps/spreed/api/v4/call/" + m_callToken,
                                    [this](bool ok, const QJsonArray &data, int) {
                                        if (!ok || m_state == Idle) return;
                                        for (const auto &val : data) {
                                            QJsonObject p = val.toObject();
                                            QString sid = p["sessionId"].toString();
                                            int inCall = p["inCall"].toInt();
                                            if (sid.isEmpty() || sid == m_signaling->sessionId() || inCall == 0)
                                                continue;
                                            if (m_remoteSessionId.isEmpty()) {
                                                m_remoteSessionId = sid;
                                                TLOG_CALL("discovered remote peer via REST:" << sid.left(20));
                                                setState(Connecting);
                                                emit callInfoChanged();
                                            }
                                            if (!m_subscribePipelines.contains(sid)) {
                                                requestPeerStream(sid);
                                                TLOG_CALL("sent requestOffer for discovered peer" << sid.left(20));
                                            }
                                    }
                                });
                            };
                            // Compliance with upstream Talk web client
                            // (v23.0.4, MCU mode): the upstream waits for
                            // the signaling layer's usersInCallChanged
                            // event before calling requestOffer; it does
                            // NOT poll the REST endpoint eagerly. An eager
                            // immediate poll can land on the MCU before
                            // the peer's publish is fully registered, so
                            // the resulting subscriber binds to an
                            // incomplete publish state — exactly the
                            // caller-sees-callee-choppy pattern. We
                            // remove the immediate poll() invocation and
                            // keep the 3-s timer purely as a backup for
                            // the documented mobile/internal-signaling
                            // path where HPB participant events may not
                            // fire. The HPB roomPeerJoined /
                            // participantJoinedCall handlers will normally
                            // trigger requestPeerStream within ms.
                            auto *pollTimer = new QTimer(this);
                            pollTimer->setInterval(3000);
                            connect(pollTimer, &QTimer::timeout, this, pollParticipants);
                            connect(this, &CallManager::stateChanged, pollTimer, [pollTimer, this]() {
                                if (m_state == Idle || m_state == Active)
                                    pollTimer->deleteLater();
                            });
                            pollTimer->start();
                        }

                        // Video is now included in the initial pipeline (no delayed renegotiation)
                        // Camera preview starts immediately when pipeline starts
                    }
                });
        });
}

void CallManager::leaveCallOnServer(const QString &token, bool wasJoined,
                                    std::function<void()> onDone)
{
    // token + wasJoined are SNAPSHOTTED by the caller before teardown's
    // local-state cleanup, because teardown clears m_callToken /
    // m_joinedCall up front (UI must not wait on the server). Reading the
    // members here would see them already cleared and skip the DELETE —
    // which is exactly the regression where the OTHER party never got the
    // "participant left" event and stayed in the call.
    if (token.isEmpty() || !wasJoined) {
        if (onDone) onDone();
        return;
    }
    // NC Talk API reads "all" from the request body, not query params.
    // all=true means "end the call for EVERYONE" and is moderators-only;
    // a normal hang-up must send all=false or a moderator leaving would
    // terminate the whole group call.
    QJsonObject body;
    body["all"] = false;
    // Must-complete: the DELETE MUST land or the other party stays "in the
    // call". delMustComplete retries on confirmed non-delivery (status 0 /
    // 5xx — the high-latency/flaky-link case, ZA server from BG), bounded
    // and non-blocking. teardown() already ran setState(Idle)+callEnded
    // synchronously; only the status-revert hook waits on onDone.
    m_api->delMustComplete("apps/spreed/api/v4/call/" + token, body, this,
        [onDone](bool ok, int statusCode) {
            if (ok) qDebug()  << "CallManager: leaveCall ack received";
            else    qWarning() << "CallManager: leaveCall did not complete (status"
                               << statusCode << ") — server participant-timeout will clean up";
            if (onDone) onDone();
        });
}

void CallManager::leaveCallBeacon()
{
    if (!m_joinedCall || m_callToken.isEmpty()) return;
    qInfo() << "CallManager: leave-call beacon for" << m_callToken;
    m_joinedCall = false;  // guard against duplicate beacons / re-entry

    QJsonObject body;
    body["all"] = false;
    m_api->del("apps/spreed/api/v4/call/" + m_callToken, body,
        [](bool ok, const QJsonObject &, int sc) {
            qInfo() << "CallManager: leave-call beacon"
                    << (ok ? "delivered" : "failed") << sc;
        });

    // On aboutToQuit the event loop is winding down; spin briefly so the
    // DELETE actually flushes instead of being dropped with the process.
    QElapsedTimer t; t.start();
    while (m_api->pendingCount() > 0 && t.elapsed() < 600)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

void CallManager::stopAllPipelines()
{
    m_glibTimer.stop();
    if (m_peerPipeline) {
        m_peerPipeline->stop();
        delete m_peerPipeline;
        m_peerPipeline = nullptr;
    }
    if (m_publishPipeline) {
        qDebug() << "CallManager::teardown — stopping publish pipeline";
        m_publishPipeline->stop();
        qDebug() << "CallManager::teardown — deleting publish pipeline";
        delete m_publishPipeline;
        m_publishPipeline = nullptr;
        qDebug() << "CallManager::teardown — publish pipeline deleted";
    }
    for (auto *sub : m_subscribePipelines) {
        sub->stop();
        delete sub;
    }
    m_subscribePipelines.clear();
    m_subscriberSids.clear();
    m_desiredSubstream.clear();
    m_subscriberRecoveries.clear();
    m_pubIceGrace.stop();
    m_pubIceRecoveries = 0;

    // Flush stale GLib sources from destroyed pipelines (libnice agents,
    // DTLS timers, etc.). Without this, creating a new webrtcbin on the
    // next call can crash because pending callbacks reference freed state.
    qDebug() << "CallManager::teardown — flushing GLib context";
    for (int i = 0; i < 200; i++)
        g_main_context_iteration(nullptr, FALSE);
    qDebug() << "CallManager::teardown — GLib flush done";

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
    stopIncomingCameraPreview();   // #13: release the camera (safe no-op if not running)
    // Snapshot the call identity BEFORE the local-state cleanup below
    // clears it — the server-leave DELETE at the end needs the token and
    // the joined flag, and without sending it the OTHER party never gets
    // the "participant left" event (it stays in the call).
    const QString leaveToken = m_callToken;
    const bool    wasJoined  = m_joinedCall;
    stopAllPipelines();

    // Local state cleanup runs immediately — anything that affects the
    // user's view of the call (mic level, screen share, participants)
    // should clear without waiting for the server. Only the callEnded
    // signal (which gates UserStatusManager::revertStuckCall) is deferred
    // until the server has acknowledged the leave, so the revert finds
    // a server in the post-call state and the pre-call status snapshot
    // can be restored.
    m_callToken.clear();
    m_remoteSessionId.clear();
    m_remotePeerName.clear();
    m_remotePeerId.clear();
    m_remotePeerClient.clear();
    m_callDuration = 0;
    m_userActionReady = false;
    m_remoteVideoMuted = true;
    m_remoteAudioMuted = true;
    m_speaking = false;
    m_speakingGrace.stop();
    m_pendingOffers.clear();
    m_pendingRequestOffers.clear();
    m_requestOfferAttempts.clear();
    m_requestOfferRetry.stop();

    // Clean up screen sharing. Set the teardown flag for symmetry with
    // stopScreenShare(): if any publisher-ICE-failed edge fires during
    // the screen pipeline destruction it must not bump the recovery
    // counter and re-enter hangUp(). Benign here today (stopAllPipelines
    // ran just above and queued signals can't reach a destroyed handler),
    // but cheap defence against future reordering.
    m_screenShareTearingDown = true;
    if (m_screenSharePipeline) {
        m_screenSharePipeline->stop();
        m_screenSharePipeline->deleteLater();
        m_screenSharePipeline = nullptr;
    }
    m_screenSharing = false;
    m_screenShareTearingDown = false;
    if (m_remoteScreenProvider) {
        m_remoteScreenProvider->disconnect();
        m_remoteScreenProvider = nullptr;
        emit remoteScreenProviderChanged();
    }
    for (auto *sub : m_screenSubscribers) {
        sub->stop();
        sub->deleteLater();
    }
    m_screenSubscribers.clear();

    // Synchronous local close — the UI must NEVER wait on the server
    // (mid-call network drops could otherwise leave the call window
    // pinned "in call" until Qt's transport timeout, a minute+ later).
    m_joinedCall = false;
    setState(Idle);
    emit callEnded(reason);

    // Server-side leave is fire-and-forget; whatever consumer needs to
    // know "the server has finished processing my leave" (currently the
    // user-status revert) listens to callServerLeaveAcked. If the network
    // is dead the signal never fires, which is fine — the server's own
    // participant-timeout cleans up that session.
    leaveCallOnServer(leaveToken, wasJoined, [this]() {
        emit callServerLeaveAcked();
    });
}

void CallManager::onAudioLevelUpdated(double level)
{
    if (qAbs(m_audioLevel - level) > 0.02) {
        m_audioLevel = level;
        emit audioLevelChanged();
    }

    // Speaking detection — only when not muted and publish pipeline is active
    if (m_muted || !m_publishPipeline || !m_publishPipeline->isRunning()) return;

    if (level > 0.05) {
        m_speakingGrace.stop();
        if (!m_speaking) {
            m_speaking = true;
            m_publishPipeline->sendStatusMessage(R"({"type":"speaking"})");
        }
    } else if (m_speaking && !m_speakingGrace.isActive()) {
        m_speakingGrace.start();
    }

    if (m_selfParticipant) {
        m_selfParticipant->setAudioLevel(level);
        m_selfParticipant->setSpeaking(m_speaking);
    }
}

// --- Participant registry (additive; mirrors signaling/pipeline state) ---

CallParticipant *CallManager::ensureParticipant(const QString &sessionId, const QString &name)
{
    if (sessionId.isEmpty() || sessionId == m_signaling->sessionId())
        return nullptr;
    if (auto it = m_participants.constFind(sessionId); it != m_participants.constEnd()) {
        it.value()->setDisplayName(name);
        return it.value();
    }
    auto *p = new CallParticipant(sessionId, /*isSelf*/false, this);
    p->setDisplayName(name);
    m_participants.insert(sessionId, p);
    m_participantOrder.append(p);
    connect(p, &CallParticipant::changed, this, &CallManager::participantsChanged);
    emit participantAdded(p);
    emit participantsChanged();
    return p;
}

CallParticipant *CallManager::ensureSelfParticipant()
{
    if (m_selfParticipant) return m_selfParticipant;
    m_selfParticipant = new CallParticipant(QStringLiteral("self"), /*isSelf*/true, this);
    m_selfParticipant->setDisplayName(tr("You"));
    m_selfParticipant->setConnState(CallParticipant::Connected);
    m_participantOrder.prepend(m_selfParticipant);   // self renders first
    connect(m_selfParticipant, &CallParticipant::changed, this, &CallManager::participantsChanged);
    emit participantAdded(m_selfParticipant);
    emit participantsChanged();
    syncSelfParticipant();
    return m_selfParticipant;
}

void CallManager::syncSelfParticipant()
{
    if (!m_selfParticipant) return;
    m_selfParticipant->setAudioMuted(m_muted);
    m_selfParticipant->setVideoMuted(!m_cameraOn);
    m_selfParticipant->setScreenSharing(m_screenSharing);
    m_selfParticipant->setCamera(m_localVideoProvider);
    m_selfParticipant->setSpeaking(m_speaking);
    m_selfParticipant->setAudioLevel(m_audioLevel);
}

void CallManager::removeParticipant(const QString &sessionId)
{
    auto it = m_participants.find(sessionId);
    if (it == m_participants.end()) return;
    CallParticipant *p = it.value();
    m_participants.erase(it);
    m_participantOrder.removeOne(p);
    emit participantRemoved(sessionId);
    emit participantsChanged();
    p->deleteLater();
}

void CallManager::clearParticipants()
{
    if (m_participantOrder.isEmpty()) return;
    const auto order = m_participantOrder;
    m_participants.clear();
    m_participantOrder.clear();
    m_selfParticipant = nullptr;
    for (auto *p : order) {
        emit participantRemoved(p->sessionId());
        p->deleteLater();
    }
    emit participantsChanged();
}

// --- Participant events ---

void CallManager::onParticipantJoinedCall(const QString &sessionId, int flags, const QString &displayName)
{
    TLOG_CALL("onParticipantJoinedCall sid=" << sessionId.left(20) << "flags=" << flags
              << "name=" << displayName << "state=" << m_state << "callToken=" << m_callToken);
    if (sessionId == m_signaling->sessionId()) {
        TLOG_CALL("ignoring own session join");
        return;
    }

    if ((m_state == Outgoing || m_state == Connecting) && m_remoteSessionId.isEmpty()) {
        m_remoteSessionId = sessionId;
        if (!displayName.isEmpty()) m_remotePeerName = displayName;
        qDebug() << "CallManager: remote peer joined:" << sessionId.left(20) << "name=" << m_remotePeerName;
        m_ringTimeout.stop();
        setState(Connecting);
        emit callInfoChanged();

        // Broadcast media state now that remote peer can receive it
        broadcastMediaState("audio", !m_muted);
        broadcastMediaState("video", m_cameraOn);

        if (m_useP2P && m_peerPipeline) {
            m_peerPipeline->createOffer();
            qDebug() << "CallManager: creating P2P offer for joined peer";
            if (auto *p = ensureParticipant(sessionId, displayName)) {
                p->setCamera(m_peerPipeline->remoteVideoProvider());
                p->setConnState(CallParticipant::Connecting);
            }
        } else if (flags & (CALL_FLAG_WITH_AUDIO | CALL_FLAG_WITH_VIDEO)) {
            // Subscribe as soon as the peer publishes ANY media (audio OR
            // video). This previously gated on the VIDEO flag only, so an
            // audio-only peer (camera off) was never subscribed — you heard
            // NOTHING from them until they turned their camera on (field
            // bug, 2026-05-22). Janus forwards the whole publisher feed
            // (audio + video) for a single subscription, so subscribing on
            // the audio flag gets the audio immediately and video when it
            // arrives. participantFlagsChanged still re-requests on a later
            // video toggle for the camera-mid-call path.
            requestPeerStream(sessionId);
            qDebug() << "CallManager: sent requestOffer for remote peer (has media)";
        } else {
            qDebug() << "CallManager: peer joined with no media flags yet — "
                        "waiting for audio/video before requesting subscriber";
        }
    }
    else if (m_state == Idle) {
        // Suppress self-ring: if the joining participant is OUR OWN user
        // on another device (same Nextcloud userId, different session),
        // this is us starting/continuing a call elsewhere — don't ring
        // ourselves. Only fires when the userId is known; if unknown we
        // fall through and ring (better a spurious ring than a missed
        // real call).
        const QString joinerUser = m_signaling->userIdForSession(sessionId);
        const QString selfUser   = m_signaling->userId();
        if (!joinerUser.isEmpty() && !selfUser.isEmpty()
            && joinerUser == selfUser) {
            qInfo() << "CallManager: ignoring call-join from our own user on "
                       "another device (" << sessionId.left(20)
                    << ") — not ringing self";
        } else {
            // Incoming call detected via signaling — route through the same
            // path as conversation-list detection so cooldown applies.
            m_remoteSessionId = sessionId;
            QString token = m_signaling->currentRoom();
            onIncomingCallDetected(displayName, token, flags);
        }
    }

    // Register every in-call peer in the model (covers peers that join an
    // already-active conference, and refreshes flags). Media providers are
    // attached later when their subscriber/offer arrives.
    if (m_state == Outgoing || m_state == Connecting || m_state == Active) {
        if (auto *p = ensureParticipant(sessionId, displayName)) {
            p->setAudioMuted(!(flags & CALL_FLAG_WITH_AUDIO));
            p->setVideoMuted(!(flags & CALL_FLAG_WITH_VIDEO));
        }
    }
}

void CallManager::onParticipantLeftCall(const QString &sessionId)
{
    if (sessionId == m_signaling->sessionId()) return;

    m_pendingRequestOffers.remove(sessionId);
    m_requestOfferAttempts.remove(sessionId);

    // Remove subscriber pipeline for this peer
    if (m_subscribePipelines.contains(sessionId)) {
        m_subscribePipelines[sessionId]->stop();
        m_subscribePipelines[sessionId]->deleteLater();
        m_subscribePipelines.remove(sessionId);
        m_subscriberSids.remove(sessionId);
        qDebug() << "CallManager: removed subscriber for" << sessionId.left(20);
    }

    removeParticipant(sessionId);

    if (sessionId == m_remoteSessionId) {
        qDebug() << "CallManager: remote peer left call";
        teardown("Call ended");
    }
}

// --- SDP events ---

void CallManager::processPendingOffers()
{
    if (m_pendingOffers.isEmpty()) return;
    qDebug() << "CallManager: processing" << m_pendingOffers.size() << "pending offer(s)";
    auto pending = m_pendingOffers;
    m_pendingOffers.clear();
    for (const auto &o : pending)
        onOfferReceived(o.fromSessionId, o.sdp, o.sid);
}

void CallManager::onOfferReceived(const QString &fromSessionId, const QString &sdp, const QString &sid)
{
    setStatusDetail("Received offer");
    qDebug() << "CallManager: received offer from" << fromSessionId.left(20) << "sid=" << sid;

    if (m_useP2P && m_peerPipeline) {
        m_peerPipeline->setRemoteOffer(sdp);
        return;
    }

    // Guard: don't create subscribers until ICE servers are available
    if (m_stunServer.isEmpty()) {
        m_pendingOffers.append({fromSessionId, sdp, sid});
        qDebug() << "CallManager: queuing offer — ICE servers not yet available";
        return;
    }

    // An offer arrived for this peer — stop retrying requestoffer for it.
    m_pendingRequestOffers.remove(fromSessionId);
    m_requestOfferAttempts.remove(fromSessionId);

    // Track the MCU's sid — signals use the hash so re-offers update seamlessly
    m_subscriberSids[fromSessionId] = sid;

    // Re-offer for an existing subscriber. A Nextcloud Talk / Janus
    // re-subscribe (e.g. the remote peer enables video) is a BRAND-NEW
    // PeerConnection: new sid, new ICE ufrag/pwd, new DTLS fingerprint.
    // webrtcsrc cannot adopt new ICE credentials on a live session —
    // SubscribeWebrtcSrc::feedOfferToSignaller() is one-shot
    // (m_offerDelivered), so feeding the re-offer into the existing element
    // silently DROPS it; the stale session keeps running until its ICE
    // consent times out (~25 s) and the whole subscription fails, so the
    // remote video (only present from the re-offer onward) never arrives
    // and the call is stuck "Connecting". Tear the stale subscriber down
    // and fall through to build a fresh one bound to the new session.
    // deleteLater (not delete): we may be inside a signaling callback;
    // destroying the element synchronously here risks re-entrant teardown.
    if (m_subscribePipelines.contains(fromSessionId)) {
        qDebug() << "CallManager: re-offer for" << fromSessionId.left(20)
                 << "— rebuilding subscriber for new session sid=" << sid;
        if (auto *old = m_subscribePipelines.take(fromSessionId)) {
            old->stop();
            old->deleteLater();
        }
    }

    // New subscriber
    auto *sub = new SubscribeWebrtcSrc(fromSessionId, this);

    connect(sub, &SubscribeWebrtcSrc::localAnswerReady,
            this, [this, fromSessionId](const QString &sdp) {
        QString currentSid = m_subscriberSids.value(fromSessionId);
        m_signaling->sendAnswer(fromSessionId, sdp, currentSid);
        qDebug() << "CallManager: sent subscriber answer to" << fromSessionId.left(20) << "sid=" << currentSid;
    });

    connect(sub, &SubscribeWebrtcSrc::iceCandidateReady,
            this, [this, fromSessionId](const QString &candidate, int mline, const QString &mid) {
        QString currentSid = m_subscriberSids.value(fromSessionId);
        m_signaling->sendCandidate(fromSessionId, makeCandidateJson(candidate, mline, mid), currentSid);
    });

    connect(sub, &SubscribeWebrtcSrc::iceGatheringComplete,
            this, [this, fromSessionId]() {
        m_signaling->sendEndOfCandidates(fromSessionId, m_subscriberSids.value(fromSessionId));
    });

    connect(sub, &SubscribeWebrtcSrc::iceStateChanged,
            this, [this, fromSessionId](const QString &state) {
        qDebug() << "CallManager: subscriber ICE:" << state;
        setStatusDetail("Subscriber ICE " + state);
        if (auto *p = m_participants.value(fromSessionId)) {
            if (state == "connected" || state == "completed")
                p->setConnState(CallParticipant::Connected);
            else if (state == "failed")
                p->setConnState(CallParticipant::Failed);
            else if (state == "disconnected")
                p->setConnState(CallParticipant::Reconnecting);
            else
                p->setConnState(CallParticipant::Connecting);
        }
        if (state == "connected" || state == "completed") {
            setStatusDetail("Connected");
            m_subscriberRecoveries.remove(fromSessionId);  // fresh budget per healthy connect
            if (m_state == Connecting) {
                setState(Active);
                m_durationTimer.start();
            }
            // #132 simulcast: Janus parks a new subscriber on substream 0
            // (180p). Ask for the desired layer now that the subscription
            // is live. Default is HIGH; CallStage refines per tile size.
            const int want = m_desiredSubstream.value(fromSessionId, 2);
            m_signaling->sendSelectStream(fromSessionId,
                                          m_subscriberSids.value(fromSessionId), want);
            broadcastMediaState("audio", !m_muted);
            broadcastMediaState("video", m_cameraOn);
            // Announce our TalQ version on the data channel so other TalQ
            // peers can show it. Re-sent here because a new subscriber may
            // have just come up and missed our earlier publish-side message.
            if (m_publishPipeline && m_publishPipeline->isRunning()) {
                const QByteArray hello = QByteArray(R"({"type":"talq.client","client":"TalQ","version":")")
                    + TALQ_VERSION + R"("})";
                m_publishPipeline->sendStatusMessage(hello);
            }
        }
        if (state == "failed") {
            qWarning() << "CallManager: subscriber ICE failed for"
                       << fromSessionId.left(20)
                       << "— recovering (call stays up)";
            recoverSubscriber(fromSessionId, QStringLiteral("ice-failed"));
        }
    });

    connect(sub, &SubscribeWebrtcSrc::mediaStateReceived,
            this, [this, fromSessionId](const QString &type) {
        CallParticipant *p = m_participants.value(fromSessionId);
        if (type == "audioOn")       { m_remoteAudioMuted = false; emit remoteMediaChanged(); if (p) p->setAudioMuted(false); }
        else if (type == "audioOff") { m_remoteAudioMuted = true;  emit remoteMediaChanged(); if (p) p->setAudioMuted(true); }
        else if (type == "videoOn")  { m_remoteVideoMuted = false; emit remoteMediaChanged(); if (p) p->setVideoMuted(false); }
        else if (type == "videoOff") { m_remoteVideoMuted = true;  emit remoteMediaChanged(); if (p) p->setVideoMuted(true); }
        else if (type == "speaking")        { if (p) p->setSpeaking(true); }
        else if (type == "stoppedSpeaking") { if (p) p->setSpeaking(false); }
    });

    connect(sub, &SubscribeWebrtcSrc::peerClientInfo,
            this, [this, fromSessionId](const QString &client, const QString &version) {
        const QString info = client + "/" + version;
        if (auto *p = m_participants.value(fromSessionId))
            p->setPeerClient(info);
        if (m_remotePeerClient != info) {
            m_remotePeerClient = info;
            qDebug() << "CallManager: peer client" << info;
            emit callInfoChanged();
        }
    });

    connect(sub, &SubscribeWebrtcSrc::error, this, [this, fromSessionId](const QString &msg) {
        qWarning() << "CallManager: subscriber pipeline error for" << fromSessionId.left(20) << ":" << msg;
    });

    connect(sub, &SubscribeWebrtcSrc::sessionEnded, this, [this, fromSessionId]() {
        qInfo() << "CallManager: subscriber feed ended by SFU for"
                << fromSessionId.left(20) << "— re-subscribing (call stays up)";
        recoverSubscriber(fromSessionId, QStringLiteral("end-session"));
    });

    m_subscribePipelines[fromSessionId] = sub;
    if (!sub->start(m_stunServer, m_turnServers, m_deviceManager ? m_deviceManager->selectedOutputDeviceId() : QString())) {
        qWarning() << "CallManager: failed to start subscriber pipeline for" << fromSessionId.left(20);
        m_subscribePipelines.remove(fromSessionId);
        m_subscriberSids.remove(fromSessionId);
        sub->deleteLater();
        return;
    }

    qDebug() << "CallManager: subscriber started, setting video provider...";
    m_remoteVideoProvider = sub->videoProvider();
    emit remoteVideoProviderChanged();
    if (auto *p = ensureParticipant(fromSessionId, {}))
        p->setCamera(sub->videoProvider());

    qDebug() << "CallManager: calling setRemoteOffer...";
    sub->setRemoteOffer(sdp);
    qDebug() << "CallManager: setRemoteOffer returned";
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

        // Publisher renegotiation (e.g. camera toggle) doesn't affect subscribers.
        // Subscribers receive from the MCU independently — no re-request needed.
    }
}
