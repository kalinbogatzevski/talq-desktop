#pragma once

#include <QDebug>
#include <QString>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <thread>
#ifdef Q_OS_WIN
#include <io.h>
#endif

// TalQ logging macros. Gated by a runtime flag:
//   - Debug builds: verbose flag defaults to true.
//   - Release builds: defaults to false, turned on by passing --debug on
//     the command line (see main.cpp).
//
// TWARN/TERR always print regardless — captured by the message handler
// installed in main.cpp.
//
// Usage:
//   TLOG("message")                    — general debug log
//   TLOG_CALL("ICE state:" << state)   — call/WebRTC debug
//   TLOG_SIG("event:" << type)         — signaling debug
//   TLOG_NET("request:" << path)       — network debug
//   TLOG_UI("click:" << widget)        — UI debug
//   TWARN("problem:" << err)           — always prints
//   TERR("fatal:" << msg)              — always prints

namespace TalqLog {
    inline bool g_verbose =
#ifdef QT_DEBUG
        true;
#else
        false;
#endif

    // The ACTUAL resolved path of the live talq_debug.log, set once by main.cpp
    // after it computes the path. Settings reads this to offer "Save a copy" /
    // "Open log folder" — do NOT recompute AppDataLocation in the UI: main.cpp
    // resolves the path BEFORE setApplicationName/OrganizationName run, so the
    // file lives at .../Roaming/talq_debug.log (no TalQ/TalQ subfolder); a naive
    // recompute would point at the wrong place.
    inline QString g_logPath;

    // True only once main.cpp's freopen(stderr→file) + message handler succeeded.
    // Gates syncToDisk() so a logless primary (freopen failed) or a secondary
    // instance never issues a futile _commit on a closed/invalid stderr handle.
    inline bool g_diskLogActive = false;

    // Force the log all the way to PHYSICAL DISK. The message handler's
    // fflush(stderr) only pushes the C stdio buffer into the OS page cache; a
    // HARD machine freeze + power-reset loses that cache, so the on-disk log is
    // missing the final seconds before the freeze — which is exactly why every
    // freeze-session log we've recovered ends startup-only or mid-idle, never on
    // the freeze itself. _commit() issues FlushFileBuffers on stderr's handle,
    // forcing the cache to disk. Cheap enough on a ~2s cadence + on errors; must
    // NOT be called per-line at high frequency (a disk sync per log line stalls
    // I/O on the main thread — see syncToDiskThrottled for the warning path).
    inline void syncToDisk() {
        if (!g_diskLogActive) return;
        std::fflush(stderr);
#ifdef Q_OS_WIN
        const int fd = _fileno(stderr);
        if (fd >= 0) _commit(fd);
#endif
    }

    // ── Background disk flusher ────────────────────────────────────────────
    // syncToDisk() calls FlushFileBuffers, which BLOCKS until the volume's write
    // cache is on physical disk — and on a busy disk (e.g. Windows Defender
    // scanning the cold DLLs that a first Settings-open pulls in) that wait is
    // SECONDS. Doing it on the GUI thread (DebugMonitor's 2s tick + the message
    // handler's per-warning throttled sync) froze the UI: the whole
    // g_mainBeat-keeps-ticking-but-the-dialog-heartbeat-stalls mystery was that
    // g_mainBeat does no logging while the heartbeat's qInfo path could reach a
    // sync. FIX: NEVER FlushFileBuffers on a caller thread. A dedicated flusher
    // thread performs every sync; callers only requestSync() (set an atomic).
    inline std::atomic<bool> g_syncRequested{false};
    inline std::atomic<bool> g_flusherStarted{false};

    // Ask the background thread to flush soon — cheap, non-blocking, thread-safe.
    inline void requestSync() { g_syncRequested.store(true, std::memory_order_relaxed); }

    inline void startBackgroundFlusher() {
        if (g_flusherStarted.exchange(true)) return;
        std::thread([]() {
            int sinceForcedMs = 0;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                sinceForcedMs += 400;
                const bool requested =
                    g_syncRequested.exchange(false, std::memory_order_relaxed);
                // Flush on request (≤400ms latency for a warning) OR on the ~2s
                // steady-state cadence — whichever comes first. Coalesced: one
                // FlushFileBuffers per wake covers everything since.
                if (requested || sinceForcedMs >= 2000) {
                    sinceForcedMs = 0;
                    syncToDisk();   // the ONLY place FlushFileBuffers runs steady-state
                }
            }
        }).detach();
    }

    // Back-compat shim: callers that used to synchronously throttle-sync now just
    // hand the work to the background flusher (never blocks the caller thread).
    inline void syncToDiskThrottled() { requestSync(); }
}

#define TLOG(msg)       do { if (TalqLog::g_verbose) qDebug().noquote() << "[TalQ]" << msg; } while (0)
#define TLOG_CALL(msg)  do { if (TalqLog::g_verbose) qDebug().noquote() << "[CALL]" << msg; } while (0)
#define TLOG_SIG(msg)   do { if (TalqLog::g_verbose) qDebug().noquote() << "[SIG]"  << msg; } while (0)
#define TLOG_NET(msg)   do { if (TalqLog::g_verbose) qDebug().noquote() << "[NET]"  << msg; } while (0)
#define TLOG_UI(msg)    do { if (TalqLog::g_verbose) qDebug().noquote() << "[UI]"   << msg; } while (0)

#define TWARN(msg)  qWarning().noquote()  << "[TalQ WARN]" << msg
#define TERR(msg)   qCritical().noquote() << "[TalQ ERR]"  << msg
