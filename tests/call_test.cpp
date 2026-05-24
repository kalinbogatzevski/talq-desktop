/**
 * TalQ WebRTC Call Test Harness
 *
 * Automated end-to-end test for MCU-based calls:
 * 1. Authenticates two users against the real Nextcloud server
 * 2. Both join the test conversation and start the call
 * 3. Both create WebRTC pipelines and send offers via HPB
 * 4. The MCU answers each peer independently
 * 5. ICE connects between each peer and the MCU
 * 6. Verifies stable ICE connection for 3 seconds
 * 7. Tears down cleanly
 *
 * Architecture (MCU mode):
 *   UserA <--WebRTC--> MCU <--WebRTC--> UserB
 *   Each peer negotiates independently with the MCU.
 *   The MCU bridges audio between participants.
 *
 * Usage: talq-call-test.exe [--token TOKEN] [--timeout SECS]
 */

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QCommandLineParser>
#include <QDebug>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkReply>
#include <QRegularExpression>
#include <gst/gst.h>
#include <glib.h>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

#include "core/ApiClient.h"
#include "core/SignalingClient.h"
#include "core/PeerPipeline.h"
#include "core/PublishPipeline.h"     // #111: real-app publish path under test
#include "core/BackgroundEngine.h"    // #20  BG-engine scenarios
#include "core/SubscribeWebrtcSrc.h"  // subscriber under test (gst-plugins-rs)
#include "core/ScreenSharePipeline.h" // #16: screen-share end-to-end scenario
#include "core/VideoFrameProvider.h"  // frame counter for screen subscriber

// #111 validation mode: when set, peer A publishes through the REAL
// PublishPipeline (synthetic high-motion 720p30 via TALQ_PUB_TESTSRC,
// through the actual encoder + rtpgccbwe-1.2M-floor chain) instead of
// PeerPipeline. Peer B subscribes it via SubscribeWebrtcSrc and we
// assert distinct fps ≈ delivered fps — i.e. the GCC floor fix stops
// the videorate CFR duplicate-padding. Headless, no camera, no human.
static bool g_pubPipe = false;

// #135 / callee-mid-call-camera-on validation mode: implies PUBPIPE,
// but defers enableCamera() ~8 s after PublishPipeline.start so the
// dummy stream runs through its full 5 s grace + halt cycle before
// the camera flips on. Exercises the exact bug pattern (dummy phase
// fully elapsed → camera enabled mid-call → measure RX). PASS bar:
// same distinct ≥ 75% of delivered. If the dummy-halt structural fix
// is correct, this passes. Headless, no human.
static bool g_cameraToggle = false;

// #132 simulcast SDP verification. Implies PUBPIPE. After offer is sent,
// parse the publisher SDP and assert it carries a=simulcast: send l;m;h
// plus three a=rid:* send lines (RFC 8853). Per-substream RX validation
// requires Janus's requestoffer substream param that the harness doesn't
// currently exercise — RX is field-verified.
static bool g_simulcast = false;

// #132 simulcast BWE-driven layer-disable. Implies SIMULCAST + PUBPIPE.
// Drives an env-gated synthetic BWE override through PublishPipeline
// (TALQ_TEST_BWE_OVERRIDE_KBPS=N) to step 1500 → 400 → 100 kbps; gate
// verdict via the qInfo "simulcast layer 'X' -> MUTED" lines from
// PublishPipeline::setLayerActive.
static bool g_simulcastDrop = false;

// #16 screen-share end-to-end scenario. Implies PUBPIPE + TALQ_PUB_TESTSRC
// + TALQ_SS_TESTSRC (synthetic videotestsrc in place of d3d11 capture).
// After both peers reach VideoActive, peer A spawns a ScreenSharePipeline
// and publishes to the MCU; peer B receives a fresh offer with
// roomType=screen, builds a separate screen subscriber, and the harness
// counts frames on its provider. Verifies the structural #10 path (own
// PC per share, separate sid, separate ICE) without any human / desktop
// interaction.
static bool g_screenShare = false;

// #17 mute-toggle propagation scenario. Implies PUBPIPE. After
// VideoActive, peer A sends mute/unmute control messages over signaling
// (the same wire format CallManager::broadcastMediaState produces).
// Peer B should observe remoteMuteChanged on its SignalingClient within
// a short window. Regression-guards the control-channel path the call
// screen uses for the remote mic/camera icons.
static bool g_muteToggle = false;

// #20 BG-engine scenarios. All three imply PUBPIPE + synthetic camera.
// Peer A wires a BackgroundEngine into its PublishPipeline before the
// pipeline starts, in the indicated mode. Verdict:
//   _BG_BLUR     mode=Blur,  expect bgBridgeFramesProcessed  >> 0
//                            and bgBridgeFramesPassThrough == 0
//   _BG_IMAGE    mode=Image, same assertion (engine bound)
//   _BG_FALLBACK mode=None,  expect bgBridgeFramesPassThrough >> 0
//                            and bgBridgeFramesProcessed   == 0
// Common in all three: B must receive > 0 frames at distinct ≈ delivered,
// proving the bridge round-trip preserved PTS+timing for the encoder.
static bool g_bgBlur     = false;
static bool g_bgImage    = false;
static bool g_bgFallback = false;

// Test configuration. Identities/token come from the environment so the
// harness never rings a real person's devices. Defaults are the two
// dedicated bot accounts in the "TalQ Auto Test" group room — NEVER a
// human account. Override via TALQ_TEST_USERA / _USERA_PASS / _USERB /
// _USERB_PASS / _TOKEN (load C:\Users\bogat\.talq-test.env first).
static const QString SERVER = "https://ncloud.123net.link";
static QString USER_A;  // resolved in main() from env (default test-talq2)
static QString USER_B;  // resolved in main() from env (default test-talq)
static QString PASS_A;
static QString PASS_B;
static const QString DEFAULT_TOKEN = "aqzoti8m";  // TalQ Auto Test (bots only)

// Credential helper (legacy CredMan path; kept for manual runs)
[[maybe_unused]] static QString loadPasswordFromCredMan()
{
#ifdef Q_OS_WIN
    PCREDENTIALW cred = nullptr;
    if (CredReadW(L"TalQ/NextcloudAppPassword", CRED_TYPE_GENERIC, 0, &cred)) {
        QString pw = QString::fromUtf8(
            reinterpret_cast<const char *>(cred->CredentialBlob),
            static_cast<int>(cred->CredentialBlobSize));
        CredFree(cred);
        return pw;
    }
#endif
    return {};
}

// Test state machine
enum TestPhase {
    Init,
    SignalingConnecting,
    JoiningRoom,
    StartingCall,
    WaitingForPeer,
    Negotiating,
    WaitingICE,
    Active,
    VideoRenegotiatingA,
    VideoRenegotiatingB,
    VideoWaitingFrames,
    VideoActive,
    TearingDown,
    Done
};

static const char *phaseStr(TestPhase p) {
    switch (p) {
    case Init: return "Init";
    case SignalingConnecting: return "SignalingConnecting";
    case JoiningRoom: return "JoiningRoom";
    case StartingCall: return "StartingCall";
    case WaitingForPeer: return "WaitingForPeer";
    case Negotiating: return "Negotiating";
    case WaitingICE: return "WaitingICE";
    case Active: return "Active";
    case VideoRenegotiatingA: return "VideoRenegotiatingA";
    case VideoRenegotiatingB: return "VideoRenegotiatingB";
    case VideoWaitingFrames: return "VideoWaitingFrames";
    case VideoActive: return "VideoActive";
    case TearingDown: return "TearingDown";
    case Done: return "Done";
    }
    return "?";
}

struct TestPeer {
    QString name;
    ApiClient *api = nullptr;
    SignalingClient *signaling = nullptr;
    PeerPipeline *pipeline = nullptr;
    PublishPipeline *pubPipeline = nullptr;            // #111 mode: peer A only
    BackgroundEngine *bgEngine  = nullptr;             // #20  BG scenarios (peer A only)
    SubscribeWebrtcSrc *subscribePipeline = nullptr;  // remote peer's stream via MCU (webrtcsrc)
    TestPhase phase = Init;
    QString sessionId;       // HPB session ID
    QString remoteSessionId; // other peer's HPB session ID
    bool joinedCall = false;
    bool iceConnected = false;
    bool pipelineStarted = false;
    bool videoRenegSdpValid = false;   // true if renegotiation SDP has active m=video
    bool videoAnswerReceived = false;  // true when MCU answers our video renegotiation
    bool simulcastSdpPass = false;     // #132: a=simulcast: send l;m;h + three a=rid lines
    bool subscriberRequested = false;  // true after requestOffer for remote peer
    bool substreamRequested = false;   // #132 debug: selectStream sent once
    QString subscriberSid;   // MCU-assigned sid from the subscriber offer
    int remoteVideoFramesBefore = 0;   // frame count before waiting
    int remoteVideoFramesAfter = 0;    // frame count after waiting
    // #16 screen-share state — peer A is the publisher, peer B the subscriber.
    // Lifecycle parallel to the camera publish/subscribe pair but on its own
    // ScreenSharePipeline + SubscribeWebrtcSrc and its own sid.
    ScreenSharePipeline *screenSharePipeline = nullptr;     // peer A
    SubscribeWebrtcSrc *screenSubscribePipeline = nullptr;  // peer B
    QString screenShareSid;                                 // peer A outbound
    QString screenSubscriberSid;                            // peer B inbound
    bool screenSharePubIceConnected = false;
    bool screenShareKicked = false;
    int screenFramesBefore = 0;
    int screenFramesAfter  = 0;
    // #17 mute toggle scenario — peer B observes remote A's mute changes.
    bool muteToggleKicked = false;
    int remoteAudioMutedEvents = 0;
    int remoteAudioUnmutedEvents = 0;
    int remoteVideoMutedEvents = 0;
    int remoteVideoUnmutedEvents = 0;
    QString stunServer;
    QList<TurnServer> turnServers;
    // Pending ICE candidates (received before pipeline started)
    QList<std::tuple<QString, int, QString>> pendingCandidates;
    // Pending subscriber ICE candidates (received before subscribe pipeline started)
    QList<std::tuple<QString, int, QString>> subPendingCandidates;
    // #16 pending candidates for the screen subscribe pipeline (offer can
    // race ICE trickles); flushed when the screen subscriber starts.
    QList<std::tuple<QString, int, QString>> screenSubPendingCandidates;

    void log(const QString &msg) {
        qDebug().noquote() << QString("[%1] %2").arg(name, msg);
    }

    void setPhase(TestPhase p) {
        phase = p;
        log(QString("Phase -> %1").arg(phaseStr(p)));
    }
};

class CallTest : public QObject
{
    Q_OBJECT
public:
    CallTest(const QString &token, int timeout, QObject *parent = nullptr)
        : QObject(parent), m_token(token)
    {
        m_timeout.setSingleShot(true);
        m_timeout.setInterval(timeout * 1000);
        connect(&m_timeout, &QTimer::timeout, this, [this]() {
            qCritical() << "TIMEOUT -- test did not complete in time";
            printSummary(false);
            qApp->exit(1);
        });

        m_activeTimer.setInterval(3000);
        m_activeTimer.setSingleShot(true);
        connect(&m_activeTimer, &QTimer::timeout, this, &CallTest::onActiveTimerDone);
    }

    void run()
    {
        m_timeout.start();
        gst_init(nullptr, nullptr);

        if (PASS_A.isEmpty() || PASS_B.isEmpty()) {
            qCritical() << "Missing test credentials. Set TALQ_TEST_USERA_PASS"
                           " and TALQ_TEST_USERB_PASS (source .talq-test.env).";
            qApp->exit(1);
            return;
        }
        qDebug().noquote() << QString("Identities: A=%1  B=%2  (no human accounts)")
                                  .arg(USER_A, USER_B);

        setupPeer(m_peerA, "UserA(" + USER_A + ")", SERVER, USER_A, PASS_A);
        setupPeer(m_peerB, "UserB(" + USER_B + ")", SERVER, USER_B, PASS_B);

        m_peerA.setPhase(SignalingConnecting);
        m_peerB.setPhase(SignalingConnecting);
        m_peerA.signaling->start();
        m_peerB.signaling->start();
    }

private:
    void setupPeer(TestPeer &peer, const QString &name, const QString &server,
                   const QString &user, const QString &password)
    {
        peer.name = name;
        peer.api = new ApiClient(this);
        peer.api->setServerUrl(server);
        peer.api->setCredentials(user, password);

        peer.signaling = new SignalingClient(peer.api, this);

        // Signaling connected
        connect(peer.signaling, &SignalingClient::connectedChanged, this, [this, &peer]() {
            if (!peer.signaling->isConnected()) return;
            peer.sessionId = peer.signaling->sessionId();
            peer.log("Signaling connected, sessionId=" + peer.sessionId.left(30) + "...");

            // Fetch STUN/TURN
            fetchIceServers(peer);
        });

        // Answer received from MCU (in MCU mode, the MCU answers our offer)
        connect(peer.signaling, &SignalingClient::answerReceived, this,
                [this, &peer](const QString &from, const QString &sdp,
                              const QString &roomType) {
            peer.log("Answer received from " + from.left(20) + "... roomType="
                     + (roomType.isEmpty() ? "<empty>" : roomType)
                     + " (" + QString::number(sdp.length()) + " chars)");
            // #16: screen-pub answer goes to the screen publisher pipeline,
            // not the camera one — otherwise the camera webrtcbin sees a
            // mismatched SDP and the screen publisher waits forever.
            if (roomType == "screen" && peer.screenSharePipeline) {
                peer.screenSharePipeline->setRemoteAnswer(sdp);
                return;
            }
            if (peer.pubPipeline) {
                peer.pubPipeline->setRemoteAnswer(sdp);
                peer.setPhase(WaitingICE);
                return;
            }
            if (peer.pipeline) {
                peer.pipeline->setRemoteAnswer(sdp);
                // Track whether this is a video renegotiation answer
                if (peer.phase == VideoRenegotiatingA || peer.phase == VideoRenegotiatingB) {
                    peer.videoAnswerReceived = true;
                    peer.log("Video renegotiation answer received from MCU");
                    checkBothVideoRenegotiated();
                } else {
                    peer.setPhase(WaitingICE);
                }
            }
        });

        // Offer received from MCU (subscriber flow: MCU sends offer for remote peer's stream)
        connect(peer.signaling, &SignalingClient::offerReceived, this,
                [this, &peer](const QString &from, const QString &sdp, const QString &sid,
                              const QString &roomType) {
            peer.log("Subscriber offer from MCU for remote: " + from.left(20)
                     + "... sid=" + sid + " roomType=" + (roomType.isEmpty() ? "<empty>" : roomType)
                     + " (" + QString::number(sdp.length()) + " chars)");
            // The MCU assigns a fresh sid per (re)offer; the subscriber's
            // answer + candidates MUST echo it back to the remote peer's
            // session id (not ours) or Janus can't bind them to the
            // subscriber handle and ICE fails. Read live so re-offers work.
            if (roomType == "screen") {
                // #16 — fresh screen subscriber, separate from the camera
                // one. Janus issues this offer in response to our prior
                // "sendoffer roomType=screen" minimal message.
                peer.screenSubscriberSid = sid;
                startScreenSubscribePipeline(peer, from, sdp);
            } else {
                peer.subscriberSid = sid;
                startSubscribePipeline(peer, from, sdp);
            }
        });

        // #17 — observe remote mute state transitions. The signal fires on
        // every "mute"/"unmute" session message from any peer in the room.
        connect(peer.signaling, &SignalingClient::remoteMuteChanged, this,
                [&peer](const QString &sid, const QString &media, bool muted) {
            peer.log(QString("remoteMuteChanged from %1: %2 = %3")
                     .arg(sid.left(12), media, muted ? "MUTED" : "UNMUTED"));
            if (media == "audio") {
                if (muted) peer.remoteAudioMutedEvents++;
                else       peer.remoteAudioUnmutedEvents++;
            } else if (media == "video") {
                if (muted) peer.remoteVideoMutedEvents++;
                else       peer.remoteVideoUnmutedEvents++;
            }
        });

        // ICE candidates from MCU. Route exactly like the real app
        // (CallManager::candidateReceived): Janus trickles candidates for
        // TWO independent transports — the publisher (our own session id)
        // and each subscriber feed (the remote peer's session id). Feeding
        // every candidate into the publisher pipeline (the old behaviour)
        // left the subscribe pipeline without remote candidates, so its
        // ICE never left "new" and zero remote frames ever arrived.
        connect(peer.signaling, &SignalingClient::candidateReceived, this,
                [this, &peer](const QString &from, const QJsonObject &candidate,
                              const QString &roomType) {
            QString cand = candidate["candidate"].toString();
            int mline = candidate["sdpMLineIndex"].toInt();
            QString mid = candidate["sdpMid"].toString();
            peer.log("ICE candidate from " + from.left(12) + " roomType="
                     + (roomType.isEmpty() ? "<empty>" : roomType)
                     + ": " + cand.left(60));

            // #16 screen pub transport: candidates for our own session id
            // with roomType=screen go to the screen publisher.
            if (roomType == "screen" && from == peer.sessionId) {
                if (peer.screenSharePipeline)
                    peer.screenSharePipeline->addIceCandidate(cand, mline, mid);
                return;
            }
            // #16 screen sub transport: candidates for the remote peer
            // with roomType=screen go to the screen subscriber.
            if (roomType == "screen") {
                if (peer.screenSubscribePipeline)
                    peer.screenSubscribePipeline->addIceCandidate(cand, mline, mid);
                else
                    peer.screenSubPendingCandidates.append({cand, mline, mid});
                return;
            }

            // Publisher transport: candidates for our own session id.
            if (from == peer.sessionId) {
                if (peer.pubPipeline)
                    peer.pubPipeline->addIceCandidate(cand, mline, mid);
                else if (peer.pipeline && peer.pipeline->isRunning())
                    peer.pipeline->addIceCandidate(cand, mline, mid);
                else
                    peer.pendingCandidates.append({cand, mline, mid});
                return;
            }

            // Subscriber transport: candidates for the remote peer's feed.
            if (peer.subscribePipeline) {
                peer.subscribePipeline->addIceCandidate(cand, mline, mid);
            } else {
                peer.subPendingCandidates.append({cand, mline, mid});
            }
        });

        // Room joined via signaling
        connect(peer.signaling, &SignalingClient::roomJoined, this, [this, &peer]() {
            peer.log("Room joined via signaling");
            peer.setPhase(StartingCall);
            joinCall(peer);
        });

        // Participant joined call — track remote peer's session ID
        connect(peer.signaling, &SignalingClient::participantJoinedCall, this,
                [this, &peer](const QString &sessionId, int flags, const QString &) {
            peer.log(QString("Participant in call: %1 flags=%2").arg(sessionId.left(20)).arg(flags));
            if (sessionId != peer.sessionId)
                peer.remoteSessionId = sessionId;
        });
    }

    void fetchIceServers(TestPeer &peer)
    {
        auto *reply = peer.api->getRaw("apps/spreed/api/v3/signaling/settings");
        connect(reply, &QNetworkReply::finished, this, [this, &peer, reply]() {
            reply->deleteLater();
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            QJsonObject data = doc["ocs"]["data"].toObject();

            // Helper: extract URLs that may be a JSON array or a space-separated string
            auto extractUrls = [](const QJsonValue &val) -> QStringList {
                if (val.isArray()) {
                    QStringList list;
                    for (const auto &u : val.toArray())
                        list.append(u.toString());
                    return list;
                }
                if (val.isString())
                    return val.toString().split(' ', Qt::SkipEmptyParts);
                return {};
            };

            // STUN
            QJsonArray stunArr = data["stunservers"].toArray();
            if (!stunArr.isEmpty()) {
                QStringList stunUrls = extractUrls(stunArr[0].toObject()["urls"]);
                if (!stunUrls.isEmpty()) peer.stunServer = stunUrls[0];
            }

            // TURN
            QJsonArray turnArr = data["turnservers"].toArray();
            for (const auto &t : turnArr) {
                TurnServer ts;
                QStringList turnUrls = extractUrls(t.toObject()["urls"]);
                for (const auto &u : turnUrls)
                    ts.urls.append(u);
                ts.username = t.toObject()["username"].toString();
                ts.credential = t.toObject()["credential"].toString();
                peer.turnServers.append(ts);
            }

            peer.log(QString("ICE servers: STUN=%1, TURN=%2 servers")
                     .arg(peer.stunServer).arg(peer.turnServers.size()));

            // Join room
            peer.setPhase(JoiningRoom);
            peer.api->post("apps/spreed/api/v4/room/" + m_token + "/participants/active",
                          [this, &peer](bool ok, const QJsonObject &, int) {
                if (!ok) peer.log("WARNING: room join REST failed (may already be joined)");
                peer.signaling->joinRoom(m_token);
            });
        });
    }

    void joinCall(TestPeer &peer)
    {
        // flags: 1=IN_CALL|2=WITH_AUDIO|4=WITH_VIDEO -> 7. Publish video
        // from the start and subscribe once — mirrors the real app's
        // CallManager flow. No audio-first + forceReconnect reneg dance
        // (that stop/recreate churn manufactured the orphan webrtcbin/
        // DTLS transports that wedged subscriber DTLS at role=server).
        QJsonObject body;
        body["flags"] = 7;
        peer.api->post("apps/spreed/api/v4/call/" + m_token, body,
                      [this, &peer](bool ok, const QJsonObject &, int status) {
            if (!ok) {
                peer.log(QString("ERROR: join call failed, status=%1").arg(status));
                qApp->exit(1);
                return;
            }
            peer.joinedCall = true;
            peer.log("Joined call successfully");
            peer.setPhase(WaitingForPeer);

            // In MCU mode, each peer independently creates a pipeline and sends offer
            // The MCU answers and bridges audio between peers
            startMcuPipeline(peer);
        });
    }

    void startMcuPipeline(TestPeer &peer)
    {
        // #111 proxy: peer A publishes through the real PublishPipeline.
        if (g_pubPipe && &peer == &m_peerA) {
            startPublishPipeline(peer);
            return;
        }
        peer.pipeline = new PeerPipeline(this);
        peer.setPhase(Negotiating);

        // Wire pipeline signals
        // In MCU mode, the offer goes to the MCU (recipient = own session)
        connect(peer.pipeline, &PeerPipeline::localOfferReady, this,
                [this, &peer](const QString &sdp) {
            peer.log("Local offer ready (" + QString::number(sdp.length()) + " chars)");
            // Publisher offers video from the start now — validate it so
            // the pass criteria still gates on a valid active m=video.
            peer.videoRenegSdpValid = validateVideoSdp(peer, sdp);
            peer.signaling->sendOffer(peer.sessionId, sdp, "mcu-test");
        });

        connect(peer.pipeline, &PeerPipeline::localAnswerReady, this,
                [this, &peer](const QString &sdp) {
            peer.log("Local answer ready (" + QString::number(sdp.length()) + " chars)");
            peer.signaling->sendAnswer(peer.sessionId, sdp, "mcu-test");
        });

        connect(peer.pipeline, &PeerPipeline::iceCandidateReady, this,
                [this, &peer](const QString &cand, int mline, const QString &mid) {
            peer.log("Local ICE candidate: " + cand.left(80));
            QJsonObject c;
            c["candidate"] = cand;
            c["sdpMLineIndex"] = mline;
            c["sdpMid"] = mid;
            peer.signaling->sendCandidate(peer.sessionId, c, "mcu-test");
        });

        connect(peer.pipeline, &PeerPipeline::iceStateChanged, this,
                [this, &peer](const QString &state) {
            peer.log("ICE state: " + state);
            if (state == "connected" || state == "completed") {
                peer.iceConnected = true;
                peer.setPhase(Active);
                checkBothActive();
            } else if (state == "failed") {
                peer.log("ERROR: ICE failed!");
                // Don't exit immediately -- let the other peer also report
            }
        });

        connect(peer.pipeline, &PeerPipeline::error, this, [&peer](const QString &err) {
            peer.log("Pipeline error: " + err);
        });

        // Start pipeline
        bool ok = peer.pipeline->start(peer.stunServer, peer.turnServers);
        if (!ok) {
            peer.log("ERROR: Pipeline failed to start");
            qApp->exit(1);
            return;
        }
        peer.pipelineStarted = true;
        peer.log("Pipeline started");

        // Publish video from the start so the first publisher offer
        // carries active m=video. Peer A uses the REAL camera (real
        // capture+H264 encode → MCU → peer B decodes it — the meaningful
        // real-path proof); peer B uses videotestsrc, because two
        // mfvideosrc consumers of one device in one process race
        // ("Internal data stream error"). The real app has a single
        // consumer, so it always uses the real camera.
        const bool useSyntheticVideo = (&peer == &m_peerB);
        peer.pipeline->enableCamera(0, false, useSyntheticVideo);

        // Flush pending ICE candidates
        for (auto &[cand, mline, mid] : peer.pendingCandidates)
            peer.pipeline->addIceCandidate(cand, mline, mid);
        peer.pendingCandidates.clear();

        // GLib bus pump (GLib main loop runs in separate thread for ICE)
        auto *busTimer = new QTimer(this);
        connect(busTimer, &QTimer::timeout, this, [&peer]() {
            if (peer.pipeline) peer.pipeline->pollBus();
        });
        busTimer->start(50);

        // Delay createOffer by 500ms to let pipeline reach PLAYING
        QTimer::singleShot(500, this, [this, &peer]() {
            if (!peer.pipeline) return;
            peer.log("Creating offer for MCU...");
            peer.pipeline->createOffer();
        });
    }

    // #111: publish peer A via the real PublishPipeline (synthetic
    // high-motion 720p30 through the actual encoder + rtpgccbwe 1.2M
    // floor). It auto-offers on on-negotiation-needed (no createOffer()).
    void startPublishPipeline(TestPeer &peer)
    {
        peer.pubPipeline = new PublishPipeline(this);
        peer.setPhase(Negotiating);

        connect(peer.pubPipeline, &PublishPipeline::localOfferReady, this,
                [this, &peer](const QString &sdp) {
            peer.log("PublishPipeline offer (" + QString::number(sdp.length()) + " chars)");
            peer.videoRenegSdpValid = validateVideoSdp(peer, sdp);
            // #132: assert multi-stream publisher SDP. We accept TWO valid
            // shapes:
            //   (a) Canonical RFC 8853: ONE m=video block with
            //       a=simulcast: send l;m;h plus three a=rid:* send lines.
            //   (b) gst-webrtcbin 1.28 actual emit: THREE m=video blocks,
            //       each with rid=X in its a=fmtp:96 line and distinct
            //       SSRCs. Functionally identical for Janus videoroom —
            //       the publisher pushes three SSRC'd streams the SFU
            //       routes per-subscriber. PROXY scenario confirms peer B
            //       receives clean video. Documenting the alternate shape
            //       here so the harness gates the FEATURE not the
            //       SDP-text-formatting choice.
            if (g_simulcast) {
                bool hasSimulcast = false;
                bool ridL = false, ridM = false, ridH = false;
                int videoMlines = 0;
                bool fmtpRidL = false, fmtpRidM = false, fmtpRidH = false;
                for (const QString &line : sdp.split('\n')) {
                    QString t = line.trimmed();
                    if (t.startsWith("m=video")) ++videoMlines;
                    if (t.startsWith("a=simulcast:")) {
                        QString rest = t.section(':', 1).trimmed();
                        if (rest.startsWith("send ")) {
                            QStringList rids = rest.mid(5).split(';');
                            hasSimulcast = rids.contains("l")
                                        && rids.contains("m")
                                        && rids.contains("h");
                        }
                    }
                    if (t == "a=rid:l send") ridL = true;
                    if (t == "a=rid:m send") ridM = true;
                    if (t == "a=rid:h send") ridH = true;
                    if (t.startsWith("a=fmtp:96") && t.contains("rid=l")) fmtpRidL = true;
                    if (t.startsWith("a=fmtp:96") && t.contains("rid=m")) fmtpRidM = true;
                    if (t.startsWith("a=fmtp:96") && t.contains("rid=h")) fmtpRidH = true;
                }
                bool canonical = hasSimulcast && ridL && ridM && ridH;
                bool gstBin = (videoMlines >= 3) && fmtpRidL && fmtpRidM && fmtpRidH;
                peer.simulcastSdpPass = canonical || gstBin;
                peer.log(QString("SIMULCAST SDP: %1 (canonical=%2 gstbin-multi-mline=%3)")
                    .arg(peer.simulcastSdpPass ? "PASS" : "FAIL")
                    .arg(canonical).arg(gstBin));
            }
            peer.signaling->sendOffer(peer.sessionId, sdp, "mcu-test");
        });
        connect(peer.pubPipeline, &PublishPipeline::iceCandidateReady, this,
                [this, &peer](const QString &cand, int mline, const QString &mid) {
            QJsonObject c;
            c["candidate"] = cand;
            c["sdpMLineIndex"] = mline;
            c["sdpMid"] = mid;
            peer.signaling->sendCandidate(peer.sessionId, c, "mcu-test");
        });
        connect(peer.pubPipeline, &PublishPipeline::iceStateChanged, this,
                [this, &peer](const QString &state) {
            peer.log("PublishPipeline ICE: " + state);
            if (state == "connected" || state == "completed") {
                peer.iceConnected = true;
                peer.setPhase(Active);
                checkBothActive();
            } else if (state == "failed") {
                peer.log("ERROR: PublishPipeline ICE failed!");
            }
        });
        connect(peer.pubPipeline, &PublishPipeline::error, this,
                [&peer](const QString &err) {
            peer.log("PublishPipeline error: " + err);
        });

        // #20 BG harness: install a BackgroundEngine into peer A's
        // publisher BEFORE start() so the very first camera frame
        // through onBgSample sees the engine in the right mode. The
        // engine is parented to TestPeer.api (lives the full run); the
        // pipeline only holds a non-owning pointer.
        if (&peer == &m_peerA && (g_bgBlur || g_bgImage || g_bgFallback)) {
            peer.bgEngine = new BackgroundEngine(peer.api);
            if (g_bgBlur) {
                peer.bgEngine->setBlurStrength(10);
                peer.bgEngine->setMode(BackgroundEngine::Mode::Blur);
                peer.log("BG engine attached, Mode=Blur strength=10");
            } else if (g_bgImage) {
                // Use the first bundled background; no disk dependency
                // — qrc resource path is stable across machines.
                peer.bgEngine->setImagePath(":/bg/backgrounds/1_office.jpg");
                peer.bgEngine->setMode(BackgroundEngine::Mode::Image);
                peer.log("BG engine attached, Mode=Image (bundled #1)");
            } else {
                // FALLBACK: engine is installed but Mode=None. We expect
                // onBgSample to take the zero-copy push-through path
                // every frame (no engine round-trip).
                peer.bgEngine->setMode(BackgroundEngine::Mode::None);
                peer.log("BG engine attached, Mode=None (push-through fallback)");
            }
            peer.pubPipeline->setBackgroundEngine(peer.bgEngine);
        }

        bool ok = peer.pubPipeline->start(peer.stunServer, peer.turnServers,
                                          QString(), true /*withVideo*/, 0, false);
        if (!ok) {
            peer.log("ERROR: PublishPipeline failed to start");
            qApp->exit(1);
            return;
        }
        peer.pipelineStarted = true;
        if (g_cameraToggle) {
            // Callee-mid-call-camera-on simulation (#135): leave the
            // publish pipeline in "camera off / dummy → 5 s grace →
            // wire silent" for ~8 s, THEN enable the camera so the
            // SubscribeWebrtcSrc on peer B sees exactly the pattern
            // that produces 30/~10 distinct dup-pad in the field. If
            // the dummy-halt fix in PublishPipeline.cpp is correct,
            // the subsequent steady-state must read distinct ≈ delivered.
            peer.log("PublishPipeline started — camera DEFERRED 8 s "
                     "(mid-call enable scenario, #135)");
            QTimer::singleShot(8000, this, [&peer]() {
                if (!peer.pubPipeline) return;
                peer.log("PublishPipeline: enableCamera now (mid-call)");
                peer.pubPipeline->enableCamera(0, false);
            });
        } else {
            peer.log("PublishPipeline started (synthetic high-motion 720p30)");
            peer.pubPipeline->enableCamera(0, false);
        }

        if (g_simulcastDrop && &peer == &m_peerA) {
            // #132 SIMULCAST_DROP: step the synthetic BWE through three
            // thresholds. The publisher's PublishPipeline::onGccBitrate
            // reads TALQ_TEST_BWE_OVERRIDE_KBPS each tick; setting it
            // here forces applyBweToLayers to close layers in sequence.
            // Verdict is the qInfo "simulcast layer 'X' -> MUTED" lines.
            QTimer::singleShot(8000, this, [&peer]() {
                qputenv("TALQ_TEST_BWE_OVERRIDE_KBPS", "1500");
                peer.log("BWE override -> 1500 kbps (expect h close)");
            });
            QTimer::singleShot(13000, this, [&peer]() {
                qputenv("TALQ_TEST_BWE_OVERRIDE_KBPS", "400");
                peer.log("BWE override -> 400 kbps (expect m close)");
            });
            QTimer::singleShot(18000, this, [&peer]() {
                qputenv("TALQ_TEST_BWE_OVERRIDE_KBPS", "100");
                peer.log("BWE override -> 100 kbps (only l remains)");
            });
        }

        for (auto &[cand, mline, mid] : peer.pendingCandidates)
            peer.pubPipeline->addIceCandidate(cand, mline, mid);
        peer.pendingCandidates.clear();

        auto *busTimer = new QTimer(this);
        connect(busTimer, &QTimer::timeout, this, [&peer]() {
            if (peer.pubPipeline) peer.pubPipeline->pollBus();
        });
        busTimer->start(50);
    }

    // #16 — Peer A's screen publisher. Wires the ScreenSharePipeline to
    // the signaling channel the same way CallManager::startScreenShare
    // does in the real app, but driven directly from the harness.
    void startScreenShareOn(TestPeer &peer)
    {
        if (peer.screenSharePipeline) return;
        peer.log("Starting screen share (synthetic videotestsrc)");
        peer.screenSharePipeline = new ScreenSharePipeline(this);

        connect(peer.screenSharePipeline, &ScreenSharePipeline::localOfferReady,
                this, [this, &peer](const QString &sdp) {
            peer.screenShareSid = QString::number(qHash(sdp)).left(10);
            peer.log("Screen pub offer ready, sid=" + peer.screenShareSid
                     + " (" + QString::number(sdp.length()) + " chars)");
            peer.signaling->sendOffer(peer.signaling->sessionId(), sdp,
                                      peer.screenShareSid, {},
                                      "screen", peer.signaling->sessionId());
        });

        connect(peer.screenSharePipeline, &ScreenSharePipeline::iceCandidateReady,
                this, [this, &peer](const QString &cand, int mline, const QString &mid) {
            QJsonObject c;
            c["candidate"] = cand;
            c["sdpMLineIndex"] = mline;
            c["sdpMid"] = mid;
            peer.signaling->sendCandidate(peer.signaling->sessionId(), c,
                                          peer.screenShareSid, "screen");
        });

        connect(peer.screenSharePipeline, &ScreenSharePipeline::iceGatheringComplete,
                this, [this, &peer]() {
            peer.signaling->sendEndOfCandidates(peer.signaling->sessionId(),
                                                peer.screenShareSid, "screen");
        });

        connect(peer.screenSharePipeline, &ScreenSharePipeline::iceStateChanged,
                this, [this, &peer](const QString &state) {
            peer.log("Screen pub ICE: " + state);
            if ((state == "connected" || state == "completed")
                && !peer.screenSharePubIceConnected) {
                peer.screenSharePubIceConnected = true;
                // Tell HPB to issue a fresh subscriber offer to the remote
                // peer (it then trickles a roomType=screen offer back).
                QJsonObject data;
                data["type"] = QString("sendoffer");
                data["roomType"] = QString("screen");
                peer.signaling->sendMinimalMessage(peer.remoteSessionId, data);
                peer.log("Screen pub connected — sent sendoffer screen to "
                         + peer.remoteSessionId.left(20));
            }
        });

        connect(peer.screenSharePipeline, &ScreenSharePipeline::error,
                this, [&peer](const QString &err) {
            peer.log("Screen pub ERROR: " + err);
        });

        // Bus pump.
        auto *busTimer = new QTimer(this);
        connect(busTimer, &QTimer::timeout, this, [&peer]() {
            if (peer.screenSharePipeline) peer.screenSharePipeline->pollBus();
        });
        busTimer->start(50);

        // No real HMONITOR / HWND — TALQ_SS_TESTSRC bypasses both. Pass
        // monitorIndex=0 just to satisfy the signature.
        if (!peer.screenSharePipeline->start(peer.stunServer, peer.turnServers,
                                             0 /*monitorIndex*/, 0 /*windowHandle*/)) {
            peer.log("ERROR: ScreenSharePipeline failed to start");
        }
    }

    // #16 — Peer B's screen subscriber. Parallel to startSubscribePipeline
    // but on a separate SubscribeWebrtcSrc + sid so it doesn't fight with
    // the camera subscriber that's already running.
    void startScreenSubscribePipeline(TestPeer &peer, const QString &remoteSession,
                                      const QString &sdp)
    {
        if (peer.screenSubscribePipeline) {
            peer.log("Screen subscriber already exists, setting new offer");
            peer.screenSubscribePipeline->setRemoteOffer(sdp);
            return;
        }

        peer.screenSubscribePipeline = new SubscribeWebrtcSrc(remoteSession, this);

        connect(peer.screenSubscribePipeline, &SubscribeWebrtcSrc::localAnswerReady,
                this, [this, &peer, remoteSession](const QString &answer) {
            peer.log("Screen sub answer -> " + remoteSession.left(12)
                     + " sid=" + peer.screenSubscriberSid);
            peer.signaling->sendAnswer(remoteSession, answer,
                                       peer.screenSubscriberSid, {}, "screen");
        });

        connect(peer.screenSubscribePipeline, &SubscribeWebrtcSrc::iceCandidateReady,
                this, [this, &peer, remoteSession](const QString &cand, int mline,
                                                   const QString &mid) {
            QJsonObject c;
            c["candidate"] = cand;
            c["sdpMLineIndex"] = mline;
            c["sdpMid"] = mid;
            peer.signaling->sendCandidate(remoteSession, c,
                                          peer.screenSubscriberSid, "screen");
        });

        connect(peer.screenSubscribePipeline, &SubscribeWebrtcSrc::iceStateChanged,
                this, [&peer](const QString &state) {
            peer.log("Screen sub ICE: " + state);
        });

        connect(peer.screenSubscribePipeline, &SubscribeWebrtcSrc::error,
                this, [&peer](const QString &err) {
            peer.log("Screen sub error: " + err);
        });

        if (!peer.screenSubscribePipeline->start(peer.stunServer, peer.turnServers)) {
            peer.log("ERROR: screen SubscribeWebrtcSrc failed to start");
            return;
        }
        peer.log("Screen SubscribeWebrtcSrc started");

        peer.screenSubscribePipeline->setRemoteOffer(sdp);

        for (auto &[cand, mline, mid] : peer.screenSubPendingCandidates)
            peer.screenSubscribePipeline->addIceCandidate(cand, mline, mid);
        peer.screenSubPendingCandidates.clear();
    }

    void startSubscribePipeline(TestPeer &peer, const QString &remoteSession, const QString &sdp)
    {
        if (peer.subscribePipeline) {
            peer.log("Already have subscribe pipeline, setting new offer");
            peer.subscribePipeline->setRemoteOffer(sdp);
            return;
        }

        peer.subscribePipeline = new SubscribeWebrtcSrc(remoteSession, this);

        connect(peer.subscribePipeline, &SubscribeWebrtcSrc::localAnswerReady, this,
                [this, &peer, remoteSession](const QString &answer) {
            peer.log("Subscriber answer -> " + remoteSession.left(12)
                     + " sid=" + peer.subscriberSid);
            peer.signaling->sendAnswer(remoteSession, answer, peer.subscriberSid);
        });

        connect(peer.subscribePipeline, &SubscribeWebrtcSrc::iceCandidateReady, this,
                [this, &peer, remoteSession](const QString &cand, int mline, const QString &mid) {
            QJsonObject c;
            c["candidate"] = cand;
            c["sdpMLineIndex"] = mline;
            c["sdpMid"] = mid;
            peer.signaling->sendCandidate(remoteSession, c, peer.subscriberSid);
        });

        connect(peer.subscribePipeline, &SubscribeWebrtcSrc::iceStateChanged, this,
                [this, &peer](const QString &state) {
            peer.log("Subscriber ICE: " + state);
            // #132 debug: once the subscription is live, optionally request a
            // specific simulcast substream from Janus (selectStream → HPB →
            // videoroom configure). The per-second RX video log prints the
            // decoded WxH, so we can SEE whether Janus actually switches the
            // forwarded layer (e.g. 320x180 → 1280x720) or ignores us.
            if ((state == "connected" || state == "completed")
                && qEnvironmentVariableIsSet("TALQ_TEST_SELECT_SUBSTREAM")
                && !peer.substreamRequested) {
                peer.substreamRequested = true;
                const int ss = qEnvironmentVariable("TALQ_TEST_SELECT_SUBSTREAM").toInt();
                // small delay so the default (substream 0) stream is flowing
                // first — the log then shows the before→after resolution.
                QTimer::singleShot(4000, this, [this, &peer, ss]() {
                    if (!peer.subscribePipeline) return;
                    peer.log(QString("selectStream → substream %1 (sid=%2)")
                             .arg(ss).arg(peer.subscriberSid.left(10)));
                    peer.signaling->sendSelectStream(peer.remoteSessionId,
                                                     peer.subscriberSid, ss, 2);
                });
            }
        });

        connect(peer.subscribePipeline, &SubscribeWebrtcSrc::error, this,
                [&peer](const QString &err) {
            peer.log("Subscriber error: " + err);
        });

        bool ok = peer.subscribePipeline->start(peer.stunServer, peer.turnServers);
        if (!ok) {
            peer.log("ERROR: SubscribePipeline failed to start");
            return;
        }
        peer.log("SubscribePipeline started");

        // Bus pump for subscriber
        auto *busTimer = new QTimer(this);
        connect(busTimer, &QTimer::timeout, this, [&peer]() {
            // SubscribePipeline doesn't have pollBus — GStreamer runs its own context
        });

        peer.subscribePipeline->setRemoteOffer(sdp);

        // Flush any subscriber candidates that arrived before the pipeline
        // existed (SubscribePipeline::addIceCandidate self-queues until its
        // remote description is set, so ordering here is safe).
        for (auto &[cand, mline, mid] : peer.subPendingCandidates)
            peer.subscribePipeline->addIceCandidate(cand, mline, mid);
        peer.subPendingCandidates.clear();
    }

    void checkBothActive()
    {
        if (m_peerA.iceConnected && m_peerB.iceConnected && !m_videoTestStarted) {
            qDebug() << "\n===== BOTH PEERS PUBLISHING (audio+video) TO MCU =====";
            m_videoTestStarted = true;
            // Both publishers are up with video. Request each peer's
            // subscriber stream EXACTLY ONCE (real-app CallManager
            // behaviour) after a short settle, then wait for frames.
            QTimer::singleShot(1500, this, [this]() {
                requestSubscribersOnce();
                QTimer::singleShot(3000, this, &CallTest::startFrameWait);
            });
        }
    }

    // Request each peer's view of the OTHER peer's stream, exactly once
    // per peer — the same single-shot subscribe the real app does in
    // CallManager::requestPeerStream. No re-requesting, no reneg: that
    // churn is what spawned orphan webrtcbin/DTLS transports.
    void requestSubscribersOnce()
    {
        auto req = [this](TestPeer &p) {
            if (p.subscriberRequested || p.remoteSessionId.isEmpty()) return;
            p.subscriberRequested = true;
            p.log("requestOffer (once) for remote " + p.remoteSessionId.left(20));
            p.signaling->requestOffer(p.remoteSessionId, "video");
        };
        req(m_peerA);
        req(m_peerB);
    }

    // Validate an SDP offer for video and return true if valid
    bool validateVideoSdp(TestPeer &peer, const QString &sdp)
    {
        bool hasVideoLine = false;
        bool hasBundleOnly = false;
        bool hasSendDirection = false;
        bool hasH264Codec = false;
        bool hasPort0 = false;
        const auto lines = sdp.split('\n');
        bool inVideoSection = false;
        for (const auto &line : lines) {
            QString trimmed = line.trimmed();
            if (trimmed.startsWith("m=")) {
                inVideoSection = trimmed.startsWith("m=video");
                if (inVideoSection) {
                    hasVideoLine = true;
                    hasPort0 = trimmed.startsWith("m=video 0 ") || trimmed == "m=video 0";
                    peer.log("SDP video m-line: " + trimmed);
                }
            }
            if (inVideoSection) {
                if (trimmed == "a=bundle-only") hasBundleOnly = true;
                if (trimmed == "a=sendonly" || trimmed == "a=sendrecv") hasSendDirection = true;
                if (trimmed.contains("H264")) hasH264Codec = true;
            }
        }

        peer.log(QString("SDP analysis: hasVideo=%1 port0=%2 bundleOnly=%3 sendDir=%4 H264=%5")
                 .arg(hasVideoLine).arg(hasPort0).arg(hasBundleOnly).arg(hasSendDirection).arg(hasH264Codec));

        if (!hasVideoLine) {
            peer.log("BUG: Renegotiation SDP has NO m=video line!");
            return false;
        } else if (hasPort0 && !hasBundleOnly && !hasSendDirection) {
            peer.log("BUG: m=video 0 without bundle-only -- video rejected!");
            return false;
        } else if (hasVideoLine && hasSendDirection && hasH264Codec) {
            if (hasPort0 && hasBundleOnly)
                peer.log("OK: m=video 0 with a=bundle-only -- valid BUNDLE offer");
            else if (!hasPort0)
                peer.log("OK: m=video with active port");
            peer.log("SUCCESS: Renegotiation SDP has valid video section");
            return true;
        }
        peer.log("UNCERTAIN: Video line present but missing direction or codec");
        return false;
    }

    // Install a video-validating offer handler on a peer
    void installVideoOfferHandler(TestPeer &peer)
    {
        disconnect(peer.pipeline, &PeerPipeline::localOfferReady, nullptr, nullptr);
        connect(peer.pipeline, &PeerPipeline::localOfferReady, this,
                [this, &peer](const QString &sdp) {
            peer.log("Video renegotiation offer ready (" + QString::number(sdp.length()) + " chars)");
            peer.videoRenegSdpValid = validateVideoSdp(peer, sdp);
            // Send offer to MCU so the full flow completes
            peer.signaling->sendOffer(peer.sessionId, sdp, "mcu-test");
        });
    }

    // forceReconnect a peer: stop pipeline, restart with video, re-request subscriber
    void forceReconnectWithVideo(TestPeer &peer)
    {
        peer.log("forceReconnect: stopping pipeline...");
        if (peer.pipeline) {
            peer.pipeline->stop();
            peer.pipeline->deleteLater();
            peer.pipeline = nullptr;
        }
        // Stop subscriber too — MCU will re-offer after new publisher
        if (peer.subscribePipeline) {
            peer.subscribePipeline->stop();
            peer.subscribePipeline->deleteLater();
            peer.subscribePipeline = nullptr;
        }

        // Update call flags to include VIDEO
        QJsonObject flagsBody;
        flagsBody["flags"] = 7;  // IN_CALL + AUDIO + VIDEO
        peer.api->put("apps/spreed/api/v4/call/" + m_token, flagsBody,
            [&peer](bool ok, const QJsonObject &, int) {
            peer.log(ok ? "Call flags updated to 7 (WITH_VIDEO)" : "WARNING: flags update failed");
        });

        // Create new pipeline with video from the start
        peer.pipeline = new PeerPipeline(this);

        connect(peer.pipeline, &PeerPipeline::localOfferReady, this,
                [this, &peer](const QString &sdp) {
            peer.log("forceReconnect offer ready (" + QString::number(sdp.length()) + " chars)");
            peer.videoRenegSdpValid = validateVideoSdp(peer, sdp);
            peer.signaling->sendOffer(peer.sessionId, sdp, "mcu-test");
        });

        connect(peer.pipeline, &PeerPipeline::localAnswerReady, this,
                [this, &peer](const QString &sdp) {
            peer.signaling->sendAnswer(peer.sessionId, sdp, "mcu-test");
        });

        connect(peer.pipeline, &PeerPipeline::iceCandidateReady, this,
                [this, &peer](const QString &cand, int mline, const QString &mid) {
            QJsonObject c;
            c["candidate"] = cand;
            c["sdpMLineIndex"] = mline;
            c["sdpMid"] = mid;
            peer.signaling->sendCandidate(peer.sessionId, c, "mcu-test");
        });

        connect(peer.pipeline, &PeerPipeline::iceStateChanged, this,
                [this, &peer](const QString &state) {
            peer.log("forceReconnect ICE: " + state);
            if (state == "connected" || state == "completed") {
                peer.iceConnected = true;
                peer.videoAnswerReceived = true;  // MCU answered our offer
                // Do NOT request the subscriber here. Requesting on our own
                // publisher ICE-connect races the OTHER peer's video publish
                // — the MCU then offers an audio-only feed (the 630-char
                // offer → no video pad ever). The single requestOffer is
                // issued by checkBothVideoRenegotiated(), which fires only
                // AFTER both peers have published video.
                checkBothVideoRenegotiated();
            }
        });

        connect(peer.pipeline, &PeerPipeline::error, this, [&peer](const QString &err) {
            peer.log("forceReconnect error: " + err);
        });

        // Start with video enabled (withVideo=true via TALQ_TEST_AUDIO using videotestsrc)
        // PeerPipeline doesn't have a withVideo param in start(), so we start then enableCamera
        bool ok = peer.pipeline->start(peer.stunServer, peer.turnServers);
        if (!ok) {
            peer.log("ERROR: forceReconnect pipeline failed to start");
            return;
        }

        // Bus pump
        auto *busTimer = new QTimer(this);
        connect(busTimer, &QTimer::timeout, this, [&peer]() {
            if (peer.pipeline) peer.pipeline->pollBus();
        });
        busTimer->start(50);

        // Enable camera immediately (before createOffer, so video is in initial SDP)
        peer.log("Enabling camera before offer...");
        peer.pipeline->enableCamera(0, false);

        // Create offer after a brief delay for pipeline to settle
        QTimer::singleShot(500, this, [this, &peer]() {
            if (!peer.pipeline) return;
            peer.log("Creating forceReconnect offer with video...");
            peer.pipeline->createOffer();
        });
    }

    void startVideoRenegotiation()
    {
        qDebug() << "\n===== BIDIRECTIONAL VIDEO TEST (forceReconnect) =====";

        // Step 1: User A does forceReconnect with video
        m_peerA.setPhase(VideoRenegotiatingA);
        forceReconnectWithVideo(m_peerA);

        // Step 2: After 1.5s, User B does forceReconnect with video
        QTimer::singleShot(1500, this, [this]() {
            if (m_peerB.phase == TearingDown) return;
            qDebug() << "\n--- User B forceReconnect with video ---";
            m_peerB.setPhase(VideoRenegotiatingB);
            forceReconnectWithVideo(m_peerB);
        });

        // Safety timeout
        QTimer::singleShot(18000, this, [this]() {
            if (!m_frameWaitStarted) {
                qWarning() << "Video forceReconnect timed out, checking frames anyway...";
                startFrameWait();
            }
        });
    }

    void checkBothVideoRenegotiated()
    {
        // Run EXACTLY ONCE. This is invoked from both peers' ICE-state
        // handlers, which fire repeatedly (connected, then completed,
        // then on every check). Without this guard it re-issued
        // requestOffer ~7×, so Janus kept replacing the subscriber
        // session with fresh ICE creds and DTLS never converged on a
        // stable transport (no srtp key → no media).
        if (m_videoRenegHandled) return;
        if (m_peerA.videoAnswerReceived && m_peerB.videoAnswerReceived) {
            m_videoRenegHandled = true;
            qDebug() << "\n===== BOTH PEERS VIDEO RENEGOTIATION COMPLETE =====";

            // Update call flags to include VIDEO (7 = IN_CALL + AUDIO + VIDEO)
            // The MCU uses these flags to decide what media to forward
            qDebug() << "Updating call flags to include VIDEO (flags=7)...";
            QJsonObject flagsBody;
            flagsBody["flags"] = 7;
            m_peerA.api->put("apps/spreed/api/v4/call/" + m_token, flagsBody,
                [this](bool ok, const QJsonObject &, int) {
                m_peerA.log(ok ? "Call flags updated to 7" : "WARNING: flags update failed");
            });
            m_peerB.api->put("apps/spreed/api/v4/call/" + m_token, flagsBody,
                [this](bool ok, const QJsonObject &, int) {
                m_peerB.log(ok ? "Call flags updated to 7" : "WARNING: flags update failed");
            });

            // Wait for flags to propagate, then request subscriber streams
            QTimer::singleShot(1500, this, [this]() {
                qDebug() << "Requesting subscriber streams from MCU...";
                if (!m_peerA.remoteSessionId.isEmpty() && !m_peerA.subscriberRequested) {
                    m_peerA.log("requestOffer for remote: " + m_peerA.remoteSessionId.left(20));
                    m_peerA.signaling->requestOffer(m_peerA.remoteSessionId, "video");
                    m_peerA.subscriberRequested = true;
                }
                if (!m_peerB.remoteSessionId.isEmpty() && !m_peerB.subscriberRequested) {
                    m_peerB.log("requestOffer for remote: " + m_peerB.remoteSessionId.left(20));
                    m_peerB.signaling->requestOffer(m_peerB.remoteSessionId, "video");
                    m_peerB.subscriberRequested = true;
                }

                // Wait for MCU to send subscriber offers with video
                QTimer::singleShot(2000, this, &CallTest::startFrameWait);
            });
        }
    }

    void startFrameWait()
    {
        if (m_frameWaitStarted) return;
        m_frameWaitStarted = true;

        m_peerA.setPhase(VideoWaitingFrames);
        m_peerB.setPhase(VideoWaitingFrames);

        // Record initial frame counts from SubscribePipeline (where MCU delivers remote video)
        auto getRemoteFrames = [](TestPeer &p) -> int {
            if (p.subscribePipeline && p.subscribePipeline->videoProvider())
                return p.subscribePipeline->videoProvider()->frameCount();
            if (p.pipeline && p.pipeline->remoteVideoProvider())
                return p.pipeline->remoteVideoProvider()->frameCount();
            return 0;
        };
        m_peerA.remoteVideoFramesBefore = getRemoteFrames(m_peerA);
        m_peerB.remoteVideoFramesBefore = getRemoteFrames(m_peerB);

        qDebug() << "Waiting 6 seconds for video frames to flow through MCU...";
        qDebug() << "  User A remote frames so far:" << m_peerA.remoteVideoFramesBefore;
        qDebug() << "  User B remote frames so far:" << m_peerB.remoteVideoFramesBefore;

        // Wait for steady-state RX. In camera-toggle mode the publisher
        // defers enableCamera by 8 s, so push the measurement window out
        // to 14 s (8 s defer + ~6 s for the post-enable encoder to settle
        // and the receiver's distinct-fps probe to read a stable 1-s
        // window).
        const int measureWaitMs = g_simulcastDrop ? 25000
                              : g_cameraToggle  ? 14000 : 6000;
        QTimer::singleShot(measureWaitMs, this, [this, getRemoteFrames]() {
            m_peerA.remoteVideoFramesAfter = getRemoteFrames(m_peerA);
            m_peerB.remoteVideoFramesAfter = getRemoteFrames(m_peerB);

            int aNewFrames = m_peerA.remoteVideoFramesAfter - m_peerA.remoteVideoFramesBefore;
            int bNewFrames = m_peerB.remoteVideoFramesAfter - m_peerB.remoteVideoFramesBefore;

            qDebug() << "\n===== VIDEO FRAME RESULTS =====";
            qDebug() << "  User A received" << aNewFrames << "remote video frames (total:" << m_peerA.remoteVideoFramesAfter << ")";
            qDebug() << "  User B received" << bNewFrames << "remote video frames (total:" << m_peerB.remoteVideoFramesAfter << ")";

            // Also check local video provider (preview) to confirm camera is producing frames
            auto localFrames = [](TestPeer &p) -> int {
                if (p.pubPipeline && p.pubPipeline->localVideoProvider())
                    return p.pubPipeline->localVideoProvider()->frameCount();
                if (p.pipeline && p.pipeline->localVideoProvider())
                    return p.pipeline->localVideoProvider()->frameCount();
                return -1;
            };
            qDebug() << "  User A local preview frames:" << localFrames(m_peerA);
            qDebug() << "  User B local preview frames:" << localFrames(m_peerB);

            if (g_pubPipe && m_peerB.subscribePipeline) {
                // Average over 5 consecutive 1-s samples instead of a
                // single snapshot — the per-second metric is intrinsically
                // noisy on synthetic snow (run-to-run variance ±10 pp).
                // A point-sample can land low even when the windowed
                // ratio is comfortably above 75%.
                int dlvSum = 0, dstSum = 0, samples = 0;
                for (int i = 0; i < 5; ++i) {
                    QElapsedTimer t; t.start();
                    while (t.elapsed() < 1000)
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                    dlvSum += m_peerB.subscribePipeline->rxVideoFps();
                    dstSum += m_peerB.subscribePipeline->rxDistinctVideoFps();
                    ++samples;
                }
                int dlv  = samples ? dlvSum / samples : 0;
                int dist = samples ? dstSum / samples : 0;
                qDebug() << "\n===== #111 PROXY (B receives A's PublishPipeline) =====";
                qDebug().nospace() << "  RX delivered(avg/" << samples
                                   << ")=" << dlv << " fps  distinct(avg)=" << dist << " fps";
                bool ok = dlv >= 20 && dist * 100 >= dlv * 75;
                qDebug() << (ok
                    ? "  #111 PROXY: PASS (distinct >=75% of delivered — no dup-pad)"
                    : "  #111 PROXY: FAIL (distinct << delivered — still padding)");
            }

            if (aNewFrames == 0 && bNewFrames == 0) {
                qDebug() << "  NOTE: MCU did not route video between peers.";
                qDebug() << "  This may be expected for some MCU configurations (e.g., Janus in 1:1 mode).";
                qDebug() << "  The MCU may only forward video when 3+ participants are present,";
                qDebug() << "  or it may require subscriber-side negotiation not yet implemented.";
            }

            m_peerA.setPhase(VideoActive);
            m_peerB.setPhase(VideoActive);

            // #17: mute toggle sequence over signaling. After the call is
            // up, peer A broadcasts mute audio → mute video → unmute audio
            // → unmute video at 600 ms intervals. Peer B's signaling layer
            // should fire remoteMuteChanged for each transition. Pass when
            // we observe ALL four expected events on peer B.
            if (g_muteToggle && !m_peerA.muteToggleKicked) {
                m_peerA.muteToggleKicked = true;
                auto sendMute = [this](const QString &media, bool muted) {
                    QJsonObject payload;
                    payload["name"] = media;
                    m_peerA.signaling->sendSessionMessage(
                        m_peerA.remoteSessionId,
                        muted ? "mute" : "unmute",
                        payload, QString());
                    m_peerA.log(QString("broadcast %1 %2 to remote")
                                .arg(muted ? "mute" : "unmute", media));
                };
                QTimer::singleShot( 200, this, [sendMute]{ sendMute("audio", true);  });
                QTimer::singleShot( 800, this, [sendMute]{ sendMute("video", true);  });
                QTimer::singleShot(1500, this, [sendMute]{ sendMute("audio", false); });
                QTimer::singleShot(2200, this, [sendMute]{ sendMute("video", false); });
                QTimer::singleShot(3500, this, [this]() {
                    qDebug() << "\n===== #17 MUTE TOGGLE =====";
                    qDebug() << "  peer B observed audio muted   x"
                             << m_peerB.remoteAudioMutedEvents;
                    qDebug() << "  peer B observed audio unmuted x"
                             << m_peerB.remoteAudioUnmutedEvents;
                    qDebug() << "  peer B observed video muted   x"
                             << m_peerB.remoteVideoMutedEvents;
                    qDebug() << "  peer B observed video unmuted x"
                             << m_peerB.remoteVideoUnmutedEvents;
                    const bool ok = m_peerB.remoteAudioMutedEvents   >= 1
                                 && m_peerB.remoteAudioUnmutedEvents >= 1
                                 && m_peerB.remoteVideoMutedEvents   >= 1
                                 && m_peerB.remoteVideoUnmutedEvents >= 1;
                    qDebug() << (ok
                        ? "  #17 MUTE TOGGLE: PASS (all 4 transitions propagated)"
                        : "  #17 MUTE TOGGLE: FAIL (missing transitions)");
                    qDebug() << "\nMute-toggle scenario complete. Tearing down...";
                    teardown();
                });
                return;
            }

            // #16: now that the call is up, exercise the screen-share path
            // before teardown. Peer A publishes a synthetic screen capture;
            // peer B's screen subscriber should receive frames.
            if (g_screenShare && !m_peerA.screenShareKicked) {
                m_peerA.screenShareKicked = true;
                startScreenShareOn(m_peerA);
                // 12 s window: ScreenSharePipeline has a 10 s ICE +
                // 6 s capture-frame watchdog, plus we want ~3 s of frames
                // after first frame to be confident the stream is steady.
                m_peerB.screenFramesBefore = 0;
                QTimer::singleShot(12000, this, [this]() {
                    int sf = 0;
                    if (m_peerB.screenSubscribePipeline
                        && m_peerB.screenSubscribePipeline->videoProvider())
                        sf = m_peerB.screenSubscribePipeline->videoProvider()->frameCount();
                    m_peerB.screenFramesAfter = sf;
                    int delta = sf - m_peerB.screenFramesBefore;
                    qDebug() << "\n===== #16 SCREEN SHARE =====";
                    qDebug() << "  User B received" << delta
                             << "screen frames (total:" << sf << ")";
                    const bool ok = delta >= 30;   // ~3 s @ 10 fps minimum
                    qDebug() << (ok
                        ? "  #16 SCREEN SHARE: PASS (frames flowing through MCU)"
                        : "  #16 SCREEN SHARE: FAIL (no screen frames)");
                    qDebug() << "\nScreen-share scenario complete. Tearing down...";
                    teardown();
                });
                return;
            }

            qDebug() << "\nBidirectional video test complete. Tearing down...";
            teardown();
        });
    }

    void onActiveTimerDone()
    {
        // This timer is no longer used (replaced by video renegotiation flow)
        // Keep for safety in case checkBothActive isn't reached
        qDebug() << "Active period complete. Tearing down...";
        teardown();
    }

    void teardown()
    {
        m_peerA.setPhase(TearingDown);
        m_peerB.setPhase(TearingDown);

        // Stop pipelines
        if (m_peerA.subscribePipeline) m_peerA.subscribePipeline->stop();
        if (m_peerB.subscribePipeline) m_peerB.subscribePipeline->stop();
        if (m_peerA.pipeline) m_peerA.pipeline->stop();
        if (m_peerB.pipeline) m_peerB.pipeline->stop();

        // Leave call via API
        int *pending = new int(2);
        auto checkDone = [this, pending]() {
            if (--(*pending) <= 0) {
                delete pending;
                // Leave room
                m_peerA.api->del("apps/spreed/api/v4/room/" + m_token + "/participants/active",
                    [](bool, const QJsonObject &, int) {});
                m_peerB.api->del("apps/spreed/api/v4/room/" + m_token + "/participants/active",
                    [](bool, const QJsonObject &, int) {});
                m_peerA.signaling->stop();
                m_peerB.signaling->stop();

                QTimer::singleShot(1000, this, [this]() {
                    bool icePassed = m_peerA.iceConnected && m_peerB.iceConnected;
                    bool videoSdpOk = m_peerA.videoRenegSdpValid && m_peerB.videoRenegSdpValid;
                    int aFrames = m_peerA.remoteVideoFramesAfter - m_peerA.remoteVideoFramesBefore;
                    int bFrames = m_peerB.remoteVideoFramesAfter - m_peerB.remoteVideoFramesBefore;
                    // The whole point of webrtcsrc was to fix "no remote
                    // video", so decoded frames on BOTH subscribers is the
                    // hard pass bar — not informational anymore.
                    bool framesOk = aFrames > 0 && bFrames > 0;
                    bool pass = icePassed && videoSdpOk && framesOk;
                    if (g_pubPipe) {
                        int dlv  = m_peerB.subscribePipeline
                                     ? m_peerB.subscribePipeline->rxVideoFps() : 0;
                        int dist = m_peerB.subscribePipeline
                                     ? m_peerB.subscribePipeline->rxDistinctVideoFps() : 0;
                        // #111 bar: B must receive A's real-PublishPipeline
                        // stream with distinct ≈ delivered (no CFR dup-pad).
                        bool proxyOk = bFrames > 0 && dlv >= 20
                                       && dist * 100 >= dlv * 75;
                        pass = icePassed && m_peerA.videoRenegSdpValid && proxyOk;
                        qDebug().nospace() << "#111 verdict: delivered=" << dlv
                            << " distinct=" << dist << " -> "
                            << (proxyOk ? "PASS" : "FAIL");
                    }
                    if (g_simulcast) {
                        // #132 verdict: simulcast SDP block must be present
                        // in peer A's publisher offer (only peer A runs
                        // PublishPipeline in PUBPIPE mode).
                        bool simPass = m_peerA.simulcastSdpPass;
                        pass = pass && simPass;
                        qDebug().nospace() << "SIMULCAST verdict: SDP_pass="
                            << simPass << " -> " << (simPass ? "PASS" : "FAIL");
                    }
                    if (g_simulcastDrop) {
                        // Visual-only verdict: grep run output for
                        // "simulcast layer '<rid>' -> MUTED" qInfo lines
                        // from PublishPipeline::setLayerActive. The
                        // synthetic-BWE steps fire at +8s/+13s/+18s; the
                        // 25-s measure window has cleared by teardown.
                        qDebug() << "SIMULCAST_DROP verdict: visual; grep "
                                    "run output for \"simulcast layer '<rid>' -> MUTED\"";
                    }
                    if (g_bgBlur || g_bgImage || g_bgFallback) {
                        // #20 BG verdict. Engine round-trip is expensive
                        // (BlockingQueuedConnection + GL compose), so we
                        // don't bar on processed-frame count == delivered.
                        // The bar is: the right counter advanced AT ALL
                        // and the other one stayed quiet, AND B saw real
                        // decoded frames (proving the bridge preserved
                        // PTS so the encoder could keep encoding).
                        const quint64 fp = m_peerA.pubPipeline
                            ? m_peerA.pubPipeline->bgBridgeFramesProcessed() : 0;
                        const quint64 pt = m_peerA.pubPipeline
                            ? m_peerA.pubPipeline->bgBridgeFramesPassThrough() : 0;
                        const bool wantOn = g_bgBlur || g_bgImage;
                        const bool counterPass = wantOn
                            ? (fp > 0 && pt == 0)
                            : (pt > 0 && fp == 0);
                        // B receiving frames is the structural check —
                        // proves the bridge didn't black-frame the call.
                        const bool deliveryPass = bFrames > 0;
                        const bool bgPass = counterPass && deliveryPass;
                        pass = pass && bgPass;
                        qDebug().nospace()
                            << "BG verdict: mode="
                            << (g_bgBlur ? "Blur" : g_bgImage ? "Image" : "None")
                            << " processed=" << fp
                            << " passthrough=" << pt
                            << " bFrames=" << bFrames
                            << " -> " << (bgPass ? "PASS" : "FAIL");
                    }
                    printSummary(icePassed);
                    qApp->exit(pass ? 0 : 1);
                });
            }
        };

        m_peerA.api->del("apps/spreed/api/v4/call/" + m_token, [checkDone](bool, const QJsonObject &, int) {
            checkDone();
        });
        m_peerB.api->del("apps/spreed/api/v4/call/" + m_token, [checkDone](bool, const QJsonObject &, int) {
            checkDone();
        });
    }

    void printSummary(bool passed)
    {
        bool videoSdpA = m_peerA.videoRenegSdpValid;
        bool videoSdpB = m_peerB.videoRenegSdpValid;
        int aRemoteFrames = m_peerA.remoteVideoFramesAfter - m_peerA.remoteVideoFramesBefore;
        int bRemoteFrames = m_peerB.remoteVideoFramesAfter - m_peerB.remoteVideoFramesBefore;
        bool aReceiving = aRemoteFrames > 0;
        bool bReceiving = bRemoteFrames > 0;
        bool bidir = aReceiving && bReceiving;
        bool fullPass = passed && videoSdpA && videoSdpB;

        qDebug() << "\n==========================================";
        if (fullPass && bidir)
            qDebug() << "   CALL TEST: PASSED (bidirectional video flowing)";
        else if (fullPass)
            qDebug() << "   CALL TEST: PASSED (video SDP valid, but MCU not routing frames)";
        else
            qDebug() << "   CALL TEST: FAILED";
        qDebug() << "==========================================";
        qDebug().noquote() << QString("  User A: %1 -> %2")
                    .arg(USER_A, -10).arg(phaseStr(m_peerA.phase));
        qDebug().noquote() << QString("  User B: %1 -> %2")
                    .arg(USER_B, -10).arg(phaseStr(m_peerB.phase));
        qDebug().noquote() << QString("  ICE A: %1  ICE B: %2")
                    .arg(m_peerA.iceConnected ? "OK" : "NO")
                    .arg(m_peerB.iceConnected ? "OK" : "NO");
        qDebug().noquote() << QString("  Video SDP A: %1")
                    .arg(videoSdpA ? "VALID (active m=video)" : "INVALID");
        qDebug().noquote() << QString("  Video SDP B: %1")
                    .arg(videoSdpB ? "VALID (active m=video)" : "INVALID");
        qDebug().noquote() << QString("  Video A->MCU->B: %1 (%2 frames)")
                    .arg(bReceiving ? "FLOWING" : "NO FRAMES").arg(bRemoteFrames);
        qDebug().noquote() << QString("  Video B->MCU->A: %1 (%2 frames)")
                    .arg(aReceiving ? "FLOWING" : "NO FRAMES").arg(aRemoteFrames);
        if (!bidir && fullPass) {
            qDebug() << "  [INFO] MCU did not route video. This is normal for some";
            qDebug() << "  MCU configs (Janus 1:1 mode, missing subscriber flow, etc.)";
        }
        qDebug() << "==========================================";
    }

    QString m_token;
    TestPeer m_peerA;
    TestPeer m_peerB;
    QTimer m_timeout;
    QTimer m_activeTimer;
    bool m_videoTestStarted = false;
    bool m_videoRenegHandled = false;   // checkBothVideoRenegotiated once-guard
    bool m_frameWaitStarted = false;
};

int main(int argc, char *argv[])
{
    qputenv("TALQ_TEST_AUDIO", "1");

    // #111 proxy: peer A publishes via the real PublishPipeline. Imply
    // the synthetic source so one env var turns the whole mode on.
    if (qEnvironmentVariableIsSet("TALQ_TEST_CAMERA_TOGGLE")) {
        // #135: implies PUBPIPE + synthetic, plus the deferred-camera
        // mid-call enable scenario that exercises the exact field bug.
        g_pubPipe = true;
        g_cameraToggle = true;
        qputenv("TALQ_PUB_TESTSRC", "1");
    }
    if (qEnvironmentVariableIsSet("TALQ_TEST_SIMULCAST")) {
        // #132: implies PUBPIPE + synthetic. Harness asserts the offer SDP
        // carries the a=simulcast block + three a=rid lines.
        g_pubPipe = true;
        g_simulcast = true;
        qputenv("TALQ_PUB_TESTSRC", "1");
    }
    if (qEnvironmentVariableIsSet("TALQ_TEST_SIMULCAST_DROP")) {
        // #132: implies SIMULCAST + PUBPIPE. Drives synthetic BWE steps via
        // PublishPipeline's TALQ_TEST_BWE_OVERRIDE_KBPS env override.
        g_pubPipe = true;
        g_simulcast = true;
        g_simulcastDrop = true;
        qputenv("TALQ_PUB_TESTSRC", "1");
    }
    if (qEnvironmentVariableIsSet("TALQ_TEST_MUTE_TOGGLE")) {
        // #17 — bare PUBPIPE call, then sequence of mute/unmute broadcasts.
        g_pubPipe = true;
        g_muteToggle = true;
        qputenv("TALQ_PUB_TESTSRC", "1");
    }
    if (qEnvironmentVariableIsSet("TALQ_TEST_SCREENSHARE")) {
        // #16: implies PUBPIPE + synthetic camera (so the call gets to
        // VideoActive before the screen share kicks in) + synthetic
        // capture for ScreenSharePipeline. Peer A publishes the screen,
        // peer B counts frames via screenSubscribePipeline.
        g_pubPipe = true;
        g_screenShare = true;
        qputenv("TALQ_PUB_TESTSRC", "1");
        qputenv("TALQ_SS_TESTSRC", "1");
    }
    if (qEnvironmentVariableIsSet("TALQ_TEST_PUBPIPE")) {
        g_pubPipe = true;
        qputenv("TALQ_PUB_TESTSRC", "1");
    }
    // #20 BG-engine scenarios. All three imply PUBPIPE + synthetic source.
    // BLUR/IMAGE run the GL compose path; FALLBACK exercises the zero-cost
    // Off-mode push-through. Verdict is bridge counters + B-frame count.
    if (qEnvironmentVariableIsSet("TALQ_TEST_BG_BLUR")) {
        g_pubPipe = true;
        g_bgBlur  = true;
        qputenv("TALQ_PUB_TESTSRC", "1");
    }
    if (qEnvironmentVariableIsSet("TALQ_TEST_BG_IMAGE")) {
        g_pubPipe = true;
        g_bgImage = true;
        qputenv("TALQ_PUB_TESTSRC", "1");
    }
    if (qEnvironmentVariableIsSet("TALQ_TEST_BG_FALLBACK")) {
        g_pubPipe    = true;
        g_bgFallback = true;
        qputenv("TALQ_PUB_TESTSRC", "1");
    }

    QCoreApplication app(argc, argv);

    // Resolve identities from env (bot defaults; never a human account).
    auto envOr = [](const char *k, const QString &def) {
        QString v = qEnvironmentVariable(k);
        return v.isEmpty() ? def : v;
    };
    USER_A = envOr("TALQ_TEST_USERA", "test-talq2");
    USER_B = envOr("TALQ_TEST_USERB", "test-talq");
    PASS_A = qEnvironmentVariable("TALQ_TEST_USERA_PASS");
    PASS_B = qEnvironmentVariable("TALQ_TEST_USERB_PASS");

    QCommandLineParser parser;
    parser.addOption({"token", "Conversation token", "TOKEN",
                      envOr("TALQ_TEST_TOKEN", DEFAULT_TOKEN)});
    parser.addOption({"timeout", "Test timeout in seconds", "SECS", "60"});
    parser.addHelpOption();
    parser.process(app);

    QString token = parser.value("token");
    int timeout = parser.value("timeout").toInt();
    // Kalin: a broken call drops by ~40s, so never wait past ~60s.
    if (timeout <= 0 || timeout > 60) timeout = 60;

    // Hard watchdog on a dedicated thread: a blocking teardown (e.g.
    // PeerPipeline camera-disable / GStreamer state→NULL) can stall the
    // Qt event loop so the QTimer timeout never fires. This thread is
    // independent of the loop and force-exits at the deadline no matter
    // what. Exit 124 = watchdog kill (distinct from 0 pass / 1 fail).
    std::thread([timeout]() {
        std::this_thread::sleep_for(std::chrono::seconds(timeout));
        std::fprintf(stderr,
            "\n[WATCHDOG] hard %ds deadline hit - force terminate\n", timeout);
        std::fflush(stderr);
        std::_Exit(124);
    }).detach();

    qDebug() << "TalQ Call Test Harness (MCU mode)";
    qDebug() << "Server:" << SERVER;
    qDebug() << "Users:" << USER_A << "vs" << USER_B;
    qDebug() << "Room:" << token;
    qDebug() << "Timeout:" << timeout << "seconds";
    qDebug() << "";

    auto *test = new CallTest(token, timeout, &app);
    QTimer::singleShot(0, test, &CallTest::run);

    return app.exec();
}

#include "call_test.moc"
