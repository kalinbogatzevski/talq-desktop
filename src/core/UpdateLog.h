#pragma once

#include "core/LogRedaction.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>

#include <string>
#include <string_view>

// UpdateLog -- talq_update.log, a small file next to talq_debug.log that holds
// the automatic-update lines ("Update: installer downloaded", "countdown
// started", "auto-install held by ...", "launching the installer") plus one
// line per start. Header-only, Qt6::Core only. Test: log-files-test.
//
// talq_debug.log is truncated at every start, and the session that performed an
// update lost its log entirely (see LogArchive.h). This file is never truncated
// at startup: every line is appended, and only once it reaches kMaxBytes is the
// oldest part dropped, keeping the newest kKeepBytes from a line boundary.
//
// Each write opens, appends and closes. Never keep it open: an open handle is
// inherited by a child process (the installer), which is how the debug log was
// lost. The lines bypass the Qt message handler, so they are redacted here.

namespace talq::updatelog {

inline constexpr qint64 kMaxBytes = 256 * 1024;
inline constexpr qint64 kKeepBytes = 128 * 1024;

// Set once by main.cpp, for the primary instance only. Empty = no file, which
// is what every test target that compiles MainWindow.cpp gets.
inline QString g_path;
inline QString g_version;

// "<dir of the debug log>/talq_update.log"; empty for an empty path.
inline QString pathBeside(const QString &debugLogPath)
{
    if (debugLogPath.isEmpty()) return {};
    return QFileInfo(debugLogPath).absolutePath() + QStringLiteral("/talq_update.log");
}

// The newest `keep` bytes of `content`, starting at a line boundary. Content
// that already fits is returned unchanged; a tail with no line break in it is
// a fragment of one line and is dropped.
inline QByteArray keepNewest(const QByteArray &content, qsizetype keep)
{
    if (keep < 0) keep = 0;
    if (content.size() <= keep) return content;
    qsizetype from = content.size() - keep;
    if (from > 0 && content.at(from - 1) != '\n') {
        const qsizetype nl = content.indexOf('\n', from);
        from = nl < 0 ? content.size() : nl + 1;
    }
    return content.mid(from);
}

// "yyyy-MM-dd HH:mm:ss.zzz [<version> pid <pid>] <text>\n", redacted, on one line.
inline QByteArray formatLine(const QDateTime &when, const QString &version, qint64 pid,
                             const QString &text)
{
    QString oneLine = text;
    oneLine.replace(QLatin1Char('\r'), QLatin1Char(' '));
    oneLine.replace(QLatin1Char('\n'), QLatin1Char(' '));
    const QByteArray raw = (when.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
                            + QStringLiteral(" [%1 pid %2] ").arg(version).arg(pid)
                            + oneLine + QLatin1Char('\n')).toUtf8();
    const std::string safe = LogRedaction::redact(
        std::string_view(raw.constData(), static_cast<size_t>(raw.size())));
    return QByteArray(safe.data(), static_cast<qsizetype>(safe.size()));
}

// Appends `line` to `path`. When the file would pass `maxBytes`, it is first
// rewritten (atomically) to its newest `keepBytes`. If that rewrite fails the
// line is appended anyway: a line is never dropped for the cap's sake.
inline bool append(const QString &path, const QByteArray &line,
                   qint64 maxBytes = kMaxBytes, qint64 keepBytes = kKeepBytes)
{
    if (path.isEmpty() || line.isEmpty()) return false;
    const QFileInfo fi(path);
    if (fi.exists() && fi.size() + line.size() > maxBytes) {
        QFile in(path);
        if (in.open(QIODevice::ReadOnly)) {
            QByteArray kept = keepNewest(in.readAll(), keepBytes);
            in.close();
            kept += line;
            QSaveFile out(path);
            if (out.open(QIODevice::WriteOnly) && out.write(kept) == kept.size() && out.commit())
                return true;
        }
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return false;
    return f.write(line) == line.size();
}

// One stamped line in the update log (g_path), or nothing when it is not set.
inline bool note(const QString &text)
{
    if (g_path.isEmpty()) return false;
    return append(g_path, formatLine(QDateTime::currentDateTime(), g_version,
                                     QCoreApplication::applicationPid(), text));
}

} // namespace talq::updatelog
