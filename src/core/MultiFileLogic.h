// Sending several files in one go (paste, drop, the file dialog) -- the decisions that need no
// widget or network, so they are tested on their own (tests/multi_file_logic_test.cpp).
//
// Until this existed the composer held ONE staged file: pasting or dropping several kept the
// first (or, for a drop that emitted each, the last) and said nothing about the rest, and the
// Send-file dialog was single-select.
//
//   * addStaged        what is staged after more files arrive: duplicates are not added twice,
//                      what is not a file (a folder) is skipped, and a cap stops an accidental
//                      "select all" of a huge folder. EVERYTHING that is left out is COUNTED,
//                      so the composer can say so; nothing is dropped silently.
//   * uploadNameFor    the name a file is uploaded under (the one sanitiser, shared with
//                      MessageListModel so the queue and the engine cannot disagree).
//   * uniqueUploadNames  names made distinct within one batch. Two files called "image.png" from
//                      two folders would otherwise be uploaded to the SAME path, the second
//                      replacing the first: both shares would then point at the second file.

#pragma once

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

namespace talq::multifile {

// More than this many files in one message is almost certainly an accident (a select-all in a
// folder), and every one of them is an upload the user has to wait for.
constexpr int kMaxFiles = 50;
// How many incoming paths are looked at on disk at most. A paste or a drop can carry any number of
// paths (a clipboard is not under the user's eye), and each look is a stat that can block on a dead
// or slow network share (or reach out to a remote host named in a UNC path) -- on the UI thread.
// Everything beyond this is counted as left out, never touched.
constexpr int kMaxInspected = 4 * kMaxFiles;

struct AddResult {
    QStringList files;     // the staged list afterwards, first = the file that carries the caption
    int added = 0;
    int duplicates = 0;    // already staged
    int notFiles = 0;      // a folder
    int missing = 0;       // a path that is not there (moved or deleted since it was copied/dragged)
    int overLimit = 0;     // left out because kMaxFiles was reached
};

inline QString normalizedPath(const QString &p)
{
    QString n = QDir::cleanPath(p);
#ifdef Q_OS_WIN
    n = n.toLower();       // Windows paths compare without case
#endif
    return n;
}

inline bool isRegularFile(const QString &path)
{
    const QFileInfo fi(path);
    return fi.exists() && fi.isFile();
}

inline bool pathExists(const QString &path)
{
    return QFileInfo::exists(path);
}

// Add `incoming` after `staged`. `isFile` and `exists` are injectable so a test needs no files on
// disk. A path that is not a file is a FOLDER if it exists and MISSING if it does not: a shortcut
// whose target is gone, or a file moved between the copy and the paste, is not "a folder".
inline AddResult addStaged(const QStringList &staged, const QStringList &incoming,
                           const std::function<bool(const QString &)> &isFile = isRegularFile,
                           int maxFiles = kMaxFiles,
                           const std::function<bool(const QString &)> &exists = pathExists,
                           int maxInspected = kMaxInspected)
{
    AddResult r;
    r.files = staged;
    // Every path seen so far (staged, added, or refused): a repeat is a duplicate and costs no look.
    QSet<QString> seen;
    for (const QString &p : staged)
        seen.insert(normalizedPath(p));
    int inspected = 0;
    for (const QString &p : incoming) {
        if (p.isEmpty())
            continue;
        const QString key = normalizedPath(p);       // string work only, no disk
        if (seen.contains(key)) {
            ++r.duplicates;
            continue;
        }
        seen.insert(key);
        // The cap comes BEFORE the look at the disk: a full list, or a flood of paths, must not cost
        // a stat per entry.
        if (r.files.size() >= maxFiles || inspected >= maxInspected) {
            ++r.overLimit;
            continue;
        }
        ++inspected;
        if (!isFile(p)) {
            if (exists(p)) ++r.notFiles; else ++r.missing;
            continue;
        }
        r.files.append(p);
        ++r.added;
    }
    return r;
}

// The name a file is uploaded under: its own file name made safe for a WebDAV path.
inline QString uploadNameFor(const QString &path)
{
    QString name = QFileInfo(path).fileName();
    name.remove(QRegularExpression(QStringLiteral("[/\\\\?#%]")));
    name.replace(QStringLiteral(".."), QStringLiteral("_"));
    if (name.isEmpty())
        name = QStringLiteral("upload");
    return name;
}

// `names` made distinct from each other, in order, keeping the first of a kind as it is:
// "a.png", "a.png" -> "a.png", "a (2).png". Compared without case (Windows, and a Nextcloud
// whose storage is case-insensitive would treat "A.png" and "a.png" as one file).
inline QStringList uniqueUploadNames(const QStringList &names)
{
    QStringList out;
    QSet<QString> used;
    for (const QString &name : names) {
        QString candidate = name;
        int n = 1;
        while (used.contains(candidate.toLower())) {
            ++n;
            const int dot = name.lastIndexOf(QLatin1Char('.'));
            // A leading dot (".gitignore") is no extension.
            if (dot > 0)
                candidate = name.left(dot) + QStringLiteral(" (%1)").arg(n) + name.mid(dot);
            else
                candidate = name + QStringLiteral(" (%1)").arg(n);
        }
        used.insert(candidate.toLower());
        out.append(candidate);
    }
    return out;
}

} // namespace talq::multifile
