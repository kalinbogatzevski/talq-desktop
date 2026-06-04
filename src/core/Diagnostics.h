#pragma once

#include <QString>

// One-click diagnostics for field bug reports. collectSystemDiagnostics()
// returns a human-readable plain-text block covering the OS, displays + GPU
// adapters (the #1 thing behind screen-share failures), the deployed GStreamer
// element set + WGC availability, the input/output devices, and the call-
// relevant settings. The Settings "Collect diagnostics" button prepends an
// [App] header (version/codename/channel/brand — which carry the compile
// defines only present in SettingsDialog/MainWindow), appends the recent
// talq_debug.log, and writes the whole thing to a .txt the user can send.
namespace talq {

// System / displays / GPU / GStreamer / WGC / devices / call settings.
// No app-version compile defines needed here (the caller adds the [App] header).
QString collectSystemDiagnostics();

// Last `maxBytes` of the live talq_debug.log (TalqLog::g_logPath), or a short
// note if no log is available. Bounded so a multi-MB log doesn't bloat the file.
QString tailDebugLog(qint64 maxBytes);

} // namespace talq
