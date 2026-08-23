#include "core/CallManager.h"
#include "core/BackgroundEngine.h"
#include "core/LeakStats.h"
#include "core/VideoEncoderUtil.h"   // talqAvoidNvenc() latch
#include "core/HwEncoderProbe.h"   // talqHwEncoderProbeExcludes() — #74 probe exclusions
#include "core/EncodeTier.h"
#include "core/Diagnostics.h"
#include "core/TalqLog.h"
#include "core/WasapiDucking.h"
#include "core/DebugMonitor.h"   // readProcessMemoryMB() for the host-protection watchdog
#include "ui/ShareOverlay.h"
#include "models/ConversationListModel.h"
#include <QJsonObject>
#include <QDateTime>
#include <QtMath>
#include <QSet>
#include <QSettings>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QUdpSocket>
#include <QHostInfo>
#include <QRandomGenerator>
#include <QTimer>
#include <QThread>
#include <QSet>
#include <limits>
#include <memory>
#include <vector>
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
// A participant who dialled in over SIP (Participant::FLAG_WITH_PHONE). TalQ
// defined only 1/2/4 before 0.65.4, so a phone caller was indistinguishable
// from someone with their camera off -- they were shown as a silent, blank
// video tile rather than as a person on the telephone.
static constexpr int CALL_FLAG_WITH_PHONE = 8;

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
        { QStringLiteral("classic"),  tr("Classic bell")    },
        { QStringLiteral("bright"),   tr("Bright bell")     },
        { QStringLiteral("soft"),     tr("Soft bell")       },
        { QStringLiteral("landline"), tr("Landline (US)")   },
        { QStringLiteral("uk"),       tr("Double ring (UK)")},
        { QStringLiteral("oldphone"), tr("Old telephone")   },
        { QStringLiteral("trill"),    tr("Digital trill")   },
        { QStringLiteral("default"),  QStringLiteral("TalQ tone")       },
        { QStringLiteral("none"),     tr("None (silent)")   },
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
    // Outgoing (calling…) tone is user-selectable too (Calls/outgoingRingtone) —
    // some of the bundled tones suit the calling side better than receiving.
    // Default "ringback" = the synthesized brrr-brrr ringback (prior behaviour);
    // a bundled id plays its WAV; "default" = the synthesized TalQ tone; "none"
    // stays silent.
    {
        QSettings so("TalQ", "TalQ");
        so.beginGroup("Calls");
        const QString oid = so.value("outgoingRingtone", "landline").toString();
        so.endGroup();
        if (oid == "none") return;
        if (oid == "default") {
            PlaySoundA(incoming.constData(), nullptr, SND_MEMORY | SND_ASYNC | SND_LOOP);
            return;
        }
        if (oid != "ringback") {
            QFile f(QStringLiteral(":/sounds/ring_%1.wav").arg(oid));
            if (f.open(QIODevice::ReadOnly)) {
                m_ringtoneData = f.readAll();
                PlaySoundA(m_ringtoneData.constData(), nullptr,
                           SND_MEMORY | SND_ASYNC | SND_LOOP);
                return;
            }
            // fall through to the ringback if the bundled file is missing
        }
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
    if (id == "ringback") {
        buf = generateOutgoingTone();          // the outgoing brrr-brrr ringback
    } else if (id == "default") {
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

void CallManager::playBusyTone()
{
#ifdef Q_OS_WIN
    // #80 -- one-shot busy tone (no loop). Static buffer stays alive for the
    // async playback; UI-thread-only. PlaySoundA is single-channel so this
    // naturally replaces any outgoing ringback that was playing.
    static QByteArray buf;
    QFile f(QStringLiteral(":/sounds/busy_soft.wav"));
    if (!f.open(QIODevice::ReadOnly)) return;
    buf = f.readAll();
    PlaySoundA(buf.constData(), nullptr, SND_MEMORY | SND_ASYNC);
#endif
}

void CallManager::onPeerBusy(const QString &peerName)
{
    // #80 -- the peer we're ringing is on another call; their TalQ auto-replied
    // with the busy marker. Only act while we're still placing/ringing out.
    if (m_state != Outgoing && m_state != Connecting) return;
    const QString who = peerName.isEmpty() ? m_remotePeerName : peerName;
    qInfo() << "CallManager: peer" << who << "is on another call (busy) — busy tone + popup";
    playBusyTone();
    emit peerBusy(who);
}

void CallManager::addPeerToActiveCall(const QString &callerToken, const QString &callerName)
{
    if (m_state != Active && m_state != Connecting) {
        emit addToCallResult(false, tr("You're not in a call."));
        return;
    }
    if (callerToken.isEmpty() || callerToken == m_callToken || !m_api) return;
    const bool withVideo = m_withVideo;
    const QString selfUser = m_signaling ? m_signaling->userId() : QString();
    qInfo() << "CallManager: add-to-call — resolving caller from" << callerToken
            << "(active call 1:1=" << isOneToOneCall() << ")";
    // Resolve the caller's userId from their (1:1) conversation, then branch.
    m_api->fetchParticipants(callerToken, this,
        [this, callerName, withVideo, selfUser](bool ok, const QJsonArray &parts, const QString &) {
        if (!ok) { emit addToCallResult(false, tr("Couldn't reach %1.").arg(callerName)); return; }
        QString callerUserId;
        for (const QJsonValue &v : parts) {
            const QJsonObject p = v.toObject();
            if (p.value(QStringLiteral("actorType")).toString() != QLatin1String("users")) continue;
            const QString uid = p.value(QStringLiteral("actorId")).toString();
            if (!uid.isEmpty() && uid != selfUser) { callerUserId = uid; break; }
        }
        if (callerUserId.isEmpty()) { emit addToCallResult(false, tr("Couldn't identify %1.").arg(callerName)); return; }
        if (!isOneToOneCall())
            addAndRingIntoCall(m_callToken, callerUserId, callerName);
        else
            promoteOneToOneToGroup(callerUserId, callerName, withVideo);
    });
}

void CallManager::addAndRingIntoCall(const QString &token, const QString &userId, const QString &name)
{
    // Add the user to the (group) call room, then resolve their attendeeId and
    // ring them into the ongoing call. "Already a participant" is fine (ignored).
    m_api->addRoomParticipant(token, userId, this, [this, token, userId, name](bool, const QString &) {
        m_api->fetchParticipants(token, this, [this, token, userId, name](bool ok, const QJsonArray &parts, const QString &) {
            if (!ok) { emit addToCallResult(false, tr("Couldn't add %1 to the call.").arg(name)); return; }
            int attendeeId = -1;
            for (const QJsonValue &v : parts) {
                const QJsonObject p = v.toObject();
                if (p.value(QStringLiteral("actorType")).toString() == QLatin1String("users")
                    && p.value(QStringLiteral("actorId")).toString() == userId) {
                    attendeeId = p.value(QStringLiteral("attendeeId")).toInt();
                    break;
                }
            }
            if (attendeeId < 0) { emit addToCallResult(false, tr("Couldn't ring %1.").arg(name)); return; }
            m_api->ringAttendee(token, attendeeId, this, [this, name](bool ok3, const QString &err3) {
                emit addToCallResult(ok3, ok3 ? tr("Added %1 to the call.").arg(name)
                                              : tr("Couldn't ring %1: %2").arg(name, err3));
            });
        });
    });
}

void CallManager::promoteOneToOneToGroup(const QString &callerUserId, const QString &callerName, bool withVideo)
{
    const QString peerUserId = m_remotePeerUserId;
    const QString peerName   = m_remotePeerName;
    if (peerUserId.isEmpty()) { emit addToCallResult(false, tr("Can't turn this call into a group.")); return; }
    const QString groupName = tr("%1, %2").arg(peerName, callerName);
    m_api->createRoom(2 /* group */, groupName, QString(), this,
        [this, peerUserId, callerUserId, callerName, peerName, withVideo](bool ok, const QString &newToken, const QString &) {
        if (!ok || newToken.isEmpty()) { emit addToCallResult(false, tr("Couldn't create the group call.")); return; }
        // Add both others to the new group.
        m_api->addRoomParticipant(newToken, peerUserId, this, [](bool, const QString &) {});
        m_api->addRoomParticipant(newToken, callerUserId, this,
            [this, newToken, callerName, peerName, withVideo](bool, const QString &) {
            // Join the new group call ourselves, then ring both others into it.
            startCall(newToken, withVideo);
            m_api->fetchParticipants(newToken, this, [this, newToken](bool ok2, const QJsonArray &parts, const QString &) {
                if (!ok2) return;
                const QString selfUser = m_signaling ? m_signaling->userId() : QString();
                for (const QJsonValue &v : parts) {
                    const QJsonObject p = v.toObject();
                    if (p.value(QStringLiteral("actorType")).toString() != QLatin1String("users")) continue;
                    if (p.value(QStringLiteral("actorId")).toString() == selfUser) continue;
                    const int attendeeId = p.value(QStringLiteral("attendeeId")).toInt();
                    if (attendeeId >= 0) m_api->ringAttendee(newToken, attendeeId, this, [](bool, const QString &) {});
                }
            });
            emit addToCallResult(true, tr("Started a group call with %1 and %2.").arg(peerName, callerName));
        });
    });
}

// --- CallManager ---

// Forward decl: the call-flags helper is a file-local static defined further
// down, but the A2 session-reset handler in the ctor needs it.
static int callFlags(bool withVideo, bool withAudio);

CallManager::CallManager(ApiClient *api, SignalingClient *signaling, MediaDeviceManager *deviceMgr, QObject *parent)
    : QObject(parent)
    , m_api(api)
    , m_signaling(signaling)
    , m_deviceManager(deviceMgr)
{
    // Restore the "NVENC / HW H264 is unusable on this machine" latches (set the
    // first time a HW encoder session failed to open — e.g. an iGPU-pinned
    // 2-GPU/Optimus laptop). With them set, the encoder ladder skips the failing
    // HW path from the very first call, so the machine never repeats the mid-call
    // encoder failure that used to drop the call.
    //
    // But these latches must NOT be permanent: the GPU can change between sessions
    // (a docked eGPU, a swapped card, a driver update) and a toolchain/GStreamer
    // upgrade can change what works — a stale latch otherwise pins a now-capable
    // box to 3×software-x264 (CPU-melting; field: Ivan's NVIDIA box wrongly stuck
    // on software after the 0.57 toolchain upgrade, nvh264enc available + GPU
    // Capable yet forced to x264). So key the latches on an ENVIRONMENT
    // FINGERPRINT — app version + GPU adapter names. When it changes, clear the
    // latches so the next call re-probes hardware; it re-latches if HW still
    // genuinely fails.
    {
        QSettings vs("TalQ", "TalQ");
        QStringList gpuFp = talq::gpuAdapterNames();
        gpuFp.sort();   // stable regardless of DXGI adapter enumeration order
        const QString envFp = QStringLiteral(TALQ_VERSION) + QLatin1Char('|')
                              + gpuFp.join(QLatin1Char(','));
        if (vs.value("Video/encoderLatchEnv").toString() != envFp) {
            vs.remove("Video/avoidNvenc");
            vs.remove("Video/forceSoftwareVideo");
            vs.setValue("Video/encoderLatchEnv", envFp);
            qInfo().noquote() << "CallManager: encoder environment changed -> "
                                 "re-probing HW video encoder (cleared latches); env =" << envFp;
        }
        talqAvoidNvenc().store(vs.value("Video/avoidNvenc", false).toBool());
        talqForceSoftwareVideo().store(
            vs.value("Video/forceSoftwareVideo", false).toBool());
    }

    // #20 — long-lived BackgroundEngine. Constructed even when the
    // feature is Off so the publisher can ask for it later without a
    // null check; the engine itself is the gate (Mode::None → no-op).
    m_backgroundEngine = new BackgroundEngine(this);

    // 0.60.2 (2026-07-13 field RCA) — engineDisabled / backgroundImageFailed
    // had ZERO connects anywhere in the codebase, so a background failure
    // (no GL 3.3 context, ORT init fault, missing/corrupt image file) was
    // 100% silent: the user picked Blur and got raw camera, with not one
    // log line saying why. Loud qWarning so field logs finally show it, +
    // the EXISTING video-quality notice channel (the in-call chip — no new
    // UI surface invented; outside a call the notice is simply not shown,
    // and it resets on call teardown like every other quality notice).
    connect(m_backgroundEngine, &BackgroundEngine::engineDisabled,
            this, [this](const QString &reason) {
        // 0.60.5: the engine now FAILS CLOSED. When segmentation dies we no
        // longer pass the camera through — we obscure the whole frame — so
        // this notice must NOT say "sending normal video". It used to, and it
        // was a privacy lie in both directions: pre-0.60.5 a failed segmenter
        // silently substituted a generic centred-ellipse mask, which left the
        // user's actual room SHARP inside the ellipse while the UI reassured
        // them the effect was simply "unavailable". The user is the only one
        // who can consent to being seen, so tell them exactly what is on the
        // wire and how to change it.
        qWarning() << "CallManager: BACKGROUND ENGINE DISABLED —" << reason
                   << "— the user's Blur/Image choice cannot be applied; video is"
                      " being OBSCURED (fail-closed), NOT passed through";
        setVideoQualityNotice(tr("Background effect failed — your video is hidden "
                                 "until it recovers. Turn the background off in "
                                 "Settings to send normal video."));
    });
    connect(m_backgroundEngine, &BackgroundEngine::backgroundImageFailed,
            this, [this](const QString &path) {
        qWarning() << "CallManager: background image failed to load:" << path
                   << "— sending normal video (user must pick a different"
                      " image in Settings to retry)";
        setVideoQualityNotice(tr("Background image couldn't be loaded — sending normal video"));
    });
    connect(m_backgroundEngine, &BackgroundEngine::backgroundPassthroughNoImage,
            this, [this]() {
        // 0.60.6 (Petia's foggy-window RCA): Image mode with no image chosen is a
        // MISCONFIGURATION, not a hide-my-room request, so the engine sends real
        // video (there is no concealment intent to betray). But it must not be
        // SILENT — Petia sat through a whole call not knowing why her camera
        // looked wrong. Tell the user their real video is going out and why.
        qInfo() << "CallManager: background is Image mode with NO image selected —"
                   " sending normal video (pick an image in Settings)";
        setVideoQualityNotice(tr("No background image selected — sending normal video. "
                                 "Pick one in Settings."));
    });
    applyBackgroundSettings();

    // #share-reliability: bound how long we wait for proof a screen share is
    // live (ICE connected + outbound RTP flowing) before the policy retries a
    // fresh pipeline. Without this connect()+interval the whole confirm/retry
    // feature is dead (the timer would never fire its handler). 8s comfortably
    // covers ICE + first RTP on a healthy link; provisional pending live tuning.
    m_shareConfirmTimer.setSingleShot(true);
    m_shareConfirmTimer.setInterval(8000);
    connect(&m_shareConfirmTimer, &QTimer::timeout,
            this, &CallManager::onShareConfirmTimeout);

    // D3 — camera-toggle coalescer (see toggleCamera). Single-shot; each toggle
    // restarts it so only the FINAL desired state is applied to the MF device.
    m_cameraApplyTimer.setSingleShot(true);
    m_cameraApplyTimer.setInterval(250);
    connect(&m_cameraApplyTimer, &QTimer::timeout, this, &CallManager::applyCameraState);

    // #81 -- camera intent watchdog. If the user turns the camera on but no
    // self-frame has arrived a few seconds later, the enable silently failed
    // with nothing watching it (enableCamera early-returns on a stale
    // m_cameraEnabled, arming no frame watchdog). Force one real disable->enable
    // to clear the stale state; that re-arms PublishPipeline's own watchdog, so
    // a genuinely unavailable device then flows into the existing cameraError
    // path (grace-retry -> "Camera unavailable" notice + revert to audio-only).
    m_cameraIntentTimer.setSingleShot(true);
    m_cameraIntentTimer.setInterval(3000);
    connect(&m_cameraIntentTimer, &QTimer::timeout, this, [this]() {
        if (!m_cameraOn || !m_publishPipeline) return;
        if (m_state != Active && m_state != Connecting && m_state != Reconnecting) return;
        if (m_publishPipeline->cameraFirstFrameSeen()) return;   // camera came up fine
        if (m_publishPipeline->isCameraOn() && m_cameraGraceRetries > 0) return; // grace-retry already in flight
        qWarning() << "CallManager: camera intended ON but no self-frame after grace "
                      "— forcing a disable->enable to recover a wedged/stale camera (#81)";
        const int deviceIndex = videoDeviceIndex();
        const bool hd1080 = preferHd1080();
        // disable then delayed enable — mirror the grace-retry ordering so an
        // async NULL can't overtake the sync PLAYING and park the device.
        QTimer::singleShot(0, this, [this]() {
            if (m_publishPipeline) m_publishPipeline->disableCamera();
        });
        QTimer::singleShot(300, this, [this, deviceIndex, hd1080]() {
            if (!m_publishPipeline || !m_cameraOn) return;
            if (m_state == Idle || m_state == Ending) return;
            if (m_publishPipeline->isCameraOn()) return;   // already back up
            qInfo() << "CallManager: camera intent-watchdog — re-attempting enableCamera (#81)";
            m_publishPipeline->enableCamera(deviceIndex, hd1080);
        });
    });

    // D7 — device hotplug auto-resume. When a camera/mic is plugged or unplugged
    // mid-call and our camera was INTENDED but had failed (unavailable), retry it
    // now that the device set changed — a camera that comes back auto-resumes
    // instead of staying dark. We deliberately do NOT auto-SWITCH the active
    // device on a new plug (Zoom/Teams behaviour: make it available, don't yank).
    if (m_deviceManager) {
        connect(m_deviceManager, &MediaDeviceManager::devicesChanged, this, [this]() {
            if ((m_state == Active || m_state == Connecting)
                && m_cameraOn && m_cameraUnavailable) {
                qInfo() << "CallManager: devices changed — retrying camera after device loss";
                m_cameraUnavailable = false;
                m_cameraGraceRetries = 0;   // fresh device event: fresh grace-retry budget
                emit cameraChanged();
                m_cameraApplyTimer.start();   // coalesced re-apply
            }
        });
    }

    // HPB participant events
    connect(m_signaling, &SignalingClient::participantJoinedCall,
            this, &CallManager::onParticipantJoinedCall);
    connect(m_signaling, &SignalingClient::participantLeftCall,
            this, &CallManager::onParticipantLeftCall);

    // A peer's DELIBERATE hangup (not a transient drop) — end a 1:1 MCU call
    // immediately instead of sitting in the 28s peer-grace "Reconnecting" hold.
    // A moderator force-muted us. Honour it locally rather than only telling
    // the user: the point of a force-mute is that the room stops hearing this
    // client, so it must actually stop transmitting.
    connect(m_signaling, &SignalingClient::forceMuted, this, [this]() {
        if (m_muted) return;          // already silent, nothing to do
        toggleMute();
        qInfo() << "CallManager: muted by moderator";
    });

    connect(m_signaling, &SignalingClient::peerHungUp,
            this, &CallManager::onPeerHungUp);

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
        if (tryAdoptReturningPeer(sessionId)) return;   // #bug3 -- peer back from grace
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

    // #bug2 -- a peer's signaling session LEFT the room (HPB room/leave; this
    // event used to be logged and dropped). If we hold a subscriber for that
    // peer it is now a zombie decoding SSRCs that no longer exist (its publisher
    // reconnected under a new session). Rebuild via recoverSubscriber -- which
    // is call-safe, bounded, and re-requests the offer -- NOT onParticipantLeftCall,
    // which teardown()s the whole 1:1 call when sid==m_remoteSessionId and would
    // drop the call on a transient publisher-reconnect blip.
    connect(m_signaling, &SignalingClient::roomPeerLeft,
            this, [this](const QString &sessionId) {
        // Screen-sub cleanup must NOT be gated on call state. A peer's session can
        // vanish (room/leave only, with NO participantLeftCall — #bug2) while WE
        // are in Reconnecting from our OWN publisher rebuild; if we skipped cleanup
        // then, the zombie screen subscriber would pin our camera to a single LOW
        // layer for the rest of the call (the exact latch fix B targets). This is a
        // safe no-op when there is no sub, so run it before the call-state guard.
        removeScreenSubscriber(sessionId);
        // A3a — a room/leave means THIS sid is DEAD (HPB session ids are
        // per-WebSocket-session; a reconnect mints a NEW sid that arrives via
        // roomPeerJoined and resubscribes fresh). The old code recoverSubscriber'd
        // the dead sid, which re-requested an offer the MCU rejects forever
        // ("Not allowed to request offer." every ~8s = the not_allowed STORM Pavel
        // triggered when his session churned 46PWZPP8->EVJJ0EE7). Instead DROP the
        // zombie + purge its requestoffer bookkeeping so we stop chasing it. Purge
        // unconditionally (even when not Active) so a churn during our own
        // Reconnecting can't leave dead-sid state armed.
        const bool hadPending = m_pendingRequestOffers.contains(sessionId)
                                || m_subscribePipelines.contains(sessionId);
        dropSubscriber(sessionId);
        if (hadPending)
            qInfo() << "CallManager: peer" << sessionId.left(20)
                    << "left room -- dropped its dead-sid subscriber (A3a; new sid resubscribes)";
        if (m_pendingRequestOffers.isEmpty())
            m_requestOfferRetry.stop();
    });

    // A2 (0.53.x robustness) — our signaling session was RESET (resume rejected /
    // fresh session id minted mid-call). The old session's Janus MCU publisher
    // AND subscriber handles are detached server-side, so without this we keep
    // RTP-blasting a dead publisher handle (black-holed: packets-sent may keep
    // rising so the publisher-stall watchdog never trips) while every peer
    // re-subscribes to a session with nothing behind it — the room-wide outage
    // Pavel's session-flip caused. Recover deterministically under the new sid.
    connect(m_signaling, &SignalingClient::sessionReset, this,
            [this](const QString &newSid) {
        if (m_state != Connecting && m_state != Active && m_state != Reconnecting)
            return;   // not in a call -> nothing to recover
        qWarning() << "CallManager: SIGNALING SESSION RESET -> new sid"
                   << newSid.left(16) + "..."
                   << "— re-registering call, rebuilding publisher + subscribers";
        setState(Reconnecting);

        // (1) Re-register our NEW session in the server CALL record. The
        // SignalingClient already re-POSTed the ROOM (participants/active), but
        // call membership (inCall flags) was bound to the dead session, so peers
        // would see us in the room yet not in the call. A targeted silent POST
        // re-adds us without the heavyweight initial-join orchestration
        // (joinCallOnServer also re-fetches STUN/TURN + rebuilds — not wanted here).
        if (!m_callToken.isEmpty()) {
            QJsonObject body;
            body["flags"] = callFlags(m_cameraOn, !m_muted);
            body["silent"] = true;            // already mid-call — never re-ring
            body["recordingConsent"] = false;
            m_api->post("apps/spreed/api/v4/call/" + m_callToken, body,
                [](bool ok, const QJsonObject &, int sc) {
                    if (!ok) qWarning() << "CallManager: session-reset call re-register failed, status=" << sc;
                });
        }

        // (2) Rebuild + re-offer our publisher under the new sid. buildAndStartPublisher
        // reads the live sessionId(), so the rebuilt offer carries the new session.
        m_pubRetryAttempts = 0;
        recoverPublisher("signaling-session-reset");

        // (3) Our subscriber handles were under the dead session too. Drop each
        // now (briefly blanks remote tiles) but DEFER the re-subscribe until the
        // publisher recovery returns us to Active: a requestoffer issued while
        // Reconnecting gets rejected (our new publish isn't registered yet) AND the
        // m_requestOfferRetry net itself self-cancels in Reconnecting, so
        // re-requesting now would abandon peers. m_resubscribeOnActive replays the
        // re-request on the Active transition (setState), where the MCU accepts it.
        const auto peers = m_subscribePipelines.keys();
        for (const QString &sid : peers)
            dropSubscriber(sid);
        m_resubscribeOnActive = true;
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
        // Drop ONLY this peer's screen subscriber (and recompute suppression). A
        // different peer that is still sharing keeps our camera suppressed — the
        // old code nulled the single render pointer unconditionally, wrongly
        // restoring our full simulcast while another peer's share was still up.
        removeScreenSubscriber(sessionId);
    });

    // 0.52.7 — a requestoffer was rejected "not_allowed". The HPB error carries no
    // sid, so attribute it to EVERY sid we currently have a requestoffer
    // outstanding for (in the field this is one peer whose camera publisher slot
    // is settling/reaped around a screen-share renegotiation — the app-window-share
    // call-drop repro). The retry tick escalates a sid that keeps getting rejected
    // to a full recoverSubscriber rebuild instead of resending requestoffer forever.
    // KNOWN LIMITATION (bounded, non-fatal): with >1 pending peer, a rejection
    // aimed at peer A also ticks a merely-SLOW-but-unrejected peer B, so B may be
    // escalated to recoverSubscriber early. B is NOT dropped and the call is NOT
    // torn down (recoverSubscriber is bounded at kMaxSubRecoveries and never hangs
    // up); it only spends B's recovery budget sooner. The field case is 1:1 (exact
    // attribution); a precise per-sid fix would need a request-id the HPB doesn't
    // echo — which we deliberately do NOT add (inventing signaling fields has
    // broken calls twice).
    connect(m_signaling, &SignalingClient::requestOfferRejected, this, [this]() {
        for (const QString &sid : m_pendingRequestOffers)
            ++m_requestOfferRejections[sid];
    });

    // Keep the self participant mirrored to our own media state.
    for (auto sig : { &CallManager::muteChanged, &CallManager::cameraChanged,
                      &CallManager::screenShareChanged,
                      &CallManager::localVideoProviderChanged })
        connect(this, sig, this, [this]{ syncSelfParticipant(); });

    // Camera suppression (drop our camera to a single LOW layer while a screen is
    // shared) is re-evaluated EXPLICITLY wherever the set of active screen shares
    // changes: remote add (screen-offer handler), remote remove
    // (removeScreenSubscriber), local share start/stop, and publisher rebuild. It
    // is deliberately NOT wired to remoteScreenProviderChanged — that render-
    // pointer signal also fires mid-rebuild (a re-offer nulls then re-sets the
    // provider in one event-loop turn), which would briefly un-suppress and churn
    // the camera's m/h simulcast layers exactly while a screen decode is rebuilt.

    // HPB WebSocket signaling messages
    connect(m_signaling, &SignalingClient::offerReceived,
            this, [this](const QString &from, const QString &sdp, const QString &sid, const QString &roomType) {
        if (roomType == "screen") {
            // Incoming screen share — create a subscriber for it
            qDebug() << "CallManager: received screen share offer from" << from.left(20);
            // Same orphan-after-hangup guard: a screen offer (or a peer mid-share)
            // can land a beat after teardown in the dual-call/glare scramble and
            // this branch builds a full SubscribePipeline (real wasapi2sink if the
            // screen carries audio) + a remote-screen tile before onOfferReceived's
            // guard. Gate on torn-down (Idle/Ending) only; teardown() clears
            // m_screenSubscribers before leaving the active states, so returning
            // here can't strand a stale subscriber.
            if (callTornDown()) {
                qInfo() << "CallManager: ignoring screen-share offer from"
                        << from.left(20) << "— call torn down (state" << m_state << ")";
                return;
            }
            // Redundant re-assert guard. The publisher re-asserts its screen
            // sendoffer a few seconds into a re-share (the reap-race recovery
            // added in 0.51.17). If our current subscriber for this peer is
            // GENUINELY DECODING FRAMES right now, the re-offer is redundant —
            // rebuilding would tear the working decode down and flap the view to
            // black. Suppress it. Liveness is keyed on decoded-frame advancement
            // (m_screenSubStallTicks, sampled every ~2s in updateCallStats), NOT
            // ICE state: in MCU mode a dead feed keeps ICE "connected" (the
            // publisher slot was reaped/replaced, or unshareScreen was lost), so an
            // ICE-based latch would wrongly suppress the very re-assert that should
            // rebuild it and strand the viewer on a permanent black frame. A black/
            // never-started/stale sub has stall-ticks > 1 and falls through to
            // rebuild on the fresh feed — so a lost unshareScreen cannot strand the
            // viewer. Threshold is <=1 (not <=3): the sampler ticks every ~2s, so
            // <=1 means "a frame within the last ~2-4s" = genuinely live; a feed
            // that died when the sender stopped is many ticks stale by the time the
            // +5s/+11s re-assert offer arrives, so it reliably rebuilds.
            if (m_screenSubscribers.contains(from)
                && m_screenSubStallTicks.value(from, 99) <= 1) {
                qInfo() << "CallManager: ignoring redundant screen re-offer from"
                        << from.left(20) << "— subscriber live (frames flowing, no flap)";
                return;
            }
            // 0.52.15 — STARTUP GRACE (anti-thrash). The live-guard above only
            // suppresses a sub decoding RIGHT NOW. A sub that has NEVER decoded a
            // frame (frameMark==0) is still negotiating ICE/DTLS + waiting for its
            // first keyframe (the MCU auto-PLIs new subscribers; we PLI at
            // build+0.5/1.5/3s) — ~2-5s on a re-share. The publisher's +5/+11s
            // reap-race re-assert must NOT rebuild it, or the fresh sub is killed
            // before its first I-frame decodes → it never reaches "live" → the
            // infinite ~6s rebuild loop that stranded a re-shared screen on
            // "Starting" (Kalin↔Ilko 0.52.14). So while young AND never-decoded:
            // re-PLI to hurry the keyframe and IGNORE the re-offer. A GENUINE
            // re-share (sub WAS live → frameMark>0) skips this and falls through to
            // rebuild (drops the frozen frame). Can't strand a dead build: once
            // older than the grace it falls through and rebuilds once. ageMs>=0
            // fails open (missing stamp → huge age → rebuild, never suppress).
            if (m_screenSubscribers.contains(from)
                && m_screenSubFrameMark.value(from, 0) == 0) {
                const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                const qint64 ageMs = nowMs - m_screenSubBuiltMs.value(from, 0);
                const QString ice = m_screenSubIceState.value(from);
                // 0.57.22 — ICE COMPLETED is not "still coming up". Once completed
                // with frameMark==0 the sub is past pairing; the only thing
                // outstanding is its first keyframe (~1-2s normally). Protect it for
                // a SHORT keyframe budget only (re-PLI + ignore — a keyframe in
                // flight must not be torn down), then FALL THROUGH to the
                // rebuild-stale path: past the budget it is keyframe-STARVED and the
                // reap-race re-assert is the fix, not a threat. Holding it for the
                // full ICE-progress grace stranded a re-share (field 14:57: completed
                // at +2s, no frame, the +9s re-assert ignored, sharer gave up).
                // Rebuilding here is 0.52.14-safe: ICE is DONE, not mid-pairing.
                // Missing stamp despite ice=="completed" can't strand: completedMs==0
                // routes to the pairing branch, where "completed" no longer holds.
                const qint64 completedMs = m_screenSubCompletedMs.value(from, 0);
                if (completedMs > 0) {
                    const qint64 sinceCompletedMs = nowMs - completedMs;
                    if (sinceCompletedMs >= 0
                        && sinceCompletedMs < kScreenSubKeyframeBudgetMs) {
                        qInfo() << "CallManager: screen re-offer from" << from.left(20)
                                << "— ICE completed" << sinceCompletedMs
                                << "ms ago, first keyframe still due — re-PLI + ignore";
                        if (auto *s = m_screenSubscribers.value(from)) s->requestKeyframe();
                        return;
                    }
                    qInfo() << "CallManager: screen re-offer from" << from.left(20)
                            << "— ICE completed" << sinceCompletedMs
                            << "ms ago with NO frame — keyframe-starved, rebuilding";
                    // fall through to the rebuild-stale path below
                } else {
                    // Hold (re-PLI + ignore) while the sub is still coming up — by
                    // EITHER signal: (a) it is actively PAIRING ICE (checking/
                    // connected, no frame yet), or (b) it is still within the young-
                    // build wall-clock grace. (a) is the load-bearing one: the
                    // reap-race re-assert lands at +5s AND +11s, but
                    // kScreenSubStartupGraceMs is shorter than +11s, so a sub that
                    // reaches "checking" early and stays there (Ilko 0.52.16) would
                    // age out and be torn down by the +11s rebuild mid-pairing. A sub
                    // pairing ICE must never be killed by a redundant re-offer; the
                    // only re-offers are the 2 reap re-asserts, so "hold while
                    // pairing" can't strand it (no further offer follows, and a real
                    // stop→reshare clears this state via removeScreenSubscriber).
                    // CAP the icePairing hold by a wall-clock bound (> the +11s reap
                    // horizon, measured from the last ICE-progress re-stamp): a sub
                    // WEDGED in "checking" (lost unshareScreen, no
                    // removeScreenSubscriber) must eventually fall through to rebuild
                    // on a later offer, not be held until ICE reports "failed".
                    // Within the cap it still protects a legitimately pairing sub
                    // from the +11s mid-pairing teardown.
                    const bool icePairing =
                        (ice == "checking" || ice == "connected")
                        && ageMs >= 0 && ageMs < kScreenSubIceProgressGraceMs;
                    if (icePairing || (ageMs >= 0 && ageMs < kScreenSubStartupGraceMs)) {
                        qInfo() << "CallManager: screen re-offer from" << from.left(20)
                                << "— sub still coming up (ice=" << ice << "age=" << ageMs
                                << "ms, no frame yet) — re-PLI + ignore (anti-thrash)";
                        if (auto *s = m_screenSubscribers.value(from)) s->requestKeyframe();
                        return;
                    }
                }
            }
            // A re-share (stop → share again) sends a fresh offer for a
            // session we may still hold a screen subscriber for. Feeding
            // the new offer into the OLD SubscribePipeline leaves its
            // already-negotiated decode wired to the dead stream — the
            // viewer is stuck on a frozen last frame of the previous
            // share. Same class as the camera re-attach bug: tear the
            // stale subscriber down and fall through to build a fresh one.
            if (auto *stale = m_screenSubscribers.take(from)) {
                m_screenSubFrameMark.remove(from);    // rebuilding — drop stale
                m_screenSubStallTicks.remove(from);   // frame-liveness state
                m_screenSubBuiltMs.remove(from);      // startup-grace stamp (re-stamped on start)
                m_screenSubIceState.remove(from);     // ICE-progress (re-set on start)
                m_screenSubCompletedMs.remove(from);  // keyframe-budget stamp (re-set on completed)
                // 0.53.1 — do NOT clear m_pendingScreenSubCandidates here: the
                // bundled candidates for the NEW sid arrived just before this re-offer
                // and the screen-offer handler must flush them into the rebuilt sub
                // (else it starves → stuck "Starting"). The rolling buffer is capped +
                // take()-cleared on flush; stale candidates are harmless (no valid pair).
                qDebug() << "CallManager: screen re-offer for" << from.left(20)
                         << "— rebuilding screen subscriber (avoid frozen frame)";
                stale->stop();
                VideoFrameProvider *staleProv = stale->videoProvider();
                stale->deleteLater();
                // Only drop the render slot if WE were rendering THIS peer's
                // (now-stale) screen — in a multi-sharer call another peer may be
                // the one on-screen, and the re-add below re-points to this peer's
                // fresh provider anyway. Suppression is deliberately NOT recomputed
                // here (the take()+re-add must not churn it — done at the explicit
                // recompute after the new subscriber is in place).
                if (m_remoteScreenProvider == staleProv) {
                    m_remoteScreenProvider = nullptr;
                    emit remoteScreenProviderChanged();
                }
                if (auto *p = m_participants.value(from)) p->setScreen(nullptr);
                // 0.53.0 — drain the OLD screen sub's libnice agent + TURN deallocate
                // + UDP socket close BEFORE building the new one. cleanup()'s
                // set_state(NULL) is synchronous on the GStreamer graph, but the
                // libnice teardown (agent close, TURN dealloc, socket release) is
                // async; building the new webrtcbin in the SAME event-loop turn
                // collides the two agents on the same TURN 5-tuple → the new ICE
                // sticks at "checking" (field: 8-of-18 on a re-share). A bounded,
                // NON-blocking GLib drain lets the old agent's teardown run first —
                // the exact pattern stopScreenShare() + full-call teardown use.
                // FALSE = non-blocking: returns the instant the queue empties (~0 ms
                // when nothing is pending), so it never stalls the UI.
                for (int i = 0; i < 50; ++i) g_main_context_iteration(nullptr, FALSE);
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
                    this, [this, from, sub](const QString &state) {
                // 0.52.17 — drop a queued ICE event from a SUPERSEDED sub. On a rebuild
                // the old sub is deleteLater'd, but its Qt connection stays live until
                // the deferred-delete runs, so a late event it already posted could
                // write the NEW sub's m_screenSubIceState/m_screenSubBuiltMs under the
                // same 'from' key (now load-bearing for rebuild-protection). Compare the
                // captured sub identity (pointer only — never dereferenced) to current.
                if (m_screenSubscribers.value(from) != sub) return;
                qDebug() << "CallManager: screen subscriber ICE:" << state;
                m_screenSubIceState[from] = state;  // 0.52.17 — drives rebuild-protection in the offer handler
                // 0.52.16/0.52.17 — re-anchor the startup grace to negotiation
                // PROGRESS, not build time. kScreenSubStartupGraceMs is stamped at
                // build; a slow re-share can burn all of it while ICE is still
                // "checking", so the publisher's +5/+11s reap-race re-assert rebuilds
                // a sub that was about to connect → the churn. 0.52.16 re-stamped on
                // connected/completed ONLY — but Ilko's 0.52.16 receiver log showed
                // the sub reaching "checking" (remote candidates in, pairs forming)
                // and the +11s rebuild killing it RIGHT THERE, landing on a fresh sid
                // whose candidates were mis-delivered to the dying sub → it never
                // even reached "checking" → screen stuck on "Starting your share".
                // So "checking" is ALSO forward progress and MUST re-stamp: a sub
                // actively pairing ICE is never torn down by a redundant re-offer. A
                // genuinely stuck sub stamps "checking" once and still ages out of the
                // grace ~8s later (the edge fires once, so no live re-stamp loop).
                // Once a frame decodes (frameMark>0) this branch no longer applies.
                // 0.57.22 — "completed" NO LONGER re-stamps: completion is the END of
                // pairing, not progress toward it. Re-stamping there restarted the
                // 20s grace at the exact moment the sub became either live or
                // keyframe-starved — shielding the starved case from the +5/+11s
                // re-asserts that would rebuild it (field 14:57: completed at +2s, no
                // frame ever, re-offer at +7.5s-since-completion ignored → stranded).
                if ((state == "checking" || state == "connected")
                    && m_screenSubscribers.contains(from)
                    && m_screenSubFrameMark.value(from, 0) == 0)
                    m_screenSubBuiltMs[from] = QDateTime::currentMSecsSinceEpoch();
                // 0.57.22 — stamp ICE completion ONCE per sub build: switches the
                // offer-handler guard from the generous pairing hold to the short
                // kScreenSubKeyframeBudgetMs keyframe budget. Lockstep-cleared with
                // the other per-sub maps (rebuild/failed-start/remove/leave). Also
                // re-PLI right here: the transport is provably up NOW, while the
                // build-time PLIs (+0.5/1.5/3s) may have fired pre-DTLS and been lost.
                if (state == "completed"
                    && m_screenSubscribers.contains(from)
                    && !m_screenSubCompletedMs.contains(from)) {
                    m_screenSubCompletedMs[from] = QDateTime::currentMSecsSinceEpoch();
                    if (m_screenSubFrameMark.value(from, 0) == 0)
                        sub->requestKeyframe();
                }
                // Screen-subscriber ICE/DTLS-failure auto-retry (backlog: a
                // cross-region screen share landing on "failed" — or a peer's
                // share never appearing at all — previously just sat there
                // forever: nothing here ever proactively asked for a fresh
                // offer, and the publisher's own reap-race re-assert only fires
                // during an ACTUAL re-share, not a first-attempt failure. The
                // underlying transport race (server-side, cross-region) is
                // proven probabilistic — a fresh attempt often succeeds — so
                // request a new offer ourselves, bounded, instead of leaving
                // the viewer stuck on "Starting…"/black indefinitely.
                if (state == "failed") {
                    const int attempts = ++m_screenSubFailRetries[from];
                    if (attempts > kScreenSubFailMaxRetries) {
                        qWarning() << "CallManager: screen subscriber for" << from.left(20)
                                   << "failed ICE" << attempts << "times — giving up";
                        return;
                    }
                    qWarning() << "CallManager: screen subscriber ICE failed for"
                               << from.left(20) << "— requesting a fresh offer (attempt"
                               << attempts << "of" << kScreenSubFailMaxRetries << ")";
                    if (auto *dead = m_screenSubscribers.take(from)) {
                        dead->deleteLater();
                        m_screenSubFrameMark.remove(from);
                        m_screenSubStallTicks.remove(from);
                        m_screenSubBuiltMs.remove(from);
                        m_screenSubIceState.remove(from);
                        m_screenSubCompletedMs.remove(from);
                    }
                    QTimer::singleShot(1500, this, [this, from]() {
                        if (m_state != Active && m_state != Connecting) return;
                        m_signaling->requestOffer(from, QStringLiteral("screen"));
                    });
                }
            });
            connect(sub, &SubscribePipeline::iceGatheringComplete,
                    this, [this, from, sid]() {
                m_signaling->sendEndOfCandidates(from, sid, "screen");
            });
            m_screenSubscribers[from] = sub;
            qDebug() << "CallManager: starting screen subscriber, STUN:" << m_stunServer;
            if (!sub->start(m_stunServer, effectiveTurnServers())) {
                qWarning() << "CallManager: failed to start screen subscriber pipeline";
                m_screenSubscribers.remove(from);
                sub->deleteLater();
                // Clear ALL per-session screen state in lockstep (matches
                // removeScreenSubscriber): a failed start must not leak this 'from's
                // queued early candidates / grace stamp / ICE state, or a later
                // re-share's flush would replay stale (wrong-sid) candidates.
                m_screenSubFrameMark.remove(from);
                m_screenSubStallTicks.remove(from);
                m_screenSubBuiltMs.remove(from);
                m_screenSubIceState.remove(from);
                m_screenSubCompletedMs.remove(from);
                m_pendingScreenSubCandidates.remove(from);
                // A re-offer's stale subscriber was already torn down above; if the
                // rebuild fails, leaving the participant flagged screen-sharing with
                // a null screen would be inconsistent. Clear it + recompute so we
                // don't stay latched to LOW.
                if (auto *p = m_participants.value(from)) {
                    p->setScreen(nullptr);
                    p->setScreenSharing(false);
                }
                updateCameraSuppression();
                return;
            }
            qDebug() << "CallManager: screen subscriber started, setting offer...";
            m_screenSubBuiltMs[from] = QDateTime::currentMSecsSinceEpoch();  // 0.52.15 startup-grace baseline
            m_screenSubCompletedMs.remove(from);  // 0.57.22 — fresh build: keyframe budget re-arms on ITS completed edge
            m_remoteScreenProvider = sub->videoProvider();
            emit remoteScreenProviderChanged();
            if (auto *p = ensureParticipant(from, {})) {
                p->setScreen(sub->videoProvider());
                p->setScreenSharing(true);
            }
            // A remote peer is now screen-sharing — drop our camera to a single
            // LOW layer (relieves the sharer's decode + every sender's encode).
            updateCameraSuppression();
            // Receiving a peer's SCREEN is definitive proof they are live in the
            // call — so make sure we ALSO subscribe their PRIMARY (camera+mic)
            // feed. On a normal mid-call share this is already done
            // (requestPeerStream self-dedupes on m_subscribePipelines /
            // m_pendingRequestOffers, so it's a no-op). On a REJOIN to an
            // already-active call the peer generates no fresh join event for
            // us, participantJoinedCall may not re-fire, and the REST poll
            // backup self-disables once a session is adopted / is deleted when
            // the call goes Active before its first tick — so the primary
            // subscribe was being missed, leaving the peer's screen visible but
            // their camera AND audio absent (field bug, 2026-06-03). The screen
            // offer is the reliable hook that was missing.
            if (m_state != Idle && m_state != Ending)
                requestPeerStream(from);
            // Flush any screen candidates the MCU trickled BEFORE this sub existed
            // (queued in m_pendingScreenSubCandidates above). addIceCandidate queues
            // them inside the pipeline; the setRemoteOffer below flushes the queue
            // into webrtcbin — identical contract to the camera subscriber path.
            // Without this a fresh sub could start with ZERO remote candidates → ICE
            // stuck → "Starting your share" forever (0.52.16 receiver log).
            const auto pendingScreen = m_pendingScreenSubCandidates.take(from);
            for (const auto &pc : pendingScreen)
                sub->addIceCandidate(pc.candidate, pc.mline, pc.mid);
            if (!pendingScreen.isEmpty())
                qInfo() << "CallManager: flushed" << pendingScreen.size()
                        << "early screen candidate(s) into new sub for" << from.left(20);
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

        // Orphan-after-hangup guard (covers the screen branch below + the MCU
        // routing): a late candidate the MCU trickles a beat after teardown must
        // not feed an orphan subscriber into "new"→connected. Gate on torn-down
        // ONLY (Idle/Ending) — NOT Outgoing/Incoming: during an outgoing MCU ring
        // the publisher's OWN remote candidates trickle from Janus and are routed
        // below (it needs them to connect), and early subscriber candidates must
        // still queue into m_pendingSubCandidates.
        if (callTornDown()) return;

        // Route by roomType: screen candidates go to screen pipelines
        // (screen share is independent of the camera media path).
        if (roomType == "screen") {
            const bool isOwnSession = (fromSessionId == m_signaling->sessionId());
            if (m_screenSubscribers.contains(fromSessionId)) {
                m_screenSubscribers[fromSessionId]->addIceCandidate(cStr, mline, mid);
            } else if (m_screenSharePipeline && isOwnSession) {
                // Our OWN screen publisher's remote candidates (from the MCU).
                m_screenSharePipeline->addIceCandidate(cStr, mline, mid);
                return;
            }
            // 0.53.1 — ALSO buffer a remote peer's screen candidates in a small
            // rolling window, EVEN while a subscriber is live. The MCU trickles a
            // subscriber's candidates bundled with/just before its offer; the
            // publisher's +5/+11s reap re-assert can then REBUILD this sub a beat
            // AFTER those candidates land — routed only to the (about-to-die) old
            // sub they're lost, and the rebuilt sub STARVES → stuck "Starting your
            // share" (Ilko 0.53.0, sid 8389…/10:41:57: "flushing 0 queued candidates"
            // → no remote candidates → no ICE). handleScreenOffer flushes this buffer
            // into the (re)built sub, covering BOTH the initial-build (0.52.16) and
            // rebuild starvation. !isOwnSession mirrors the camera path: never buffer
            // our own session (we build no screen subscriber for ourselves → leak).
            if (!isOwnSession) {
                auto &buf = m_pendingScreenSubCandidates[fromSessionId];
                buf.append({cStr, mline, mid});
                while (buf.size() > 16) buf.removeFirst();   // ~1-2 offers' worth
            }
            return;
        }

        // Video candidates (MCU path)
        if (fromSessionId == m_signaling->sessionId() && m_publishPipeline) {
            m_publishPipeline->addIceCandidate(cStr, mline, mid);
        } else if (m_subscribePipelines.contains(fromSessionId)) {
            m_subscribePipelines[fromSessionId]->addIceCandidate(cStr, mline, mid);
        } else if (fromSessionId != m_signaling->sessionId()) {
            // Subscriber for this peer isn't built yet: the MCU trickles its
            // remote candidates with/just before the offer, which can arrive
            // ~100ms before onOfferReceived constructs the SubscribeWebrtcSrc.
            // Dropping them leaves the subscriber with no remote candidates ->
            // ICE stuck at "new" -> permanent "waiting for video". Queue per
            // session; onOfferReceived flushes once the subscriber exists.
            auto &pend = m_pendingSubCandidates[fromSessionId];
            pend.append({cStr, mline, mid});
            while (pend.size() > 16) pend.removeFirst();   // same cap as the screen-share twin
        }
    });

    // GLib main context pump + bus polling
    connect(&m_glibTimer, &QTimer::timeout, this, [this]() {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        if (m_publishPipeline) m_publishPipeline->pollBus();
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
        else if (m_state == Outgoing) {
            // #bug4 -- arm the late-answer window BEFORE teardown clears the
            // token (field 2026-07-06: the callee joined 2s after this fired
            // and was treated as a brand-new incoming call).
            m_lastRingoutToken     = m_callToken;
            m_lastRingoutTime      = QDateTime::currentDateTime();
            m_lastRingoutWithVideo = m_withVideo;
            teardown("No answer");
        }
    });

    // Stats timer — update call info every 2 seconds
    m_statsTimer.setInterval(2000);
    connect(&m_statsTimer, &QTimer::timeout, this, &CallManager::updateCallStats);

    // Publisher reconnect timer (Zoom-style — NEVER auto-drops the call).
    // Armed with exponential backoff by recoverPublisher() while the call is
    // in the Reconnecting state; each fire rebuilds the publisher pipeline and
    // re-offers to the MCU. A fire that arrives after we already recovered
    // (back to Active) or after teardown is a no-op — the m_state guard makes
    // a stale fire harmless. The ONLY ways out of Reconnecting are: ICE
    // recovers → Active, or the user cancels / the peer leaves → teardown.
    m_pubRetryTimer.setSingleShot(true);
    connect(&m_pubRetryTimer, &QTimer::timeout, this, [this]() {
        if (m_state != Reconnecting) return;
        rebuildPublisherAndReoffer();
    });

    // #bug3 -- peer-grace: a transient remote-peer inCall=0 / drop+rejoin must
    // NOT end a 1:1 call. We hold Reconnecting for this window and auto-re-
    // subscribe when the same userId returns (tryAdoptReturningPeer). Only a
    // true no-return within the window ends the call.
    m_peerGraceTimer.setSingleShot(true);
    m_peerGraceTimer.setInterval(28000);
    connect(&m_peerGraceTimer, &QTimer::timeout, this, [this]() {
        if (!m_peerGraceActive) return;             // already recovered / torn down
        qInfo() << "CallManager: remote peer did not return within grace -- ending call";
        m_peerGraceActive = false;
        teardown("Call ended");                     // the ONLY teardown for peer-left now
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

    // 0.51.x dynamic load controller — ~1 s tick, armed per call by
    // startLoadController() and stopped in teardown().
    connect(&m_loadTimer, &QTimer::timeout, this, &CallManager::onLoadTick);

    // requestoffer retry — upstream resends ~every 8s until the MCU
    // delivers the offer (a single send races MCU room-creation and
    // leaves that peer silent). Stops once nothing is outstanding.
    m_requestOfferRetry.setInterval(8000);
    connect(&m_requestOfferRetry, &QTimer::timeout, this, [this]() {
        if (m_state != Connecting && m_state != Active) {
            m_pendingRequestOffers.clear();
            m_requestOfferAttempts.clear();
            m_requestOfferRejections.clear();
            m_requestOfferRetry.stop();
            return;
        }
        const auto sids = m_pendingRequestOffers;
        for (const QString &sid : sids) {
            if (m_subscribePipelines.contains(sid)) {
                m_pendingRequestOffers.remove(sid);
                m_requestOfferAttempts.remove(sid);
                m_requestOfferRejections.remove(sid);
                continue;
            }
            // 0.52.7 — persistent "not_allowed": the MCU is ACTIVELY rejecting our
            // requestoffer (not just silent), so this peer's publisher slot is
            // stale/un-offerable and a bare resend can NEVER escape it (the fresh
            // offer would be dropped by the one-shot feedOfferToSignaller). Escalate
            // to a full recoverSubscriber rebuild — it tears the stale subscriber
            // down + re-requests on a clean, 8-bounded budget, by which time the
            // slot has usually settled. This is the app-window-share call-drop fix.
            // Threshold 3 (~24s) means a peer whose publish is merely SLOW but
            // SILENT (no rejection) keeps its rejection count at 0 and still rides
            // the full plain-resend budget below — no regression to that case. The
            // remove() BEFORE recoverSubscriber is LOAD-BEARING: it satisfies that
            // function's "already recovering" early-return guard so the rebuild runs.
            if (m_requestOfferRejections.value(sid) >= 3) {
                qWarning() << "CallManager: requestoffer for" << sid.left(20)
                           << "rejected 'not_allowed'" << m_requestOfferRejections.value(sid)
                           << "times — escalating to subscriber rebuild";
                m_pendingRequestOffers.remove(sid);
                m_requestOfferAttempts.remove(sid);
                m_requestOfferRejections.remove(sid);
                recoverSubscriber(sid, QStringLiteral("requestoffer-not-allowed"));
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
                m_requestOfferRejections.remove(sid);
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

    // H.264 DECODER inventory, rank-ordered — diagnostic only, changes nothing.
    //
    // The status above is derived from three hard-coded factory names, so it
    // reports "Software only" for any box whose hardware decoder is registered
    // under a different name (e.g. qsvh264dec) — and it says nothing at all
    // about which decoder decodebin will actually CHOOSE, which is decided by
    // rank. A field machine (Intel UHD 620) decoded 264 frames through
    // avdec_h264 while its QSV *encoder* worked fine, i.e. the QSV plugin
    // loaded — so "no hardware decoder" and "hardware decoder present but
    // out-ranked" were indistinguishable from the log. They need different
    // fixes, so print the actual candidate list: name, rank, and whether
    // GStreamer classes it as Hardware. decodebin picks the top entry.
    if (GstCaps *h264 = gst_caps_new_empty_simple("video/x-h264")) {
        GList *all = gst_element_factory_list_get_elements(
            GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO,
            GST_RANK_NONE);
        GList *cand = gst_element_factory_list_filter(all, h264, GST_PAD_SINK, FALSE);
        cand = g_list_sort(cand, gst_plugin_feature_rank_compare_func);
        QStringList entries;
        for (GList *l = cand; l; l = l->next) {
            auto *f = GST_ELEMENT_FACTORY(l->data);
            const char *klass = gst_element_factory_get_metadata(f, GST_ELEMENT_METADATA_KLASS);
            const bool hw = klass && strstr(klass, "Hardware") != nullptr;
            entries << QStringLiteral("%1(rank=%2%3)")
                          .arg(QString::fromUtf8(GST_OBJECT_NAME(f)))
                          .arg(gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(f)))
                          .arg(hw ? QStringLiteral(",HW") : QStringLiteral(",sw"));
        }
        qInfo().noquote() << "CallManager: H264 decoders (decodebin picks the first):"
                          << (entries.isEmpty() ? QStringLiteral("NONE") : entries.join(QStringLiteral(" ")));
        gst_plugin_feature_list_free(cand);
        gst_plugin_feature_list_free(all);
        gst_caps_unref(h264);
    }

    detectGpuClass();
}

// Classify the device's ENCODE capability (drives the camera + screen caps).
// Re-run per call (from buildAndStartPublisher) so a Settings "GPU performance"
// override change takes effect on the NEXT call without an app restart — the
// HW-encoder + adapter probes are fixed at runtime, only Video/gpuClassOverride
// changes. Based on a hardware-H.264-encoder probe + the GPU model name + the
// override — NOT the decode tier, which mislabelled capable iGPUs (Intel Iris Xe,
// AMD Radeon) as weak and throttled them to 480p/720p.
void CallManager::detectGpuClass()
{
    // A hardware H.264 encoder counts only if it EXISTS, was not marked
    // non-working by the #74 out-of-process probe on THIS GPU, and the runtime
    // "HW encoder failed, fell back to x264" latch is not set. This catches the
    // Pavel class: a box whose HW encoder is present but actually running on
    // software — the old bare-factory check mislabelled it Capable/WeakIgpu.
    bool hwEnc = false;
    for (const char *e : { "nvh264enc", "qsvh264enc", "mfh264enc" }) {
        GstElementFactory *f = gst_element_factory_find(e);
        if (!f) continue;
        gst_object_unref(f);
        if (talqHwEncoderProbeExcludes(e)) continue;   // #74: probed non-working here
        hwEnc = true;
        break;
    }
    if (talqForceSoftwareVideo().load()) hwEnc = false; // runtime HW->x264 fallback latch

    // Low logical-core CPUs demote one tier (even a working encoder shares the
    // box with capture/convert/decode on few threads).
    const int cores = QThread::idealThreadCount();
    const bool lowCpu = (cores > 0 && cores <= 4);

    talq::GpuClassOverride ov = talq::GpuClassOverride::Auto;
    {
        QSettings s("TalQ", "TalQ");
        s.beginGroup("Video");
        ov = static_cast<talq::GpuClassOverride>(s.value("gpuClassOverride", 0).toInt());
        s.endGroup();
    }
    m_gpuClass = talq::gpuClassFromSignals(hwEnc, talq::gpuAdapterNames(), ov, lowCpu);

    // Persist for the Settings "Send HD on this device" checkbox (greyed on Capable).
    {
        QSettings s("TalQ", "TalQ");
        s.beginGroup("Video");
        s.setValue("lastGpuClass", int(m_gpuClass));
        s.endGroup();
    }
    qInfo().noquote() << "CallManager: GPU class:" << talq::gpuClassName(m_gpuClass)
                      << "(hwEncode=" << hwEnc << " cores=" << cores
                      << " lowCpu=" << lowCpu << " override=" << int(ov) << ")";
}

void CallManager::setVideoQualityNotice(const QString &text)
{
    if (text == m_videoQualityNotice) return;
    m_videoQualityNotice = text;
    emit videoQualityNoticeChanged();
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
        if (w > 0 && h > 0) {
            if (h > m_peerPeakRxHeight) m_peerPeakRxHeight = h;  // honest HIGH-label basis
            return QStringLiteral("%1×%2").arg(w).arg(h);
        }
    }
    return {};
}

bool CallManager::hasRemoteScreen() const
{
    for (auto *sub : m_screenSubscribers)
        if (sub && sub->isRunning()) return true;
    return false;
}

QString CallManager::remoteScreenResolutionLabel() const
{
    // #76 -- the EXACT received resolution of the incoming share ("W×H"). We show
    // the real decoded size rather than a bucketed height tier: a shared window or
    // a 16:10 display has an arbitrary height that doesn't map to a display
    // standard (a 1200×800 window is not "1080p"; a 1920×1200 screen is not
    // "1440p"). First running screen subscriber with a decoded frame wins. The
    // screen SubscribePipeline exposes its size via its VideoFrameProvider (unlike
    // the camera SubscribeWebrtcSrc which has rxWidth/rxHeight directly).
    for (auto *sub : m_screenSubscribers) {
        if (!sub || !sub->isRunning() || !sub->videoProvider()) continue;
        const QSize sz = sub->videoProvider()->lastFrameSize();
        if (sz.isEmpty()) continue;
        return QString::number(sz.width()) + QChar(0x00D7)
             + QString::number(sz.height());
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
    if (m_screenSharing && m_screenSharePipeline) {
        // Live GCC-applied screen encoder bitrate (ramps ~2.8 → 12 Mbps and
        // drops under congestion); fall back to an indicative 2.5 Mbps only
        // until the first GCC update lands so the gauge is never blank.
        const double screen = m_screenSharePipeline->currentVideoBitrate();
        bps += screen > 0.0 ? screen : 2.5e6;
    }
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
    if (m_screenSharing && m_screenSharePipeline) {
        // Live GCC-applied screen encoder bitrate (real, congestion-controlled),
        // read fresh so the label tracks the ramp; indicative 2.5 Mbps only
        // until the first GCC update lands.
        const double screen = m_screenSharePipeline->currentVideoBitrate();
        bps += screen > 0.0 ? screen : 2.5e6;
    }
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
    // Keep the TURN RTT telemetry (ROUTING panel) fresh for the life of the
    // call: probeNearestTurnAsync() is otherwise a one-shot fired once on
    // room-join, so selectedTurnRttMs() would freeze at whatever it measured
    // then. Re-running the full UDP-STUN probe on every 2s stats tick would
    // spam every offered TURN host far more than a telemetry number needs, so
    // gate it to every 4th tick (~8s) -- frequent enough to look "live" next
    // to the signaling RTT (~25s cadence), rare enough to stay polite to the
    // network. Reuses probeNearestTurnAsync() as-is (same nearest-selection
    // logic it already does on join); the brief mid-probe window where
    // effectiveTurnServers() reverts to the full list is the same accepted
    // fallback it already uses when nothing has answered yet.
    if ((++m_turnRttTick % 4) == 0)
        probeNearestTurnAsync();

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

    // #bug2 -- subscriber frame-stall watchdog. A subscriber whose decoded
    // frame count stops advancing (peer in-call, not video-muted) is a zombie
    // feed left behind when the publisher reconnected under new SSRCs: the MCU
    // keeps the old subscriber's ICE/DTLS alive, so no ICE-failed/session-ended
    // fires and the remote tile just freezes. Rebuild it. Deferred-fire because
    // recoverSubscriber take()s from m_subscribePipelines (mutating it mid-loop
    // would be UB) -- collect stalled sids, then recover after the loop.
    QStringList stalledSubs;
    for (auto it = m_subscribePipelines.constBegin();
         it != m_subscribePipelines.constEnd(); ++it) {
        const QString &sid = it.key();
        SubscribeWebrtcSrc *s = it.value();
        if (!s || !s->isRunning()) {
            m_subStall.remove(sid);
            m_signalQuality.remove(sid);
            continue;
        }
        const int fc = s->videoProvider() ? s->videoProvider()->frameCount() : 0;
        CallParticipant *p = m_participants.value(sid);
        const bool muted   = (p && p->videoMuted());
        const bool pending = m_pendingRequestOffers.contains(sid);

        // Per-tile signal-quality glyph (loss+jitter bars, Zoom/Teams-style).
        // Read back this subscriber's LAST completed inbound-rtp get-stats
        // reply (one-tick-stale — pollInboundRtp() below refreshes it for
        // the NEXT tick, exactly like the publisher stall watchdog reads
        // outboundPacketsSent() one tick after pollOutboundRtp()), bucket +
        // hysteresis it through SignalQualityPolicy, and publish the
        // committed level onto the participant so CallStage repaints via its
        // existing changed() signal — no new signal plumbing needed.
        const bool connectedNow = p && p->connState() == CallParticipant::Connected;
        const int sigLevel = m_signalQuality[sid].onTick(
            connectedNow, s->rxStatsValid(),
            s->rxPacketsLost(), s->rxPacketsReceived(), s->rxJitterMs());
        if (p) p->setSignalQuality(sigLevel);
        s->pollInboundRtp();   // async refresh for the next tick

        if (fc > 0) m_neverDecodedRecoveries.remove(sid);   // D2 fix — real frame clears the budget
        // D2 — also rebuild a CONNECTED but never-decoded feed (permanent
        // "Starting…"/black tile) after ~12 s of a video-ON peer never producing
        // a frame. 6 ticks × 2 s = 12 s: generous enough for a slow first keyframe.
        constexpr int kNeverDecodedTicks = 6;
        if (m_subStall[sid].onTick(fc, muted, pending, kNeverDecodedTicks)) {
            // D2 fix — a NEVER-decoded fire (fc still 0) must be BOUNDED by its own
            // counter. recoverSubscriber's normal budget resets on every ICE-connect,
            // which a never-decoding feed (camera-off-not-flagged / broken codec)
            // keeps re-hitting → it would rebuild forever every ~12 s. Cap at 3,
            // cleared only by a real decoded frame (above). Frame-STALL fires (a
            // feed that DID decode then froze) stay on the normal budget.
            if (fc == 0) {
                if (m_neverDecodedRecoveries.value(sid) >= 3) {
                    if (p) p->setConnState(CallParticipant::Failed);
                    continue;   // give up on this feed's video; the call continues
                }
                ++m_neverDecodedRecoveries[sid];
            }
            stalledSubs << sid;
        }
    }
    for (const QString &sid : stalledSubs) {
        qWarning() << "CallManager: subscriber" << sid.left(20)
                   << "frame-stalled -- rebuilding (publisher likely reconnected)";
        recoverSubscriber(sid, QStringLiteral("frame-stall"));
    }

    // Screen-subscriber FRAME-liveness sampler (runs here in updateCallStats, the
    // ~2s stats tick — same poll as the #bug2 camera watchdog above). Feeds the
    // redundant-re-assert guard in the screen offer handler: a screen re-offer is
    // suppressed (no flap) only while the current sub is decoding frames RIGHT NOW.
    // A black/dead screen sub (lost unshareScreen, or an MCU publisher-slot
    // replacement — ICE stays "connected" so the #bug2 watchdog can't help and no
    // failed edge fires) goes frame-stale here within ~2 ticks and is then rebuilt
    // by the next re-offer, instead of being stranded forever on a frozen frame.
    // Seed the mark at 0 (not -1) so a CONNECTED-BUT-NEVER-DECODED sub (frameCount
    // stuck at 0) does NOT count as "advanced" — it accrues stall ticks from the
    // first sample and so is rebuildable, instead of being treated as live.
    for (auto it = m_screenSubscribers.constBegin();
         it != m_screenSubscribers.constEnd(); ++it) {
        const QString &sid = it.key();
        SubscribePipeline *s = it.value();
        const int fc = (s && s->videoProvider()) ? s->videoProvider()->frameCount() : 0;
        if (fc > m_screenSubFrameMark.value(sid, 0)) {
            // First real frame after zero — the connection is genuinely
            // healthy now; give a LATER failure its own fresh ICE-failed
            // retry budget instead of inheriting today's exhausted count.
            if (m_screenSubFrameMark.value(sid, 0) == 0)
                m_screenSubFailRetries.remove(sid);
            m_screenSubFrameMark[sid] = fc;
            m_screenSubStallTicks[sid] = 0;          // a new frame this tick -> live
        } else {
            m_screenSubStallTicks[sid] = m_screenSubStallTicks.value(sid, 0) + 1;
        }
    }

    // Publisher outbound-RTP stall watchdog. The publisher webrtcbin keeps
    // ice-connection-state "completed" when libnice loses send consent (the
    // peer left -- even a normal hang-up -- without a clean signaling leave
    // reaching us), and the "consent revoked" warning never hits the bus, so
    // nothing else observes the dead send leg while the encoder + nicesink
    // hot-loop ~89x/s (it froze a peer's laptop). packets-sent ceasing to rise
    // is the one observable signal. On a sustained stall, route through the
    // EXISTING recoverPublisher (publisher-only rebuild, Zoom-style -- never a
    // call drop): the rebuild stop()s the flooding pipeline, ending the flood.
    // Suppressed while a rebuild/retry is in flight or a screen-share is tearing
    // down, so it can't double- or false-fire; the policy fires once then
    // re-arms. When not eligible, reset so a resumed publisher re-baselines.
    if (m_publishPipeline && m_publishPipeline->isRunning()
        && !m_screenShareTearingDown && !m_pubRebuildInFlight
        && !m_pubRetryTimer.isActive()) {
        m_publishPipeline->pollOutboundRtp();   // async refresh for the next tick
        const bool expectedToSend = m_cameraOn || !m_muted;
        if (m_pubStall.onTick(m_publishPipeline->outboundPacketsSent(), expectedToSend)) {
            qWarning() << "CallManager: publisher outbound-RTP stalled -- recovering "
                          "(consent likely revoked; publisher ICE still 'completed')";
            if (m_state == Active || m_state == Connecting)
                setState(Reconnecting);
            recoverPublisher(QStringLiteral("publish-stall"));
        }
    } else {
        m_pubStall.reset();
    }

    // Remote peer
    if (!m_remoteSessionId.isEmpty())
        lines << "Remote: " + m_remoteSessionId.left(16) + "...";

    // Codec info
    lines << "Codec: Opus (WebRTC)";
    lines << "Transport: DTLS-SRTP";

    m_callStats = lines.join("\n");

    // Send-fps badge: DRAIN PublishPipeline::sendFps() exactly once per tick
    // and cache it. paintTile() must only ever read the cached m_sendFps via
    // the sendFps() getter -- never call the mutating drain itself.
    m_sendFps = m_publishPipeline ? m_publishPipeline->sendFps() : 0;

    emit callStatsChanged();
}

void CallManager::setState(CallState newState)
{
    if (m_state == newState) return;
    m_state = newState;
    qInfo() << "CallManager: state ->" << newState;
    // 0.40.15 — Connecting→Active promotion race fix. If the publisher
    // ICE already reached "connected"/"completed" BEFORE we got the
    // participant-joined signal (the common case on a fast SFU with a
    // slow-to-accept callee), the iceStateChanged handler missed its
    // chance to promote us. As soon as we transition into Connecting,
    // catch up: if we've already seen publisher ICE connected at some
    // point this call, jump straight to Active. The line above already
    // updated m_state to Connecting; recursing into setState(Active)
    // re-enters with newState=Active and proceeds normally.
    if (newState == Connecting && m_pubIceConnectedSeen) {
        qInfo() << "CallManager: media ICE already connected at Connecting-time"
                   " — promoting straight to Active"
                << "(MCU)";
        setState(Active);
        m_durationTimer.start();
        return;
    }
    if (newState == Outgoing || newState == Incoming) startRingtone();
    else stopRingtone();
    if (newState == Active) {
        // 0.52.10 — start the duration clock on the FIRST Active only. A reconnect
        // (Reconnecting→Active, or a "waiting for others" resync) re-enters Active
        // and must NOT restart it, which is what showed 00:00 mid-call.
        if (!m_callElapsed.isValid()) m_callElapsed.start();
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
        // A2 fix — a signaling session reset dropped all subscribers while
        // Reconnecting and deferred the re-subscribe to here (publisher is now
        // re-registered + Active, so the MCU will accept requestoffers). Re-request
        // every in-call peer; requestPeerStream self-dedupes on already-subscribed.
        if (m_resubscribeOnActive) {
            m_resubscribeOnActive = false;
            for (auto *p : m_participants)
                if (p && !p->isSelf() && !m_subscribePipelines.contains(p->sessionId()))
                    requestPeerStream(p->sessionId());
        }
    } else {
        m_statsTimer.stop();
    }
    if (newState == Idle) {
        m_callElapsed.invalidate();   // 0.52.10 — reset duration only on a real call end
        clearParticipants();
    }
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
    m_callJoinAttempts = 0;
    m_lastRingoutToken.clear();   // #bug4 -- a manual (re)dial supersedes any pending late-answer window
    m_peerGraceActive = false; m_peerGraceTimer.stop();   // #bug3 -- fresh call, no stale grace
    m_withVideo = withVideo;
    m_cameraOn = withVideo;
    m_cameraUnavailable = false;   // fresh call: clear any prior failure
    m_cameraGraceRetries = 0;      // fresh call: fresh grace-retry budget
    m_micUnavailable = false;      // fresh call: clear any prior mic failure
    emit cameraChanged();
    m_muted = false;
    m_callDuration = 0;
    setState(Outgoing);
    setStatusDetail("Joining room");
    m_ringTimeout.start();

    // Joining an ALREADY-ACTIVE call (the remote kept the room/call open):
    // peers already in the call produced no inCall 0->N transition (we cached
    // their flags while idle), so no participantJoinedCall fires and we ring to
    // "no answer". Drop the cached flags so any next participants update
    // re-emits a JOINED for everyone in the call. #bug5 — this is BEST-EFFORT
    // only: when we are ALREADY in the signaling room (caller idling in the
    // chat) no room join happens and the HPB pushes NOTHING on our call join
    // (edge-based protocol, no fetch request exists), so the resync alone left
    // the caller ringing out 60s against an in-call peer (field 2026-07-08).
    // The primary discovery is joinCallOnServer's REST participant poll
    // (prompt 1.2s one-shot + 3s backup), which replays the missed edge into
    // onParticipantJoinedCall with the properly mapped HPB sid. That poll was
    // long documented here as "the guaranteed discovery" — it is NOT (field
    // 2026-07-13): its NC→HPB translation is itself fed ONLY by HPB events,
    // so an HPB that never names the answered callee in ANY event leaves the
    // poll blind too (it silently skipped every row for the whole 60s ring).
    // noteRestPeerEvidence() is the backstop for exactly that gap — it
    // promotes out of Outgoing on REST evidence alone.
    m_signaling->forceCallParticipantResync();

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
    // Already CONFIRMED in the right signaling room → proceed. Gate on
    // roomJoinAcked(), not currentRoom()==token: currentRoom is set
    // optimistically the instant joinRoom is called (e.g. MainWindow's
    // deferred-room rejoin at call end), so a fast path keyed on it would fire
    // joinCallOnServer while the participants/active POST + WS "room" ack are
    // still in flight — the call would then join with no HPB room membership
    // and receive no participant/offer events (silent call). roomJoinAcked()
    // is true only after the "room" response actually landed.
    if (m_signaling->isConnected()
        && m_signaling->currentRoom() == m_callToken
        && m_signaling->roomJoinAcked()
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

bool CallManager::isOneToOneCall() const
{
    return m_conversations
        && m_conversations->conversationTypeForToken(m_callToken) == 1;
}

bool CallManager::isPeerUserSession(const QString &sessionId) const
{
    if (m_remotePeerUserId.isEmpty()) return false;
    const QString uid = m_signaling->userIdForSession(sessionId);
    return !uid.isEmpty() && uid == m_remotePeerUserId;
}

// #bug4 — choose the best still-in-call sibling session of the 1:1 peer: prefer
// one we already hold a live subscriber for, then one whose last-known call
// flags claim media, then any. Empty when none remain.
QString CallManager::pickPeerSiblingSid() const
{
    QString withFlags, any;
    for (const QString &sid : m_peerInCallSids) {
        if (sid == m_remoteSessionId) continue;
        if (m_subscribePipelines.contains(sid)) return sid;
        if (m_signaling->callFlagsForSession(sid)
            & (CALL_FLAG_WITH_AUDIO | CALL_FLAG_WITH_VIDEO))
            withFlags = sid;
        else
            any = sid;
    }
    return !withFlags.isEmpty() ? withFlags : any;
}

bool CallManager::tryAdoptReturningPeer(const QString &sessionId)
{
    if (!m_peerGraceActive) return false;
    if (sessionId == m_signaling->sessionId()) return false;
    const QString joinUser = m_signaling->userIdForSession(sessionId);
    // Require a userId match so a DIFFERENT person joining during grace is not
    // mistaken for the returning peer. If userId is unknown, fall back to an
    // exact sid match (covers the rare same-session inCall flap).
    const bool sameUser = !joinUser.isEmpty() && !m_remotePeerUserId.isEmpty()
                          && joinUser == m_remotePeerUserId;
    const bool sameSid  = (sessionId == m_graceLeftSid);
    if (!sameUser && !sameSid) return false;
    qInfo() << "CallManager: 1:1 peer returned (sid=" << sessionId.left(20)
            << "user=" << joinUser << ") -- re-adopting, canceling grace (#bug3)";
    m_remoteSessionId = sessionId;
    emit callInfoChanged();
    m_peerGraceActive = false;
    m_peerGraceTimer.stop();
    m_graceLeftSid.clear();
    requestPeerStream(sessionId);          // single re-subscribe funnel
    // Only flip back to Active if the publisher recovery isn't ALSO mid-flight
    // (a link flap can down both). If it is, stay Reconnecting; the publisher
    // ICE-connected path flips us to Active when it recovers.
    if (m_state == Reconnecting
        && !m_pubRetryTimer.isActive() && !m_pubRebuildInFlight) {
        setState(Active);
        setStatusDetail("Connected");
    }
    return true;
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
    m_subStall.remove(sessionId);   // #bug2 -- fresh baseline for the rebuilt feed
    m_signalQuality.remove(sessionId);
    m_pendingRequestOffers.remove(sessionId);
    m_requestOfferAttempts.remove(sessionId);
    m_requestOfferRejections.remove(sessionId);   // 0.52.7 — fresh rejection budget

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

// A3a — tear down a peer's subscriber WITHOUT re-requesting an offer (unlike
// recoverSubscriber, which rebuilds). For when the peer's signaling session is
// definitively dead (room/leave): its sid will never be offerable again, so we
// drop the zombie + purge all its requestoffer bookkeeping so the retry tick
// stops hammering the MCU with requestoffers it rejects forever. The peer's NEW
// session resubscribes fresh via roomPeerJoined.
void CallManager::dropSubscriber(const QString &sessionId)
{
    VideoFrameProvider *deadProv = nullptr;
    if (auto *dead = m_subscribePipelines.take(sessionId)) {
        deadProv = dead->videoProvider();
        dead->stop();
        dead->deleteLater();   // may be inside this sub's own queued signal
    }
    m_subscriberSids.remove(sessionId);
    m_subStall.remove(sessionId);
    m_signalQuality.remove(sessionId);
    m_pendingRequestOffers.remove(sessionId);
    m_requestOfferAttempts.remove(sessionId);
    m_requestOfferRejections.remove(sessionId);
    m_subscriberRecoveries.remove(sessionId);
    m_neverDecodedRecoveries.remove(sessionId);   // D2 fix
    if (m_remoteVideoProvider && m_remoteVideoProvider == deadProv) {
        m_remoteVideoProvider = nullptr;
        emit remoteVideoProviderChanged();
    }
    if (auto *p = m_participants.value(sessionId)) {
        p->setCamera(nullptr);
        p->setConnState(CallParticipant::Reconnecting);
    }
}

// Camera grace-period auto-retry (backlog: MF 0xc00d36e6 first-enable
// failure). On Windows, mfvideosrc can occasionally fail to open a camera
// the very FIRST time this call tries -- the device is still settling after
// a previous process/session released it, or the request races Windows'
// camera-privacy handshake -- and then opens fine a moment later. Retrying
// blindly forever would mask a genuinely broken/missing camera (busy in
// another app, disabled by policy, unplugged), so this is bounded: at most
// kCameraGraceMaxRetries attempts, short delay between them, and ONLY while
// no frame has ever been decoded for this enable attempt (see
// PublishPipeline::cameraFirstFrameSeen()) -- a camera that WAS streaming
// and then failed is a real mid-call failure and must surface immediately
// (the existing device-hotplug path already resumes it if it comes back).
static constexpr int kCameraGraceMaxRetries   = 2;
static constexpr int kCameraGraceRetryDelayMs = 700;

bool CallManager::buildAndStartPublisher()
{
    // Publisher SID (matches NC Talk: Date.now().toString()). A FRESH sid on
    // every (re)build makes Janus spin a clean publisher session, discarding
    // any half-dead one left from a previous attempt.
    const QString pubSid = QString::number(QDateTime::currentMSecsSinceEpoch());

    qDebug() << "CallManager: creating PublishPipeline...";
    m_publishPipeline = new PublishPipeline(this);
    m_pubStall.reset();   // fresh outbound-RTP baseline for this (re)built publisher
    m_publishPipeline->setBackgroundEngine(m_backgroundEngine);
    m_localVideoProvider = m_publishPipeline->localVideoProvider();
    emit localVideoProviderChanged();

    connect(m_publishPipeline, &PublishPipeline::localOfferReady,
            this, [this, pubSid](const QString &sdp) {
        setStatusDetail("Sending offer to MCU");
        m_signaling->sendOffer(m_signaling->sessionId(), sdp, pubSid);
        qDebug() << "CallManager: sent publish offer to own session, sid=" << pubSid;
    });

    // Self-heal: forced-exact camera caps mfvideosrc can't deliver → no frames;
    // PublishPipeline resets the pick to Auto, we re-arm via permissive nego.
    connect(m_publishPipeline, &PublishPipeline::cameraNegotiationFailed,
            this, [this]() {
        qWarning() << "CallManager: camera negotiation failed — re-arming via Auto/permissive";
        if (!m_publishPipeline) return;
        m_publishPipeline->disableCamera();
        QTimer::singleShot(150, this, [this]() {
            if (!m_publishPipeline) return;
            m_publishPipeline->enableCamera(videoDeviceIndex(), preferHd1080());
        });
    });

    connect(m_publishPipeline, &PublishPipeline::iceCandidateReady,
            this, [this, pubSid](const QString &candidate, int mline, const QString &mid) {
        m_signaling->sendCandidate(m_signaling->sessionId(), makeCandidateJson(candidate, mline, mid), pubSid);
    });

    connect(m_publishPipeline, &PublishPipeline::iceGatheringComplete,
            this, [this, pubSid]() {
        m_signaling->sendEndOfCandidates(m_signaling->sessionId(), pubSid);
    });

    connect(m_publishPipeline, &PublishPipeline::iceStateChanged,
            this, [this](const QString &state) {
        qDebug() << "CallManager: publisher ICE:" << state;
        if (m_state != Active)
            setStatusDetail("Publisher ICE " + state);
        if (state == "connected" || state == "completed") {
            // 0.40.15 — sticky flag for the Connecting→Active race (publisher
            // ICE may connect before participant-joined flips us to Connecting).
            m_pubIceConnectedSeen = true;
            if (m_state == Connecting) {
                setState(Active);
                m_durationTimer.start();
            }
            // Reconnect succeeded — the rebuilt publisher is up. Cancel the
            // pending retry, clear the in-flight guard, return to Active.
            if (m_state == Reconnecting) {
                qInfo() << "CallManager: publisher reconnected (" << state << ") — call resumed";
                // #bug3 -- if the PEER is also mid-grace (a link flap downed
                // both), stay Reconnecting; tryAdoptReturningPeer flips us to
                // Active when the peer returns. Whichever recovery finishes last
                // makes the Active flip.
                if (!m_peerGraceActive) {
                    setState(Active);
                    setStatusDetail("Connected");
                }
            }
            m_pubRetryTimer.stop();
            m_pubRetryAttempts   = 0;
            m_pubRebuildInFlight = false;
        } else if (state == "failed") {
            // A failed edge collateral to a screen-share teardown must never
            // touch the call (#138).
            if (m_screenShareTearingDown) {
                qInfo() << "CallManager: publisher ICE transient 'failed' during "
                           "screen-share teardown — ignored (call stays up)";
                return;
            }
            // This rebuild attempt has concluded (failed); clear the in-flight
            // guard so recoverPublisher() can arm the next backoff. Zoom-style:
            // NEVER auto-drop — enter Reconnecting and keep retrying.
            m_pubRebuildInFlight = false;
            if (m_state == Active || m_state == Connecting)
                setState(Reconnecting);
            recoverPublisher("ice-failed");
        }
    });

    connect(m_publishPipeline, &PublishPipeline::audioLevelUpdated,
            this, &CallManager::onAudioLevelUpdated);

    connect(m_publishPipeline, &PublishPipeline::error, this, [this](const QString &msg) {
        qWarning() << "CallManager: publish pipeline error:" << msg;
        teardown(msg);
    });

    connect(m_publishPipeline, &PublishPipeline::hwVideoEncoderUnavailable, this, [this]() {
        // NVENC can't open on this machine (e.g. an iGPU-pinned 2-GPU/Optimus
        // laptop). The publisher already latched talqAvoidNvenc() in-process;
        // persist it so EVERY future call + launch skips NVENC and uses Intel
        // QSV / software from the start — the machine fails NVENC once, never
        // again.
        QSettings s("TalQ", "TalQ");
        s.setValue("Video/avoidNvenc", talqAvoidNvenc().load());
        s.setValue("Video/forceSoftwareVideo", talqForceSoftwareVideo().load());
        qWarning() << "CallManager: persisted video-encoder latch — avoidNvenc="
                   << talqAvoidNvenc().load()
                   << "forceSoftware=" << talqForceSoftwareVideo().load();
        // B3 — the HW encoder gave out MID-CALL. The latch now forces software,
        // but the LIVE publisher still holds the dead HW encoders and a camera
        // re-toggle reuses them; only a fresh start() picks up the latch. So
        // rebuild the publisher in-call (recoverPublisher -> buildAndStartPublisher
        // -> software x264) instead of leaving video dead "until the next call".
        if (m_cameraOn && (m_state == Active || m_state == Connecting)) {
            qWarning() << "CallManager: rebuilding publisher on SOFTWARE encoder (in-call fallback)";
            setState(Reconnecting);
            m_pubRetryAttempts = 0;
            recoverPublisher("encoder-fallback-software");
        }
    });

    connect(m_publishPipeline, &PublishPipeline::softwareVideoEncoderUsed, this, [this]() {
        // 0.52.5 — persistent sender-visible chip (no working HW encoder → 480p).
        setVideoQualityNotice(tr("Sending a single lower-resolution video stream to keep this device responsive."));
        if (m_softwareEncoderNotified) return;   // toast: once per call (publisher can rebuild)
        m_softwareEncoderNotified = true;
        qInfo() << "CallManager: camera on SOFTWARE video encoder — notifying user";
        emit softwareVideoEncoderNotice();
    });

    // 0.52.5 — the publisher pinned the send at the capability floor under load
    // (only a non-Capable/software box can trip this; a Capable HW box floors at
    // full quality). Show/clear the persistent "limited to 480p" chip on our own
    // screen so the sender is never silently stuck low without knowing why.
    connect(m_publishPipeline, &PublishPipeline::sendQualityFloored, this, [this](bool atFloor) {
        if (atFloor)
            setVideoQualityNotice(tr("Limited to 480p — this device can't keep up at higher quality"));
        else if (m_videoQualityNotice.startsWith(tr("Limited to 480p")))
            setVideoQualityNotice(QString());   // clear only the floor reason; a software notice persists
    });

    connect(m_publishPipeline, &PublishPipeline::cameraError, this, [this](const QString &reason) {
        qWarning() << "CallManager: camera error:" << reason;

        // Grace-period auto-retry: only for a FIRST-ENABLE failure (no self
        // camera frame has ever been decoded this attempt) and only while
        // the user still wants the camera on. Bounded — see the constants'
        // comment above buildAndStartPublisher().
        const bool neverStreamed = m_publishPipeline
            && !m_publishPipeline->cameraFirstFrameSeen();
        if (neverStreamed && m_cameraOn
            && m_cameraGraceRetries < kCameraGraceMaxRetries) {
            ++m_cameraGraceRetries;
            const int deviceIndex = videoDeviceIndex();
            const bool hd1080 = preferHd1080();
            qInfo() << "CallManager: camera first-enable failed (" << reason
                     << ") — grace retry" << m_cameraGraceRetries << "of"
                     << kCameraGraceMaxRetries;
            // Deferred: cameraError can fire from inside
            // PublishPipeline::pollBus() while it iterates the GStreamer
            // bus, so touching the camera inline could re-enter the bus
            // we're popping. disableCamera() releases the MF device handle;
            // the retry re-opens it after a short settle delay.
            QTimer::singleShot(0, this, [this]() {
                if (m_publishPipeline) m_publishPipeline->disableCamera();
            });
            QTimer::singleShot(kCameraGraceRetryDelayMs, this,
                    [this, deviceIndex, hd1080]() {
                // The call may have ended, or the user may have toggled the
                // camera off, while the retry was pending -- both must be
                // respected rather than clobbered by a stale retry.
                if (!m_publishPipeline || !m_cameraOn) return;
                if (m_state == Idle || m_state == Ending) return;
                if (m_publishPipeline->isCameraOn()) return;   // already back up
                qInfo() << "CallManager: camera grace-retry — re-attempting enableCamera";
                m_publishPipeline->enableCamera(deviceIndex, hd1080);
            });
            return;   // don't surface the hard error yet -- give it a chance
        }

        m_cameraOn = false;
        // Idiot-proofing: do NOT stay silent. Flag the device as unavailable
        // so the call surface shows a loud "Camera unavailable" notice with
        // recovery steps, instead of sitting on "Starting camera..." forever.
        m_cameraUnavailable = true;
        m_cameraGraceRetries = 0;   // next manual toggle-on gets a fresh grace budget
        emit cameraChanged();
        // Tell the other side our video is off so their tile reads
        // "Camera off" rather than waiting forever for frames that the
        // failed camera will never send.
        broadcastMediaState("video", false);
        updateCallFlags();
        qDebug() << "CallManager: camera unavailable, continuing audio-only";
        // Reset the publisher's camera chain so a later toggle can RETRY
        // (after the user closes the other app / fixes Windows privacy).
        // Deferred: cameraError can fire from inside PublishPipeline::pollBus()
        // while it iterates the GStreamer bus, so bouncing the camera inline
        // could re-enter the bus we're popping.
        QTimer::singleShot(0, this, [this]() {
            if (m_publishPipeline) m_publishPipeline->disableCamera();
        });
    });

    connect(m_publishPipeline, &PublishPipeline::audioError, this, [this](const QString &reason) {
        qWarning() << "CallManager: microphone error:" << reason;
        // Non-fatal (mirrors cameraError): the publisher already fell back to a
        // SILENT source so the call connected — a mic that won't open must
        // never drop the whole call. Flag it so the surface shows a loud
        // "microphone unavailable" banner instead of pretending we're sending
        // audio, and tell the peer our audio is off so they don't wait on it.
        m_micUnavailable = true;
        // Defer the wire broadcast: audioError can fire synchronously from
        // inside PublishPipeline::start() before the call has fully joined.
        QTimer::singleShot(0, this, [this]() {
            if (m_state == Idle || m_state == Ending) return;
            broadcastMediaState("audio", false);
            updateCallFlags();
        });
    });

    // AEC (0.51.x single-pipeline fix): the webrtcechoprobe now lives INLINE in
    // the publisher pipeline on a single shared real wasapi2sink — so it shares
    // one clock + base-time with webrtcdsp (the actual fix for "runs but doesn't
    // cancel"; the old separate SharedFarEndBus pipeline could never time-align).
    // We just enable AEC + route the shared playout sink to the selected output
    // device BEFORE start(); the publisher builds the tail in start() and
    // degrades to AEC-off (no call drop) if it can't. Each subscriber then feeds
    // its decoded audio into the publisher's far-end mixer (see setFarEndAppsrc).
    {
        QSettings s("TalQ", "TalQ");
        s.beginGroup("Audio");
        const bool aecWanted = s.value("echoCancellation", true).toBool();
        s.endGroup();
        if (aecWanted) {
            m_publishPipeline->setEchoCancellation(true);
            m_publishPipeline->setFarEndOutputDevice(
                m_deviceManager ? m_deviceManager->selectedOutputDeviceId() : QString());
            qInfo() << "CallManager: AEC enabled — inline webrtcechoprobe in the publisher pipeline";
        } else {
            // ⚠ SAY IT LOUDLY. This branch used to be silent, so a machine with
            // Audio/echoCancellation=false ran an ENTIRE CALL with no echo
            // cancellation and nothing in the log said so — you had to notice the
            // ABSENCE of the line above.
            //
            // That matters because of who hears the damage: echo is heard by the
            // OTHER party. If this is off here, THEY hear their own voice come
            // back, and nothing on their side is at fault or can fix it. Measured
            // 2026-08-12: with AEC on, our send path suppresses echo 8-14 dB BELOW
            // the room noise floor and preserves the near-end talker during
            // double-talk (+1.23 dB). With it off, the far end's audio goes
            // straight back out. The difference between "TalQ echoes" and "TalQ is
            // clean" can be this one setting.
            qWarning() << "CallManager: ECHO CANCELLATION IS DISABLED "
                          "(Audio/echoCancellation=false in settings). The OTHER party "
                          "will hear their own voice echoed back from this machine. "
                          "Nothing on their side can fix it — re-enable AEC here.";
        }
    }

    qDebug() << "CallManager: calling PublishPipeline::start()...";
    detectGpuClass();   // re-read the GPU-performance override so a Settings change applies this call
    m_publishPipeline->setGpuClass(m_gpuClass);   // encode-load cap (Capable=none / iGPU=480p / sw=480p+shed)
    // If any screen share is live (publisher rebuild mid-share — local OR a
    // remote peer's), keep the camera simulcast suppressed on the fresh
    // pipeline too. Runs before start() so its build-time layer gate applies.
    updateCameraSuppression();
    if (!m_publishPipeline->start(m_stunServer, effectiveTurnServers(),
        m_deviceManager ? m_deviceManager->selectedInputDeviceId() : QString(),
        m_withVideo, videoDeviceIndex(), preferHd1080())) {
        qWarning() << "CallManager: failed to start publish pipeline";
        return false;
    }
    // Weak-device single-stream notice: fires for ANY weak tier (Software OR
    // WeakIgpu OR a low-core Auto demotion), not just the software-encoder
    // case above (softwareVideoEncoderUsed) which only covers layer-0 x264.
    // isSingleStream() reflects m_singleStream, set inside start(), so it's
    // valid now that start() has returned successfully. Purely additive —
    // no else-clear here (this notice string is shared with other conditions,
    // e.g. the receive-side "High load" notice; existing teardown clears
    // handle removal).
    if (m_publishPipeline && m_publishPipeline->isSingleStream())
        setVideoQualityNotice(tr("Sending a single lower-resolution video stream to keep this device responsive."));
    m_glibTimer.start(20);

    // Camera comes up immediately for a video call (mfvideosrc starts async, so
    // this doesn't block); isCameraOn() prevents a double-enable.
    if (m_cameraOn && m_publishPipeline && !m_publishPipeline->isCameraOn()) {
        qDebug() << "CallManager: enabling camera immediately (video call)";
        m_publishPipeline->enableCamera(videoDeviceIndex(), preferHd1080());
        m_localVideoProvider = m_publishPipeline->localVideoProvider();
        emit localVideoProviderChanged();
    }
    // 0.51.x: arm the dynamic load controller for this call (rebuildPublisherAndReoffer
    // also lands here, so the controller restarts on a publisher reconnect — fine,
    // it just re-ramps from level 0).
    startLoadController();

    // 0.51.x AEC: on a publisher REBUILD (rebuildPublisherAndReoffer also calls
    // this) the fresh pipeline has a new far-end mixer with NO peers — re-point
    // every surviving subscriber at it (each still holds a ref to the now-dead old
    // appsrc, which would silence that peer + kill AEC for the rest of the call).
    // No-op on the initial build (no subscribers yet; onOfferReceived attaches
    // them) and when AEC is off.
    if (m_publishPipeline->aecPlayoutActive()) {
        for (auto it = m_subscribePipelines.constBegin(); it != m_subscribePipelines.constEnd(); ++it) {
            if (!it.value()) continue;
            if (GstElement *src = m_publishPipeline->addFarEndPeer(it.key()))
                it.value()->setFarEndAppsrc(src);
        }
    }
    return true;
}

void CallManager::recoverPublisher(const QString &reason)
{
    // Never ends the call — only schedules the next rebuild. teardown owns all
    // termination. The ONLY exits from Reconnecting are: ICE recovers (→Active),
    // or user Cancel / peer-left / room-closed (→teardown).
    if (m_state != Reconnecting && m_state != Active)
        return;                       // Idle/Ending → teardown handles cleanup
    if (m_pubRebuildInFlight)
        return;                       // a rebuild is mid-flight; don't stack
    if (m_pubRetryTimer.isActive())
        return;                       // next attempt already scheduled

    // Exponential backoff, capped at 30s, INDEFINITE (no attempt cap). First
    // attempt waits 1s — preserves the old "transient blip" tolerance.
    static const int kBackoff[] = { 1000, 2000, 4000, 8000, 15000, 30000 };
    const int delay = kBackoff[qMin(m_pubRetryAttempts, 5)];
    ++m_pubRetryAttempts;
    setStatusDetail(tr("Reconnecting… (attempt %1)").arg(m_pubRetryAttempts));
    qInfo() << "CallManager: publisher reconnect (" << reason << ") — attempt"
            << m_pubRetryAttempts << "in" << delay << "ms (no auto-drop)";
    m_pubRetryTimer.start(delay);
}

void CallManager::rebuildPublisherAndReoffer()
{
    if (m_state != Reconnecting) return;   // recovered or torn down meanwhile

    m_pubRebuildInFlight = true;
    qInfo() << "CallManager: rebuilding publisher (reconnect attempt"
            << m_pubRetryAttempts << ")";

    // Tear down ONLY the publisher, re-entrancy-safe. Disconnect its signals
    // FIRST so a queued stale ICE edge from the dying pipeline can't be mistaken
    // for the new one, then deleteLater (NOT synchronous delete — we may be one
    // hop from its own callbacks; mirrors the bug-11 teardown fix). Subscribers, the
    // GLib timer and the participant registry stay intact — the MCU keeps the
    // room and peer feeds alive across a publisher-only rebuild.
    if (m_publishPipeline) {
        m_publishPipeline->disconnect(this);
        m_publishPipeline->stop();
        m_publishPipeline->deleteLater();
        m_publishPipeline = nullptr;
    }

    // Rebuild on the CACHED STUN/TURN — no network fetch, so this works even
    // while the link is still flapping. Success is signalled asynchronously by
    // the new pipeline's ICE reaching connected (which clears the in-flight
    // guard and returns to Active). If it can't even start, don't drop — arm
    // the next backoff.
    if (!buildAndStartPublisher()) {
        qWarning() << "CallManager: publisher rebuild failed to start — will retry";
        if (m_publishPipeline) {
            m_publishPipeline->disconnect(this);
            m_publishPipeline->stop();
            m_publishPipeline->deleteLater();
            m_publishPipeline = nullptr;
        }
        m_pubRebuildInFlight = false;
        recoverPublisher("rebuild-start-failed");
    }
}

void CallManager::maybeReplyBusy(const QString &callerName, const QString &token)
{
    // A second call rang while we're already in a call. Instead of silently
    // dropping it (old behaviour), let both sides know (#77): a desktop
    // notification for us, and an auto "on another call" chat reply to the
    // caller so they aren't left staring at an unanswered ring. Nextcloud Talk
    // has no native busy signal, so the chat line IS the busy signal (and it
    // works for every client). Guarded so a caller who keeps ringing can't be
    // spammed with repeat replies.
    if (token.isEmpty() || token == m_callToken) return;   // never our own call room
    const QDateTime now = QDateTime::currentDateTime();
    if (token == m_lastBusyReplyToken && m_lastBusyReplyTime.isValid()
        && m_lastBusyReplyTime.msecsTo(now) < 60000)
        return;                                             // once per call, per minute
    m_lastBusyReplyToken = token;
    m_lastBusyReplyTime  = now;
    qInfo() << "CallManager: busy — second incoming call from" << callerName
            << "in" << token << "— notifying self + auto-replying";
    emit busyIncomingCall(callerName, token);
    // Auto chat reply only for 1:1 conversations — an "I'm on another call" line
    // dropped into a busy group room is noise.
    if (m_conversations && m_conversations->conversationTypeForToken(token) == 1)
        emit busyAutoReply(token, tr("\xF0\x9F\x93\x9E On another call right now — I'll get back to you."));
}

void CallManager::onIncomingCallDetected(const QString &callerName, const QString &token, int callFlag)
{
    if (m_state != Idle) {
        maybeReplyBusy(callerName, token);
        return;
    }

    // Cooldown: don't re-detect the same call we just declined
    if (token == m_lastDeclinedToken
        && m_lastDeclinedTime.isValid()
        && m_lastDeclinedTime.msecsTo(QDateTime::currentDateTime()) < 10000) {
        qDebug() << "CallManager: ignoring re-detection of recently declined call" << token;
        return;
    }

    // Cooldown: a call we just TRIED to place that failed (e.g. server 5xx)
    // leaves the room flagged "in call" server-side; the signaling echo would
    // otherwise immediately re-ring us as a phantom incoming call on our own
    // token (showing our own camera self-preview). Suppress that briefly.
    if (token == m_lastOutgoingToken
        && m_lastOutgoingTime.isValid()
        && m_lastOutgoingTime.msecsTo(QDateTime::currentDateTime()) < 10000) {
        qDebug() << "CallManager: ignoring phantom incoming re-ring of just-failed outgoing" << token;
        return;
    }

    // Cooldown: a call we just HUNG UP from. Leaving the call churns the room's
    // participant list; the peer's inCall flag can transiently flip back to 7
    // (0->7 edge) mid-teardown, which reads as "a peer joined the call" and
    // rings us on the token we just left. Suppress for a short window (the flap
    // settles in <1s; use 5s for margin). Field: phantom incoming from the peer
    // ~2s after we hung up.
    if (token == m_lastHangupToken
        && m_lastHangupTime.isValid()
        && m_lastHangupTime.msecsTo(QDateTime::currentDateTime()) < 5000) {
        qDebug() << "CallManager: ignoring phantom incoming re-ring right after hangUp" << token;
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
    // Bounded hand-off — see FrameHandoff.h. This is the incoming-call
    // SELF-preview (camera, via startIncomingCameraPreview), not a screen
    // share — ScreenSharePipeline::m_previewHandoff guards that path. An
    // unbounded queue here is the same class of risk FrameHandoff.h exists to
    // remove: every decoded-frame producer in this app posts through its own
    // bounded gate so no single one can grow without limit.
    if (!self->m_selfPreviewHandoff.tryAcquire()) {
        gst_sample_unref(sample);
        const long long d = self->m_selfPreviewHandoff.dropped();
        if (talq::FrameHandoffGate::isLogWorthy(d))
            qWarning() << "CallManager: self-preview behind — dropped" << d
                       << "frame(s) to bound memory";
        return GST_FLOW_OK;
    }
    QPointer<CallManager> guard(self);
    QMetaObject::invokeMethod(self, [guard, sample]() {
        if (guard) {
            if (guard->m_previewProvider)
                guard->m_previewProvider->feedFrame(sample);
            guard->m_selfPreviewHandoff.release();
        }
        gst_sample_unref(sample);
    }, Qt::QueuedConnection);
    return GST_FLOW_OK;
}

// --- call recording -------------------------------------------------------
//
// TalQ does not record anything itself: the recording backend joins the call
// as a headless participant and records server-side, so the client's whole job
// is to ask for it and to make the state unmistakable to everyone in the room.
// That second half is the important one -- a participant who does not know
// they are being recorded is the failure this feature must never produce, so
// the indicator is driven off the ROOM's state (which every participant sees)
// rather than off whether this client started it.

int CallManager::recordingState() const
{
    if (!m_conversations || m_callToken.isEmpty()) return 0;
    return m_conversations->callRecordingForToken(m_callToken);
}

void CallManager::startRecording(bool video)
{
    if (m_callToken.isEmpty() || !canControlRecording()) return;
    // Room::RECORDING_VIDEO = 1, RECORDING_AUDIO = 2 (Room.php:54-55).
    const int status = video ? 1 : 2;
    const QString token = m_callToken;
    m_api->startRecording(token, status, [this](bool ok, const QJsonObject &data, int) {
        if (ok) return;
        // Surface the server's own reason rather than a generic failure: the
        // 400 body carries {"error": "..."} and the most likely values are
        // about configuration, which the user can act on.
        const QString err = data.value(QStringLiteral("error")).toString();
        // Logged, not raised: there is no in-call notice channel for a
        // non-fatal control failure, and callFailed() would read as the CALL
        // having died. The visible consequence is simply that the recording
        // indicator never lights. Surfacing this in the call UI is a follow-up.
        qWarning() << "recording: start refused --" << err;
    });
}

void CallManager::stopRecording()
{
    if (m_callToken.isEmpty() || !canControlRecording()) return;
    m_api->stopRecording(m_callToken, [this](bool ok, const QJsonObject &data, int) {
        if (ok) return;
        const QString err = data.value(QStringLiteral("error")).toString();
        qWarning() << "recording: stop refused --" << err;
    });
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
    m_callJoinAttempts = 0;
    m_peerGraceActive = false; m_peerGraceTimer.stop();   // #bug3 -- fresh call, no stale grace
    m_cameraUnavailable = false;   // fresh call: clear any prior failure
    m_cameraGraceRetries = 0;      // fresh call: fresh grace-retry budget
    m_micUnavailable = false;      // fresh call: clear any prior mic failure
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
    // Stamp the token we're leaving so a teardown-flap can't immediately re-ring
    // us as a phantom incoming call. When we hang up, the room's participant list
    // churns: the peer's inCall flag can flip 7->0->7->0 within ~150ms as both
    // sides leave, and the transient 0->7 edge looks exactly like "a peer just
    // joined the call = incoming ring". Suppress re-detection on this token for a
    // short window (see onIncomingCallDetected). Field: Kalin↔Ilko, a phantom
    // incoming call from Ilko fired ~2s after Kalin hung up.
    m_lastHangupToken = m_callToken;
    m_lastHangupTime  = QDateTime::currentDateTime();
    // #79 -- also stash the peer session(s) we were in a call with, BEFORE
    // teardown() clears them. A post-hangup inCall flap from any of these is a
    // teardown phantom even when observed under a different room token (after we
    // rejoin the viewed room), which the token-keyed cooldown would miss.
    m_lastHangupSids = m_peerInCallSids;
    if (!m_remoteSessionId.isEmpty()) m_lastHangupSids.insert(m_remoteSessionId);
    // Tell the peer this is a DELIBERATE hangup so a 1:1 MCU call ends on their
    // side immediately, instead of waiting out the 28s peer-grace "Reconnecting"
    // hold (which exists to survive a transient drop and can't distinguish the
    // two). 1:1 only — group calls already end promptly on the peer. Sent
    // BEFORE teardown(): hanging up leaves the CALL but stays in the room, so the
    // signaling socket is still open. Best-effort — if it's lost, the peer falls
    // back to grace→timeout (no regression).
    if (isOneToOneCall() && !m_remoteSessionId.isEmpty()) {
        QJsonObject data;
        data["type"] = QString("hangup");
        // #bug4 -- the peer user may hold several sessions; the adopted sid is
        // not necessarily the device they are looking at. Hint them all
        // (best-effort, TalQ-private; non-TalQ siblings ignore it).
        QSet<QString> targets = m_peerInCallSids;
        targets.insert(m_remoteSessionId);
        for (const QString &sid : targets)
            m_signaling->sendMinimalMessage(sid, data);
        qDebug() << "CallManager: sent hangup hint to" << targets.size() << "peer session(s)";
    }
    teardown("Hung up");
}

void CallManager::onPeerHungUp(const QString &sessionId)
{
    // The peer told us they hung up on purpose. End a 1:1 MCU call now rather
    // than waiting out the peer-grace hold. Match either our current peer or the
    // session we just entered grace for (participantLeftCall may have arrived
    // first and cleared m_remoteSessionId / set m_graceLeftSid). Ignore for
    // a group call or a stranger so a stray hint can't drop the wrong call.
    if (m_state == Idle) return;
    if (!isOneToOneCall()) return;
    // #bug4 -- any device of the 1:1 peer: the hint may arrive from a sibling
    // session that isn't the one we currently subscribe.
    if (sessionId != m_remoteSessionId && sessionId != m_graceLeftSid
        && !isPeerUserSession(sessionId)) return;
    qInfo() << "CallManager: peer signalled hangup — ending 1:1 call immediately";
    teardown("Call ended");
}

void CallManager::toggleMute() {
    m_muted = !m_muted;
    if (m_publishPipeline) m_publishPipeline->setMuted(m_muted);
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
    // Turning the camera back on is a retry: clear the "unavailable" notice
    // so the banner/caption disappear. If the device fails again, the
    // PublishPipeline::cameraError path re-raises the flag. Also give it a
    // fresh grace-retry budget, same as a brand-new call.
    if (m_cameraOn) { m_cameraUnavailable = false; m_cameraGraceRetries = 0; }
    emit cameraChanged();   // UI button reflects the intent immediately

    // D3 — COALESCE the actual device enable/disable. Fast mute-mashing (Pavel
    // toggled the camera ~10× in one call) otherwise fires a burst of
    // enable/disable on the exclusive MF source, where an async NULL can overtake
    // a later sync PLAYING and park the device mid-transition (camera wedged off
    // while the UI shows it on). Apply only the FINAL desired state ~250 ms after
    // the last click; the single-shot timer restart collapses the burst.
    m_cameraApplyTimer.start();
}

// D3 — apply the coalesced camera desired-state to the live pipeline. Idempotent:
// enableCamera/disableCamera early-return if already in that state.
void CallManager::applyCameraState() {
    // A stray fire (timer scheduled then the call ended) must never touch a fresh
    // call's media — teardown stops the timer, this is belt-and-suspenders.
    if (m_state != Active && m_state != Connecting && m_state != Reconnecting) return;
    if (m_publishPipeline) {
        if (m_cameraOn) {
            m_publishPipeline->enableCamera(videoDeviceIndex(), preferHd1080());
            m_localVideoProvider = m_publishPipeline->localVideoProvider();
            m_cameraIntentTimer.start();   // #81 -- watch for a silent no-frame enable
        } else {
            m_publishPipeline->disableCamera();
            m_localVideoProvider = nullptr;
            m_cameraIntentTimer.stop();
        }
        emit localVideoProviderChanged();
    }
    // Broadcast video state + update call flags on server (coalesced with the
    // media apply so peers see only the final state, not every intermediate flip).
    broadcastMediaState("video", m_cameraOn);
    updateCallFlags();
}

void CallManager::cameraFrameConfirmed()
{
    // A real self-camera frame is proof the device recovered after an earlier
    // cameraError. Clear the "camera unavailable" notice (idempotent).
    if (!m_cameraUnavailable) return;
    m_cameraUnavailable = false;
    qDebug() << "CallManager: self-camera frame seen — clearing 'camera unavailable' notice";
    emit cameraChanged();
}

void CallManager::updatePowerInhibit()
{
    // A call must not be interrupted by the screensaver or by the machine
    // sleeping. The display is only held on when there is something to LOOK at
    // — camera on, we are sharing, or a peer is sharing to us — so an
    // audio-only call still lets the screen turn off.
    const bool inCall = m_state != Idle;
    const bool needsDisplay = inCall && (m_cameraOn || m_screenSharing
                                         || !m_screenSubscribers.isEmpty());
    m_powerInhibit.update(inCall, needsDisplay);
}

void CallManager::updateCameraSuppression()
{
    // Piggy-backs on every trigger that already recomputes share state (local
    // share start/stop, remote-screen change, publisher rebuild), and sits
    // BEFORE the m_publishPipeline guard below so it still runs when there is
    // no publisher — e.g. an audio-only call.
    updatePowerInhibit();

    // Any screen share in the room — ours OR a remote peer's — reduces every
    // camera to a small PIP, so drop our camera to a single LOW simulcast layer
    // for its duration. This cuts encode load on every sender AND, just as
    // importantly, decode load on the sharer: a weak/iGPU box already maxed
    // encoding the screen then only has a 180p peer camera to decode instead of
    // a full 1080p one. setScreenShareSuppression() early-outs when unchanged,
    // so calling this redundantly from every trigger (local share start/stop,
    // remote-screen change, publisher rebuild) is safe.
    if (!m_publishPipeline)
        return;
    // Drive off the SET of active remote screen subscribers, not the single
    // render pointer m_remoteScreenProvider: with two peers sharing, one stopping
    // nulls that pointer while the other is still sharing — keying on the map
    // keeps us correctly suppressed until the LAST remote share ends.
    const bool suppress = m_screenSharing || !m_screenSubscribers.isEmpty();
    m_publishPipeline->setScreenShareSuppression(suppress);
}

void CallManager::removeScreenSubscriber(const QString &sessionId)
{
    auto *sub = m_screenSubscribers.take(sessionId);
    m_screenSubFrameMark.remove(sessionId);    // peer's screen gone — drop
    m_screenSubStallTicks.remove(sessionId);   // frame-liveness state
    m_screenSubBuiltMs.remove(sessionId);      // startup-grace stamp
    m_screenSubIceState.remove(sessionId);     // ICE-progress state
    m_screenSubCompletedMs.remove(sessionId);  // keyframe-budget stamp
    m_screenSubFailRetries.remove(sessionId);  // ICE-failed retry budget
    m_pendingScreenSubCandidates.remove(sessionId);  // queued early candidates
    // Clear the peer's share flags BEFORE the no-subscriber early-return below.
    // These used to be cleared only at the END of this function, so a call that
    // found no subscriber (already torn down by the stall-watchdog rebuild, or a
    // duplicate/late unshareScreen) returned with screenSharing() still true and
    // a DANGLING screen() provider. CallStage::stageSource() matches a sharer on
    // exactly `p->screenSharing() && p->screen()`, so the stage stayed wedged in
    // share mode forever — repainting freed memory every frame.
    if (auto *p = m_participants.value(sessionId)) {
        p->setScreen(nullptr);
        p->setScreenSharing(false);
    }
    if (!sub)
        return;                       // no remote screen from this peer — nothing to do
    // If this peer's screen was the one being rendered, unbind it so the UI
    // stops painting a dead feed.
    if (m_remoteScreenProvider && m_remoteScreenProvider == sub->videoProvider()) {
        // Broad disconnect() is INTENTIONAL here: this provider is about to be
        // deleteLater()'d with its subscriber, so every painter bound to it must
        // stop now (it is being destroyed, not merely reassigned).
        m_remoteScreenProvider->disconnect();
        m_remoteScreenProvider = nullptr;
        // If ANOTHER peer is still sharing, switch the single render slot to it
        // rather than going blank (TalQ shows one remote screen at a time).
        if (!m_screenSubscribers.isEmpty()) {
            auto it = m_screenSubscribers.begin();
            m_remoteScreenProvider = it.value()->videoProvider();
            if (auto *np = m_participants.value(it.key()))
                np->setScreen(m_remoteScreenProvider);
        }
        emit remoteScreenProviderChanged();
    }
    sub->stop();
    sub->deleteLater();
    updateCameraSuppression();        // a sharer vanished — recompute (don't latch LOW)
}

void CallManager::dispatchScreenSendoffer()
{
    // A peer can only learn about an ongoing screen share from this message —
    // there is no screen-share participant flag to poll. Only announce while we
    // are genuinely sharing (a queued re-assert timer may fire after the user
    // stopped, or after a different pipeline replaced this one).
    if (!m_screenSharing || !m_screenSharePipeline) return;
    // Notify every known call participant, not just peers we currently have
    // a PRIMARY subscribe pipeline for. Restricting to m_subscribePipelines
    // makes this announcement fragile to exactly the kind of transient
    // subscribe-bookkeeping gap a signaling-layer hiccup (a stale room
    // reference, a slow reconnect) can cause -- if OUR OWN subscribe to a
    // peer isn't tracked yet at the moment we start sharing, that peer would
    // silently never be told a screen offer is available at all (field:
    // Kalin -> Ilko share never arriving despite the primary call otherwise
    // working). m_participants is the full room roster and doesn't depend
    // on our own subscribe state being in sync.
    QSet<QString> peers(m_subscribePipelines.keyBegin(), m_subscribePipelines.keyEnd());
    for (auto it = m_participants.constBegin(); it != m_participants.constEnd(); ++it) {
        // Skip a participant we already know is dead: onParticipantLeftCall
        // normally prunes m_participants, but if that leave event was lost in
        // the same signaling hiccup this widening exists to survive, a stale
        // (Failed) session can linger. A participant with a LIVE subscribe
        // pipeline is in-call by construction and always included above; the
        // roster additions here are the peers we haven't subscribed yet, so
        // only exclude the clearly-dead ones rather than re-target a ghost.
        if (const auto *p = it.value(); p && p->connState() == CallParticipant::Failed
            && !m_subscribePipelines.contains(it.key()))
            continue;
        peers.insert(it.key());
    }
    for (const QString &peerId : std::as_const(peers)) {
        QJsonObject data;
        data["type"] = QString("sendoffer");
        data["roomType"] = QString("screen");
        m_signaling->sendMinimalMessage(peerId, data);
        qInfo() << "CallManager: sent sendoffer screen to" << peerId.left(20);
    }
}

void CallManager::startScreenShare(int monitorIndex, quintptr windowHandle, bool presentation)
{
    if (m_state != Active && m_state != Connecting) return;

    // Remember the target so a confirm-timeout retry / queued back-to-back
    // start rebuilds the SAME screen/window without re-prompting.
    m_ssMonitorIndex = monitorIndex;
    m_ssWindowHandle = windowHandle;
    // Per-share, remembered for the same reason: a confirm-timeout retry must
    // rebuild at the SAME quality the user asked for. Never written back to
    // Video/screenShareQuality — the toggle lifts the clamp, it is not a
    // preference change.
    m_ssPresentation = presentation;

    // #share-reliability — gate the start through the policy. StartQueued means
    // a previous share is still releasing its capture device; the released()
    // handler fires this start once it's safe to re-acquire (the back-to-back
    // "wait several seconds between shares" fix). Anything other than StartNow
    // (already starting/confirming/active) is ignored — no double-start.
    const ShareAction act = m_sharePolicy.requestStart();
    if (act == ShareAction::StartQueued) {
        qInfo() << "CallManager: screen share queued behind teardown";
        return;
    }
    if (act != ShareAction::StartNow)
        return;

    // Fresh user-initiated share: optimistic — probe hardware again. The HW-
    // release-window check in buildAndStartSharePipeline() (or a later
    // confirm-timeout retry) re-forces software when a collision is likely.
    // Deliberately AFTER the policy gate: a StartQueued fired from a retry
    // teardown keeps the retry's software decision.
    m_ssForceSwEncoder = false;

    m_screenSharing = true;
    buildAndStartSharePipeline(monitorIndex, windowHandle);
    if (!m_screenSharePipeline)
        return;   // build failed; already cleaned up + policy reset

    // Stop the camera's simulcast while sharing — the share is a second encode
    // on top of the camera layers and the two together stall a weak/iGPU box.
    // m_screenSharing is already true above, so the helper resolves to suppress.
    updateCameraSuppression();

    // The monitor border is shown inside buildAndStartSharePipeline() so the
    // initial start, a confirm-timeout retry, and a queued back-to-back start
    // all frame the screen uniformly.

    emit screenShareChanged();
}

talq::ShareCap CallManager::screenShareCapForLevel(int level, bool forceSwEncoder) const
{
    const talq::ShareCap cap = talq::screenShareCap(
        level, talq::screenTierMaxHeight(m_gpuClass), forceSwEncoder);
    if (cap.tierClamped)
        qInfo().nospace() << "CallManager: GPU class " << talq::gpuClassName(m_gpuClass)
                          << " -- capping screen share to " << cap.w << "x" << cap.h
                          << " (weak/iGPU encoder)";
    if (cap.swClamped)
        qInfo().nospace() << "CallManager: software screen encode — capping "
                             "capture to " << cap.w << "x" << cap.h;
    return cap;
}

// Build, wire and start a fresh screen-share pipeline. Used for the initial
// start and for each confirm-timeout retry / queued back-to-back start, so the
// retry path reuses the exact proven signaling wiring.
void CallManager::buildAndStartSharePipeline(int monitorIndex, quintptr windowHandle)
{
    // Never overwrite a live pipeline -- that orphans it (leaking its glib
    // timer + wired signals). Guards the race where a stale deferred-rebuild
    // timer (kShareDeviceSettleMs) fires after a new share was already started
    // within the settle window (#0.47.0 review).
    if (m_screenSharePipeline) {
        qWarning() << "CallManager: buildAndStartSharePipeline called with a "
                      "live pipeline -- ignoring (stale rebuild?)";
        return;
    }

    // Default 1080p (level 1). This was 1440p (level 2) while the CAMERA
    // defaulted to 720p; that asymmetry starved a full-screen 2K scroll of bits
    // at the ~12 Mbps ceiling and showed up as smearing (field 2026-07-28) —
    // the artifacts were bit starvation, not latency. Halving the framerate to
    // 15 alongside this roughly doubles the bits per frame, which is what a
    // scroll actually needs; text is sharper for it. Presentation mode restores
    // the user's full level at 30 fps for slides and video.
    //
    // The 2026-06-04 argument for 1440p (hi-DPI window shares soften at 1080p)
    // still holds for WINDOW shares, which is exactly what Presentation mode is
    // for — it is now a per-share choice instead of a default everyone pays.
    // Existing users keep their saved choice; only the default moves.
    m_ssQuality = QSettings("TalQ", "TalQ")
                      .value("Video/screenShareQuality", 1).toInt();

    m_screenSharePipeline = new ScreenSharePipeline(this);

    // #reshare-hw-collision — force the SOFTWARE encoder ONLY as a genuine
    // last-resort backstop: a confirm-timeout Retry (m_ssForceSwEncoder) proved
    // the HW attempt sent no RTP. The old PROACTIVE "re-share within 60 s →
    // software" window was a MISDIAGNOSIS — it forced x264/1080p on shares whose
    // HW encoder actually worked. The real failure was our own outbound-RTP
    // confirm FALSE-NEGATIVE (bursty static-screen RTP never made the old
    // "2 consecutive rises" streak; fixed in ScreenSharePipeline::pollOutboundRtp
    // with a cumulative-delta test): it tore down a LIVE HW share, and only the
    // retry that followed actually collided the encoder session. With the
    // confirm fixed, HW re-shares confirm on the first attempt at native res;
    // software appears only if a real HW attempt genuinely produces no RTP.
    const bool forceSwEncoder = m_ssForceSwEncoder;
    if (forceSwEncoder) {
        qInfo() << "CallManager: forcing SOFTWARE screen encoder — confirm-timeout "
                   "retry (HW produced no RTP)";
        m_screenSharePipeline->setForceSoftwareEncoder(true);
    }
    {
        // Level -> pre-encode downscale cap (set before start(), which the
        // pipeline reads once). Full decision chain (base cap -> 4K hard
        // ceiling -> GPU-tier clamp -> software-fallback 1080p clamp) + the
        // field rationale: core/ShareCapPolicy.h.
        const talq::ShareQuality sq = talq::shareQualityFor(m_ssQuality, m_ssPresentation);
        const talq::ShareCap cap = screenShareCapForLevel(sq.level, forceSwEncoder);
        m_screenSharePipeline->setQualityCap(cap.w, cap.h);
        // Framerate must be set before start() — it is baked into the appsrc caps.
        m_screenSharePipeline->setShareFps(sq.fps);
        qInfo().nospace() << "CallManager: share quality level " << sq.level
                          << " @ " << sq.fps << " fps (presentation="
                          << (m_ssPresentation ? "on" : "off")
                          << ", stored level " << m_ssQuality << ")";
    }

    connect(m_screenSharePipeline, &ScreenSharePipeline::localOfferReady,
            this, [this](const QString &sdp) {
        // Keep the sid STABLE across a confirm-timeout retry. The retry does
        // NOT unshare, so the server-side screen publisher SURVIVES — and the
        // signaling server pins it to the FIRST sid it saw, rejecting ICE
        // candidates for any other ("candidate message sid (X) does not match
        // publisher sid (Y)", hub.go:3019, storm log 2026-07-08) — a retry
        // under a fresh sid could never converge no matter what the encoder
        // did. A re-offer with the MATCHING sid takes the server's update()
        // path instead. stopScreenShare() clears the sid, so a genuinely new
        // share still gets a fresh signaling identity.
        if (m_screenShareSid.isEmpty())
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
            // ICE up is HALF the confirmation; the policy marks the share live
            // only once outbound RTP is also flowing (mediaFlowing below).
            if (m_sharePolicy.onIceConnected() == ShareAction::Confirmed)
                onShareConfirmed();
            // Announce the share to peers (HPB creates a subscriber for each).
            qInfo() << "CallManager: screen pub connected — dispatching "
                       "sendoffer to" << m_subscribePipelines.size() << "peer(s)";
            dispatchScreenSendoffer();

            // Re-share discovery race: when this share started within the
            // server's reap window of a prior unshare, the sendoffer above
            // likely raced the still-closing publisher slot and was dropped —
            // the peer ends up stuck on "starting share…" with a dead feed and
            // no fallback (discovery is sendoffer-only). Re-assert a couple of
            // times so the peer re-discovers once the reap completes. Gated on
            // a recent unshare so NORMAL shares never resend (a resend tears
            // down + rebuilds a healthy remote view — fine for a stuck/absent
            // one, a needless flicker for a working one). The user's manual
            // "wait ~10 s before re-sharing" workaround is what this automates.
            if (m_lastUnshareTimer.isValid() && m_lastUnshareTimer.elapsed() < 15000) {
                qInfo() << "CallManager: re-share within reap window — "
                           "re-asserting screen sendoffer at +5s and +11s";
                // Stamp with THIS share's sid: if the user stops and starts
                // ANOTHER share before these fire, m_screenShareSid changes (it
                // is set per share on localOfferReady and cleared on stop), so a
                // stale re-assert bails instead of forcing the now-current
                // share's healthy remote view to rebuild.
                const QString sid = m_screenShareSid;
                QTimer::singleShot(5000,  this, [this, sid]{
                    if (m_screenShareSid == sid) dispatchScreenSendoffer(); });
                QTimer::singleShot(11000, this, [this, sid]{
                    if (m_screenShareSid == sid) dispatchScreenSendoffer(); });
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

    // #share-reliability — the two reliability signals. mediaFlowing() is the
    // other half of the confirmation (outbound RTP is climbing); released()
    // tells us the async teardown finished and the capture device is free.
    connect(m_screenSharePipeline, &ScreenSharePipeline::mediaFlowing,
            this, [this]() {
        if (m_sharePolicy.onMediaFlowing() == ShareAction::Confirmed)
            onShareConfirmed();
    });
    connect(m_screenSharePipeline, &ScreenSharePipeline::released,
            this, &CallManager::onSharePipelineReleased);

    connect(&m_glibTimer, &QTimer::timeout, m_screenSharePipeline, &ScreenSharePipeline::pollBus);

    if (!m_screenSharePipeline->start(m_stunServer, effectiveTurnServers(), monitorIndex, windowHandle)) {
        qWarning() << "CallManager: failed to start screen share pipeline";
        m_screenSharing = false;
        // start() can emit error() synchronously (DirectConnection ->
        // stopScreenShare), which may already have deleteLater'd + nulled the
        // pipeline before we reach here -- don't deref a null (pre-existing).
        if (m_screenSharePipeline) {
            m_screenSharePipeline->deleteLater();
            m_screenSharePipeline = nullptr;
        }
        m_sharePolicy.requestStop();   // back to a clean Idle
        return;
    }

    // Pipeline is PLAYING — now wait for the protocol to confirm the share is
    // actually live (ICE connected AND outbound RTP flowing). Arm the bounded
    // confirm timer; if confirmation doesn't arrive the policy retries a fresh
    // pipeline, which is what makes a share start reliably instead of silently
    // failing and forcing the user to try again.
    m_sharePolicy.onPipelineStarted();
    m_shareConfirmArmed = true;
    m_shareConfirmTimer.start();

    // #72 -- frame the shared MONITOR with a thin, semi-transparent border,
    // gated on the user setting; monitor shares only (window shares stay
    // frameless). Done here (not only in startScreenShare) so a confirm-timeout
    // retry or a queued back-to-back start re-shows the frame for its target
    // rather than dropping it. showForMonitor is idempotent on re-show.
    if (ShareOverlay::shouldShowForShare(
            QSettings("TalQ", "TalQ")
                .value("Video/screenShareBorder", true).toBool(),
            windowHandle)) {
        if (!m_shareOverlay)
            m_shareOverlay = new ShareOverlay();
        m_shareOverlay->showForMonitor(monitorIndex);
    }
}

void CallManager::requestPeerVideoQuality(const QString &sessionId, int substream, bool manual)
{
    if (substream < 0 || substream > 2) return;
    // `substream` is the tile-size-driven WANT. Remember it raw so a later
    // load-cap change (or a focus change) can be re-applied without the UI
    // re-deciding, then send the load-capped effective value.
    m_peerSubstreamWant[sessionId] = substream;
    // A MANUAL pick (the user explicitly chose a quality) PINS this peer past
    // the auto load controller; Auto (manual=false) clears the pin so the
    // controller governs again. Without this the load cap silently reverted the
    // user's choice every tick (the "selecting quality does nothing" bug).
    if (manual) m_peerManualSubstreamOverride[sessionId] = substream;
    else        m_peerManualSubstreamOverride.remove(sessionId);
    sendDesiredSubstream(sessionId, effectiveSubstreamFor(sessionId, substream));
}

int CallManager::effectiveSubstreamFor(const QString &sessionId, int want) const
{
    // A MANUAL receive-quality pick overrides the auto load controller entirely:
    // the user chose it, so never cap it (the load cap only governs the AUTO /
    // tile-size path). Cleared when the user returns to Auto.
    const int manual = m_peerManualSubstreamOverride.value(sessionId, -1);
    if (manual >= 0) return manual;
    // No receive-load cap in force → honour the tile-size want verbatim.
    if (m_recvLoadSubstreamCap >= 2) return want;
    // Focused tile = the peer the UI wants at the HIGHEST substream (largest
    // tile / stage). Keyed off tile size, NOT the peer-reported speaking flag
    // (unreliable on the webrtcsrc path). Exempt it unless the controller has
    // escalated to capping the focused tile too (top rungs only).
    if (!m_recvLoadCapFocused && isFocusedPeer(sessionId)) return want;
    return want < m_recvLoadSubstreamCap ? want : m_recvLoadSubstreamCap;
}

bool CallManager::isFocusedPeer(const QString &sessionId) const
{
    // "Focused" means ON THE STAGE — the big tile(s) the user is actually looking at.
    // CallStage publishes that set on every relayout (setStagePeers).
    //
    // This used to be focusedPeer(): the single peer with the highest tile-size want,
    // ties broken by arbitrary QHash order. That was fine while exactly one tile could
    // be on the stage, but the active-speaker layout puts TWO cameras side by side,
    // both wanting HIGH — so one got exempted from the receive-load cap and the other
    // was silently de-ressed, non-deterministically.
    //
    // Deriving "focused" from a tie at the max want instead would be WORSE, not better:
    // in the even gallery every remote asks for the same substream, so they would ALL
    // tie, ALL be exempt, and the sub-focused rung of the load controller would shed
    // exactly nothing — disarming it on precisely the weak/iGPU receivers it exists to
    // protect. An explicit stage set has none of that ambiguity: an even gallery has no
    // stage, so nothing is exempt and the cap bites, which is the intent.
    return m_stagePeers.contains(sessionId);
}

void CallManager::setStagePeers(const QSet<QString> &peers)
{
    m_stagePeers = peers;
}

void CallManager::sendDesiredSubstream(const QString &sessionId, int substream)
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

void CallManager::applyReceiveLoadCaps(int substreamCap, bool capFocused)
{
    substreamCap = qBound(0, substreamCap, 2);
    if (substreamCap == m_recvLoadSubstreamCap && capFocused == m_recvLoadCapFocused)
        return;  // idempotent — no change
    m_recvLoadSubstreamCap = substreamCap;
    m_recvLoadCapFocused   = capFocused;
    // Re-apply to every peer we have a remembered want for (isFocusedPeer() is
    // stable across this loop — m_peerSubstreamWant isn't mutated here).
    for (auto it = m_peerSubstreamWant.constBegin(); it != m_peerSubstreamWant.constEnd(); ++it)
        sendDesiredSubstream(it.key(), effectiveSubstreamFor(it.key(), it.value()));
}

// Parse host+port out of a TURN/STUN url. Accepts "turn:host:port?transport=udp",
// "turns://user@host:port", "stun:host:port" — with or without scheme //, user@, or query.
static bool parseTurnHostPort(const QString &url, QString &host, quint16 &port)
{
    QString s = url.trimmed();
    for (const char *scheme : {"turns://","turn://","stuns://","stun://","turns:","turn:","stuns:","stun:"})
        if (s.startsWith(QLatin1String(scheme))) { s = s.mid(int(qstrlen(scheme))); break; }
    const int at = s.indexOf(QLatin1Char('@')); if (at >= 0) s = s.mid(at + 1);
    const int q  = s.indexOf(QLatin1Char('?')); if (q  >= 0) s = s.left(q);
    const int sl = s.indexOf(QLatin1Char('/')); if (sl >= 0) s = s.left(sl);
    port = 3478;
    if (s.startsWith(QLatin1Char('['))) {                    // bracketed IPv6
        const int rb = s.indexOf(QLatin1Char(']')); if (rb < 0) return false;
        host = s.mid(1, rb - 1);
        const int c = s.indexOf(QLatin1Char(':'), rb);
        if (c >= 0) port = quint16(s.mid(c + 1).toUInt());
    } else {
        const int c = s.lastIndexOf(QLatin1Char(':'));
        if (c >= 0) { host = s.left(c); port = quint16(s.mid(c + 1).toUInt()); }
        else host = s;
    }
    if (port == 0) port = 3478;
    return !host.isEmpty();
}

// Build a 20-byte STUN Binding Request (RFC 5389): type 0x0001, length 0,
// magic cookie 0x2112A442, and a random 96-bit transaction id (returned in txid
// so the response can be matched). Every STUN/TURN server answers this on
// UDP:3478 unauthenticated, so the request→response round-trip is the true RTT.
static QByteArray makeStunBindingRequest(QByteArray &txid)
{
    QByteArray req(20, char(0));
    req[0] = char(0x00); req[1] = char(0x01);                                   // Binding Request
    req[2] = char(0x00); req[3] = char(0x00);                                   // message length = 0
    req[4] = char(0x21); req[5] = char(0x12); req[6] = char(0xA4); req[7] = char(0x42);  // magic cookie
    txid.resize(12);
    for (int i = 0; i < 12; ++i) txid[i] = char(QRandomGenerator::global()->bounded(256));
    req.replace(8, 12, txid);
    return req;
}

// UDP-STUN-probe every offered TURN host and keep only the nearest, so a TURN
// relay (when ICE needs one) is always local instead of cross-continent.
// Why STUN-over-UDP and not a TCP connect to :3478: coturn commonly filters TCP
// :3478 (→ the old probe got no answer and reported "best RTT -1", then fell
// back to ALL relays — the far-relay detour that shredded Ivan's send), and a
// TCP-connect time folds in the 3-way handshake + DNS. A STUN Binding round-trip
// on UDP:3478 is answered by every TURN server and measures the real RTT. DNS is
// resolved first and excluded from the timing. Async + non-blocking on the main
// thread; runs when the TURN list arrives on room-join. effectiveTurnServers()
// returns the full list until this resolves.
void CallManager::probeNearestTurnAsync()
{
    m_turnProbed = false;
    m_nearestTurnServers.clear();
    const int gen = ++m_turnProbeGen;
    if (m_turnServers.size() <= 1) { m_nearestTurnServers = m_turnServers; m_turnProbed = true; return; }

    // Probe state lives in a shared_ptr owned by the timer functor below. If THIS
    // CallManager is destroyed within the window, Qt destroys the (context=this)
    // functor, which releases the shared_ptr and frees the vector — no leak. The
    // sockets are parented to `this`, so ~QObject cleans them up on that same path,
    // and the readyRead / lookupHost lambdas are context=this so they never fire
    // after destruction.
    struct Probe { QString host; quint16 port; QUdpSocket *sock = nullptr; QByteArray txid; QElapsedTimer t; bool sent = false; int rtt = -1; };
    auto probes = std::make_shared<std::vector<Probe>>();
    QSet<QString> seen;
    for (const TurnServer &ts : m_turnServers)
        for (const QString &url : ts.urls) {
            QString h; quint16 p;
            if (!parseTurnHostPort(url, h, p) || seen.contains(h)) continue;
            seen.insert(h);
            Probe pr; pr.host = h; pr.port = p;
            probes->push_back(pr);
        }
    if (probes->empty()) { m_nearestTurnServers = m_turnServers; m_turnProbed = true; return; }

    for (Probe &pr : *probes) {                          // vector is fully built -> &pr is stable
        pr.sock = new QUdpSocket(this);
        Probe *prp = &pr;
        QObject::connect(prp->sock, &QUdpSocket::readyRead, this, [this, prp, gen]() {
            if (gen != m_turnProbeGen) return;
            while (prp->sock && prp->sock->hasPendingDatagrams()) {
                QByteArray buf(int(prp->sock->pendingDatagramSize()), char(0));
                prp->sock->readDatagram(buf.data(), buf.size());
                // Any STUN response echoing our 96-bit transaction id (bytes 8..19).
                if (prp->sent && prp->rtt < 0 && buf.size() >= 20 && buf.mid(8, 12) == prp->txid)
                    prp->rtt = int(prp->t.elapsed());
            }
        });
        // Resolve host → IP first (NOT counted in the RTT), then fire the STUN probe.
        QHostInfo::lookupHost(pr.host, this, [this, prp, gen](const QHostInfo &info) {
            if (gen != m_turnProbeGen || !prp->sock || info.addresses().isEmpty()) return;
            const QByteArray req = makeStunBindingRequest(prp->txid);
            prp->t.start();
            prp->sent = true;
            prp->sock->writeDatagram(req, info.addresses().first(), prp->port);
        });
    }

    QTimer::singleShot(1000, this, [this, gen, probes]() {
        auto cleanup = [&] {
            for (Probe &pr : *probes) if (pr.sock) { pr.sock->disconnect(); pr.sock->close(); pr.sock->deleteLater(); }
        };
        if (gen != m_turnProbeGen) { cleanup(); return; }   // a newer probe superseded this one
        int best = std::numeric_limits<int>::max();
        QHash<QString,int> rttByHost;
        for (Probe &pr : *probes) {
            rttByHost[pr.host] = pr.rtt;
            if (pr.rtt >= 0 && pr.rtt < best) best = pr.rtt;
        }
        // Narrow the TURN list to the nearest region only when a genuinely local
        // relay answered (best < 120 ms). UDP-STUN RTT is accurate (unlike the old
        // TCP-connect probe), so a same-region relay reads ~5-40 ms and a
        // cross-continent one ~150+ ms — the split is clean. When nothing answers
        // (best -1) or every relay is far, keep the FULL list: redundancy beats a
        // possibly-wrong "nearest".
        if (best != std::numeric_limits<int>::max() && best < 120) {
            const int margin = 40;   // same-region hosts stay; a far one (+100ms) is dropped
            QList<TurnServer> nearby;   // NOTE: 'near' is a legacy Windows macro — do not use it
            for (const TurnServer &ts : m_turnServers) {
                bool isNear = false;
                for (const QString &url : ts.urls) {
                    QString h; quint16 p;
                    if (parseTurnHostPort(url, h, p) && rttByHost.value(h, -1) >= 0
                        && rttByHost.value(h) <= best + margin) { isNear = true; break; }
                }
                if (isNear) nearby.append(ts);
            }
            if (!nearby.isEmpty()) m_nearestTurnServers = nearby;
        }
        if (m_nearestTurnServers.isEmpty()) m_nearestTurnServers = m_turnServers;  // nothing answered -> keep all
        m_turnProbed = true;
        m_turnBestRttMs = (best == std::numeric_limits<int>::max()) ? -1 : best;
        qInfo().nospace() << "CallManager: nearest-TURN probe (UDP STUN) done — best RTT "
            << (best == std::numeric_limits<int>::max() ? -1 : best) << " ms; using "
            << m_nearestTurnServers.size() << "/" << m_turnServers.size() << " TURN server(s): "
            << selectedTurnLabel();
        cleanup();
    });
}

QList<TurnServer> CallManager::effectiveTurnServers() const
{
    return (m_turnProbed && !m_nearestTurnServers.isEmpty()) ? m_nearestTurnServers : m_turnServers;
}

// Telemetry: the TURN relay host(s) actually in use (post nearest-selection).
QString CallManager::selectedTurnLabel() const
{
    QStringList hosts;
    for (const TurnServer &ts : effectiveTurnServers())
        for (const QString &url : ts.urls) {
            QString h; quint16 p;
            if (parseTurnHostPort(url, h, p) && !hosts.contains(h)) hosts << h;
        }
    return hosts.join(QStringLiteral(", "));
}

// Telemetry: the signaling/HPB server this client is connected to (host only).
QString CallManager::selectedSignalingLabel() const
{
    if (!m_signaling) return QString();
    QString u = m_signaling->signalingUrl().trimmed();
    const int s = u.indexOf(QStringLiteral("://")); if (s >= 0) u = u.mid(s + 3);
    const int at = u.indexOf(QLatin1Char('@'));     if (at >= 0) u = u.mid(at + 1);
    const int sl = u.indexOf(QLatin1Char('/'));     if (sl >= 0) u = u.left(sl);
    const int c  = u.lastIndexOf(QLatin1Char(':')); if (c  >= 0) u = u.left(c);
    return u;
}

// Telemetry: measured RTT (ms) to the selected TURN relay / HPB, -1 if unknown.
int CallManager::selectedTurnRttMs() const { return m_turnBestRttMs; }
int CallManager::selectedSignalingRttMs() const
{
    return m_signaling ? m_signaling->signalingRttMs() : -1;
}

void CallManager::startLoadController()
{
    // Kill-switch: TALQ_DISABLE_LOAD_CONTROLLER=1 disables load SHEDDING (falls
    // back to static device-tier + network/BWE only) — but the 1s timer still
    // runs so the [MEDIA] freeze heartbeat in onLoadTick is logged even when the
    // controller is killed (the natural support step on a box that freezes
    // mid-call). The enabled flag is checked inside onLoadTick, not here.
    m_loadControllerEnabled = qgetenv("TALQ_DISABLE_LOAD_CONTROLLER").trimmed() != "1";

    // Verbose [MEDIA]/[LEAK] heartbeats are OFF by default — they qInfo every
    // second and the debug log force-syncs them to disk, needless I/O on a
    // healthy call. Re-resolved per call: the TALQ_MEDIA_DIAG env var wins
    // (1/true/on/yes → enabled) for instant dev-build control; otherwise the
    // persisted "Verbose call diagnostics" setting (Debug/mediaDiagnostics,
    // default off) that testers flip in Settings. The talq::leak atomic
    // counters keep counting regardless, so the gauges are valid the instant
    // this is switched on (no warm-up needed).
    {
        const QByteArray envDiag = qgetenv("TALQ_MEDIA_DIAG").trimmed().toLower();
        if (!envDiag.isEmpty())
            m_mediaDiag = (envDiag == "1" || envDiag == "true"
                           || envDiag == "on" || envDiag == "yes");
        else
            m_mediaDiag = QSettings("TalQ", "TalQ")
                              .value("Debug/mediaDiagnostics", false).toBool();
    }

    // Synthetic-load seam (design §8.1): drive the controller from env on the
    // dev box, whose NVENC GPU won't reproduce real overload. Production leaves
    // these unset → 0 → the controller sits at level 0 until the encode/decode
    // latency probes feed real load (Stage 4). Only meaningful when enabled.
    bool okE = false, okD = false;
    const double e = qgetenv("TALQ_TEST_ENCODE_USAGE").toDouble(&okE);
    const double d = qgetenv("TALQ_TEST_DECODE_USAGE").toDouble(&okD);
    m_synthEncodeUsage = okE ? e : 0.0;
    m_synthDecodeUsage = okD ? d : 0.0;

    m_loadController = talq::MediaLoadController();   // fresh state per call
    m_fpsRamp.reset();   // weak-tier solo fps ramp restarts at 10 fps each call
    if (!m_loadTimer.isActive()) m_loadTimer.start(1000);
}

void CallManager::stopLoadController()
{
    m_loadTimer.stop();
    m_recvLoadSubstreamCap = 2;     // reset so the next call starts uncapped
    m_recvLoadCapFocused   = false;
}

void CallManager::onLoadTick()
{
    // B4 — the GStreamer log probe (main.cpp) flagged the d3d11 video-device
    // interface warning (E_NOINTERFACE 0x80004002) on a streaming thread. That
    // warning ALONE can be benign, so we do NOT act on it blindly — that would
    // risk permanently forcing a healthy box onto software decode. CORROBORATE:
    // only fall back when a live subscriber is ACTUALLY decoding badly right now
    // (frames arriving but content frozen = concealment). And do NOT persist —
    // this is a heuristic for THIS session; only a hard decoder BUS ERROR (the B2
    // path) is trustworthy enough to remember across launches. The HW-decoder
    // demote is process-global, so once done this process stays on software.
    if (talqHwDecodeFaultDetected().load() && !m_hwDecodeFallbackDone
        && !talqForceSoftwareDecode().load()) {
        bool decodingBadly = false;
        for (auto it = m_subscribePipelines.constBegin();
             it != m_subscribePipelines.constEnd(); ++it) {
            SubscribeWebrtcSrc *s = it.value();
            if (!s || !s->isRunning()) continue;
            // Frames are OUTPUT by the decoder but the content is frozen (<=1
            // distinct) = the HW decoder is stuck. Catch it even at a LOW output
            // fps: a hybrid-GPU d3d11 stall delivers ~2 fps / 1 distinct (field:
            // Ilko receiving Ivan's 1080p), which the old >=3 fps gate missed, so
            // the demote never fired and the peer stayed frozen. Gated by the
            // d3d11-fault flag above, so a healthy box never reaches here.
            if (s->rxVideoFps() >= 1 && s->rxDistinctVideoFps() <= 1) { decodingBadly = true; break; }
        }
        if (decodingBadly) {
            m_hwDecodeFallbackDone = true;
            qWarning() << "CallManager: d3d11 fault + live concealment -> SOFTWARE decode this session (demote + rebuild)";
            talqForceSoftwareDecode().store(true);
            talqDemoteHwVideoDecoders();
            const auto peers = m_subscribePipelines.keys();
            for (const QString &sid : peers)
                recoverSubscriber(sid, QStringLiteral("hw-decode-fault"));
        }
    }

    // ── HOST-PROTECTION memory watchdog (Kalin 2026-06-30; reworked 2026-07-13) ──
    // The receive/decode path can balloon and melt the host — field: Ivan's box
    // hit ~2 GB while a hybrid-GPU d3d11 decoder was stalled, with chopped audio.
    // Process memory is an INDEPENDENT signal (the decode-load proxy was disabled
    // in 0.52.1 for oscillating), so use it with hysteresis: on trip, SHED hard —
    // cap EVERY peer including the focused tile to the 180p layer (i.e. stop the
    // 1080p streams) — and restore each peer's wanted quality once it eases.
    // Decision + thresholds + the 2026-07-13 false-trip field rationale:
    // core/MemShedPolicy.h. Only the side effects + logging live here.
    talq::HostLoad host;
    host.procMb = DebugMonitor::readProcessMemoryMB();
    qint64 sysTotalMb = 0, sysAvailMb = 0;
    int    sysLoadPct = -1;
    host.haveSys =
        DebugMonitor::readSystemMemoryMB(sysTotalMb, sysAvailMb, sysLoadPct);
    host.sysTotalMb = sysTotalMb;
    host.sysAvailMb = sysAvailMb;
    host.sysLoadPct = sysLoadPct;

    switch (m_memShed.update(host)) {
    case talq::MemShedPolicy::Shed:
        // Log the SYSTEM figures alongside the process figure so the next
        // person can tell a real overload from a false one (the 2026-07-13
        // false trip was only diagnosable because the peer's box specs were
        // known out-of-band).
        qWarning().nospace()
            << "CallManager: HOST OVERLOAD — working set " << host.procMb << " MB"
            << (host.procMb > talq::kMemShedRunawayHighMb
                ? " [runaway backstop]" : " [system memory pressure]")
            << "; system: total " << host.sysTotalMb << " MB, avail " << host.sysAvailMb
            << " MB, load " << host.sysLoadPct << "%"
            << " — shedding ALL peers to 180p";
        // NOTE (0.60.2): do NOT demote to software decode from here. An earlier version
        // of this patch did exactly that when the "d3d11 fault" flag was latched — but
        // that flag was a false positive (a benign adapter-probe warning), and forcing
        // software decode is what CAUSED the CPU spiral in the first place. Shedding
        // receive quality is the correct, conservative response to a memory overload;
        // rebuilding the decode path on a guess is not.
        applyReceiveLoadCaps(/*substreamCap=*/0, /*capFocused=*/true);
        setVideoQualityNotice(tr("High load — receiving video at reduced quality"));
        break;
    case talq::MemShedPolicy::Restore:
        qInfo().nospace() << "CallManager: host load eased (working set " << host.procMb
                          << " MB; system avail " << host.sysAvailMb << " MB, load "
                          << host.sysLoadPct << "%) — restoring receive quality";
        applyReceiveLoadCaps(/*substreamCap=*/2, /*capFocused=*/false);
        if (m_videoQualityNotice.startsWith(tr("High load")))
            setVideoQualityNotice(QString());
        break;
    case talq::MemShedPolicy::NoChange:
        break;
    }
    // ─────────────────────────────────────────────────────────────────────────

    // GROUP-CALL subscribe RECONCILIATION backstop: subscribe ANY in-call peer
    // that has media but no live subscriber. The onParticipantJoinedCall fix
    // covers peers that join AFTER us, but a peer ALREADY in the call when WE
    // joined had its join event fire while we were still Idle/Incoming (before the
    // subscribe could run) — so a LATE joiner never saw an already-present peer
    // until that peer toggled their camera (field: Ivan joined last and never saw
    // Ilko). This periodic sweep closes that gap; requestPeerStream self-dedupes.
    if (m_state == Connecting || m_state == Active) {
        for (auto it = m_participants.constBegin(); it != m_participants.constEnd(); ++it) {
            CallParticipant *p = it.value();
            if (!p || p->isSelf()) continue;
            const QString &sid = it.key();
            if ((!p->audioMuted() || !p->videoMuted())
                && !m_subscribePipelines.contains(sid)
                && !m_pendingRequestOffers.contains(sid)) {
                qInfo() << "CallManager: subscribe-reconcile — in-call peer without a"
                           " subscriber:" << sid.left(20);
                requestPeerStream(sid);
            }
        }
    }

    // Measure SEND/RECEIVE load every tick REGARDLESS of the controller, so the
    // [MEDIA] freeze heartbeat below is logged even with the controller killed
    // (TALQ_DISABLE_LOAD_CONTROLLER=1). Only the load-SHEDDING work is gated.

    // SEND load: real measurement from the publisher's encoder pad probes
    // (busy-time fraction; >1 when several simulcast encoders run at once).
    const double enc = m_publishPipeline ? m_publishPipeline->encodeUsage() : 0.0;

    // RECEIVE load: a work-PROXY from each live subscriber's decoded
    // resolution×fps (webrtcsrc hides its decoder, so a true decode-latency
    // probe isn't reachable — refine later). 1 unit ≈ one 1080p30 decode.
    double dec = 0.0;
    for (auto it = m_subscribePipelines.constBegin(); it != m_subscribePipelines.constEnd(); ++it) {
        SubscribeWebrtcSrc *sub = it.value();
        if (!sub) continue;
        const double px = double(sub->rxWidth()) * double(sub->rxHeight())
                        * double(sub->rxVideoFps());
        dec += px / (1920.0 * 1080.0 * 30.0);
    }

    talq::LoadCaps caps;        // default = uncapped (lyr2/fps30); used for the log when disabled
    int loadLevel = -1;         // -1 in the log = controller disabled
    if (m_loadControllerEnabled) {
        talq::LoadSample s;
        // The synthetic seam ADDS on top of the measured load, so overload can be
        // injected for validation on the dev box (NVENC won't reproduce it). In
        // production TALQ_TEST_* are unset → pure measured load.
        s.encodeUsage = enc + m_synthEncodeUsage;
        s.decodeUsage = dec + m_synthDecodeUsage;
        s.dropBurst   = false;
        caps = m_loadController.onTick(s);
        loadLevel = m_loadController.loadLevel();
        if (m_publishPipeline) {
            int fps = caps.sendFps;
            if (m_publishPipeline->isSingleStream()) {
                // Weak-tier solo: start at 10 fps and ramp UP into headroom (the
                // MediaLoadController only sheds from full, min 15 — too high a
                // start for a weak box). Never run two full encoders at once, so
                // throttle the camera hard while screen-sharing.
                const int ramp = m_fpsRamp.onTick(s.encodeUsage);
                fps = qMin(fps, ramp);
                if (m_screenSharing) fps = qMin(fps, 10);
            }
            m_publishPipeline->setLoadCaps(caps.sendLayerCeiling, fps);
        }
        // RECEIVE-cap DISABLED (0.52.1). The decode-load proxy above is
        // rxRes×fps — i.e. the very quantity the receive cap CONTROLS — so it is
        // self-referential and OSCILLATES: receiving 1080p reads as ~full load →
        // cap the substream to low → now receiving 180p reads as ~no load →
        // un-cap → 1080p → high load again … The field log showed the requested
        // substream thrashing 2↔0 every few seconds (each flip = an SFU layer
        // switch + keyframe = a visible quality bounce), and on a capable GPU it
        // needlessly dropped a 1:1 call to 180p. webrtcsrc hides its decoder, so a
        // REAL decode-cost probe isn't reachable; until one is, NEVER cap receive
        // on this proxy. Tile-size selection (pickSubstream) still picks a sensible
        // layer per tile (small grid tiles stay low), and SEND-side shedding (real
        // encoder pad-probe latency, unaffected) still protects weak encoders.
        // Same shape as the 0.51.5 applySharedFramerate neuter: an actuator driven
        // by a bad signal does more harm than good — disable it, don't tune it.
        applyReceiveLoadCaps(2, false);
    }

    // Everything below is the verbose freeze/leak diagnostics, OFF by default
    // (m_mediaDiag — Settings "Verbose call diagnostics" or TALQ_MEDIA_DIAG).
    // It's the last work in this tick, so just bail when disabled rather than
    // qInfo-spamming the log + forcing a disk sync every second on a healthy
    // call. The talq::leak counters keep counting either way.
    if (!m_mediaDiag) return;

    // [MEDIA] freeze-diagnostic heartbeat (1s during a call). After a hard
    // freeze, the LAST [MEDIA] line shows the trajectory
    // into it: a camera that stopped feeding the encoders (cam=1 enc=0.00),
    // encode/decode load climbing on the single-engine iGPU, layer/fps thrashing,
    // or a peer's decode ballooning. lc=0 records the kill-switch state. The line
    // is fflush'd per-write and force-synced to disk by DebugMonitor's 2s tick —
    // deliberately NOT _commit'd here, so the 1s cadence can't stall the
    // main-thread bus pump on a slow disk.
    const bool camOn    = m_cameraOn;
    const bool camFrame = m_publishPipeline && m_publishPipeline->cameraFirstFrameSeen();
    const bool pubRun   = m_publishPipeline && m_publishPipeline->isRunning();
    const bool aecOn    = m_publishPipeline && m_publishPipeline->aecPlayoutActive();
    qInfo().noquote() << QString(
        "[MEDIA] st=%1 cam=%2 camFrame=%3 pub=%4 aec=%5 enc=%6 dec=%7 load=L%8 "
        "caps=lyr%9/fps%10 rxPeers=%11 rxPeak=%12p share=%13 lc=%14")
        .arg(int(m_state))
        .arg(camOn ? 1 : 0).arg(camFrame ? 1 : 0).arg(pubRun ? 1 : 0).arg(aecOn ? 1 : 0)
        .arg(QString::number(enc, 'f', 2))
        .arg(QString::number(dec, 'f', 2))
        .arg(loadLevel)
        .arg(caps.sendLayerCeiling).arg(caps.sendFps)
        .arg(m_subscribePipelines.size())
        .arg(peerPeakRxHeight())
        .arg(m_screenSharing ? 1 : 0)
        .arg(m_loadControllerEnabled ? 1 : 0);

    // [LEAK] 0.51.x OOM hunt: per-frame cross-thread delivery gauges. prevPend /
    // subPend = frames POSTED to the Qt main thread minus those DELIVERED — if
    // either climbs into the hundreds/thousands during the call, that path's
    // unbounded QueuedConnection is the leak (fix = coalesce/drop intermediates).
    // bgQ = bytes queued in the unbounded BG-bridge appsrc — if THAT climbs, the
    // leak is in-pipeline instead. rss = whole-process working set (MB) — same
    // reading as the host-protection watchdog above (host.procMb, already
    // computed this tick), so no extra GetProcessMemoryInfo call. The per-path
    // gauges above localize a leak; rss is the top-line trend that confirms
    // whether 0.63.0's round of leak fixes actually holds memory flat over a
    // long call/session during beta soak — kept for that ongoing
    // verification, not temporary.
    qInfo().noquote() << QString(
        "[LEAK] prevPend=%1 prevPost=%2 subPend=%3 subPost=%4 bgPass=%5 bgQ=%6KB rss=%7MB")
        .arg(talq::leak::previewPosted.load(std::memory_order_relaxed)
             - talq::leak::previewDelivered.load(std::memory_order_relaxed))
        .arg(talq::leak::previewPosted.load(std::memory_order_relaxed))
        .arg(talq::leak::subPosted.load(std::memory_order_relaxed)
             - talq::leak::subDelivered.load(std::memory_order_relaxed))
        .arg(talq::leak::subPosted.load(std::memory_order_relaxed))
        .arg(talq::leak::bgPassThrough.load(std::memory_order_relaxed))
        .arg(m_publishPipeline ? m_publishPipeline->bgAppsrcQueuedBytes() / 1024 : 0)
        .arg(host.procMb);
}

void CallManager::stopScreenShare()
{
    if (!m_screenSharing) return;

    // User-initiated stop: cancel any pending confirm/retry so a late timeout
    // can't resurrect a share we're tearing down.
    m_shareConfirmTimer.stop();
    m_shareConfirmArmed = false;
    m_shareRetryTeardown = false;
    m_sharePolicy.requestStop();

    // #72 — drop the monitor border first so it vanishes the instant sharing
    // ends, regardless of how the pipeline teardown below proceeds.
    if (m_shareOverlay) {
        m_shareOverlay->hide();
        m_shareOverlay->deleteLater();
        m_shareOverlay = nullptr;
    }

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
    // OUR share is over — clear the flag BEFORE recomputing so the camera
    // sends its full simulcast set again. If a remote peer is still sharing,
    // updateCameraSuppression() keeps suppression on (its predicate sees the
    // remaining m_screenSubscribers entries).
    m_screenSharing = false;
    updateCameraSuppression();
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
    // Mark the unshare instant: if a re-share starts within the reap window, the
    // screen ICE-connected handler re-asserts the sendoffer so the peer doesn't
    // get stranded on a dropped discovery message (#reshare-not-seen).
    m_lastUnshareTimer.restart();

    m_screenShareTearingDown = false;
    // Terminal teardown: drive the share policy straight back to Idle. This path
    // deleteLater()s the pipeline, which suppresses released() -- so
    // onSharePipelineReleased() never runs and the policy would otherwise stay
    // stuck in Stopping, leaving every later start "queued behind teardown"
    // forever (the retries-exhausted wedge, field report 2026-06-01).
    m_sharePolicy.reset();
    emit screenShareChanged();
}

void CallManager::onShareConfirmed()
{
    // ICE connected AND outbound RTP flowing — the share is genuinely live.
    m_shareConfirmTimer.stop();
    m_shareConfirmArmed = false;
    qInfo() << "CallManager: screen share confirmed live (ICE + outbound RTP)";
}

// Settle delay between a screen-share teardown's released() and re-acquiring the
// capture device on the rebuild. released() now proves the GStreamer NULL
// transition settled (ScreenSharePipeline blocks on it), but the DXGI desktop-
// duplication device can take a moment longer to actually free; rebuilding the
// instant released() arrives still races it (SetThreadDesktop ERROR_BUSY). 300 ms
// was grounded in Sunshine's DDAPI 200ms-x2 retry precedent, with margin.
//
// 0.51.14 — bumped 300 -> 1500. The 300 ms was enough for the WGC/DXGI capture
// DEVICE, but NOT for an AMD MediaFoundation HW H.264 encoder *session* to
// release: a back-to-back share (stop, then re-share within ~30 s) re-acquired
// mfh264enc while the prior session was still freeing, so the new encoder
// produced no output and outbound RTP never confirmed — every ~10 s confirm-
// retry re-collided, and only a long idle (~60 s) freed it (Ilko field repro
// 2026-06-18, single AMD Radeon, both camera+screen on mfh264enc). The per-poll
// packets-sent diag in ScreenSharePipeline confirms whether 1.5 s is enough or
// the screen encoder needs a software (x264) fallback on repeated HW failure.
static constexpr int kShareDeviceSettleMs = 1500;

void CallManager::onShareConfirmTimeout()
{
    if (!m_shareConfirmArmed) return;
    m_shareConfirmArmed = false;

    const ShareAction a = m_sharePolicy.onConfirmTimeout();
    if (a == ShareAction::Retry) {
        qWarning() << "CallManager: screen share not confirmed in time — "
                      "retrying with a fresh pipeline";
        emit screenShareRetrying();
        // On HW-encoder boxes the dominant no-confirm cause is a hardware
        // session collision (the encoder opens but produces nothing —
        // packets-sent pinned in the RTP confirm poll). Retrying on the SAME
        // hardware re-collides every 8 s until retries exhaust (Ilko
        // 2026-06-18 mf, Kalin 2026-07-08 qsv), so the rebuild goes software
        // (x264) — it cannot collide. Cleared on the next fresh user start.
        m_ssForceSwEncoder = true;
        // Tear the current pipeline down but KEEP the object alive so its async
        // cleanup can emit released(); onSharePipelineReleased() then deletes it
        // and rebuilds with the saved target. m_shareRetryTeardown distinguishes
        // this from a user stop in that handler; m_screenShareTearingDown
        // protects the A/V publisher during the teardown window.
        if (m_screenSharePipeline) {
            m_shareRetryTeardown = true;
            m_screenShareTearingDown = true;
            m_screenSharePipeline->stop();
        } else if (m_sharePolicy.onReleased() == ShareAction::StartNow) {
            // No live pipeline to tear down; the device is already releasing.
            // Defer the rebuild for the same DXGI settle reason as the main
            // released() path below (state-guarded against a racing user stop).
            QTimer::singleShot(kShareDeviceSettleMs, this, [this]() {
                if (m_sharePolicy.state() == ShareState::Starting)
                    buildAndStartSharePipeline(m_ssMonitorIndex, m_ssWindowHandle);
            });
        }
    } else if (a == ShareAction::Fail) {
        // Retries exhausted — surface a clear failure instead of a silently
        // dead share ("Starting remote screen share…" forever) and clean up.
        qWarning() << "CallManager: screen share failed to start after retries";
        emit screenShareFailed(
            tr("Couldn't start screen sharing. Please try again."));
        stopScreenShare();
    }
}

void CallManager::onSharePipelineReleased()
{
    // The async GStreamer teardown finished and the d3d11 capture device is
    // free. For a retry teardown, delete the retained old pipeline now; for a
    // normal/user stop the pipeline is already gone and this is a no-op.
    if (m_shareRetryTeardown) {
        m_shareRetryTeardown = false;
        m_screenShareTearingDown = false;
        if (m_screenSharePipeline) {
            m_screenSharePipeline->deleteLater();
            m_screenSharePipeline = nullptr;
        }
    }
    // If a start is queued (a retry, or a back-to-back share requested during
    // teardown), fire it now that the device is safe to re-acquire.
    if (m_sharePolicy.onReleased() == ShareAction::StartNow) {
        // Defer the rebuild by a short settle: released() now means the
        // GStreamer NULL transition has settled, but the DXGI desktop-
        // duplication capture device can take a moment longer to actually free
        // -- re-acquiring instantly races it (SetThreadDesktop ERROR_BUSY ->
        // capture stalls after one frame). State-guarded so a user stop / call
        // end during the settle (which resets the policy to Idle) can't
        // resurrect a torn-down share.
        QTimer::singleShot(kShareDeviceSettleMs, this, [this]() {
            if (m_sharePolicy.state() == ShareState::Starting)
                buildAndStartSharePipeline(m_ssMonitorIndex, m_ssWindowHandle);
        });
    }
}

VideoFrameProvider *CallManager::localScreenPreviewProvider() const
{
    return m_screenSharePipeline ? m_screenSharePipeline->previewProvider()
                                  : nullptr;
}

void CallManager::setScreenShareQuality(int level)
{
    if (level < 0 || level > 3) return;
    m_ssQuality = level;
    QSettings("TalQ", "TalQ").setValue("Video/screenShareQuality", level);
    emit screenShareQualityChanged();
    if (!m_screenSharing) return;   // applied on next share

    // Map the quality level to the downscale cap — the SAME policy chain as
    // buildAndStartSharePipeline (core/ShareCapPolicy.h), including the
    // software-fallback 1080p clamp: a share that fell back to x264 keeps its
    // 1080p ceiling across a live quality change too (realtime x264 above
    // 1080p costs more CPU than a laptop can hide mid-call).
    // Same clamp as the build path, so a live quality change cannot exceed what
    // this share was started with. Framerate is deliberately NOT changed here:
    // it is baked into the appsrc caps at start() and altering it would force a
    // renegotiation this in-place re-cap does not perform.
    const talq::ShareQuality sq = talq::shareQualityFor(level, m_ssPresentation);
    const talq::ShareCap cap = screenShareCapForLevel(sq.level, m_ssForceSwEncoder);

    // LIVE quality change -- reconfigure the RUNNING pipeline's downscale cap in
    // place (ScreenSharePipeline::setQualityCap re-sets the scale capsfilter).
    // Resolution is NOT in the SDP, so no new offer is sent -- which avoids the
    // stale-MCU-screen-handle confirm failure the old stop()->start() re-share
    // hit (ICE reconnected but the rebuilt publish never confirmed RTP -> retry
    // churn -> the share dropped after a few seconds). The encoder reconfigures
    // and emits a fresh keyframe; the peer re-syncs at the new resolution with
    // no teardown, no border flicker, no drop.
    if (m_screenSharePipeline) {
        qInfo() << "CallManager: screen-share quality ->" << level
                << "-- LIVE in-place re-cap" << cap.w << "x" << cap.h;
        m_screenSharePipeline->setQualityCap(cap.w, cap.h);
        return;
    }

    // Defensive fallback (no live pipeline object while m_screenSharing is true
    // -- should not happen): terminal stop+start at the new quality.
    const int mi = m_ssMonitorIndex;
    const quintptr wh = m_ssWindowHandle;
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

    // ORDER MATTERS (2026-07-15 review): set the image path and strength BEFORE
    // the mode. processFrame decides the Image+no-image raw pass-through by
    // reading mode then path-empty; setMode publishes with a release-store, so
    // the path-empty flag set here is visible the instant the mode is observed
    // as Image. Flipping the mode first opened a window where a user with a REAL
    // image, applied mid-call, was momentarily seen as Image+empty and sent RAW
    // camera — a concealment leak.
    m_backgroundEngine->setImagePath(url);
    m_backgroundEngine->setBlurStrength(strength);
    m_backgroundEngine->setMode(mode);
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
    // Keep our call audio at full volume even if another app opens a Windows
    // "communications" stream (which would otherwise duck us). Best-effort,
    // idempotent; this is the common funnel for outgoing and accept, so one
    // call here covers every path. No-op off Windows.
    talq::disableCommunicationsDucking();

    QJsonObject body;
    body["flags"] = callFlags(withVideo, !m_muted);
    // Match the official client's POST call/{token} parameter shape.
    body["silent"] = false;            // ring participants normally
    body["recordingConsent"] = false;  // no consent UI; server enforces only if required
    m_api->post("apps/spreed/api/v4/call/" + m_callToken, body,
        [this, withVideo](bool ok, const QJsonObject &, int statusCode) {
            if (!ok) {
                // The SERVER rejected our call-join (e.g. a 5xx). Never silently
                // drop the call with no word to the user. First absorb a brief
                // transient blip with a couple of quick auto-retries; if it
                // still fails, intercept it and surface a clear, plain-language
                // message -- and remember the token so the signaling echo does
                // not immediately re-ring us as a phantom incoming call.
                const bool transient   = (statusCode == 0) || (statusCode >= 500 && statusCode <= 599);
                const bool stillTrying = (m_state == Outgoing || m_state == Connecting || m_state == Incoming);
                if (transient && stillTrying && m_callJoinAttempts < kMaxCallJoinAttempts) {
                    ++m_callJoinAttempts;
                    const int delayMs = 600 * m_callJoinAttempts;   // 600 ms, then 1200 ms
                    qWarning() << "CallManager: call-join failed status=" << statusCode
                               << "- auto-retry" << m_callJoinAttempts << "of"
                               << kMaxCallJoinAttempts << "in" << delayMs << "ms";
                    setStatusDetail(tr("Server busy, retrying..."));
                    QTimer::singleShot(delayMs, this, [this, withVideo]() {
                        if (m_state == Outgoing || m_state == Connecting || m_state == Incoming)
                            joinCallOnServer(withVideo);
                    });
                    return;
                }

                qWarning() << "CallManager: failed to join call, status=" << statusCode
                           << "(retries exhausted; informing user)";
                m_callJoinAttempts = 0;
                m_lastOutgoingToken = m_callToken;
                m_lastOutgoingTime  = QDateTime::currentDateTime();

                const QString codeStr = (statusCode == 0)
                    ? tr("no response") : QString::number(statusCode);
                const QString title = tr("Couldn't start the call");
                QString msg;
                if (statusCode == 0 || statusCode >= 500)
                    msg = tr("The server reported an error (%1), so the call couldn't "
                             "be started.\n\nThis is a problem on the server, not on your "
                             "device. Please try again in a moment -- if it keeps "
                             "happening, let your administrator know.").arg(codeStr);
                else if (statusCode == 403)
                    msg = tr("You don't have permission to start a call in this "
                             "conversation (403).");
                else if (statusCode == 404)
                    msg = tr("This conversation could not be found on the server (404).");
                else
                    msg = tr("The call couldn't be started (%1). Please try again.").arg(codeStr);

                emit callFailed(title, msg);
                teardown("Failed to join call");
                return;
            }

            m_callJoinAttempts = 0;
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
                    // RTT-probe the offered TURN hosts now (on room-join) so that by
                    // the time a call starts we relay only through the NEAREST one.
                    probeNearestTurnAsync();

                    // Process any offers that arrived before ICE servers were available
                    processPendingOffers();

                    setStatusDetail("Starting pipeline");

                        // --- MCU mode: build + start the publisher (send leg). ---
                        // Factored into buildAndStartPublisher() so the
                        // Zoom-style reconnect path can rebuild the publisher on
                        // the cached STUN/TURN without re-joining the call.
                        if (!buildAndStartPublisher()) {
                            teardown("Failed to start audio pipeline");
                            return;
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
                                        if (!ok || m_state == Idle || !m_remoteSessionId.isEmpty())
                                            return;
                                        for (const auto &val : data) {
                                            const QJsonObject p = val.toObject();
                                            const QString ncSid = p["sessionId"].toString();
                                            if (ncSid.isEmpty()) continue;
                                            // #bug5 — GET call/{token} returns ONLY the sessions
                                            // CONNECTED to the call, and (live-verified against
                                            // this NC 33: raw row = actorType/actorId/displayName/
                                            // token/lastPing/sessionId) carries NO inCall field at
                                            // all. The old `inCall == 0 → skip` gate therefore
                                            // dropped EVERY row — the poll could never adopt
                                            // anyone, which is why the caller sat in Outgoing
                                            // ("Calling…") until the 60s ring-out. Presence in
                                            // the list IS "in the call"; use the flags when a
                                            // server variant does send them, else assume audio
                                            // (Janus forwards the peer's whole feed for a single
                                            // subscription; the real flags follow with the next
                                            // HPB participants update).
                                            int inCall = p["inCall"].toInt();
                                            if (inCall == 0)
                                                inCall = CALL_FLAG_IN_CALL | CALL_FLAG_WITH_AUDIO;
                                            // #bug5 — the REST sessionId is the NEXTCLOUD
                                            // session id, a DIFFERENT id space from the HPB
                                            // signaling sid that requestoffer/subscribers
                                            // route on. The old code compared it to the HPB
                                            // sid for the self-skip (never matched — it could
                                            // adopt OUR OWN row) and fed it straight to
                                            // requestPeerStream (the HPB has no such recipient
                                            // → no offer, ever — and the poisoned
                                            // m_remoteSessionId then blocked the real
                                            // JOINED-edge adopt for the rest of the ring, so
                                            // the caller rang out 60s against a peer whose
                                            // inCall was already 7). Skip self by USER id and
                                            // translate NC→HPB via the room-join event
                                            // mappings instead.
                                            const QString uid = p["actorId"].toString();
                                            if (p["actorType"].toString() == QLatin1String("users")
                                                && !uid.isEmpty() && uid == m_signaling->userId())
                                                continue;   // our own user (any device) — never adoptable
                                            // 2026-07-13 backstop — freshness stamp for the
                                            // promoted-without-sid window: ANY non-self row still
                                            // in the call list means "the answered peer is still
                                            // there", mapped or not (a mapped row can be
                                            // temporarily unadoptable, e.g. mid-Reconnecting, and
                                            // must not trip the vanished-peer check below).
                                            if (m_restPromoted && m_remoteSessionId.isEmpty())
                                                m_restPeerLastSeenMs = QDateTime::currentMSecsSinceEpoch();
                                            QString sid = m_signaling->hpbSessionForNcSession(ncSid);
                                            if (sid.isEmpty() && !uid.isEmpty())
                                                sid = m_signaling->sessionsForUser(uid).value(0);
                                            if (sid.isEmpty() || sid == m_signaling->sessionId()) {
                                                TLOG_CALL("REST peer" << ncSid.left(20)
                                                          << "has no known signaling sid yet — waiting"
                                                             " for its room join event");
                                                // 2026-07-13 field incident — waiting is NOT
                                                // enough. The HPB stayed silent about an ANSWERED
                                                // callee for 3 whole ring-outs (its participants
                                                // updates carried only our own session; the callee
                                                // never appeared in ANY signaling event), and both
                                                // NC→HPB maps are HPB-fed — so this branch is
                                                // exactly where the "guaranteed" REST fallback went
                                                // blind, silently skipping the answered peer every
                                                // 3s for the whole 60s ring. Hand the row to the
                                                // backstop: it promotes out of Outgoing on REST
                                                // evidence alone, keeps resolving on later ticks,
                                                // and surfaces the delivery failure LOUDLY instead
                                                // of ringing into the void. (A row that maps to our
                                                // OWN session is not peer evidence — skip those.)
                                                if (sid.isEmpty())
                                                    noteRestPeerEvidence(ncSid, p["displayName"].toString());
                                                continue;
                                            }
                                            TLOG_CALL("discovered in-call peer via REST:"
                                                      << sid.left(20) << "flags=" << inCall);
                                            // Route through the SAME handler the HPB JOINED
                                            // edge uses so every adopt side-effect applies
                                            // once, consistently: ring-timer stop, Connecting,
                                            // #bug4 peer-user stamping + m_peerInCallSids,
                                            // media-state broadcast, subscribe gating (incl.
                                            // the group fallthrough for additional peers). The
                                            // poll is then exactly a replay of the missed
                                            // edge, not a second adoption code path.
                                            onParticipantJoinedCall(sid, inCall,
                                                                    p["displayName"].toString());
                                    }
                                        // 2026-07-13 backstop — the promotion stopped the 60s
                                        // ring timeout (correctly: the call WAS answered), so a
                                        // callee who answers, waits on "connecting", and gives up
                                        // before their HPB sid ever resolves would otherwise park
                                        // us in Connecting/Active FOREVER — their hang-up is
                                        // invisible to us (participantLeftCall is HPB-fed too).
                                        // Their REST row disappearing for ~3 poll ticks is the
                                        // one signal we do have; end the call truthfully instead
                                        // of trading the old forever-ring for a forever-connect.
                                        if (m_restPromoted && m_remoteSessionId.isEmpty()
                                            && m_restPeerLastSeenMs > 0
                                            && QDateTime::currentMSecsSinceEpoch()
                                                   - m_restPeerLastSeenMs > 10000) {
                                            qWarning() << "CallManager: REST-proven peer vanished"
                                                          " from the call before their HPB session"
                                                          " ever resolved — they answered, then gave"
                                                          " up while signaling never delivered their"
                                                          " session (field 2026-07-13). Ending call.";
                                            teardown("Call ended before media could be established");
                                        }
                                });
                            };
                            // Upstream Talk (v23.0.4, MCU mode) waits for the
                            // signaling layer's usersInCallChanged event and
                            // never polls eagerly — but that only covers peers
                            // whose call-join produces an EDGE. A peer ALREADY
                            // ESTABLISHED in the call (the remote kept the call
                            // open; we join/redial) produces no edge, and the
                            // HPB protocol has no request to fetch the current
                            // in-call list (doc-checked: push-only), so this
                            // REST poll is the ONLY discovery for that peer.
                            // Fire it once ~1.2s after our publisher starts:
                            // late enough that the old immediate-poll concern
                            // (subscribing a JUST-joining peer before its
                            // publish registers → choppy) cannot apply to the
                            // target case — an established peer's publish has
                            // been registered for seconds — and a just-joining
                            // peer is instead adopted by its own JOINED edge
                            // (the poll self-disables once m_remoteSessionId is
                            // set). A rare still-early requestoffer is absorbed
                            // by the 8s requestoffer retry net + the 0.52.7
                            // not_allowed→recoverSubscriber escalation. The 3s
                            // timer stays as the backup for the documented
                            // mobile/internal-signaling path where HPB
                            // participant events may not fire.
                            // 1.0 audit — owned by m_callPollTimer and killed in
                            // stopAllPipelines (which runs on teardown AND on a
                            // fresh call start), so a re-entered setup can't leak
                            // the prior timer. Once the peer is found
                            // pollParticipants early-returns, so it idles cheaply
                            // until teardown rather than needing a self-delete edge.
                            if (m_callPollTimer) {
                                m_callPollTimer->stop();
                                m_callPollTimer->deleteLater();
                            }
                            m_callPollTimer = new QTimer(this);
                            m_callPollTimer->setInterval(3000);
                            connect(m_callPollTimer, &QTimer::timeout, this, pollParticipants);
                            m_callPollTimer->start();
                            QTimer::singleShot(1200, this, pollParticipants);
                        }

                        // Video is now included in the initial pipeline (no delayed renegotiation)
                        // Camera preview starts immediately when pipeline starts
                });
        });
}

// 2026-07-13 field incident — CLIENT-SIDE BACKSTOP for an HPB that never
// tells us the callee joined. The ONLY promotion out of Outgoing used to be
// SignalingClient's participants-update rising edge (inCall 0→N →
// participantJoinedCall → the adopt branch in onParticipantJoinedCall); our
// own publisher cannot promote (its ICE-connected handler only sets the
// sticky m_pubIceConnectedSeen and is gated on m_state == Connecting — a
// no-op while Outgoing). In the field, on 3 consecutive outgoing attempts,
// the HPB delivered participants updates containing ONLY OUR OWN session:
// the callee's HPB session never appeared in ANY signaling event (no room
// join, no participants update) even though the callee HAD ANSWERED — their
// row was in every REST call/{token} response. The REST poll, documented as
// "the guaranteed discovery", was blind too: to act it must translate the
// peer's NEXTCLOUD session id into an HPB sid via hpbSessionForNcSession()/
// sessionsForUser(), and BOTH maps are populated only from HPB events. So it
// silently skipped the answered peer every 3s while we rang 60s into the
// void, hit the ring timeout, and reported "No answer" against a callee
// stuck on "connecting" — which sent the investigation down the wrong path
// for hours. This is the poll's unmapped-row hand-off. On REST evidence
// ALONE it (a) stops the ring timeout and promotes Outgoing→Connecting so
// the 60s abort can no longer kill an answered call, (b) leaves
// m_remoteSessionId EMPTY (it is an HPB sid — the NC id is a different id
// space) so the poll keeps ticking and the adopt gate in
// onParticipantJoinedCall — extended to accept Active-while-m_restPromoted —
// runs the normal adopt/subscribe exactly once whenever the sid finally
// resolves, and (c) after ~9s of "in call per REST, unknown to the HPB"
// logs a LOUD warning naming the real failure: signaling delivery, NOT a
// no-answer. NOTE on re-asking the server: the HPB protocol has NO request
// to re-fetch room/participant state (doc-checked, push-only), and a forced
// same-room joinRoom() re-join mints a fresh NC session that collapses the
// live call (see SignalingClient::joinRoom) — so the one safe local lever is
// forceCallParticipantResync(), pulled once at promotion so any cached >0
// flags re-emit their JOINED edge on the next update the HPB does deliver.
void CallManager::noteRestPeerEvidence(const QString &ncSid, const QString &displayName)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_restPeerEvidenceMs == 0)
        m_restPeerEvidenceMs = now;
    if (m_pendingRestPeerNcSid.isEmpty())
        m_pendingRestPeerNcSid = ncSid;

    // Promote on the FIRST evidence, from Outgoing ONLY: an outgoing call we
    // placed, not yet adopted. Never from Incoming/acceptCall (that path sets
    // m_remoteSessionId before the poll can even start), never from
    // Ending/teardown, and never twice (m_restPromoted). The normal HPB adopt
    // path remains the sole owner of m_remoteSessionId.
    if (m_state == Outgoing && !m_restPromoted && m_remoteSessionId.isEmpty()) {
        m_restPromoted = true;
        m_restPeerLastSeenMs = now;
        qInfo() << "CallManager: REST poll proves a peer is in the call ("
                << ncSid.left(20) << "name=" << displayName
                << ") but the HPB has not delivered their session — promoting"
                   " to Connecting on REST evidence alone (2026-07-13 backstop)";
        if (!displayName.isEmpty()) m_remotePeerName = displayName;
        // The callee answered — a later "No answer" ring-out would be a lie.
        m_ringTimeout.stop();
        // Best-effort local lever (there is no HPB state-fetch request):
        // clear the cached inCall flags so the next participants update the
        // HPB DOES deliver re-emits a JOINED edge for everyone in the call.
        m_signaling->forceCallParticipantResync();
        setStatusDetail("Answered — waiting for signaling");
        // setState's m_pubIceConnectedSeen catch-up may carry this straight
        // to Active (publisher ICE typically connected long ago) — same
        // trajectory the HPB edge produces, just without a subscriber yet.
        setState(Connecting);
        emit callInfoChanged();
        return;   // the delivery-failure warning window starts on later ticks
    }

    // Loud failure surfacing: our media leg is up (publisher ICE connected),
    // REST keeps proving the peer is in the call, and the HPB STILL has not
    // named their session after ~9s (~3 poll ticks). That is a SIGNALING
    // DELIVERY failure and must be unmissable in the field log — the
    // 2026-07-13 logs contained no hint of it and hours were burned
    // investigating the wrong layer.
    if (!m_restBackstopWarned && m_restPromoted && m_pubIceConnectedSeen
        && m_remoteSessionId.isEmpty()
        && now - m_restPeerEvidenceMs >= 9000) {
        m_restBackstopWarned = true;
        qWarning() << "CallManager: SIGNALING DELIVERY FAILURE — peer"
                   << m_pendingRestPeerNcSid.left(20) << "(" << displayName
                   << ") has been IN CALL per REST for"
                   << (now - m_restPeerEvidenceMs) / 1000
                   << "s, our publisher ICE is connected, but the HPB never"
                      " delivered their session (no room join, no participants"
                      " update): cannot map NC→HPB sid, cannot subscribe"
                      " (rxPeers=0). This is NOT a no-answer — the callee"
                      " answered and is waiting on 'connecting'. The REST poll"
                      " keeps retrying the mapping every 3s. (field 2026-07-13)";
    }
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
    // 1.0 audit — kill the per-call REST poll timer here (runs on teardown AND
    // fresh-call start) so it can't accumulate across calls.
    if (m_callPollTimer) {
        m_callPollTimer->stop();
        m_callPollTimer->deleteLater();
        m_callPollTimer = nullptr;
    }
    if (m_publishPipeline) {
        qDebug() << "CallManager::teardown — stopping publish pipeline";
        m_publishPipeline->stop();
        // 1.0 audit — SAME use-after-free hazard the subscriber teardown below
        // documents: teardown can be reached INLINE from PublishPipeline::pollBus()
        // (the main-thread bus loop driven by m_glibTimer) via a DirectConnection
        // error() emit — e.g. all simulcast layers dying. A synchronous delete then
        // frees the pipeline whose own pollBus() frame is still on the stack, and the
        // unwind dereferences freed state (gst_message_unref / gst_object_unref / the
        // next gst_bus_pop) → 0xC0000005. deleteLater() defers the free to the next
        // event-loop turn (after pollBus unwinds); null the member NOW so re-entrant
        // guards immediately see it gone. (Mirrors ScreenSharePipeline.)
        qDebug() << "CallManager::teardown — scheduling publish pipeline deletion";
        m_publishPipeline->deleteLater();
        m_publishPipeline = nullptr;
    }
    for (auto *sub : m_subscribePipelines) {
        sub->stop();   // stops the subscriber pipeline + releases its far-end appsrc
        // 1.0 audit / 0.52.16 crash fix — NEVER synchronously `delete` a
        // SubscribeWebrtcSrc here: teardown() runs on the Qt main thread but the
        // webrtcsrc (gst-plugins-rs / libgstrswebrtc) streaming + bus-watch threads
        // are still live (cleanup() detaches the GST_STATE_NULL transition to a
        // worker thread that has NOT joined), and several of its callbacks post
        // QMetaObject::invokeMethod(self, …, Qt::QueuedConnection) events back to
        // THIS object. A synchronous delete runs ~QObject inline while such a
        // QMetaCallEvent is already queued (or being posted the instant after the
        // g_signal_handlers_disconnect in cleanup()), so Qt delivers a metacall to
        // a freed QObject → Qt6Core dereferences freed d_ptr/vtable → 0xC0000005
        // (the hangup→teardown→Idle crash; faulting RIP Qt6Core!QObject metacall,
        // crash thread dominated by libgstrswebrtc). A QPointer in the lambda does
        // NOT help: it guards the lambda BODY, not the event-delivery dispatch that
        // touches the QObject before the lambda runs. deleteLater() defers the free
        // to a clean event-loop turn after the posted metacalls drain — exactly why
        // EVERY other subscriber-destroy site (lines ~1353, ~3863, ~4003, the
        // m_screenSubscribers loop below) already uses it. Match that here.
        sub->deleteLater();
    }
    m_subscribePipelines.clear();
    m_subscriberSids.clear();
    m_desiredSubstream.clear();
    // Session ids are per-call ephemeral, so entries here can never be hit
    // again — they were pure accumulation. Its siblings were already cleared.
    m_peerManualSubstreamOverride.clear();
    m_peerSubstreamWant.clear();   // 0.51.x receive-load raw wants
    m_subscriberRecoveries.clear();
    // 1.0 audit — these two per-session maps are created lazily (m_subStall via
    // operator[] in updateCallStats; m_pendingSubCandidates on early trickle-ICE)
    // and were only ever removed per-session, never bulk-cleared on full teardown.
    // A full call teardown deletes the subscribers in the loop above WITHOUT
    // touching them, so each call leaked one-or-more entries keyed by an ephemeral
    // session id. Reset them with the rest of the per-call subscriber state.
    m_subStall.clear();
    m_signalQuality.clear();   // per-tile signal-quality glyph — same lifecycle as m_subStall
    m_pubStall.reset();
    m_pendingSubCandidates.clear();
    m_pubRetryTimer.stop();
    m_pubRetryAttempts   = 0;
    m_pubRebuildInFlight = false;
    m_pubIceConnectedSeen = false;
    // 2026-07-13 — REST-evidence backstop state is strictly per-call; reset
    // it at this single cleanup point (stopAllPipelines runs on teardown AND
    // on every fresh call start) alongside the other per-call flags, so a
    // stale promotion/warning can never leak into the next call.
    m_pendingRestPeerNcSid.clear();
    m_restPeerEvidenceMs   = 0;
    m_restPeerLastSeenMs   = 0;
    m_restPromoted         = false;
    m_restBackstopWarned   = false;
    // #bug3 -- clear peer-grace here (the single cleanup point): stopAllPipelines
    // runs first in teardown (before m_remoteSessionId.clear) AND on every fresh
    // call start, so an in-flight grace timer can't re-enter teardown and a stale
    // grace can't fire into the next call.
    m_peerGraceTimer.stop();
    m_peerGraceActive = false;
    m_remotePeerUserId.clear();
    m_graceLeftSid.clear();
    m_peerInCallSids.clear();   // #bug4 -- no peer sessions across a fresh call

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
    m_peerPeakRxHeight = 0;        // fresh basis for the next call's quality label
    stopLoadController();          // 0.51.x: disarm the tick + reset caps to full
    m_ringTimeout.stop();
    m_durationTimer.stop();
    m_cameraApplyTimer.stop();          // D3 fix — no stray coalesced toggle into the next call
    m_neverDecodedRecoveries.clear();   // D2 fix
    m_resubscribeOnActive = false;      // A2 fix
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
    m_requestOfferRejections.clear();   // 0.52.7
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
    // #72 — a call can end while screen-sharing without going through
    // stopScreenShare(), so drop the monitor-border overlay here too, else its
    // top-level window is orphaned on screen after the call.
    if (m_shareOverlay) {
        m_shareOverlay->hide();
        m_shareOverlay->deleteLater();
        m_shareOverlay = nullptr;
    }
    m_screenSharing = false;
    m_screenShareTearingDown = false;
    // The per-share sid persists in this member and localOfferReady only
    // assigns when it's EMPTY (retry sid stability) — clear it here so the
    // NEXT call's first share can't inherit this call's identity (this path
    // bypasses stopScreenShare(), which normally clears it).
    m_screenShareSid.clear();
    // The reap-window gate for screen re-share re-asserts is intra-call only;
    // invalidate it so a stop-while-sharing in THIS call can't arm a spurious
    // re-assert on the NEXT call's first share (the timer member persists).
    m_lastUnshareTimer.invalidate();
    // A call can end while a share is Active/Confirming/Stopping. m_sharePolicy
    // is a persistent member and the deleteLater() above suppresses released(),
    // so without an explicit reset the policy survives into the NEXT call in a
    // non-Idle state -- the share button would then silently do nothing (or stay
    // "queued behind teardown") for that whole call. Reset to Idle, same as the
    // terminal stopScreenShare() path (#0.46.1 review finding).
    m_sharePolicy.reset();
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
    m_screenSubFrameMark.clear();
    m_screenSubStallTicks.clear();
    m_screenSubBuiltMs.clear();
    m_screenSubIceState.clear();
    m_screenSubCompletedMs.clear();
    m_screenSubFailRetries.clear();
    m_pendingScreenSubCandidates.clear();
    m_softwareEncoderNotified = false;   // re-notify on the next call if still software
    setVideoQualityNotice(QString());    // 0.52.5 — reset the sender quality chip for the next call

    // Synchronous local close — the UI must NEVER wait on the server
    // (mid-call network drops could otherwise leave the call window
    // pinned "in call" until Qt's transport timeout, a minute+ later).
    m_joinedCall = false;
    setState(Idle);
    // Release the wake lock now the call is over — must run on teardown, not
    // only from updateCameraSuppression, which is not called on this path.
    updatePowerInhibit();
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

    // #bug3 -- a 1:1 peer returning from a grace hold (same userId, likely a NEW
    // session id). Take priority over the empty-guard adopt below so the userId
    // correlation wins (and a wrong group joiner during grace is not adopted).
    if (tryAdoptReturningPeer(sessionId)) {
        if (m_state == Active || m_state == Connecting || m_state == Reconnecting)
            ensureParticipant(sessionId, displayName);
        return;
    }

    if ((m_state == Outgoing || m_state == Connecting
         // 2026-07-13 — REST-evidence backstop follow-through: when the HPB
         // never announced the answered callee, noteRestPeerEvidence()
         // promoted us to Connecting on the REST poll's evidence alone, and
         // setState's m_pubIceConnectedSeen catch-up may have carried that
         // straight to Active. The peer's HPB sid can therefore first arrive
         // (a late HPB event, or a later poll tick finally resolving the
         // NC→HPB map) while we are ALREADY Active — the adopt below must
         // still run once, or the peer would never be subscribed (rxPeers=0
         // for the rest of the call). m_restPromoted is set ONLY by the
         // backstop, so on the happy path (HPB edge first) this condition
         // evaluates exactly as it always did.
         || (m_restPromoted && m_state == Active))
        && m_remoteSessionId.isEmpty()) {
        m_remoteSessionId = sessionId;
        // #bug4 — stamp the peer USER at adopt time (not only at grace-entry):
        // sibling adopt / leave / hangup-hint below are keyed on the user,
        // because the same person holds several sessions at once. 1:1 only — in
        // a group m_remoteSessionId is merely the first joiner, not "the peer".
        // Empty for guests: the sid-keyed behaviour then stands.
        if (isOneToOneCall())
            m_remotePeerUserId = m_signaling->userIdForSession(sessionId);
        if (!displayName.isEmpty()) m_remotePeerName = displayName;
        qDebug() << "CallManager: remote peer joined:" << sessionId.left(20) << "name=" << m_remotePeerName;
        // 2026-07-13 — stand the REST-evidence backstop down, if it was
        // armed: the peer's HPB sid is now known, so the pending
        // "promoted without a sid" state (and its delivery-failure warning
        // window) is resolved. Clearing m_restPromoted also makes the
        // Active-state clause in the condition above single-shot — together
        // with the m_remoteSessionId guard this adopt can never run twice.
        // All of this is a set of no-op assignments on the happy path.
        if (m_restPromoted)
            qInfo() << "CallManager: HPB sid for the REST-proven peer finally resolved ("
                    << sessionId.left(20) << "<- NC" << m_pendingRestPeerNcSid.left(20)
                    << ") — adopting via the normal path (2026-07-13 backstop)";
        m_restPromoted = false;
        m_pendingRestPeerNcSid.clear();
        m_restPeerEvidenceMs = 0;
        m_restPeerLastSeenMs = 0;
        // stop() on an already-stopped timer (the backstop stopped it at
        // promotion time) is a QTimer no-op — no double side effect.
        m_ringTimeout.stop();
        // Never DEMOTE an already-Active call (backstop promotion + ICE
        // catch-up) back to Connecting — setState would bounce it
        // Connecting→Active within one call stack for nothing. On the normal
        // path the state here is Outgoing/Connecting, so this is
        // byte-for-byte the old unconditional setState(Connecting).
        if (m_state != Active)
            setState(Connecting);
        emit callInfoChanged();

        // Broadcast media state now that remote peer can receive it
        broadcastMediaState("audio", !m_muted);
        broadcastMediaState("video", m_cameraOn);

        if (flags & (CALL_FLAG_WITH_AUDIO | CALL_FLAG_WITH_VIDEO)) {
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
    // #bug4 -- a SIBLING session of the 1:1 peer joined WITH media while the
    // session we locked onto never delivered a subscriber (an idle device of
    // the same user: the phone in the room, the VPN half of a reconnect,
    // whose inCall flags CLAIM media it never publishes — its requestoffer
    // loop churns data-only offers / not_allowed forever). Re-point the 1:1
    // to the sibling that actually publishes; the group fallthrough below
    // already subscribes it, this moves the call's IDENTITY onto it so leave
    // handling and the hangup hint follow the right device.
    else if ((m_state == Connecting || m_state == Active || m_state == Reconnecting)
             && !m_remoteSessionId.isEmpty()
             && isOneToOneCall() && isPeerUserSession(sessionId)
             && (flags & (CALL_FLAG_WITH_AUDIO | CALL_FLAG_WITH_VIDEO))
             && !m_subscribePipelines.contains(m_remoteSessionId)) {
        qInfo() << "CallManager: 1:1 peer sibling with media joined ("
                << sessionId.left(20) << ") — re-adopting from idle sibling"
                << m_remoteSessionId.left(20) << "(#bug4)";
        m_remoteSessionId = sessionId;
        emit callInfoChanged();
        requestPeerStream(sessionId);   // self-dedupes with the group path
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
        } else if (m_signaling->currentRoom() == m_lastRingoutToken
                   && m_lastRingoutTime.isValid()
                   && m_lastRingoutTime.msecsTo(QDateTime::currentDateTime()) < 15000) {
            // #bug4 -- late answer: the peer joined the call we were placing
            // seconds after our ring-out teardown. To the user this IS our
            // call being answered, not a new incoming one — re-join it.
            // Gated on an actual participant JOIN edge (not the conversation-
            // list poll, which could re-fire on stale room state) and single-
            // use, so a participant-flag flap can at worst re-place the call
            // once. startCall's forceCallParticipantResync re-emits the join
            // so the normal adopt path runs against a clean slate.
            const QString token   = m_lastRingoutToken;
            const bool withVideo  = m_lastRingoutWithVideo;
            m_lastRingoutToken.clear();
            m_lastRingoutTime = QDateTime();
            qInfo() << "CallManager: peer" << joinerUser << "answered" << token
                    << "within the ring-out window — re-joining our call (#bug4)";
            startCall(token, withVideo);
        } else if (m_lastHangupTime.isValid()
                   && m_lastHangupTime.msecsTo(QDateTime::currentDateTime()) < 6000
                   && m_lastHangupSids.contains(sessionId)) {
            // #79 -- a session we JUST hung up on, flapping its inCall flag
            // during teardown. It reaches us here (Idle) only after we rejoined
            // the viewed room, so currentRoom() differs from m_lastHangupToken
            // and the token-keyed cooldown in onIncomingCallDetected can't catch
            // it. Guard by the hung-up SESSION id instead: this is teardown
            // noise, not a real call-back. A genuine re-dial from the same
            // person still rings once this 6s window lapses.
            TLOG_CALL("ignoring post-hangup inCall flap from just-hung-up session "
                      << sessionId.left(20));
        } else {
            // Incoming call detected via signaling — route through the same
            // path as conversation-list detection so cooldown applies.
            const QString token = m_signaling->currentRoom();
            onIncomingCallDetected(displayName, token, flags);
            // Latch the sid only if we actually started ringing — a cooldown-
            // suppressed detection must not leave a stale m_remoteSessionId
            // behind while Idle (startCall never clears it; only teardown
            // does), which would jam the next call's adopt gate.
            if (m_state == Incoming) {
                m_remoteSessionId = sessionId;
                if (isOneToOneCall())
                    m_remotePeerUserId = m_signaling->userIdForSession(sessionId);
            }
        }
    }

    // Register every in-call peer in the model (covers peers that join an
    // already-active conference, and refreshes flags). Media providers are
    // attached later when their subscriber/offer arrives.
    // 0.40.15 — also register peers seen during Incoming (the moment we
    // ring) so by the time the callee accepts and the state moves into
    // Outgoing→Connecting→Active, the participant is already in the
    // model. Otherwise the signaling "joined call" event only fires once
    // (prevFlags 0 → inCall>0) and the peer never appears on subsequent
    // state ticks — the stage falls through to "Waiting for others to
    // join" even though the peer is plainly in the room server-side.
    if (m_state == Incoming || m_state == Outgoing
        || m_state == Connecting || m_state == Active) {
        if (auto *p = ensureParticipant(sessionId, displayName)) {
            p->setAudioMuted(!(flags & CALL_FLAG_WITH_AUDIO));
            p->setVideoMuted(!(flags & CALL_FLAG_WITH_VIDEO));
        }
        // GROUP/MCU call — subscribe to EVERY in-call peer that publishes media,
        // not just the first. The 1:1 block above only subscribes m_remoteSessionId
        // (the first joiner); every ADDITIONAL conference peer fell through to here
        // with NO subscriber, so their video never arrived until THEY toggled their
        // camera (the participantFlagsChanged off->on edge — exactly the field bug
        // "we had to stop/start cameras; Ivan saw Kalin but not Ilko"). requestPeerStream
        // self-dedupes on m_subscribePipelines / m_pendingRequestOffers, so this is a
        // no-op for the already-subscribed first peer and on repeated participant-updates.
        if ((m_state == Connecting || m_state == Active)
            && (flags & (CALL_FLAG_WITH_AUDIO | CALL_FLAG_WITH_VIDEO))
            && !m_subscribePipelines.contains(sessionId)
            && !m_pendingRequestOffers.contains(sessionId)) {
            qInfo() << "CallManager: group call — subscribing additional in-call peer"
                    << sessionId.left(20);
            requestPeerStream(sessionId);
        }
        // #bug4 -- track every in-call session of the 1:1 peer USER so leave
        // handling can tell "one device dropped" from "the peer is gone".
        if (isOneToOneCall() && isPeerUserSession(sessionId))
            m_peerInCallSids.insert(sessionId);
    }
}

void CallManager::onParticipantLeftCall(const QString &sessionId)
{
    if (sessionId == m_signaling->sessionId()) return;

    m_pendingRequestOffers.remove(sessionId);
    m_requestOfferAttempts.remove(sessionId);
    m_requestOfferRejections.remove(sessionId);   // 0.52.7 — offer landed; clear rejection budget

    // Remove subscriber pipeline for this peer
    if (m_subscribePipelines.contains(sessionId)) {
        m_subscribePipelines[sessionId]->stop();
        m_subscribePipelines[sessionId]->deleteLater();
        m_subscribePipelines.remove(sessionId);
        // 0.51.x AEC: release this peer's far-end appsrc + audiomixer request pad
        // in the publisher (the subscriber stopped pushing above; removeFarEndPeer
        // NULLs the appsrc first so any in-flight push short-circuits — no UAF).
        if (m_publishPipeline) m_publishPipeline->removeFarEndPeer(sessionId);
        m_subscriberSids.remove(sessionId);
        m_subStall.remove(sessionId);   // #bug2
        m_signalQuality.remove(sessionId);
        m_desiredSubstream.remove(sessionId);      // 1.0 audit — were leaking a
        m_peerSubstreamWant.remove(sessionId);     // 0.51.x receive-load raw want
        m_subscriberRecoveries.remove(sessionId);  // stale entry per peer-leave
        qDebug() << "CallManager: removed subscriber for" << sessionId.left(20);
    }

    // A peer that leaves the CALL while screen-sharing (incl. an ungraceful
    // crash/WiFi-drop, where no "unshareScreen" message arrives) would otherwise
    // leave a zombie screen subscriber pinning our camera to a single LOW layer
    // for the rest of the call. Drop it + recompute suppression. No-op if none.
    removeScreenSubscriber(sessionId);

    removeParticipant(sessionId);

    m_peerInCallSids.remove(sessionId);   // #bug4 -- prune FIRST; decisions key on what remains

    if (sessionId == m_remoteSessionId) {
        // #bug3 -- peer-grace (do-NOT-end + auto-recover) applies ONLY to a
        // 1:1 call with a KNOWN-userId peer. In a GROUP call m_remoteSessionId
        // is merely the first-joiner, not a sole peer, so a grace hold would wrongly
        // freeze a multi-party call; a guest's userId is unknown so a new-session
        // rejoin can't be correlated. In both those cases keep the clean immediate end.
        const bool isOneToOne = isOneToOneCall();
        const QString peerUserId = m_signaling->userIdForSession(sessionId);
        if (!isOneToOne || peerUserId.isEmpty()) {
            qDebug() << "CallManager: remote peer left call ("
                     << (!isOneToOne ? "group" : "guest")
                     << ") -- ending";
            teardown("Call ended");
            return;
        }
        // #bug4 -- the peer USER may still be in the call on ANOTHER session:
        // this was one device/VPN-half dropping, not the peer leaving. Fail
        // over to a live sibling instead of the 28s grace hold — grace can
        // only be exited by a JOIN edge, which an already-joined sibling never
        // produces, so the old code expired grace and killed a healthy call.
        const QString sibling = pickPeerSiblingSid();
        if (!sibling.isEmpty()) {
            qInfo() << "CallManager: 1:1 peer session" << sessionId.left(20)
                    << "left but sibling" << sibling.left(20)
                    << "is still in-call — failing over, NOT ending (#bug4)";
            m_remoteSessionId = sibling;
            emit callInfoChanged();
            requestPeerStream(sibling);        // no-op if already subscribed
            return;
        }
        // MCU 1:1, known peer, NO sibling remains: a transient WiFi-reconnect
        // blip is indistinguishable
        // here (the peer is often still in the room and returns seconds later under
        // a NEW NC session id; m_sessionToUserId is NOT pruned on leave so the userId
        // above resolves). Enter a peer-grace Reconnecting hold and auto-re-subscribe
        // on return. Only a true no-return ends the call (grace timer).
        qInfo() << "CallManager: remote peer left call (sid=" << sessionId.left(20)
                << "user=" << peerUserId << ") -- entering peer-grace, NOT ending";
        m_remotePeerUserId = peerUserId;
        m_graceLeftSid     = sessionId;
        m_peerGraceActive  = true;
        m_remoteSessionId.clear();        // reopen the empty-guarded adopt paths
        emit callInfoChanged();
        if (m_state == Active || m_state == Connecting)
            setState(Reconnecting);
        setStatusDetail(tr("Reconnecting, waiting for peer..."));
        if (!m_peerGraceTimer.isActive())
            m_peerGraceTimer.start();
    }
    // #bug4 -- the REAL (subscribed) sibling left while a zombie sibling is
    // still adopted: without this, only the subscriber was dropped and the
    // 1:1 end/grace logic never ran — a ghost call that never ends. Treat it
    // as the peer-user leaving that device; if it was the LAST media-bearing
    // one and none remain, fall into the same grace the adopted-sid path uses.
    else if (isOneToOneCall() && isPeerUserSession(sessionId)
             && m_peerInCallSids.isEmpty() && !m_remoteSessionId.isEmpty()
             && !m_subscribePipelines.contains(m_remoteSessionId)) {
        qInfo() << "CallManager: 1:1 peer's last live session" << sessionId.left(20)
                << "left; adopted sid" << m_remoteSessionId.left(20)
                << "has no media — entering grace (#bug4)";
        m_remotePeerUserId = m_signaling->userIdForSession(sessionId);
        m_graceLeftSid     = sessionId;
        m_peerGraceActive  = true;
        m_remoteSessionId.clear();
        emit callInfoChanged();
        if (m_state == Active || m_state == Connecting)
            setState(Reconnecting);
        setStatusDetail(tr("Reconnecting, waiting for peer..."));
        if (!m_peerGraceTimer.isActive())
            m_peerGraceTimer.start();
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

    // Orphan-audio-after-hangup guard. The MCU keeps emitting subscriber offers
    // for a beat after we leave (especially in a dual-call/glare scramble where
    // several requestOffers were in flight). teardown() clears our pending/retry
    // state, but an offer ALREADY on the wire still lands here — and without this
    // guard the block below builds a fresh SubscribeWebrtcSrc, answers it, and
    // plays the peer's audio out its own wasapi2sink while m_state is Idle, i.e.
    // with no call screen. Reject only once the call has fully torn down
    // (Idle/Ending); an offer can legitimately arrive during the Outgoing/Incoming
    // ring when a peer is already in an open room (processPendingOffers flush).
    if (callTornDown()) {
        qInfo() << "CallManager: ignoring subscriber offer from"
                << fromSessionId.left(20) << "— call torn down (state"
                << m_state << ")";
        return;
    }

    // Guard: don't create subscribers until ICE servers are available
    if (m_stunServer.isEmpty()) {
        m_pendingOffers.append({fromSessionId, sdp, sid});
        qDebug() << "CallManager: queuing offer — ICE servers not yet available";
        return;
    }

    // An offer arrived for this peer — stop retrying requestoffer for it.
    // Data-only subscriber offer (m=application/datachannel only, NO m=video)
    // means the remote publisher's media is not registered in Janus yet -- we
    // requested the offer before their camera feed came up (a race when joining
    // a call where the peer is already present, e.g. an open room). Building a
    // subscriber from it yields a permanently video-less "waiting for video"
    // tile, AND stops the requestoffer retry (m_subscribePipelines then
    // contains the sid). Instead drop this offer and keep re-requesting until
    // the MCU returns a real audio/video offer -- exactly what the official
    // Talk client does (gate on flags + retry on its requestoffer timer).
    // Verified against spreed: a correct requestoffer returns full a/v/data in
    // one shot; data-only == publisher not yet publishing media.
    if (!sdp.contains("m=video")) {
        qInfo() << "CallManager: subscriber offer from" << fromSessionId.left(20)
                << "is data-only (no m=video) -- publisher media not ready, re-requesting";
        if (!m_pendingRequestOffers.contains(fromSessionId)) {
            m_pendingRequestOffers.insert(fromSessionId);
            m_requestOfferAttempts[fromSessionId] = 0;
        }
        if (!m_requestOfferRetry.isActive())
            m_requestOfferRetry.start();
        return;
    }

    m_pendingRequestOffers.remove(fromSessionId);
    m_requestOfferAttempts.remove(fromSessionId);
    m_requestOfferRejections.remove(fromSessionId);   // 0.52.7 — real offer landed; clear rejection budget

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
        m_subStall.remove(fromSessionId);   // #bug2 -- re-baseline the fresh subscriber
        m_signalQuality.remove(fromSessionId);
    }

    // New subscriber
    auto *sub = new SubscribeWebrtcSrc(fromSessionId, this);
    // AEC (0.51.x single-pipeline fix): when the publisher built its inline
    // playout tail (echoCancellation on), route THIS peer's decoded audio into
    // the publisher's far-end mixer (and play it out via the one shared, probed
    // sink) instead of the subscriber's own wasapi2sink. addFarEndPeer returns
    // the appsrc to push into; null (AEC off / tail failed) → legacy per-sub sink.
    if (m_publishPipeline && m_publishPipeline->aecPlayoutActive()) {
        if (GstElement *src = m_publishPipeline->addFarEndPeer(fromSessionId))
            sub->setFarEndAppsrc(src);
    }

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

    // Decoded remote-mic level → this peer's VU meter AND active-speaker frame.
    // Routes to the CORRECT remote participant (fromSessionId), unlike
    // onAudioLevelUpdated which is hardwired to the self participant. The data
    // channel's speaking/stoppedSpeaking (above) gives a fast trigger, but many
    // clients send it unreliably — so we ALSO derive 'speaking' from the actual
    // decoded level here (noteAudioLevelForVad), which makes the speaker frame
    // appear from real audio for every peer.
    connect(sub, &SubscribeWebrtcSrc::audioLevelUpdated,
            this, [this, fromSessionId](double level) {
        if (auto *p = m_participants.value(fromSessionId)) {
            p->setAudioLevel(level);
            p->noteAudioLevelForVad(level);
        }
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
        // bug 3 — feed the LIVE in-call version back into the persisted,
        // freshness-stamped cache so the conversation header / sidebar / chat
        // badge show a current value after the call instead of reverting to a
        // long-stale signaling-overheard one. This also heals devices on a
        // server without standalone HPB, where signaling broadcasts never run
        // but a call's data channel does. Map session → userId first.
        if (m_signaling) {
            const QString peerUid = m_signaling->userIdForSession(fromSessionId);
            if (!peerUid.isEmpty())
                m_signaling->updatePeerClient(peerUid, info);
        }
    });

    connect(sub, &SubscribeWebrtcSrc::error, this, [this, fromSessionId](const QString &msg) {
        qWarning() << "CallManager: subscriber pipeline error for" << fromSessionId.left(20) << ":" << msg;
        // B2/B1 — a DECODER-domain error (broken HW decoder: d3d11 0x80004002,
        // not-negotiated from a *dec element, a HW h264/vp8 decoder erroring out)
        // means this feed will never decode on the hardware path. Latch software
        // decode, demote the HW decoder ranks so decodebin re-plugs avdec_h264,
        // persist for next launch, and rebuild this subscriber on software. Was:
        // logged and IGNORED -> permanent black tile. Plain transport errors are
        // deliberately left to the ICE-failed / sessionEnded paths (no new churn).
        const QString m = msg.toLower();
        const bool decoderFault =
            m.contains("0x80004002") || m.contains("not supported")
            || m.contains("not-negotiated") || m.contains("d3d11")
            || m.contains("h264dec") || m.contains("vp8dec") || m.contains("vp9dec")
            || m.contains("decode");
        if (!decoderFault) return;
        if (!talqForceSoftwareDecode().load()) {
            qWarning() << "CallManager: decoder fault -> forcing SOFTWARE video decode (demote HW, persist)";
            talqForceSoftwareDecode().store(true);
            talqDemoteHwVideoDecoders();
            QSettings("TalQ", "TalQ").setValue("Video/forceSoftwareDecode", true);
        }
        recoverSubscriber(fromSessionId, QStringLiteral("decoder-fault"));
    });

    connect(sub, &SubscribeWebrtcSrc::sessionEnded, this, [this, fromSessionId]() {
        qInfo() << "CallManager: subscriber feed ended by SFU for"
                << fromSessionId.left(20) << "— re-subscribing (call stays up)";
        recoverSubscriber(fromSessionId, QStringLiteral("end-session"));
    });

    m_subscribePipelines[fromSessionId] = sub;
    if (!sub->start(m_stunServer, effectiveTurnServers(), m_deviceManager ? m_deviceManager->selectedOutputDeviceId() : QString())) {
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

    // Trickle-ICE race fix: flush remote candidates that arrived before this
    // subscriber existed (the MCU sends them with/just before the offer, which
    // can land ~100ms before we build the subscriber). The remote description
    // is now set, so webrtcbin will accept them. Without this the subscriber
    // can start with ZERO remote candidates -> ICE stuck at "new" -> permanent
    // "waiting for video" (the timing race is why some peers connect and some
    // don't on the same call).
    if (m_pendingSubCandidates.contains(fromSessionId)) {
        const auto pend = m_pendingSubCandidates.take(fromSessionId);
        for (const auto &pc : pend)
            sub->addIceCandidate(pc.candidate, pc.mline, pc.mid);
        qInfo() << "CallManager: flushed" << pend.size()
                << "queued remote ICE candidates into subscriber" << fromSessionId.left(20);
    }
}

void CallManager::onAnswerReceived(const QString &fromSessionId, const QString &sdp)
{
    setStatusDetail("Received answer");
    qDebug() << "CallManager: received answer from" << fromSessionId.left(20);

    // MCU answer to our publisher offer (from our own session ID)
    if (m_publishPipeline) {
        m_publishPipeline->setRemoteAnswer(sdp);
        qDebug() << "CallManager: set publisher remote answer";

        // Publisher renegotiation (e.g. camera toggle) doesn't affect subscribers.
        // Subscribers receive from the MCU independently — no re-request needed.
    }
}
