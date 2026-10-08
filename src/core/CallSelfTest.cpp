// Hidden headless CallManager self-test -- see CallSelfTest.h for what it is and
// why it exists. Everything here is test scaffolding; nothing in the shipping
// code paths calls into it except the argv check at the top of main().

#include "core/CallSelfTest.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaEnum>
#include <QNetworkReply>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QTextStream>
#include <QTime>
#include <QTimer>

#include <atomic>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include <gst/gst.h>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "core/ApiClient.h"
#include "core/CallManager.h"
#include "core/LogRedaction.h"
#include "core/LogSink.h"
#include "core/GstLogFunction.h"
#include "core/MediaDeviceManager.h"
#include "core/SignalingClient.h"
#include "core/TalqLog.h"
#include "core/VideoFrameProvider.h"

namespace {

constexpr int kFramesNeeded      = 30;      // remote video frames that count as "media flowed"
constexpr int kHoldAfterMediaMs  = 4000;    // keep the call up this long once media flows
constexpr int kQuietAfterIdleMs  = 6000;    // watch the wire this long after state -> Idle
constexpr int kRoundTimeoutMs    = 100000;
constexpr int kEarlyMintWaitMs   = 30000;   // early-hang-up round: the offer must be created by then
constexpr int kHangupToIdleMs    = 20000;
constexpr int kCalleeExitWaitMs  = 30000;
constexpr int kRoomEmptyTries    = 40;      // x 3 s
constexpr int kOverallMs         = 15 * 60 * 1000;

// ---- log capture -----------------------------------------------------------
// Every Qt message goes to the redacting sink (the log file) AND into an
// in-memory list the assertions read back. The handler can run on any thread
// (GStreamer callbacks log), so the list is mutex-guarded.
std::mutex                       g_capMtx;
QStringList                      g_cap;
std::mutex                       g_hookMtx;
std::function<void()>            g_earlyHook;    // armed by an early-hang-up round
// The log line that makes an early round hang up. Normally the offer being created; in a
// --cold-session early round it is the signaling socket leaving the room (the 404 retry is
// then abandoned with the socket outside the room, which CallManager must repair).
constexpr const char *kTriggerOfferCreated = "PublishPipeline: offer created";
constexpr const char *kTriggerLeavingRoom  = "Signaling: leaving room";
std::atomic<const char *>        g_earlyTrigger{kTriggerOfferCreated};
QObject                         *g_hookCtx = nullptr;

int capSize()
{
    std::lock_guard<std::mutex> lk(g_capMtx);
    return int(g_cap.size());
}

QStringList capSlice(int from, int to)
{
    std::lock_guard<std::mutex> lk(g_capMtx);
    if (from < 0) from = 0;
    if (to > g_cap.size()) to = int(g_cap.size());
    return from < to ? g_cap.mid(from, to - from) : QStringList();
}

void selfTestMessageHandler(QtMsgType, const QMessageLogContext &, const QString &msg)
{
    const QByteArray line =
        (QTime::currentTime().toString("HH:mm:ss.zzz") + " " + msg + "\n").toUtf8();
    const std::string safe = LogRedaction::redact(
        std::string_view(line.constData(), size_t(line.size())));
    TalqLogSink::writeLine(safe.data(), safe.size());
    {
        std::lock_guard<std::mutex> lk(g_capMtx);
        g_cap.append(QString::fromUtf8(safe.data(), int(safe.size())).trimmed());
    }
    // Early-hang-up trigger: PublishPipeline::onOfferCreated logs this on the
    // GStreamer thread and THEN posts the queued event that emits localOfferReady
    // to CallManager. Queueing our hangUp from here puts it AHEAD of that event on
    // the main thread, so teardown runs with the offer created but not yet
    // delivered -- the race a hang-up has with a publisher that is mid-offer.
    // Fire ONCE, on the main thread, from the event loop -- never from inside this
    // handler.
    if (msg.contains(QLatin1String(g_earlyTrigger.load()))) {
        std::function<void()> hook;
        QObject *ctx = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_hookMtx);
            hook.swap(g_earlyHook);
            ctx = g_hookCtx;
        }
        if (hook && ctx)
            QMetaObject::invokeMethod(ctx, hook, Qt::QueuedConnection);
    }
}

// ---- registry isolation ----------------------------------------------------
// QSettings("TalQ","TalQ") goes to the registry whatever setDefaultFormat says
// (an explicit organisation/application constructor is always NativeFormat), so
// the only way to keep this process off the installed app's keys is to remap
// HKEY_CURRENT_USER for THIS process into a scratch key.
#ifdef Q_OS_WIN
const wchar_t *const kHive = L"Software\\TalQSelfTestHive";
HKEY g_hiveKey = nullptr;

bool isolateRegistry()
{
    RegDeleteTreeW(HKEY_CURRENT_USER, kHive);   // leftovers from a crashed run
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kHive, 0, nullptr, 0, KEY_ALL_ACCESS,
                        nullptr, &g_hiveKey, nullptr) != ERROR_SUCCESS)
        return false;
    if (RegOverridePredefKey(HKEY_CURRENT_USER, g_hiveKey) != ERROR_SUCCESS) {
        RegCloseKey(g_hiveKey);
        g_hiveKey = nullptr;
        return false;
    }
    return true;
}

void listHiveValues(HKEY root, const std::wstring &path, QStringList &out)
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    DWORD nSub = 0, maxSub = 0, nVal = 0, maxVal = 0;
    RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &nSub, &maxSub, nullptr,
                     &nVal, &maxVal, nullptr, nullptr, nullptr);
    for (DWORD i = 0; i < nVal; ++i) {
        std::wstring name(size_t(maxVal) + 1, L'\0');
        DWORD len = maxVal + 1;
        if (RegEnumValueW(k, i, name.data(), &len, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
            out << QString::fromStdWString(path + L"\\" + std::wstring(name.c_str(), len));
    }
    for (DWORD i = 0; i < nSub; ++i) {
        std::wstring name(size_t(maxSub) + 1, L'\0');
        DWORD len = maxSub + 1;
        if (RegEnumKeyExW(k, i, name.data(), &len, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
            listHiveValues(root, path + L"\\" + std::wstring(name.c_str(), len), out);
    }
    RegCloseKey(k);
}

// Restore the real HKCU, report which values the call path wrote into the scratch
// hive, then delete the hive. Returns the written value paths.
QStringList releaseRegistry()
{
    QStringList written;
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    if (g_hiveKey) { RegCloseKey(g_hiveKey); g_hiveKey = nullptr; }
    listHiveValues(HKEY_CURRENT_USER, kHive, written);
    RegDeleteTreeW(HKEY_CURRENT_USER, kHive);
    return written;
}
#else
bool isolateRegistry() { return true; }
QStringList releaseRegistry() { return {}; }
#endif

// ---- configuration + per-round record --------------------------------------
struct Cfg {
    QString server;                                        // no default: this file is in the PUBLIC source
                                                           // tree, so TALQ_TEST_SERVER must name the host
                                                           // (scripts/run-selftest.sh supplies it)
    QString token       = QStringLiteral("aqzoti8m");     // "TalQ Auto Test": the two bots only
    QString user, pass;                                    // the account CallManager runs as
    QString calleeUser, calleePass;                        // the harness bot that answers
    QString calleeExe;
    QString logPath;
    int     healthyRounds = 2;
    int     earlyRounds   = 3;
    // --cold-session: no REST warm-up, and the REST session (cookie jar) is dropped right
    // before every call. A brand-new Nextcloud session loses its first participants/active, so
    // the call join answers 404 -- on demand. Proves CallManager's one-shot room re-join heals it.
    bool    coldSession   = false;
};

struct Round {
    bool    early = false;
    int     n = 0;
    qint64  startedMs = 0, mediaSeenMs = 0, hangupMs = 0;
    int     startIdx = -1, idleIdx = -1, endIdx = -1;
    bool    hangupSent = false, idleSeen = false, calleeReady = false;
    int     localFrames = 0, calleeFrames = -1, calleeExit = -999;
    QString endedBy, sid;
    bool    windowHit = false;
    QStringList fails, notes;
};

QString stateName(CallManager::CallState s)
{
    return QString::fromLatin1(QMetaEnum::fromType<CallManager::CallState>().valueToKey(int(s)));
}

// Print to the log (via qInfo) AND to stdout so a wrapper script can follow along.
void say(const QString &s)
{
    qInfo().noquote() << "SELFTEST:" << s;
    std::fprintf(stdout, "SELFTEST: %s\n", s.toUtf8().constData());
    std::fflush(stdout);
}

class SelfTest : public QObject
{
public:
    explicit SelfTest(const Cfg &c)
        : m_cfg(c), m_signaling(&m_api), m_cm(&m_api, &m_signaling, &m_devices)
    {
        for (int i = 0; i < c.healthyRounds; ++i) { Round r; r.n = int(m_rounds.size()) + 1; m_rounds.append(r); }
        for (int i = 0; i < c.earlyRounds; ++i)   { Round r; r.n = int(m_rounds.size()) + 1; r.early = true; m_rounds.append(r); }
    }

    void start()
    {
        m_api.setServerUrl(m_cfg.server);
        m_api.setCredentials(m_cfg.user, m_cfg.pass);
        // Same reasoning as the harness: keep ONE signaling session for every
        // round, so an idle re-home can't swap the HPB between calls.
        m_signaling.setIdleRehomeEnabled(false);

        connect(&m_cm, &CallManager::stateChanged, this, &SelfTest::onState);
        connect(&m_cm, &CallManager::callFailed, this, [](const QString &title, const QString &msg) {
            say("callFailed: " + title + " -- " + msg.left(120).simplified());
        });
        connect(&m_signaling, &SignalingClient::connectedChanged, this, [this]() {
            if (!m_signaling.isConnected() || m_began) return;
            m_began = true;
            say("signaling connected as " + m_cfg.user);
            if (m_cfg.coldSession) {
                say("cold session: no REST warm-up; the REST session is dropped before every call");
                QTimer::singleShot(1500, this, [this]() { beginRound(); });
            } else {
                warmRestSession();
            }
        });
        connect(&m_tick, &QTimer::timeout, this, &SelfTest::tick);
        m_tick.start(500);
        QTimer::singleShot(kOverallMs, this, [this]() { fatal("overall timeout"); });
        m_signaling.start();
    }

    bool failed() const { return m_failed; }

private:
    Round &cur() { return m_rounds[m_idx]; }
    static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

    void fatal(const QString &why)
    {
        say("FATAL: " + why);
        m_failed = true;
        if (m_callee) m_callee->kill();
        if (m_cm.state() != CallManager::Idle) m_cm.hangUp();
        m_signaling.stop();
        QCoreApplication::exit(1);
    }

    // Measured with curl against ncloud (cause NOT established): in a fresh
    // cookie jar, ONE participants/active followed by POST call/{token} answers
    // 404, and GET room then shows sessionId 0 -- the room session was not
    // retained, and the PHP session cookie changed between requests. What was
    // tried before that single participants/active: GET /v4/room (list or single)
    // first -> the join held; any other first request (none, capabilities, GET
    // /v4/call/{token}, chat, signaling settings, user status) -> still 404;
    // participants/active TWICE -> held. So a plain "many requests before the first
    // call" does NOT protect a session; only having served GET /v4/room does, and the
    // real app lists its conversations before any call. Whether it could still meet
    // this (a server-side session loss while it runs) is not verified. Warm the
    // session the way that was seen to work: one conversation-list GET before
    // anything joins a room.
    void warmRestSession()
    {
        QNetworkReply *reply = m_api.getRaw("apps/spreed/api/v4/room");
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            say(QString("REST session warmed (conversation list: HTTP %1) -- first round in 1.5 s")
                    .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
            QTimer::singleShot(1500, this, [this]() { beginRound(); });
        });
    }

    // ---- round lifecycle ----------------------------------------------------
    void beginRound()
    {
        if (m_idx >= int(m_rounds.size())) { finish(); return; }
        Round &r = cur();
        say(QString("---- round %1/%2 (%3) ----").arg(r.n).arg(m_rounds.size())
                .arg(r.early ? "early hang-up, no callee" : "healthy call with callee"));
        m_phase = Phase::WaitRoom;
        waitRoomEmpty(0);
    }

    void waitRoomEmpty(int tries)
    {
        QNetworkReply *reply = m_api.getRaw("apps/spreed/api/v4/call/" + m_cfg.token);
        connect(reply, &QNetworkReply::finished, this, [this, reply, tries]() {
            reply->deleteLater();
            int n = 99;
            if (reply->error() == QNetworkReply::NoError) {
                const QJsonDocument d = QJsonDocument::fromJson(reply->readAll());
                if (d.isObject()) n = int(d.object()["ocs"].toObject()["data"].toArray().size());
            }
            if (n == 0) {
                if (cur().early) startRoundCall(); else spawnCallee();
                return;
            }
            if (tries + 1 >= kRoomEmptyTries) {
                cur().fails << QString("room %1 never emptied (%2 in-call participants after %3 s)")
                                   .arg(m_cfg.token).arg(n).arg(kRoomEmptyTries * 3);
                m_failed = true;
                fatal("room not empty -- stale in-call sessions from an earlier run");
                return;
            }
            QTimer::singleShot(3000, this, [this, tries]() { waitRoomEmpty(tries + 1); });
        });
    }

    void spawnCallee()
    {
        Round &r = cur();
        m_calleeBuf.clear();
        m_callee = new QProcess(this);
        m_callee->setProcessChannelMode(QProcess::MergedChannels);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("TALQ_TEST_USERA", m_cfg.calleeUser);
        env.insert("TALQ_TEST_USERA_PASS", m_cfg.calleePass);
        env.insert("TALQ_TEST_TOKEN", m_cfg.token);
        env.insert("TALQ_TEST_AUTO_ANSWER", "1");
        env.insert("TALQ_TEST_RING_ONLY", "1");
        env.insert("TALQ_TEST_PUBPIPE", "1");
        env.insert("TALQ_PUB_TESTSRC", "1");
        env.insert("TALQ_TEST_AUDIO_VOL", qEnvironmentVariable("TALQ_TEST_AUDIO_VOL", "0.02"));
        env.remove("GST_DEBUG");
        env.remove("TALQ_TEST_USERB");
        env.remove("TALQ_TEST_USERB_PASS");
        m_callee->setProcessEnvironment(env);
        connect(m_callee, &QProcess::readyRead, this, [this]() { drainCallee(); });
        connect(m_callee, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            drainCallee();
            if (m_idx < int(m_rounds.size())) cur().calleeExit = code;
            m_calleeDone = true;
            say(QString("callee exited code %1").arg(code));
            if (m_phase == Phase::WaitCallee) advanceAfterCallee();
        });
        m_calleeDone = false;
        say("spawning callee " + QFileInfo(m_cfg.calleeExe).fileName() + " as " + m_cfg.calleeUser);
        m_callee->start(m_cfg.calleeExe, QStringList{ "--timeout", "90" });
        if (!m_callee->waitForStarted(10000)) { fatal("callee failed to start: " + m_callee->errorString()); return; }
        r.calleeReady = false;
        QTimer::singleShot(45000, this, [this, n = r.n]() {
            if (m_idx < int(m_rounds.size()) && cur().n == n && !cur().calleeReady && m_phase == Phase::WaitRoom)
                fatal("callee never reached the room");
        });
    }

    void drainCallee()
    {
        m_calleeBuf += m_callee->readAll();
        int nl;
        while ((nl = m_calleeBuf.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(m_calleeBuf.left(nl)).trimmed();
            m_calleeBuf.remove(0, nl + 1);
            if (line.isEmpty()) continue;
            qInfo().noquote() << "CALLEE>" << line;
            if (m_idx >= int(m_rounds.size())) continue;
            Round &r = cur();
            static const QRegularExpression reFrames(R"(received (\d+) remote video frames)");
            const auto m = reFrames.match(line);
            if (m.hasMatch()) r.calleeFrames = qMax(r.calleeFrames, m.captured(1).toInt());
            if (!r.calleeReady && line.contains(QLatin1String("POLLING for the other party"))) {
                r.calleeReady = true;
                say("callee is in the room -- placing the call in 1.5 s");
                QTimer::singleShot(1500, this, [this, n = r.n]() {
                    if (m_idx < int(m_rounds.size()) && cur().n == n && m_phase == Phase::WaitRoom)
                        startRoundCall();
                });
            }
        }
    }

    void startRoundCall()
    {
        Round &r = cur();
        m_phase = Phase::InCall;
        r.startIdx = capSize();
        r.startedMs = nowMs();
        if (r.early) {
            g_earlyTrigger = m_cfg.coldSession ? kTriggerLeavingRoom : kTriggerOfferCreated;
            std::lock_guard<std::mutex> lk(g_hookMtx);
            g_hookCtx = this;
            g_earlyHook = [this]() { onOfferCreated(); };
        }
        if (m_cfg.coldSession) {
            m_api.resetSession();   // fresh cookie jar: the next REST request opens a new Nextcloud session
            say("cold session: REST session dropped");
        }
        say(QString("round %1: startCall(%2, video)").arg(r.n).arg(m_cfg.token));
        m_cm.startCall(m_cfg.token, true);
    }

    void onOfferCreated()
    {
        if (m_idx >= int(m_rounds.size()) || m_phase != Phase::InCall) return;
        Round &r = cur();
        if (!r.early || r.hangupSent || m_cm.state() == CallManager::Idle) return;
        hangUp(r, m_cfg.coldSession ? "early hang-up while the room re-join retry was running"
                                : "early hang-up with the offer created but not yet delivered");
    }

    void hangUp(Round &r, const QString &why)
    {
        r.hangupSent = true;
        r.hangupMs = nowMs();
        r.endedBy = why;
        say(QString("round %1: hangUp() -- %2 (state %3)").arg(r.n).arg(why).arg(stateName(m_cm.state())));
        m_cm.hangUp();
    }

    void tick()
    {
        if (m_idx >= int(m_rounds.size()) || m_phase != Phase::InCall) return;
        Round &r = cur();
        const CallManager::CallState st = m_cm.state();
        const qint64 el = nowMs() - r.startedMs;

        if (!r.early && !r.hangupSent) {
            VideoFrameProvider *p = m_cm.remoteVideoProvider();
            const int fc = p ? p->frameCount() : 0;
            r.localFrames = qMax(r.localFrames, fc);
            if (st == CallManager::Active && r.localFrames >= kFramesNeeded) {
                if (!r.mediaSeenMs) {
                    r.mediaSeenMs = nowMs();
                    say(QString("round %1: media flowing (%2 remote frames, %3 ms after startCall)")
                            .arg(r.n).arg(r.localFrames).arg(el));
                }
                if (nowMs() - r.mediaSeenMs >= kHoldAfterMediaMs)
                    hangUp(r, "self-test hang-up after media");
            }
            if (!r.hangupSent && el > kRoundTimeoutMs) {
                r.fails << QString("no media within %1 s (state %2, %3 remote frames)")
                               .arg(kRoundTimeoutMs / 1000).arg(stateName(st)).arg(r.localFrames);
                hangUp(r, "round timeout");
            }
        }
        if (r.early && !r.hangupSent && el > kEarlyMintWaitMs) {
            r.fails << QString("no offer was created within %1 s (state %2) -- hook did not fire")
                           .arg(kEarlyMintWaitMs / 1000).arg(stateName(st));
            hangUp(r, "early round timeout");
        }
        if (r.hangupSent && st != CallManager::Idle && nowMs() - r.hangupMs > kHangupToIdleMs) {
            r.fails << QString("state still %1, %2 s after hangUp()")
                           .arg(stateName(st)).arg(kHangupToIdleMs / 1000);
            fatal("call did not return to Idle");
        }
    }

    void onState()
    {
        if (m_idx >= int(m_rounds.size()) || m_phase != Phase::InCall) return;
        Round &r = cur();
        if (m_cm.state() != CallManager::Idle || r.idleSeen) return;
        r.idleSeen = true;
        r.idleIdx = capSize();
        if (!r.hangupSent) r.endedBy = "the call ended by itself";
        m_phase = Phase::Quiet;
        say(QString("round %1: state -> Idle (%2); watching the wire for %3 s")
                .arg(r.n).arg(r.endedBy).arg(kQuietAfterIdleMs / 1000));
        QTimer::singleShot(kQuietAfterIdleMs, this, [this]() { endQuiet(); });
    }

    void endQuiet()
    {
        cur().endIdx = capSize();
        m_phase = Phase::WaitCallee;
        if (!m_callee || m_calleeDone) { advanceAfterCallee(); return; }
        QTimer::singleShot(kCalleeExitWaitMs, this, [this, n = cur().n]() {
            if (m_idx < int(m_rounds.size()) && cur().n == n && m_phase == Phase::WaitCallee && !m_calleeDone) {
                say("callee still running -- killing it");
                m_callee->kill();
            }
        });
    }

    void advanceAfterCallee()
    {
        if (m_phase != Phase::WaitCallee) return;
        m_phase = Phase::Between;
        if (m_callee) { m_callee->deleteLater(); m_callee = nullptr; }
        evaluate(cur());
        ++m_idx;
        QTimer::singleShot(1000, this, [this]() { beginRound(); });
    }

    // ---- assertions ---------------------------------------------------------
    void evaluate(Round &r)
    {
        static const QRegularExpression reMint(R"~(sent publish offer to own session, sid=\s*"?(\d+)"?)~");
        static const QRegularExpression reAnswer(R"~(received answer from "[^"]*" sid=\s*"([^"]*)")~");
        static const QRegularExpression reBad(
            R"(ice-new-stall|ignoring (answer|candidate) for replaced publisher|Signaling: DROPPED|CallManager: failed to join call)");
        static const QRegularExpression reLateWs(R"~(Signaling: WS >> "(offer|candidate)")~");
        static const QRegularExpression reReqOffer(R"~(Signaling: WS >> "requestoffer")~");

        const QStringList all   = capSlice(r.startIdx, r.endIdx);
        const QStringList after = r.idleIdx >= 0 ? capSlice(r.idleIdx, r.endIdx) : QStringList();
        const QStringList before = r.idleIdx >= 0 ? capSlice(r.startIdx, r.idleIdx) : all;

        if (!r.idleSeen) r.fails << "state never returned to Idle";

        for (const QString &l : all)
            if (reBad.match(l).hasMatch())
                r.fails << "forbidden line: " + l.left(170);
        for (const QString &l : after) {
            if (reLateWs.match(l).hasMatch())  r.fails << "sent after state -> Idle: " + l.left(120);
            if (reMint.match(l).hasMatch())    r.fails << "publish offer sent after state -> Idle: " + l.left(120);
            if (reReqOffer.match(l).hasMatch()) r.notes << "requestoffer after Idle (not asserted): " + l.left(100);
        }

        // What --cold-session is for: the join answered 404, CallManager re-entered the room
        // exactly once, and the retried join then held (the media checks prove the latter).
        // A cold round in which no 404 happened proves NOTHING about the retry, so it fails
        // rather than passing quietly.
        auto coldChecks = [&]() {
            if (!m_cfg.coldSession) return;
            int started = 0, retried = 0, got404 = 0;
            for (const QString &l : all) {
                if (l.contains(QLatin1String("re-joining the room once"))) ++started;
                if (l.contains(QLatin1String("room re-joined -- retrying the call join"))) ++retried;
                if (l.contains(QLatin1String("failed to join call, status= 404"))) ++got404;
            }
            if (started == 0 && got404 == 0)
                r.fails << "--cold-session: the first call join did not answer 404, so the room re-join was never exercised "
                           "(the server no longer reproduces the session loss, or resetSession() no longer drops it)";
            else if (started == 0)
                r.fails << QString("the call join answered 404 %1x but no room re-join was started").arg(got404);
            else if (retried != started)
                r.fails << QString("a room re-join started %1x but the join was retried %2x").arg(started).arg(retried);
            else
                r.notes << QString("recovered from a call-join 404 with %1 room re-join(s)").arg(started);
            if (started > 1)
                r.fails << QString("%1 room re-joins in one call (the budget is 1)").arg(started);
        };

        if (r.early && m_cfg.coldSession) {
            // Hung up the instant the socket left the room for the 404 retry: the retry is abandoned
            // with the socket OUTSIDE the room. CallManager must put it back (a joined-room ack
            // after Idle), and nothing may be offered for the call we left.
            int started = 0, restored = 0;
            for (const QString &l : all)
                if (l.contains(QLatin1String("re-joining the room once"))) ++started;
            for (const QString &l : after)
                if (l.contains(QLatin1String("Signaling: joined room"))) ++restored;
            if (started == 0)
                r.fails << "--cold-session: no join 404 and no room re-join started, so the abandoned-retry path was never exercised";
            else if (!r.hangupSent)
                r.fails << "the hang-up during the room re-join never fired";
            else if (restored == 0)
                r.fails << "the retry was abandoned and the signaling socket was never put back into the room";
            else {
                r.windowHit = true;
                r.notes << "hung up while the room re-join retry was running; the socket was put back into the room";
            }
            return;
        }

        if (r.early) {
            coldChecks();
            bool offerBefore = false;
            for (const QString &l : before)
                if (reMint.match(l).hasMatch()) offerBefore = true;
            r.windowHit = r.hangupSent && !offerBefore;
            r.notes << (r.windowHit
                ? "hung up with the offer created but not yet delivered (the late-offer window)"
                : "window MISSED: the offer had already been sent before the hang-up");
            return;
        }

        for (const QString &l : all) {
            const auto m = reMint.match(l);
            if (m.hasMatch()) { r.sid = m.captured(1); break; }
        }
        if (r.sid.isEmpty()) {
            r.fails << "no 'sent publish offer' line -- the publisher never offered";
        } else {
            bool echoed = false;
            QStringList seen;
            for (const QString &l : all) {
                const auto m = reAnswer.match(l);
                if (!m.hasMatch()) continue;
                seen << m.captured(1);
                if (m.captured(1) == r.sid) echoed = true;
            }
            if (!echoed)
                r.fails << QString("no answer echoed the minted sid %1 (answers carried: %2)")
                               .arg(r.sid, seen.isEmpty() ? QStringLiteral("none") : seen.join(","));
        }
        if (r.localFrames < kFramesNeeded)
            r.fails << QString("received only %1 remote video frames (need %2)").arg(r.localFrames).arg(kFramesNeeded);
        if (r.calleeFrames < kFramesNeeded)
            r.fails << QString("the callee received only %1 video frames from us (need %2)")
                           .arg(r.calleeFrames).arg(kFramesNeeded);
        if (!r.hangupSent) {
            // The harness callee tears down as soon as IT has seen enough frames, which can be before
            // our hold after media has run out (a slower start, e.g. after a join retry, shifts it).
            // Its leaving ends the call. That is a fine end once media demonstrably flowed both ways;
            // a call that died WITHOUT media is still a failure.
            static const QRegularExpression reCalleeDone(
                R"(CALLEE> .*Bidirectional video test complete\. Tearing down)");
            bool calleeLeftFirst = false;
            for (const QString &l : before)
                if (reCalleeDone.match(l).hasMatch()) calleeLeftFirst = true;
            if (r.mediaSeenMs > 0 && r.calleeFrames >= kFramesNeeded && calleeLeftFirst)
                r.notes << "the callee finished its own test and left first, which ended the call (media had flowed both ways)";
            else
                r.fails << "the call ended before the self-test hung up";
        }

        coldChecks();
    }

    void finish()
    {
        m_phase = Phase::Done;
        // Cross-round: a fresh publisher sid every call.
        QString prev;
        for (Round &r : m_rounds) {
            if (r.early || r.sid.isEmpty()) continue;
            if (!prev.isEmpty() && prev == r.sid)
                r.fails << "publisher sid " + r.sid + " is the same as the previous call's";
            prev = r.sid;
        }
        int hits = 0, earlyN = 0;
        for (const Round &r : m_rounds) { if (r.early) { ++earlyN; if (r.windowHit) ++hits; } }

        say("================ SUMMARY ================");
        for (const Round &r : m_rounds) {
            const bool ok = r.fails.isEmpty();
            if (!ok) m_failed = true;
            say(QString("round %1 %2 [%3]: %4%5")
                    .arg(r.n).arg(ok ? "PASS" : "FAIL").arg(r.early ? "early hang-up" : "healthy")
                    .arg(r.early ? QString("sid n/a") : QString("sid=%1 localFrames=%2 calleeFrames=%3 calleeExit=%4")
                                                           .arg(r.sid).arg(r.localFrames).arg(r.calleeFrames).arg(r.calleeExit))
                    .arg(r.endedBy.isEmpty() ? QString() : QString(" -- %1").arg(r.endedBy)));
            for (const QString &f : r.fails) say("    FAIL: " + f);
            for (const QString &n : r.notes) say("    note: " + n);
        }
        if (earlyN)
            say(QString("early hang-up rounds that hit their window (%3): %1 of %2 "
                        "(a pass here is evidence only for the rounds that hit it)").arg(hits).arg(earlyN)
                            .arg(m_cfg.coldSession ? "while the 404 retry was running" : "offer created, not yet delivered"));
        m_signaling.stop();
        QTimer::singleShot(500, this, []() { QCoreApplication::exit(0); });
    }

    enum class Phase { Init, WaitRoom, InCall, Quiet, WaitCallee, Between, Done };

    Cfg                 m_cfg;
    ApiClient           m_api;
    SignalingClient     m_signaling;
    MediaDeviceManager  m_devices;
    CallManager         m_cm;
    QTimer              m_tick;
    QList<Round>        m_rounds;
    int                 m_idx = 0;
    Phase               m_phase = Phase::Init;
    bool                m_began = false, m_failed = false, m_calleeDone = true;
    QProcess           *m_callee = nullptr;
    QByteArray          m_calleeBuf;
};

QString argValue(int argc, char **argv, const char *flag)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (QByteArray(argv[i]) == flag) return QString::fromLocal8Bit(argv[i + 1]);
    return {};
}

} // namespace

int talqRunCallSelfTest(int argc, char **argv)
{
    // GST_PLUGIN_PATH / GST_PLUGIN_SCANNER were set by main() just before the
    // dispatch. Everything below stays off the shared single-instance lock, the
    // live debug log and the real registry.
    Cfg cfg;
    cfg.user       = qEnvironmentVariable("TALQ_TEST_USERA", "test-talq2");
    cfg.pass       = qEnvironmentVariable("TALQ_TEST_USERA_PASS");
    cfg.calleeUser = qEnvironmentVariable("TALQ_TEST_USERB", "test-talq");
    cfg.calleePass = qEnvironmentVariable("TALQ_TEST_USERB_PASS");
    cfg.token      = qEnvironmentVariable("TALQ_TEST_TOKEN", cfg.token);
    cfg.server     = qEnvironmentVariable("TALQ_TEST_SERVER", cfg.server);
    cfg.logPath    = argValue(argc, argv, "--log");
    if (cfg.logPath.isEmpty())
        cfg.logPath = QDir::tempPath() + "/talq_selftest.log";
    cfg.calleeExe  = argValue(argc, argv, "--callee");
    if (cfg.calleeExe.isEmpty())
        cfg.calleeExe = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath() + "/talq-call-test.exe";
    // A non-numeric or negative count used to become 0 and, with 0 + 0 rounds, "pass" having
    // asserted nothing -- refuse instead.
    auto roundsArg = [&](const char *flag, int &dst) -> bool {
        const QString v = argValue(argc, argv, flag);
        if (v.isEmpty()) return true;
        bool ok = false;
        const int n = v.toInt(&ok);
        if (!ok || n < 0) {
            std::fprintf(stdout, "SELFTEST: %s needs a whole number >= 0 (got '%s')\n", flag, qPrintable(v));
            return false;
        }
        dst = n;
        return true;
    };
    if (!roundsArg("--rounds", cfg.healthyRounds) || !roundsArg("--early-rounds", cfg.earlyRounds))
        return 2;
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--cold-session") cfg.coldSession = true;
    if (cfg.healthyRounds + cfg.earlyRounds == 0) {
        std::fprintf(stdout, "SELFTEST: 0 rounds requested -- nothing would be asserted\n");
        return 2;
    }
    if (cfg.server.isEmpty()) {
        std::fprintf(stdout, "SELFTEST: missing TALQ_TEST_SERVER (https://<nextcloud host>; scripts/run-selftest.sh sets it)\n");
        return 2;
    }

    // Never ring a person: both accounts must be the dedicated test bots.
    if (!cfg.user.startsWith("test-talq") || !cfg.calleeUser.startsWith("test-talq")) {
        std::fprintf(stdout, "SELFTEST: refusing to run -- accounts must be test bots (got %s / %s)\n",
                     qPrintable(cfg.user), qPrintable(cfg.calleeUser));
        return 2;
    }
    if (cfg.pass.isEmpty() || cfg.calleePass.isEmpty()) {
        std::fprintf(stdout, "SELFTEST: missing TALQ_TEST_USERA_PASS / TALQ_TEST_USERB_PASS (source ~/.talq-test.env)\n");
        return 2;
    }
    if (!QFileInfo::exists(cfg.calleeExe)) {
        std::fprintf(stdout, "SELFTEST: callee harness not found: %s\n", qPrintable(cfg.calleeExe));
        return 2;
    }

    // Own log file; the installed app's talq_debug.log is never opened.
    if (!freopen(cfg.logPath.toLocal8Bit().constData(), "w", stderr)) {
        std::fprintf(stdout, "SELFTEST: cannot open log %s\n", qPrintable(cfg.logPath));
        return 2;
    }
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    qputenv("GST_DEBUG", "2");
    // Synthetic camera + tone on OUR publisher: no real camera or microphone is opened.
    qputenv("TALQ_TEST_AUDIO", "1");
    qputenv("TALQ_PUB_TESTSRC", "1");
    TalqLog::g_verbose = true;
    qInstallMessageHandler(selfTestMessageHandler);

    const bool isolated = isolateRegistry();
    TalqGstLog::install();
    gst_init(&argc, &argv);

    int rc = 1;
    QStringList written;
    {
        QApplication app(argc, argv);
        app.setApplicationName("TalQ");
        app.setOrganizationName("TalQ");
        say(QString("start: user=%1 callee=%2 room=%3 rounds=%4+%5 registry-isolated=%6 log=%7")
                .arg(cfg.user, cfg.calleeUser, cfg.token).arg(cfg.healthyRounds).arg(cfg.earlyRounds)
                .arg(isolated ? "yes" : "NO").arg(cfg.logPath));
        if (!isolated) {
            say("FATAL: could not redirect HKEY_CURRENT_USER -- refusing to touch the real settings");
            return 2;
        }
        {
            SelfTest test(cfg);
            test.start();
            rc = app.exec();
            if (test.failed()) rc = 1;
        }
        std::lock_guard<std::mutex> lk(g_hookMtx);
        g_earlyHook = nullptr;
        g_hookCtx = nullptr;
    }
    written = releaseRegistry();
    std::fprintf(stdout, "SELFTEST: registry values written by the call path (scratch hive, now deleted): %d\n",
                 int(written.size()));
    for (const QString &w : written)
        std::fprintf(stdout, "SELFTEST:   %s\n", qPrintable(w));
    std::fprintf(stdout, "SELFTEST RESULT: %s (exit %d)\n", rc == 0 ? "PASS" : "FAIL", rc);
    std::fflush(stdout);
    return rc;
}
