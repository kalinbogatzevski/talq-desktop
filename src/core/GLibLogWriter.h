#pragma once

#include <glib.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#include <ctime>
#include <unistd.h>
#endif

#include "LogRedaction.h"
#include "LogSink.h"

// TalqGLibLog -- TalQ's GLib log writer, installed first thing in main().
//
// Why it exists (2026-09-16 call crash, RCA D1): with no writer installed,
// every g_warning/g_critical from libnice, GLib or GObject goes through
// g_log_writer_default -> g_log_writer_standard_streams ->
// g_log_writer_supports_color, and on Win32 that pushes and pops the CRT
// invalid-parameter handler for each message. The MSYS2 mingw64 GLib has no
// thread-local handler, so two threads logging at once interleave one global
// slot; g_return_if_fail in g_win32_pop_invalid_parameter_handler then logs
// inside the outer g_log, GLib treats that recursion as fatal, shows a modal
// MessageBoxW on the logging thread and aborts. A burst of libnice "cannot
// have more than 8 turn servers" warnings killed a call that way. Reproduced
// with GLib's default writer in tests/glib_log_race_test.cpp (fatal within
// 0.2 s, even with 2 threads); none of this writer's calls touch the handler.
//
// Routing (GLib 2.86.3 gmessages.c): g_log/g_warning -> g_logv ->
// g_log_default_handler -> g_log_structured_array -> this writer, and
// g_log_structured goes straight to g_log_structured_array. TalQ and its
// deployed libraries install no g_log_set_handler, so both paths arrive here.
// GLib calls its own fallback writer only for recursion, which this writer
// never causes because it never logs.
//
// Fatal levels are unchanged: GLib aborts in g_log_structured_array (and in
// g_logv) AFTER the writer returns, so g_error still aborts, with its line
// already written. GLib shows its fatal MessageBox only when its own formatter
// ran for a fatal record; this writer does not use that formatter, so the
// process goes straight to abort() and main.cpp's SIGABRT handler instead of
// blocking a streaming thread on a dialog.
//
// Output keeps GLib's non-colour layout (without the blank line GLib puts
// before a warning) so existing greps still match:
//   (talq.exe:14632): libnice-WARNING **: 11:37:11.406: Agent ... : cannot ...
// Debug and info records are filtered exactly as GLib's default writer does
// (G_MESSAGES_DEBUG / DEBUG_INVOCATION, via g_log_writer_default_would_drop).

namespace TalqGLibLog {

namespace detail {

inline const char *levelName(GLogLevelFlags level)
{
    switch (level & G_LOG_LEVEL_MASK) {
    case G_LOG_LEVEL_ERROR:    return "ERROR";
    case G_LOG_LEVEL_CRITICAL: return "CRITICAL";
    case G_LOG_LEVEL_WARNING:  return "WARNING";
    case G_LOG_LEVEL_MESSAGE:  return "Message";
    case G_LOG_LEVEL_INFO:     return "INFO";
    case G_LOG_LEVEL_DEBUG:    return "DEBUG";
    default:                   return nullptr;
    }
}

inline void appendClock(std::string &out)
{
    char buf[16];
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::snprintf(buf, sizeof buf, "%02u:%02u:%02u.%03u",
                  unsigned(st.wHour), unsigned(st.wMinute), unsigned(st.wSecond), unsigned(st.wMilliseconds));
#else
    timeval tv;
    gettimeofday(&tv, nullptr);
    tm t;
    localtime_r(&tv.tv_sec, &t);
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", t.tm_hour, t.tm_min, t.tm_sec, int(tv.tv_usec / 1000));
#endif
    out += buf;
}

inline unsigned long processId()
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// Control characters are escaped as GLib's formatter does, so a stray byte
// cannot forge a new log line.
inline void appendEscaped(std::string &out, const char *p, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0x7f) {
            char e[8];
            std::snprintf(e, sizeof e, "\\x%02x", unsigned(c));
            out += e;
        } else {
            out += static_cast<char>(c);
        }
    }
}

inline std::size_t fieldLength(const GLogField &f)
{
    return f.length < 0 ? std::strlen(static_cast<const char *>(f.value)) : std::size_t(f.length);
}

} // namespace detail

// One record as one line, in GLib's non-colour layout. Uses no GLib call that
// can log or touch the invalid-parameter handler (g_get_prgname is an atomic
// pointer read).
inline std::string formatRecord(GLogLevelFlags level, const GLogField *fields, gsize n_fields)
{
    const GLogField *message = nullptr;
    const GLogField *domain = nullptr;
    for (gsize i = 0; i < n_fields; ++i) {
        if (!fields[i].key || !fields[i].value) continue;
        if (!message && std::strcmp(fields[i].key, "MESSAGE") == 0) message = &fields[i];
        else if (!domain && std::strcmp(fields[i].key, "GLIB_DOMAIN") == 0) domain = &fields[i];
    }

    std::string line;
    line.reserve(64 + (message ? detail::fieldLength(*message) : 16));
    if (!domain) line += "** ";
    const char *prg = g_get_prgname();
    line += '(';
    line += prg ? prg : "process";
    line += ':';
    line += std::to_string(detail::processId());
    line += "): ";
    if (domain) {
        line.append(static_cast<const char *>(domain->value), detail::fieldLength(*domain));
        line += '-';
    }
    if (const char *name = detail::levelName(level)) {
        line += name;
    } else {
        char custom[24];
        std::snprintf(custom, sizeof custom, "LOG-%x", unsigned(level & G_LOG_LEVEL_MASK));
        line += custom;
    }
    if (level & G_LOG_FLAG_RECURSION) line += " (recursed)";
    if (level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING)) line += " **";
    line += ": ";
    detail::appendClock(line);
    line += ": ";
    if (message)
        detail::appendEscaped(line, static_cast<const char *>(message->value), detail::fieldLength(*message));
    else
        line += "(NULL) message";
    line += '\n';
    return line;
}

inline GLogWriterOutput writer(GLogLevelFlags level, const GLogField *fields, gsize n_fields,
                               gpointer /*user_data*/) noexcept
{
    // No g_return_val_if_fail here: it would log from inside the writer.
    if (!fields || n_fields == 0) return G_LOG_WRITER_UNHANDLED;

    const char *domain = nullptr;
    for (gsize i = 0; i < n_fields; ++i) {
        if (fields[i].key && fields[i].length < 0 && std::strcmp(fields[i].key, "GLIB_DOMAIN") == 0) {
            domain = static_cast<const char *>(fields[i].value);
            break;
        }
    }
    if (g_log_writer_default_would_drop(level, domain)) return G_LOG_WRITER_HANDLED;

    try {
        const std::string safe = LogRedaction::redact(formatRecord(level, fields, n_fields));
        TalqLogSink::writeLine(safe.data(), safe.size());
    } catch (...) {
        // Out of memory while formatting: drop the record rather than let an
        // exception unwind through GLib's C frames.
    }
    return G_LOG_WRITER_HANDLED;
}

// g_printerr() is not the log API and never reaches the writer; libraries use
// it for direct diagnostics. Route it through the same mask and sink so it can
// neither leak a credential nor cut a line in half.
inline void printerrHandler(const gchar *text)
{
    if (!text) return;
    try {
        const std::string safe = LogRedaction::redact(text);
        TalqLogSink::writeLine(safe.data(), safe.size());
    } catch (...) {
    }
}

// Installs the writer (and the g_printerr handler). GLib allows exactly one
// g_log_set_writer_func per process (a second call is a g_error), so call this
// once, before gst_init or anything else that can start a GLib thread.
inline void install()
{
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) return;
    g_log_set_writer_func(writer, nullptr, nullptr);
    g_set_printerr_handler(printerrHandler);
}

} // namespace TalqGLibLog
