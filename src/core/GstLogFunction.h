#pragma once

#include <gst/gst.h>

#include <string>

#include "LogRedaction.h"
#include "LogSink.h"

// TalqGstLog -- GStreamer debug output, redacted, into the same sink as the Qt
// and GLib logs.
//
// Why (2026-09-16, RCA D11): GStreamer's default log function printed
//   ERROR webrtcnice nice.c:541:_add_turn_server:<webrtcbin15:ice> Could not
//   set TURN server turn://<user>:<credential>@<host>:3478 on libnice
// verbatim into talq_debug.log (TalQ runs GST_DEBUG=2 by default), fifteen
// times in one session, and the log was shared. This replaces the default with
// a function that formats the line with gst_debug_log_get_line -- documented
// as "formatted in the same way as gst_debug_log_default() ... without color"
// -- masks it, and writes it as ONE call under TalqLogSink's mutex. On Windows
// the default wrote each line in five separate g_printerr calls, which is why
// lines in that log were torn by concurrent GLib warnings.
//
// Levels are unchanged: GST_DEBUG still sets the category thresholds in
// gst_init, and GStreamer only calls a log function for records that pass them.

namespace TalqGstLog {

G_GNUC_NO_INSTRUMENT
inline void logFunction(GstDebugCategory *category, GstDebugLevel level, const gchar *file,
                        const gchar *function, gint line, GObject *object,
                        GstDebugMessage *message, gpointer /*user_data*/)
{
    gchar *raw = gst_debug_log_get_line(category, level, file, function, line, object, message);
    if (!raw) return;
    try {
        const std::string safe = LogRedaction::redact(raw);
        TalqLogSink::writeLine(safe.data(), safe.size());
    } catch (...) {
        // Out of memory while masking: drop the line rather than write it raw.
    }
    g_free(raw);
}

// Call BEFORE gst_init(). Removing gst_debug_log_default before init is what
// stops gst_init from adding it at all (gstinfo.c, add_default_log_func).
// A developer who sets GST_DEBUG_FILE keeps GStreamer's own behaviour -- the
// trace goes to that file, not talq_debug.log -- and this returns false.
inline bool install()
{
    const gchar *file = g_getenv("GST_DEBUG_FILE");
    if (file && *file) return false;
    gst_debug_remove_log_function(gst_debug_log_default);
    gst_debug_add_log_function(logFunction, nullptr, nullptr);
    return true;
}

} // namespace TalqGstLog
