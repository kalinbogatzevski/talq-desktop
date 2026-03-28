#pragma once

#include <QDebug>

// TalQ logging macros — always present in code, controlled by build type.
// Debug builds: all logs print to stderr.
// Release builds: only warnings and errors print.
//
// Usage:
//   TLOG("message")                    — general debug log
//   TLOG_CALL("ICE state:" << state)   — call/WebRTC debug
//   TLOG_SIG("event:" << type)         — signaling debug
//   TLOG_NET("request:" << path)       — network debug
//   TLOG_UI("click:" << widget)        — UI debug
//   TWARN("problem:" << err)           — always prints (both Debug and Release)
//   TERR("fatal:" << msg)              — always prints

#ifdef QT_DEBUG
  #define TLOG(msg)       qDebug().noquote() << "[TalQ]" << msg
  #define TLOG_CALL(msg)  qDebug().noquote() << "[CALL]" << msg
  #define TLOG_SIG(msg)   qDebug().noquote() << "[SIG]" << msg
  #define TLOG_NET(msg)   qDebug().noquote() << "[NET]" << msg
  #define TLOG_UI(msg)    qDebug().noquote() << "[UI]" << msg
#else
  #define TLOG(msg)       qt_noop()
  #define TLOG_CALL(msg)  qt_noop()
  #define TLOG_SIG(msg)   qt_noop()
  #define TLOG_NET(msg)   qt_noop()
  #define TLOG_UI(msg)    qt_noop()
#endif

// Warnings and errors always print regardless of build type
#define TWARN(msg)  qWarning().noquote() << "[TalQ WARN]" << msg
#define TERR(msg)   qCritical().noquote() << "[TalQ ERR]" << msg
