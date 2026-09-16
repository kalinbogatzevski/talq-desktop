#pragma once

#include <cstddef>
#include <cstdio>
#include <mutex>

// TalqLogSink -- the one place a finished log line is written. main.cpp
// freopen()s stderr onto talq_debug.log, and three producers end up here: the
// Qt message handler, the GLib log writer (GLibLogWriter.h) and the GStreamer
// log function. One mutex around one fwrite means a line from one producer can
// never be cut by another. The 2026-09-16 log has GStreamer lines torn by
// libnice warnings: GStreamer's default log function writes each line in five
// separate calls on Windows.
//
// Only the write itself runs under the mutex. Callers format and redact
// before calling, so nothing that can log is ever called while it is held.

namespace TalqLogSink {

inline std::mutex g_lineMutex;

inline void writeLine(const char *data, std::size_t len)
{
    if (!data || len == 0) return;
    // A process that never redirected stderr (a GUI-subsystem secondary
    // instance) has no fd behind it; GLib's own writer skips the same case.
#ifdef _WIN32
    if (_fileno(stderr) < 0) return;
#else
    if (fileno(stderr) < 0) return;
#endif
    std::lock_guard<std::mutex> lock(g_lineMutex);
    std::fwrite(data, 1, len, stderr);
    std::fflush(stderr);
}

} // namespace TalqLogSink
