#pragma once

// The update installers UpdateChecker downloads into %TEMP%, and the one rule for removing the
// ones that are spent.
//
// The installer is saved under its published asset name, in the folder TempFileLogic.h sweeps
// (which leaves installers alone):
//   TalQ-v<version>-Setup.exe             the generic build's installer
//   <channel>-TalQ-v<version>-Setup.exe   a branded build's installer, where <channel> is the
//                                         build's update channel id, compared without case
// UpdateChecker::startDownload() clears older installers, but only when the NEXT download starts
// and only by the first shape. So a branded build kept every installer it ever ran (one ~60 MB
// file per update), and every build kept the last one for good. This sweep, run once a session,
// removes each installer whose version is the running one or older: that install has happened or
// been passed by, and the file is only dead weight that also draws AV attention.
//
// The rules, pinned by installer-sweep-test:
//   - by exact SHAPE: the whole name must be one of the two above, the version two to four runs of
//     at most six digits; anything looser is kept. VersionCompare's parser is lenient ("" reads as
//     0, letters are skipped), so a name is never handed to it unchecked;
//   - only versions <= the running one: a newer installer may be the next update, and it stays
//     UpdateChecker's;
//   - never a file on the keep list, matched by name without case: the installer an install is
//     waiting on and the one being downloaded. Those are below the running version only for a
//     pinned rollback, which is why the list exists at all;
//   - only plain, non-hidden files directly in the folder: no directories, no links, no recursion;
//   - a running version that is not a plain version removes nothing;
//   - a file that cannot be removed (the installer that relaunched TalQ may still be running) is
//     counted and left; permissions are never changed.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include "VersionCompare.h"

#include <cstddef>
#include <string>

namespace talq {

// Two to four dot-separated runs of 1..6 digits, nothing else ("0.81.2").
inline bool isPlainVersion(const std::string &v)
{
    int segments = 0;
    std::size_t run = 0;
    for (char c : v) {
        if (c >= '0' && c <= '9') {
            if (++run > 6) return false;
        } else if (c == '.') {
            if (run == 0) return false;
            ++segments;
            run = 0;
        } else {
            return false;
        }
    }
    if (run == 0) return false;
    ++segments;
    return segments >= 2 && segments <= 4;
}

// The version in an installer name of exactly one of the two shapes, or "" for any other name.
// `channel` is a branded build's update channel id; "" (the generic build) means no prefixed name
// is ours.
inline std::string installerVersionOf(const std::string &fileName, const std::string &channel)
{
    static const std::string kStem = "TalQ-v";
    static const std::string kTail = "-Setup.exe";
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };

    std::size_t from = 0;
    if (fileName.compare(0, kStem.size(), kStem) != 0) {
        const std::size_t n = channel.size();
        if (n == 0 || fileName.size() <= n + 1 || fileName[n] != '-')
            return std::string();
        for (std::size_t i = 0; i < n; ++i)
            if (lower(fileName[i]) != lower(channel[i]))
                return std::string();
        from = n + 1;
        if (fileName.compare(from, kStem.size(), kStem) != 0)
            return std::string();
    }
    from += kStem.size();
    if (fileName.size() < from + kTail.size()
        || fileName.compare(fileName.size() - kTail.size(), kTail.size(), kTail) != 0)
        return std::string();
    const std::string v = fileName.substr(from, fileName.size() - kTail.size() - from);
    return isPlainVersion(v) ? v : std::string();
}

// The name UpdateChecker saves a downloaded installer under in the temp folder: the published
// asset name when it is exactly one of the two shapes above (every release's is: the release
// script refuses a version that is not plain digits), otherwise the fixed "TalQ-Setup.exe". The
// asset name comes from the update manifest and is joined onto the temp folder's path, so a name
// carrying a separator, "..", a drive or stream colon, or anything else, is never used as a path.
// Only the local name falls back; the download still comes from the manifest's URL, and an update
// is never refused over its name (a client that refuses its manifest cannot be updated out of it).
inline std::string installerDownloadName(const std::string &assetName, const std::string &channel)
{
    return installerVersionOf(assetName, channel).empty() ? std::string("TalQ-Setup.exe")
                                                          : assetName;
}

// True when `fileName` is an installer of `runningVersion` or older.
inline bool installerIsSpent(const std::string &fileName, const std::string &channel,
                             const std::string &runningVersion)
{
    if (!isPlainVersion(runningVersion))
        return false;
    const std::string v = installerVersionOf(fileName, channel);
    return !v.empty() && !versionNewer(v, runningVersion);
}

struct InstallerSweepResult {
    int    removed = 0;   // spent installers taken away
    int    failed  = 0;   // spent, but could not be removed (still running, no permission)
    int    kept    = 0;   // installers that stay: newer than the running version, or on the keep list
    qint64 bytes   = 0;   // size of what was removed
};

// One non-recursive pass over `dirPath`. `keepPaths` are paths (or bare names) of installers that
// must stay whatever their version; empty entries are ignored.
inline InstallerSweepResult sweepSpentInstallers(const QString &dirPath,
                                                 const std::string &runningVersion,
                                                 const std::string &channel,
                                                 const QStringList &keepPaths)
{
    InstallerSweepResult r;
    if (!isPlainVersion(runningVersion))
        return r;
    QStringList keepNames;
    for (const QString &p : keepPaths)
        if (!p.isEmpty())
            keepNames << QFileInfo(p).fileName();

    // Without QDir::Hidden and QDir::System: a hidden file, or a shortcut, is not a download.
    const QDir dir(dirPath);
    const QFileInfoList entries = dir.entryInfoList({QStringLiteral("*TalQ-v*-Setup.exe")},
                                                    QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot,
                                                    QDir::Unsorted);
    for (const QFileInfo &fi : entries) {
        if (!fi.isFile() || fi.isSymLink())
            continue;
        const std::string name = fi.fileName().toStdString();
        if (installerVersionOf(name, channel).empty())
            continue;   // not one of ours
        if (!installerIsSpent(name, channel, runningVersion)
            || keepNames.contains(fi.fileName(), Qt::CaseInsensitive)) {
            ++r.kept;
            continue;
        }
        const qint64 size = fi.size();
        if (QFile::remove(fi.absoluteFilePath())) {
            ++r.removed;
            r.bytes += size;
        } else {
            ++r.failed;
        }
    }
    return r;
}

} // namespace talq
