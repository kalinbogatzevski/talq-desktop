#pragma once

#include <QDebug>

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
}

#define TLOG(msg)       do { if (TalqLog::g_verbose) qDebug().noquote() << "[TalQ]" << msg; } while (0)
#define TLOG_CALL(msg)  do { if (TalqLog::g_verbose) qDebug().noquote() << "[CALL]" << msg; } while (0)
#define TLOG_SIG(msg)   do { if (TalqLog::g_verbose) qDebug().noquote() << "[SIG]"  << msg; } while (0)
#define TLOG_NET(msg)   do { if (TalqLog::g_verbose) qDebug().noquote() << "[NET]"  << msg; } while (0)
#define TLOG_UI(msg)    do { if (TalqLog::g_verbose) qDebug().noquote() << "[UI]"   << msg; } while (0)

#define TWARN(msg)  qWarning().noquote()  << "[TalQ WARN]" << msg
#define TERR(msg)   qCritical().noquote() << "[TalQ ERR]"  << msg
