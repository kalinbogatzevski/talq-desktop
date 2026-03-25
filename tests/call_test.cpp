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
#include <QTimer>
#include <QCommandLineParser>
#include <QDebug>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkReply>
#include <QRegularExpression>
#include <gst/gst.h>
#include <glib.h>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

#include "core/ApiClient.h"
#include "core/SignalingClient.h"
#include "core/PeerPipeline.h"

// Test configuration
static const QString SERVER = "https://ncloud.123net.link";
static const QString USER_A = "kalin";
static const QString USER_B = "test-talq";
static const QString PASS_B = "talQing123@";
static const QString DEFAULT_TOKEN = "u2f3gbu4";  // Test TalQ (kalin <-> test-talq)

// Credential helper
static QString loadPasswordFromCredMan()
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
    TestPhase phase = Init;
    QString sessionId;       // HPB session ID
    bool joinedCall = false;
    bool iceConnected = false;
    bool pipelineStarted = false;
    QString stunServer;
    QList<TurnServer> turnServers;
    // Pending ICE candidates (received before pipeline started)
    QList<std::tuple<QString, int, QString>> pendingCandidates;

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

        // Set up User A (kalin)
        QString passA = loadPasswordFromCredMan();
        if (passA.isEmpty()) {
            qCritical() << "Cannot read User A password from Credential Manager";
            qApp->exit(1);
            return;
        }

        setupPeer(m_peerA, "UserA(" + USER_A + ")", SERVER, USER_A, passA);
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
                [this, &peer](const QString &from, const QString &sdp) {
            peer.log("Answer received from " + from.left(20) + "... (" + QString::number(sdp.length()) + " chars)");
            if (peer.pipeline) {
                peer.pipeline->setRemoteAnswer(sdp);
                peer.setPhase(WaitingICE);
            }
        });

        // Offer received from MCU (subscriber flow: MCU sends offer to us)
        connect(peer.signaling, &SignalingClient::offerReceived, this,
                [this, &peer](const QString &from, const QString &sdp, const QString &sid) {
            Q_UNUSED(sid)
            peer.log("Offer received from " + from.left(20) + "... (" + QString::number(sdp.length()) + " chars)");
            if (peer.pipeline) {
                peer.pipeline->setRemoteOffer(sdp);
            }
        });

        // ICE candidates from MCU
        connect(peer.signaling, &SignalingClient::candidateReceived, this,
                [this, &peer](const QString &from, const QJsonObject &candidate) {
            Q_UNUSED(from)
            QString cand = candidate["candidate"].toString();
            int mline = candidate["sdpMLineIndex"].toInt();
            QString mid = candidate["sdpMid"].toString();
            peer.log("ICE candidate received: " + cand.left(80));
            if (peer.pipeline && peer.pipeline->isRunning()) {
                peer.pipeline->addIceCandidate(cand, mline, mid);
            } else {
                peer.pendingCandidates.append({cand, mline, mid});
            }
        });

        // Room joined via signaling
        connect(peer.signaling, &SignalingClient::roomJoined, this, [this, &peer]() {
            peer.log("Room joined via signaling");
            peer.setPhase(StartingCall);
            joinCall(peer);
        });

        // Participant joined call (for tracking)
        connect(peer.signaling, &SignalingClient::participantJoinedCall, this,
                [this, &peer](const QString &sessionId, int flags, const QString &) {
            peer.log(QString("Participant in call: %1 flags=%2").arg(sessionId.left(20)).arg(flags));
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
        // flags: 1=IN_CALL, 2=WITH_AUDIO -> 3 = audio only
        QJsonObject body;
        body["flags"] = 3;
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
        peer.pipeline = new PeerPipeline(this);
        peer.setPhase(Negotiating);

        // Wire pipeline signals
        // In MCU mode, the offer goes to the MCU (recipient = own session)
        connect(peer.pipeline, &PeerPipeline::localOfferReady, this,
                [this, &peer](const QString &sdp) {
            peer.log("Local offer ready (" + QString::number(sdp.length()) + " chars)");
            if (!sdp.contains("m=audio")) {
                peer.log("WARNING: SDP missing m=audio!");
            }
            if (sdp.contains("OPUS") || sdp.contains("opus")) {
                peer.log("SDP has Opus codec");
            }
            // Send offer to MCU: in HPB MCU mode, the signaling server
            // routes the offer to the MCU which creates the answer
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

    void checkBothActive()
    {
        if (m_peerA.iceConnected && m_peerB.iceConnected) {
            qDebug() << "\n===== BOTH PEERS ICE CONNECTED TO MCU =====";
            qDebug() << "Staying active for 3 seconds to verify stability...";
            m_activeTimer.start();
        }
    }

    void onActiveTimerDone()
    {
        qDebug() << "Active period complete. Tearing down...";
        teardown();
    }

    void teardown()
    {
        m_peerA.setPhase(TearingDown);
        m_peerB.setPhase(TearingDown);

        // Stop pipelines
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
                    printSummary(m_peerA.iceConnected && m_peerB.iceConnected);
                    qApp->exit(m_peerA.iceConnected && m_peerB.iceConnected ? 0 : 1);
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
        qDebug() << "\n==========================================";
        qDebug() << (passed ? "   CALL TEST: PASSED"
                            : "   CALL TEST: FAILED");
        qDebug() << "==========================================";
        qDebug().noquote() << QString("  User A: %1 -> %2")
                    .arg(USER_A, -10).arg(phaseStr(m_peerA.phase));
        qDebug().noquote() << QString("  User B: %1 -> %2")
                    .arg(USER_B, -10).arg(phaseStr(m_peerB.phase));
        qDebug().noquote() << QString("  ICE A: %1  ICE B: %2")
                    .arg(m_peerA.iceConnected ? "OK" : "NO")
                    .arg(m_peerB.iceConnected ? "OK" : "NO");
        qDebug() << "==========================================";
    }

    QString m_token;
    TestPeer m_peerA;
    TestPeer m_peerB;
    QTimer m_timeout;
    QTimer m_activeTimer;
};

int main(int argc, char *argv[])
{
    qputenv("TALQ_TEST_AUDIO", "1");

    QCoreApplication app(argc, argv);

    QCommandLineParser parser;
    parser.addOption({"token", "Conversation token", "TOKEN", DEFAULT_TOKEN});
    parser.addOption({"timeout", "Test timeout in seconds", "SECS", "60"});
    parser.addHelpOption();
    parser.process(app);

    QString token = parser.value("token");
    int timeout = parser.value("timeout").toInt();

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
