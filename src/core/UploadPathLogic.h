#pragma once

// The path an upload is read from.
//
// Windows refuses to traverse some junctions from a protected folder (Program Files...) and Qt 6
// will not follow an "untrusted mount point", so the path is first rewritten with every junction
// on it replaced by its target (MessageListModel::sendFileWithCaption). Two things this has to get
// right:
//
//  - The ROOT. The rebuild splits the absolute path on '/' and joins the parts again, which
//    silently dropped the root of a path that does not start with a drive letter:
//    "//server/share/dir/file.txt" came out as the RELATIVE "server/share/dir/file.txt", was read
//    against the working directory, failed to open, failed again in the temp-copy fallback, and
//    every upload from a network share ended in "Cannot open file".
//  - WHERE the junction lives. A junction on a REMOTE volume (a UNC path, or a mapped drive) is
//    resolved by the server for its own clients, and what junctionTarget() reports is the SERVER's
//    local path ("D:/data/2026"). Substituting that and reading it here would open the CLIENT's
//    D: drive: a missing file at best, a different file of the same path at worst. So a path on a
//    remote volume is never rewritten; the open (or the copy fallback) takes it as it is.

#include <QFileInfo>
#include <QLibrary>
#include <QString>
#include <QStringList>

#include <functional>

namespace talq {

// True when `absPath` ("//server/share/x" or "Z:/x") is on a volume that is not local to this
// machine. A UNC path always is; a drive letter is asked of the OS (DRIVE_REMOTE == 4).
inline bool pathIsOnRemoteVolume(const QString &absPath)
{
    if (absPath.startsWith(QLatin1String("//")))
        return true;
#ifdef Q_OS_WIN
    if (absPath.size() >= 2 && absPath.at(1) == QLatin1Char(':')) {
        // Resolved at run time: this header is included by large translation units that must not
        // see <windows.h> and its macros.
        using GetDriveTypeWFn = unsigned int (__stdcall *)(const wchar_t *);
        static const GetDriveTypeWFn getDriveType = reinterpret_cast<GetDriveTypeWFn>(
            QLibrary(QStringLiteral("kernel32")).resolve("GetDriveTypeW"));
        if (getDriveType) {
            const wchar_t root[4] = {static_cast<wchar_t>(absPath.at(0).unicode()), L':', L'\\', 0};
            return getDriveType(root) == 4;
        }
    }
#endif
    return false;
}

// The target of `path` when it is a junction, else an empty string.
inline QString localJunctionTarget(const QString &path)
{
    const QFileInfo info(path);
    return info.isJunction() ? info.junctionTarget() : QString();
}

// `junctionTargetOf` and `isRemote` are seams for tests (a junction on a network share cannot be
// made in a unit test); empty means the real ones above.
inline QString resolveJunctionsInPath(
    const QString &localPath,
    const std::function<QString(const QString &)> &junctionTargetOf = {},
    const std::function<bool(const QString &)> &isRemote = {})
{
    const QString absPath = QFileInfo(localPath).absoluteFilePath();
    const QStringList parts = absPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return localPath;

    // A remote path is taken as it is: the server resolves its own junctions.
    if (isRemote ? isRemote(absPath) : pathIsOnRemoteVolume(absPath))
        return absPath;

    // "C:/..." has no root of its own (the drive is the first part); a POSIX path has "/". Both
    // must survive the rebuild.
    QString resolved;
    if (absPath.startsWith(QLatin1Char('/')))
        resolved = QStringLiteral("/");
    resolved += parts.first() + QLatin1Char('/');

    for (int i = 1; i < parts.size(); ++i) {
        resolved += parts[i];
        const QString target = junctionTargetOf ? junctionTargetOf(resolved)
                                                : localJunctionTarget(resolved);
        if (!target.isEmpty())
            resolved = target;
        if (i < parts.size() - 1)
            resolved += QLatin1Char('/');
    }
    return resolved;
}

} // namespace talq
