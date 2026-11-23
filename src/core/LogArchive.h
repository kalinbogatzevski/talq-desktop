#pragma once

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

// LogArchive -- keeps the previous session's talq_debug.log before main.cpp
// truncates the live file (freopen "w"). Header-only, Qt6::Core only.
// Test: log-files-test.
//
// Why the copy fallback exists (homework #34): the session that PERFORMED an
// automatic update lost its whole log. The old process starts the installer
// with QProcess::startDetached, which lets the child inherit every inheritable
// handle, and the redirected stderr (the live log) is one. The installer keeps
// that handle open while it installs and starts the new TalQ. The new TalQ's
// rename then fails with a sharing violation (the inherited handle does not
// share delete access), and QFile::rename's own fallback makes things worse: it
// copies, fails to remove the source and then DELETES ITS COPY, so nothing is
// left. freopen "w" truncated the file in place a moment later. Reproduced with
// a GUI-subsystem parent that freopens stderr and starts a detached child; not
// yet observed with the real installer. QFile::copy reads through the same
// sharing and succeeds, so a failed rename is followed by a plain copy.

namespace talq::logarchive {

enum class Outcome {
    NoPreviousLog,   // nothing to keep (first run, or the file is gone)
    Renamed,         // the normal case
    Copied,          // the rename failed; a copy was made and the live file stays
    Lost,            // neither worked: the truncate that follows loses the old log
};

struct Result {
    Outcome outcome = Outcome::NoPreviousLog;
    QString archivePath;
    QString renameError;   // QFile::errorString() of the failed rename
    QString copyError;     // QFile::errorString() of the failed copy
};

// "<dir>/<stem>_<yyyyMMdd_HHmmss>.log", stamped with the old session's last
// write, so a later restart never overwrites it and the names sort by age.
inline QString archiveNameFor(const QString &logPath, const QDateTime &lastWrite)
{
    const QFileInfo fi(logPath);
    return fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + QLatin1Char('_')
        + lastWrite.toString(QStringLiteral("yyyyMMdd_HHmmss")) + QStringLiteral(".log");
}

// Moves (or failing that, copies) the log at `logPath` to its archive name.
// The live file is never removed or changed here; the caller truncates it.
inline Result archivePrevious(const QString &logPath)
{
    Result r;
    if (!QFile::exists(logPath)) return r;
    QDateTime lastWrite = QFileInfo(logPath).lastModified();
    if (!lastWrite.isValid()) lastWrite = QDateTime::currentDateTime();
    r.archivePath = archiveNameFor(logPath, lastWrite);
    QFile::remove(r.archivePath);          // a same-second restart overwrites
    QFile live(logPath);
    if (live.rename(r.archivePath)) {
        r.outcome = Outcome::Renamed;
        return r;
    }
    r.renameError = live.errorString();
    // QFile::copy refuses an existing destination; whatever the rename left
    // there (normally nothing, see above) is not a complete copy.
    QFile::remove(r.archivePath);
    QFile source(logPath);
    if (source.copy(r.archivePath)) {
        r.outcome = Outcome::Copied;
        return r;
    }
    r.copyError = source.errorString();
    QFile::remove(r.archivePath);
    r.outcome = Outcome::Lost;
    return r;
}

// Removes the oldest archives of `logPath` beyond the newest `keep`. Only
// "<stem>_*.log" next to the log is touched. Returns how many were removed.
inline int pruneArchives(const QString &logPath, int keep)
{
    const QFileInfo fi(logPath);
    QDir d(fi.absolutePath());
    const QStringList olds = d.entryList(
        QStringList() << fi.completeBaseName() + QStringLiteral("_*.log"),
        QDir::Files, QDir::Name);           // timestamped names: oldest first
    int removed = 0;
    for (int i = 0; i < olds.size() - keep; ++i)
        if (QFile::remove(d.absoluteFilePath(olds.at(i)))) ++removed;
    return removed;
}

// What happened to the previous log, said on every start in talq_update.log,
// so the next update shows which path was taken even when all went well.
inline QString summary(const Result &r)
{
    const QString name = QFileInfo(r.archivePath).fileName();
    switch (r.outcome) {
    case Outcome::NoPreviousLog:
        return QStringLiteral("none");
    case Outcome::Renamed:
        return QStringLiteral("archived as %1").arg(name);
    case Outcome::Copied:
        return QStringLiteral("copied to %1 (move failed: %2)").arg(name, r.renameError);
    case Outcome::Lost:
        return QStringLiteral("LOST (move failed: %1; copy failed: %2)")
            .arg(r.renameError, r.copyError);
    }
    return {};
}

// The line the new session logs about it; empty when there is nothing to say.
inline QString describe(const Result &r)
{
    const QString name = QFileInfo(r.archivePath).fileName();
    switch (r.outcome) {
    case Outcome::NoPreviousLog:
    case Outcome::Renamed:
        return {};
    case Outcome::Copied:
        return QStringLiteral("the previous session's log could not be moved (%1); "
                              "copied to %2 instead")
            .arg(r.renameError, name);
    case Outcome::Lost:
        return QStringLiteral("the previous session's log could not be moved (%1) "
                              "or copied to %2 (%3); this session overwrites it")
            .arg(r.renameError, name, r.copyError);
    }
    return {};
}

} // namespace talq::logarchive
