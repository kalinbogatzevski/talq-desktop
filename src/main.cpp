#include <QApplication>
#include <QIcon>
#include <QRegularExpression>
#include <cerrno>
#include <QSharedMemory>
#include <QLocalServer>
#include <QLocalSocket>
#include <QScreen>
#include <QPainter>
#include <QSplashScreen>
#include <QStandardPaths>
#include <QSettings>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QTime>
#include <csignal>
#include <cstdlib>
#include <exception>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#include <dwmapi.h>
#include <dbghelp.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dbghelp.lib")
#endif

// Crash backstop. A native fault (access violation, etc.) would otherwise
// make TalQ vanish silently with no trace — unacceptable for a comms app.
// This last-resort filter writes a minidump + a marker line next to the
// log so EVERY crash is diagnosable, then lets the process terminate
// (cleanly, with evidence) instead of disappearing. It is the safety net
// BEHIND the real fix: every foreseeable media failure is made non-fatal
// at its source so we never reach here in normal operation.
#ifdef Q_OS_WIN
namespace {
wchar_t g_crashDumpPath[MAX_PATH] = L"";

// Shared dump writer. `info` is non-null only for the SEH path; abort()
// and std::terminate have no exception record (pass nullptr).
void talqWriteMiniDump(EXCEPTION_POINTERS *info)
{
    if (!g_crashDumpPath[0]) return;
    HANDLE f = CreateFileW(g_crashDumpPath, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = info;
    mei.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                      MiniDumpWithIndirectlyReferencedMemory,
                      info ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
    fprintf(stderr, "[TalQ FATAL] minidump written\n");
    fflush(stderr);
}

LONG WINAPI talqCrashFilter(EXCEPTION_POINTERS *info)
{
    // stderr is freopen'd to the log file; record the crash there first so
    // it survives even if minidump writing fails.
    fprintf(stderr,
            "\n[TalQ FATAL] unhandled exception 0x%08lX at %p — writing dump\n",
            info ? info->ExceptionRecord->ExceptionCode : 0,
            info ? (void*)info->ExceptionRecord->ExceptionAddress : nullptr);
    fflush(stderr);
    talqWriteMiniDump(info);
    return EXCEPTION_EXECUTE_HANDLER;  // terminate with evidence, not silently
}

// SetUnhandledExceptionFilter does NOT catch abort() (SIGABRT). GLib
// g_assert/g_error and — critically — a Rust panic inside the gstrswebrtc
// plugin (the new subscribe path) terminate via abort(), which would
// otherwise vanish with no dump. This backstop catches that class too.
extern "C" void talqAbortHandler(int)
{
    fprintf(stderr, "\n[TalQ FATAL] abort()/SIGABRT — likely a GLib assertion "
                     "or Rust panic in a GStreamer plugin. Writing dump.\n");
    fflush(stderr);
    talqWriteMiniDump(nullptr);
    signal(SIGABRT, SIG_DFL);
    _exit(134);  // 128 + SIGABRT, with evidence
}

void talqTerminateHandler()
{
    fprintf(stderr, "\n[TalQ FATAL] std::terminate (unhandled C++ exception). "
                     "Writing dump.\n");
    fflush(stderr);
    talqWriteMiniDump(nullptr);
    _exit(134);
}
} // namespace
#endif

#include "core/ApiClient.h"
#include "core/AuthManager.h"
#include "core/MessageCache.h"
#include "core/NotificationManager.h"
#include "core/PushClient.h"
#include "core/SignalingClient.h"
#include "models/ConversationListModel.h"
#include "models/MessageListModel.h"
#include "models/ThreadListModel.h"
#include "core/MediaDeviceManager.h"
#include "core/CallManager.h"
#include "core/UserStatusManager.h"
#include "core/DebugMonitor.h"
#include "core/AppSettings.h"
#include "core/EmojiData.h"
#include "core/TalqLog.h"
#include "ui/MainWindow.h"
#include "ui/NotificationPopup.h"
#include "ui/NotificationStack.h"
#include "painter/ChatPainter.h"
#include "painter/SidebarPainter.h"
#include "painter/PainterTheme.h"
#include <QFontDatabase>
#include <QMessageBox>
#include <gst/gst.h>

int main(int argc, char *argv[])
{
    // Scan argv manually (before QApplication/gst_init) so verbose logging
    // is active during startup, including gst_init plugin-scan output.
    bool debugRequested = false;
    for (int i = 1; i < argc; ++i) {
        QByteArray a(argv[i]);
        if (a == "--debug" || a == "-d") { debugRequested = true; break; }
    }
    // Logging is ALWAYS on (release included): a comms app that can die
    // mid-call must never do so without a log + minidump next to it. The
    // FILE-LEVEL log + crash backstop are unconditional. What's tunable is:
    //   (a) `g_verbose` — captures qDebug app diagnostics. DEFAULT ON
    //       (user-stated 2026-05-23: "debug log must be always enabled");
    //       Settings → Diagnostics → "Detailed logging" can opt OUT.
    //   (b) heavy GST tracing (GST_DEBUG=2 + per-element categories) — still
    //       opt-IN via `--debug` because it floods the log and costs perf.
    bool detailedLogging = true;
    {
        QSettings s("TalQ", "TalQ");
        s.beginGroup("Diagnostics");
        detailedLogging = s.value("detailedLogging", true).toBool();
        s.endGroup();
    }
    bool verbose = detailedLogging || debugRequested;
#ifdef QT_DEBUG
    verbose = true;
#endif
    if (verbose) TalqLog::g_verbose = true;

    // Set GStreamer plugin path relative to the exe (before gst_init)
    {
        std::string exePath(argv[0]);
        auto lastSlash = exePath.find_last_of("\\/");
        std::string appDir = (lastSlash != std::string::npos) ? exePath.substr(0, lastSlash) : ".";
        std::string gstPath = appDir + "/gst-plugins";
        qputenv("GST_PLUGIN_PATH", QByteArray::fromStdString(gstPath));
        // Point at the deployed out-of-process plugin scanner. Without it
        // GStreamer logs "Couldn't create helper process" and falls back to
        // scanning EVERY plugin in-process, which balloons the main process by
        // ~450 MB on each launch (field: ~780 MB peak) and doesn't cache the
        // registry cleanly, so it re-scans every time. With the scanner present
        // GStreamer scans out-of-process and caches the registry — fast, lean
        // startups. The scanner is deployed next to talq.exe by the build/deploy
        // scripts.
        std::string scanner = appDir + "/gst-plugin-scanner.exe";
        qputenv("GST_PLUGIN_SCANNER", QByteArray::fromStdString(scanner));
    }

    // Single-instance detection — MUST run before any logging side effects.
    // The log setup below archives + truncates (freopen "w") the shared
    // talq_debug.log. A *second* launch (e.g. clicking the taskbar icon while
    // TalQ is already running) used to do that too and only THEN discover it
    // should exit — corrupting the PRIMARY's in-progress log: the primary keeps
    // writing at its old file offset, so the truncated file is re-extended with
    // a multi-MB NUL gap that destroys an active call's diagnostics. So decide
    // primary-vs-secondary HERE; only the primary owns (archives/truncates) the
    // log file. The secondary still signals the primary to surface its window,
    // but later — after QApplication exists (QLocalSocket needs it) — at the
    // guard below. The lock must outlive the whole run, so it is declared in
    // main()'s scope.
    QSharedMemory singleInstance("TalQ_SingleInstance_Lock");
    singleInstance.attach();
    singleInstance.detach();
    const bool isPrimary = singleInstance.create(1);

    QString logPath;
    {
        qputenv("QT_FORCE_STDERR_LOGGING", "1");
        // Heavy GStreamer tracing is the floody one (per-element categories at
        // level 4-6) — keep it OPT-IN via `--debug` / QT_DEBUG, even when
        // detailedLogging is on. Detailed-logging-on alone gives full app
        // qDebug + GST_DEBUG=2 (info), which is enough to diagnose negotiation
        // failures / screen-share start issues without a log-size explosion.
        const bool heavyGst = debugRequested;
#ifdef QT_DEBUG
        const bool heavyGstQtDebug = true;
#else
        const bool heavyGstQtDebug = false;
#endif
        if (heavyGst || heavyGstQtDebug) {
            qputenv("GST_DEBUG",
                    "2,mfvideosrc:6,ksvideosrc:6,videoconvert:5,"
                    "capsfilter:5,GST_CAPS:5,GST_PADS:4,webrtcsrc:5,webrtcbin:4");
        } else if (verbose) {
            // Detailed (default): info-level GST + app qDebug, lean enough to
            // run continuously yet rich enough to diagnose a screen-share or
            // call failure from the log alone.
            qputenv("GST_DEBUG", "2");
        } else {
            // User opted out (Settings → Diagnostics → off): ERR/WARN only.
            qputenv("GST_DEBUG", "1");
        }
        logPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/talq_debug.log";
        QDir().mkpath(QFileInfo(logPath).absolutePath());
        // Publish the resolved path so Settings can offer "Save a copy" / "Open
        // log folder" without recomputing it (this runs before the app/org name
        // is set, so the path differs from a naive AppDataLocation lookup).
        TalqLog::g_logPath = logPath;
        // Preserve previous sessions' logs before this one truncates the live
        // file. A crash usually takes the app down without a clean diagnosis,
        // and the old "w" truncate destroyed the crashed session's log on the
        // next launch (e.g. the callee hang-up freeze). A single ".prev" wasn't
        // enough — a SECOND restart would purge it. So ARCHIVE each previous
        // session under a timestamped name (never overwritten by later
        // restarts) and keep the most recent N. The live log stays at the fixed
        // talq_debug.log name (the crash backstop + in-app viewer read it).
        // PRIMARY-only: a secondary instance must never archive/truncate the
        // primary's live log (see the single-instance note above).
        if (isPrimary) {
            const QString logDir = QFileInfo(logPath).absolutePath();
            if (QFile::exists(logPath)) {
                QDateTime mtime = QFileInfo(logPath).lastModified();
                if (!mtime.isValid()) mtime = QDateTime::currentDateTime();
                const QString archived = logDir + "/talq_debug_"
                    + mtime.toString("yyyyMMdd_HHmmss") + ".log";
                QFile::remove(archived);          // same-second restart → overwrite
                QFile::rename(logPath, archived);
            }
            // Prune oldest archives, keep the most recent N sessions. Names are
            // timestamped so QDir::Name sort is chronological (oldest first).
            constexpr int kKeepSessions = 12;
            QDir d(logDir);
            const QStringList olds = d.entryList(
                QStringList() << QStringLiteral("talq_debug_*.log"),
                QDir::Files, QDir::Name);
            for (int i = 0; i < olds.size() - kKeepSessions; ++i)
                QFile::remove(d.absoluteFilePath(olds.at(i)));
        }
        // Redirect C stderr so GStreamer / any fprintf output is captured
        // (Release builds have no attached console). If freopen fails, stderr
        // is now closed per POSIX — skip installing our handler so subsequent
        // writes don't go to a dead FD, and let Qt's default handler remain.
        // A secondary instance must NOT open (truncate) the shared log file.
        FILE *fp = isPrimary ? freopen(logPath.toUtf8().constData(), "w", stderr)
                             : nullptr;
        if (fp) {
            qInstallMessageHandler([](QtMsgType t, const QMessageLogContext &, const QString &msg) {
                // Non-verbose: drop qDebug() noise, keep info/warn/critical
                // so the always-on log stays lean but still diagnostic.
                if (!TalqLog::g_verbose && t == QtDebugMsg) return;
                QByteArray line = (QTime::currentTime().toString("HH:mm:ss.zzz") + " " + msg + "\n").toUtf8();
                fwrite(line.constData(), 1, line.size(), stderr);
                fflush(stderr);
                // A warning/error is often the last thing logged before a hard
                // freeze (e.g. a camera/encoder fault on the weak-iGPU box). flush
                // alone only reaches the OS cache, which a power-reset loses — so
                // force these lines to physical disk so the freeze precursor
                // actually survives. (Periodic 2s sync handles the steady state.)
                if (t == QtFatalMsg)
                    TalqLog::syncToDisk();           // last line before abort — always
                else if (t == QtWarningMsg || t == QtCriticalMsg)
                    TalqLog::syncToDiskThrottled();  // rate-limited against a warning burst
            });
            // Disk logging is now live — allow syncToDisk()/syncToDiskThrottled()
            // to actually _commit (they no-op until this is set, so a logless
            // primary / secondary never flushes a closed handle).
            TalqLog::g_diskLogActive = true;
#ifdef Q_OS_WIN
            // Crash backstop — ALWAYS armed (release included): a native
            // fault / abort / Rust panic now leaves a minidump next to the
            // log instead of TalQ vanishing without a trace.
            {
                QString dump = logPath;
                dump.replace(QStringLiteral(".log"), QString());
                dump += QStringLiteral(".crash.dmp");
                const std::wstring w = dump.toStdWString();
                wcsncpy(g_crashDumpPath, w.c_str(), MAX_PATH - 1);
                SetUnhandledExceptionFilter(talqCrashFilter);
                // abort()/SIGABRT and unhandled C++ exceptions bypass the
                // SEH filter. A Rust panic in gstrswebrtc or a GLib
                // assertion takes this path — capture it too.
                signal(SIGABRT, talqAbortHandler);
                std::set_terminate(talqTerminateHandler);
            }
#endif
            qInfo().noquote() << "[TalQ]"
                              << (verbose ? (debugRequested ? "verbose (heavy GST trace)" : "detailed")
                                          : "minimal (Settings opted out)")
                              << "logging enabled; log at" << logPath;
        } else if (isPrimary) {
            // PRIMARY-only: fp==nullptr here means freopen genuinely FAILED.
            // (A secondary instance has fp==nullptr by design — it skipped
            // freopen — so it must NOT take this error path / show the modal.)
            const int savedErrno = errno;
#ifdef Q_OS_WIN
            // Only nag with a modal if the user explicitly asked for
            // --debug; a normal run continues silently without a log.
            if (debugRequested) {
                const QString msg = QStringLiteral(
                    "TalQ couldn't open the log at:\n\n%1\n\nerrno=%2.\n\n"
                    "This usually means AppData is on an unreachable network "
                    "share, or antivirus/policy is blocking writes. The app "
                    "will continue without a log file.").arg(logPath).arg(savedErrno);
                MessageBoxW(nullptr,
                    reinterpret_cast<const wchar_t*>(msg.utf16()),
                    L"TalQ — diagnostic log setup failed",
                    MB_OK | MB_ICONWARNING);
            }
#endif
            logPath.clear();
        }
    }

    gst_init(&argc, &argv);

    QApplication app(argc, argv);

    // ── Hard dependency gate ────────────────────────────────────────────
    // TalQ links GStreamer directly and webrtcsrc (Rust) will __fastfail-
    // abort the ENTIRE process — uncatchable by SEH/SIGABRT/std::terminate
    // — the instant it cannot find an element it needs (this is exactly
    // how 0.29.5 killed the called party: `decodebin3` was not deployed).
    // So a missing media plugin must be caught HERE, before any call is
    // possible, and TalQ must refuse to run with a clear message rather
    // than appear to work and then vanish mid-call.
    {
        auto have = [](const char *f) {
            GstElementFactory *e = gst_element_factory_find(f);
            if (e) { gst_object_unref(e); return true; }
            return false;
        };
        // Every one of these must exist (one representative element per
        // plugin TalQ's publish/subscribe/audio pipelines depend on).
        const char *required[] = {
            "tee","queue","capsfilter","valve","fakesink","funnel",   // coreelements
            "appsrc","appsink",                                       // app
            "decodebin3","uridecodebin3",                             // playback (THE 0.29.5 killer)
            "webrtcbin",                                              // webrtc (publish)
            "webrtcsrc",                                              // rswebrtc (subscribe)
            "rtpbin","rtpjitterbuffer",                               // rtpmanager
            "rtph264pay","rtph264depay","rtpopuspay","rtpopusdepay",  // rtp
            "dtlssrtpenc","dtlssrtpdec",                              // dtls
            "nicesrc","nicesink",                                     // nice
            "srtpenc","srtpdec",                                      // srtp
            "opusenc","opusdec","opusparse",                          // opus + audioparsers
            "audioconvert","audioresample","videoconvert","videorate", // conv/resample/CFR
            "h264parse","jpegdec","level","webrtcdsp",                // parsers/jpeg/meter/aec
        };
        // Hardware-optional capabilities: at least one provider must exist.
        struct Group { const char *label; QStringList alts; };
        const Group groups[] = {
            { "camera capture",       {"mfvideosrc","ksvideosrc"} },
            { "microphone capture",   {"wasapi2src","wasapisrc"} },
            { "speaker output",       {"wasapi2sink","wasapisink","autoaudiosink"} },
            { "H.264 encoder",        {"nvh264enc","qsvh264enc","mfh264enc","x264enc","openh264enc"} },
            { "H.264 decoder",        {"nvh264dec","qsvh264dec","d3d11h264dec","openh264dec"} },
            { "VP8/VP9 decoder",      {"vp8dec","vp9dec"} },
        };
        QStringList missing;
        for (const char *f : required)
            if (!have(f)) missing << QString::fromLatin1(f);
        for (const Group &g : groups) {
            bool any = false;
            for (const QString &a : g.alts) if (have(a.toLatin1().constData())) { any = true; break; }
            if (!any) missing << (QString::fromLatin1(g.label) + " (need one of: " + g.alts.join(", ") + ")");
        }
        if (!missing.isEmpty()) {
            const QString body = QStringLiteral(
                "TalQ cannot start: required media components are missing "
                "from this installation.\n\nMissing:\n  • %1\n\n"
                "This means the installer or update did not deliver the "
                "complete GStreamer plugin set. Reinstall TalQ using the "
                "full installer. (Calls would otherwise crash the app and "
                "the person you are calling.)")
                .arg(missing.join("\n  • "));
            qCritical().noquote() << "[TalQ] DEPENDENCY GATE FAILED — missing:" << missing.join(", ");
#ifdef Q_OS_WIN
            MessageBoxW(nullptr,
                reinterpret_cast<const wchar_t*>(body.utf16()),
                L"TalQ — missing media components",
                MB_OK | MB_ICONERROR);
#else
            QMessageBox::critical(nullptr, "TalQ — missing media components", body);
#endif
            return 2;
        }
        qInfo() << "[TalQ] dependency gate passed —"
                << (int(sizeof(required)/sizeof(required[0])) + int(sizeof(groups)/sizeof(groups[0])))
                << "media checks OK";
    }

    // Single-instance guard (detection ran earlier, before any log setup, so a
    // secondary never archives/truncates the primary's live log).
    if (!isPrimary) {
        // Another instance holds the lock. Ask it to surface its window —
        // this is the path hit when the user clicks the pinned taskbar
        // icon while TalQ is hidden in the tray (Windows launches a fresh
        // process instead of activating the hidden one). Without this the
        // running instance stays hidden and only the tray icon can restore it.
        qWarning() << "TalQ is already running — signaling it to show.";
        QLocalSocket sock;
        sock.connectToServer(QStringLiteral("TalQ_SingleInstance_Pipe"));
        if (sock.waitForConnected(800)) {
            sock.write("SHOW");
            sock.flush();
            sock.waitForBytesWritten(800);
            sock.disconnectFromServer();
        }
        return 0;
    }
#ifdef TALQ_BRAND_123NET
    app.setApplicationName("123NET TalQ");
    app.setOrganizationName("123NET");
#else
    app.setApplicationName("TalQ");
    app.setOrganizationName("TalQ");
#endif
    app.setWindowIcon(QIcon(":/logo.png"));

    // 0.48.0 -- one-time: force auto-install of updates ON for ALL existing
    // users (including anyone who had explicitly disabled it), so the whole
    // base lands on current builds. `updates/autoInstall` already defaults to
    // true, so this only flips the explicit-false users. Guarded by a
    // version-stamped flag so it runs EXACTLY once; users may still turn it
    // off again afterwards (we do not re-force it on every launch).
    {
        QSettings s;
        if (!s.value(QStringLiteral("updates/forcedAutoInstall_v0480"), false).toBool()) {
            s.setValue(QStringLiteral("updates/autoInstall"), true);
            s.setValue(QStringLiteral("updates/forcedAutoInstall_v0480"), true);
        }
    }

    // EmojiData reads/writes recents via QSettings — must run after
    // setApplicationName/setOrganizationName so the right storage is used.
    EmojiData::initialize();

    // Probe AppData writability. Corporate Windows profiles sometimes
    // redirect Roaming to a network share that's unreachable (or read-only
    // from sandboxed apps) — silently failing to write the message cache
    // would make the app feel broken. Surface it once at startup.
    {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dir);
        QFile probe(dir + QStringLiteral("/.talq-writeprobe"));
        if (!probe.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(nullptr, QObject::tr("TalQ — storage warning"),
                QObject::tr("TalQ couldn't write to its data folder:\n\n%1\n\n"
                            "Message cache and local state won't persist across restarts. "
                            "This usually means your Windows profile is on a network share "
                            "that's currently unreachable, or group policy is blocking the app. "
                            "Contact your IT admin if this keeps happening.").arg(dir));
        } else {
            probe.close();
            probe.remove();
        }
    }

    // Bundled body font — Inter (SIL OFL). Registered once for the whole
    // app so every widget inherits a consistent, screen-optimised type.
    {
        int id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
        if (id >= 0) {
            const auto families = QFontDatabase::applicationFontFamilies(id);
            if (!families.isEmpty()) {
                QFont inter(families.first());
                inter.setPixelSize(13);
                inter.setHintingPreference(QFont::PreferFullHinting);
                QApplication::setFont(inter);
            }
        }
    }

#ifdef TALQ_BUILD_TS
    app.setApplicationVersion(TALQ_VERSION "-" TALQ_BUILD_TS);
#else
    app.setApplicationVersion(TALQ_VERSION);
#endif

    // Splash screen — dark themed, centered logo, smooth rendering
    {
#ifdef TALQ_BRAND_123NET
        QPixmap logoPix(":/123net-logo.png");
#else
        QPixmap logoPix(":/logo.png");
#endif
        // Create a dark splash canvas
        QPixmap splashCanvas(380, 280);
        splashCanvas.fill(QColor("#121210"));
        QPainter p(&splashCanvas);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setRenderHint(QPainter::Antialiasing);

        // Draw logo centered
        QPixmap logo = logoPix.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        p.drawPixmap((380 - logo.width()) / 2, 60, logo);

        // App name
        QFont nameFont;
        nameFont.setPixelSize(24);
        nameFont.setWeight(QFont::DemiBold);
        p.setFont(nameFont);
        p.setPen(QColor("#e4e0da"));
        p.drawText(QRect(0, 170, 380, 30), Qt::AlignCenter, app.applicationName());

        // Version
        QFont verFont;
        verFont.setPixelSize(12);
        p.setFont(verFont);
        p.setPen(QColor("#8a8680"));
        p.drawText(QRect(0, 200, 380, 20), Qt::AlignCenter, "v" + app.applicationVersion());

        // Connecting status
        p.drawText(QRect(0, 240, 380, 20), Qt::AlignCenter, "Connecting...");
        p.end();

        static QSplashScreen splash(splashCanvas);
        splash.show();
        app.processEvents();

        // Splash stays at least 1.5s for branding visibility
        QTimer::singleShot(1500, &splash, [&splash]() {
            splash.close();
        });
    }

    // Core services
    ApiClient api;
    AuthManager auth(&api);
    ConversationListModel conversations(&api);
    MessageCache cache;
    MessageListModel messages(&api, &cache);
    ThreadListModel threads(&api);
    threads.setCache(&cache);
    threads.setConversations(&conversations);  // pull the room read marker for per-topic unread counts
    NotificationManager notifications;
    PushClient push(&api);
    SignalingClient signaling(&api);
    MediaDeviceManager deviceManager;
    CallManager callManager(&api, &signaling, &deviceManager);
    // 0.41.5-beta — let CallManager look up the room type when picking
    // P2P vs MCU for a call (forces P2P for 1:1 even with HPB present).
    callManager.setConversations(&conversations);

    UserStatusManager userStatus(&api, &auth);
    DebugMonitor debug;
    AppSettings appSettings;

    MainWindow window(
        &api, &auth, &conversations, &messages, &threads,
        &notifications, &push, &signaling, &deviceManager,
        &callManager, &userStatus, &debug, &appSettings
    );

    // Single-instance IPC: a second launch (notably clicking the pinned
    // taskbar icon while we're hidden in the tray) connects here; we then
    // surface via openConversation("") which un-hides, restores the prior
    // fullscreen/maximized state, raises, and does the Windows
    // SetForegroundWindow focus-steal bypass.
    QLocalServer::removeServer(QStringLiteral("TalQ_SingleInstance_Pipe"));
    QLocalServer ipcServer;
    if (!ipcServer.listen(QStringLiteral("TalQ_SingleInstance_Pipe")))
        qWarning() << "TalQ: single-instance IPC listen failed:"
                   << ipcServer.errorString();
    QObject::connect(&ipcServer, &QLocalServer::newConnection, &window, [&ipcServer, &window]() {
        while (QLocalSocket *s = ipcServer.nextPendingConnection()) {
            QObject::connect(s, &QLocalSocket::disconnected, s, &QLocalSocket::deleteLater);
            s->close();
        }
        window.openConversation(QString());
    });

    // Custom notification popup
    // Notification stack — multiple toasts stack at the bottom-right,
    // rapid repeats from the same conversation coalesce, oldest ages out
    // when a 5th arrives.
    NotificationStack notifStack;
    QObject::connect(&notifications, &NotificationManager::desktopPopupRequested,
                     &notifStack, &NotificationStack::notify);
    QObject::connect(&notifStack, &NotificationStack::clicked,
                     &window, [&window](const QString &token) {
        window.openConversation(token);
    });

    // Update conversation list preview when new messages arrive
    QObject::connect(&messages, &MessageListModel::newMessagesAtEnd, &conversations, [&messages, &conversations]() {
        int count = messages.rowCount();
        if (count == 0) return;
        auto idx = messages.index(0);
        QString author = messages.data(idx, MessageListModel::ActorNameRole).toString();
        QString text = messages.data(idx, MessageListModel::MessageTextRole).toString();
        text.remove(QRegularExpression("<[^>]*>"));
        if (text.length() > 80) text = text.left(80) + "...";
        conversations.updateLastMessage(messages.conversationToken(), author, text);
    });

    // Notify on new polled messages in active conversation
    QObject::connect(&messages, &MessageListModel::newMessagesAtEnd, &notifications, [&messages, &notifications, &auth]() {
        int count = messages.rowCount();
        if (count == 0) return;
        auto idx = messages.index(0);  // model is newest-first, index 0 = latest message
        QString actorId = messages.data(idx, MessageListModel::ActorIdRole).toString();
        if (actorId == auth.userId()) return;
        // Honor the sender's "Send silently" choice — same wire flag the
        // web client checks. Without this, a scheduled-silent or right-click
        // silent message would still pop a desktop toast on the recipient.
        if (messages.data(idx, MessageListModel::SilentRole).toBool()) return;
        QString actorName = messages.data(idx, MessageListModel::ActorNameRole).toString();
        QString text = messages.data(idx, MessageListModel::MessageTextRole).toString();
        text.remove(QRegularExpression("<[^>]*>"));
        if (text.length() > 100) text = text.left(100) + "...";
        notifications.notify(actorName, text, false, messages.conversationToken());
    });

    // Notify on new messages in OTHER conversations
    QObject::connect(&conversations, &ConversationListModel::newUnreadMessage,
                     &notifications, [&notifications, &messages](const QString &name, const QString &lastMsg, const QString &token) {
        if (token == messages.conversationToken()) return;
        QString preview = lastMsg;
        preview.remove(QRegularExpression("<[^>]*>"));
        if (preview.length() > 80) preview = preview.left(80) + "...";
        notifications.notify(name, preview, true, token);
    });

    // Incoming call detection
    QObject::connect(&conversations, &ConversationListModel::incomingCallDetected,
                     &callManager, [&callManager](const QString &name, const QString &token, int callFlag) {
        callManager.onIncomingCallDetected(name, token, callFlag);
    });

    // "Call ended" desktop notification. callEnded fires once per terminal
    // teardown. Skip the user's OWN hang-up (they just clicked it); notify when
    // the call ends for another reason — peer left, connection lost after the
    // reconnect retries gave nothing, declined, no answer — so the call window
    // vanishing is never silent/confusing. alwaysSound=true shows it even if the
    // call window had focus; empty token keeps it a non-interactive toast.
    QObject::connect(&callManager, &CallManager::callEnded, &notifications,
                     [&notifications](const QString &reason) {
        const QString r = reason.toLower();
        if (r.contains("hung up") || r.contains("cancel")) return;
        QString msg;
        if (r.contains("declined"))        msg = QObject::tr("Call declined.");
        else if (r.contains("no answer"))  msg = QObject::tr("No answer.");
        else if (r.contains("failed") || r.contains("ice")
                 || r.contains("pipeline") || r.contains("start"))
            msg = QObject::tr("Connection lost.");
        else                               msg = QObject::tr("The call ended.");
        notifications.notify(QObject::tr("Call ended"), msg, /*alwaysSound=*/true, QString());
    });

    // Screen-share reliability — surface the auto-retry + the give-up to the
    // user instead of a silent dead share (Ilko field report: "I clicked share
    // and nothing happened for ~30 s"). screenShareRetrying fires per bounded
    // auto-retry (so the user sees TalQ is working on it, not frozen);
    // screenShareFailed fires once after the retry budget is exhausted, with a
    // ready-to-show reason. Both are non-interactive toasts (empty token).
    QObject::connect(&callManager, &CallManager::screenShareRetrying, &notifications,
                     [&notifications]() {
        notifications.notify(QObject::tr("Screen sharing"),
                             QObject::tr("Still starting your screen share…"),
                             /*alwaysSound=*/false, QString());
    });
    QObject::connect(&callManager, &CallManager::screenShareFailed, &notifications,
                     [&notifications](const QString &reason) {
        notifications.notify(QObject::tr("Screen sharing"), reason,
                             /*alwaysSound=*/true, QString());
    });

    // A call the SERVER rejected (e.g. HTTP 5xx on POST call/{token}) must never
    // be a silent drop. CallManager intercepts it and emits callFailed with a
    // plain-language title + message; show it as a prominent, must-dismiss
    // dialog so a non-technical user knows exactly what happened and what to do.
    QObject::connect(&callManager, &CallManager::callFailed, &window,
                     [&window](const QString &title, const QString &message) {
        QMessageBox::warning(&window, title, message);
    });

    // Tray unread count
    QObject::connect(&conversations, &ConversationListModel::totalUnreadChanged,
                     &notifications, [&conversations, &notifications]() {
        notifications.updateUnreadCount(conversations.totalUnread());
    });

    // bug 9 — a topic (thread) created by another member must appear LIVE in
    // the open room's TopicTabBar, not only after a manual room switch. The
    // thread list (ThreadListModel) was refreshed nowhere on incoming
    // activity. Refresh it on the same push/signaling signals as messages,
    // scoped to the OPEN room and rate-limited so a busy room doesn't spam
    // GET /chat/{token}. ThreadListModel persists its selected chip across the
    // model reset, so a live refresh won't drop the active topic.
    auto refreshOpenRoomThreads = [&threads](const QString &roomToken) {
        if (threads.conversationToken().isEmpty()) return;                       // no room open
        if (!roomToken.isEmpty() && threads.conversationToken() != roomToken)    // not the open room
            return;
        static qint64 lastThreadRefreshMs = 0;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastThreadRefreshMs < 4000) return;                           // rate-limit
        lastThreadRefreshMs = now;
        threads.refresh();
    };

    // Push events -> refresh
    QObject::connect(&push, &PushClient::pushReceived, &conversations, [&conversations, &messages, refreshOpenRoomThreads](const QString &type) {
        qDebug() << "Push event received:" << type << "-- refreshing conversations + messages";
        conversations.refresh();
        messages.refresh();  // instant read status + new message pickup
        refreshOpenRoomThreads(QString());  // bug 9: open room's topic list (push carries no room token)
    });

    // HPB signaling chat events -> refresh. This is the channel the official
    // NC Talk client uses for instant chat updates (including read receipts).
    // On servers where notify_push is silent for chat (no events fire on the
    // /push/ws WebSocket), this is the only mechanism that delivers read-
    // marker advances without a follow-up message from the other party.
    QObject::connect(&signaling, &SignalingClient::chatRefreshNeeded,
                     &messages, [&messages, &conversations, refreshOpenRoomThreads](const QString &roomToken) {
        conversations.refresh();
        // Only refresh the open chat if the event is for that room — refreshing
        // a different room would just fetch+discard 50 messages of someone
        // else's conversation.
        if (messages.conversationToken() == roomToken) {
            messages.refresh();
            refreshOpenRoomThreads(roomToken);  // bug 9: open room's topic list
        }
    });

    // Start push + signaling after login (both fresh login AND session restore)
    auto startServices = [&auth, &conversations, &push, &signaling]() {
        if (auth.isLoggedIn()) {
            push.start();
            signaling.start();
            conversations.startAutoRefresh();
        } else {
            push.stop();
            signaling.stop();
            conversations.stopAutoRefresh();
        }
    };
    QObject::connect(&auth, &AuthManager::loggedInChanged, &conversations, startServices);
    QObject::connect(&auth, &AuthManager::restoringChanged, &conversations, [&auth, startServices]() {
        if (!auth.isRestoringSession() && auth.isLoggedIn())
            startServices();
    });

    // User status: UserStatusManager owns presence + heartbeat and, on
    // login, reverts a crash-stuck Talk 'call' status. Wired on both fresh
    // login and session restore (mirrors the push/signaling start path).
    QObject::connect(&auth, &AuthManager::loggedInChanged, &userStatus, [&auth, &userStatus]() {
        if (auth.isLoggedIn()) userStatus.onLoggedIn();
        else                   userStatus.onLoggedOut();
    });
    QObject::connect(&auth, &AuthManager::restoringChanged, &userStatus, [&auth, &userStatus]() {
        if (!auth.isRestoringSession() && auth.isLoggedIn())
            userStatus.onLoggedIn();
    });

    // Free the server-side call participant on every clean exit so a
    // closed/quit/logged-out client never leaves "in a call" lingering.
    QObject::connect(&auth, &AuthManager::loggedInChanged, &callManager, [&auth, &callManager]() {
        if (!auth.isLoggedIn()) callManager.leaveCallBeacon();
    });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &callManager, [&callManager]() {
        callManager.leaveCallBeacon();
    });

    // Restore session
    auth.tryRestore();

    // Debug monitor feed — pull cache stats from the real painters so the
    // [MEM-DETAIL] line in talq_debug.log reflects what's actually growing.
    QObject::connect(&debug, &DebugMonitor::updated, [&]() {
        debug.setMessageCount(messages.rowCount());
        debug.setConversationCount(conversations.rowCount());
        debug.setPendingRequests(api.pendingCount());
        debug.setEmojiCacheStats(EmojiData::pixmapCacheCount(),
                                 EmojiData::pixmapCacheBytes());
        if (auto *cp = window.chatPainter()) {
            debug.setChatAvatarStats(cp->avatarCacheCount(), cp->avatarCacheBytes());
            debug.setPreviewCacheCount(cp->previewCacheCount());
            debug.setPreviewCacheBytes(cp->previewCacheBytes());
            debug.setLayoutCacheStats(cp->layoutCacheCount(), cp->layoutCacheBytes());
            // Aggregate avatar count for the top-row UI shows chat-side cache.
            debug.setAvatarCacheCount(cp->avatarCacheCount());
        }
        if (auto *sb = window.sidebar()) {
            debug.setSidebarAvatarStats(sb->avatarCacheCount(), sb->avatarCacheBytes());
        }
    });

    return app.exec();
}
