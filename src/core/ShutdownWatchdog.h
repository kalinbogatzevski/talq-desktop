// ShutdownWatchdog.h — a last-resort guarantee that a requested quit always
// terminates the process.
//
// Why: teardown of GStreamer / a wedged GPU driver / a blocked worker thread,
// OR a quit() that never propagates because a nested modal event loop is
// running, can leave talq.exe alive after the user asked it to quit. A
// lingering process blocks Quit AND locks the program files so an auto-update
// can't replace them (field 2026-06-18: "I have to kill the process manually" +
// "the upgrade doesn't finish").
//
// Arm this at the moment quit is REQUESTED (tray Quit, auto-update relaunch,
// test hook) — NOT in aboutToQuit, which only fires once quit() has already
// propagated. The detached thread runs independently of the Qt event loop and
// every destructor, so it fires no matter where the shutdown wedges. A normal,
// timely exit terminates the process (and this thread with it) before the grace
// window elapses, so it never trips in the common case.
#pragma once

#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <QtGlobal>
#include "TalqLog.h"

namespace talq {

inline std::atomic<bool> g_shutdownWatchdogArmed{false};

// Idempotent — first caller wins, later calls are no-ops.
inline void armShutdownWatchdog(int graceSeconds = 6)
{
    bool expected = false;
    if (!g_shutdownWatchdogArmed.compare_exchange_strong(expected, true))
        return;
    qWarning() << "[SHUTDOWN] quit requested — force-exit watchdog armed ("
               << graceSeconds << "s grace)";
    TalqLog::syncToDisk();
    std::thread([graceSeconds]() {
        std::this_thread::sleep_for(std::chrono::seconds(graceSeconds));
        qWarning() << "[SHUTDOWN] grace window elapsed — force-exiting "
                      "(shutdown wedged; see the last [SHUTDOWN] line)";
        TalqLog::syncToDisk();
        std::_Exit(0);
    }).detach();
}

} // namespace talq
