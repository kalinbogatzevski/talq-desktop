#pragma once

// Names that come from the server and become local file names (test: local-file-name-test).
//
// A file's name in a chat message is the `name` of its `file` parameter, used as the server sends
// it (Message.cpp cleans nothing). On NTFS several things in a name mean something else: a colon
// names an alternate data stream ("a.mp3:x" writes the stream "x" of "a.mp3", or a drive), a
// device stem ("NUL.txt", "COM1") can open a device, a trailing dot or space is dropped without a
// word (so the file made is not the path asked for), and < > " | ? * and control characters make
// the open fail.
//
//   safeCacheSuffix   the suffix of a cache file whose stem is ours (the audio cache keys on the
//                     file id): ASCII letters and digits only, at most 10, otherwise none. It is a
//                     type hint for the decoder and nothing more.
//   safeDownloadName  the name of a file the user will find in Downloads: kept as close to the
//                     sender's as Windows allows, so a non-Latin name survives. The rules are
//                     below; the extension is part of the name and gets the same treatment.
//   numberedName      the n-th alternative for a taken name: "name (n).ext".
//   createUniqueFile  makes the file, under the name or its first free numbered alternative, with
//                     QIODevice::NewOnly: the OS creates it only if it does not exist, so a file
//                     another program makes between a look and the write is never truncated.
//
// Not covered: a name longer than the 255 characters a Windows path component holds (the
// numbered alternative adds a few) still fails the open, and is reported as a failed save.

#include <QChar>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QString>

#include <algorithm>
#include <memory>

namespace talq {

inline QString safeCacheSuffix(const QString &suffix)
{
    constexpr int kMax = 10;
    if (suffix.isEmpty() || suffix.size() > kMax)
        return QString();
    for (const QChar c : suffix) {
        const char16_t u = c.unicode();
        const bool ok = (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'z')
                        || (u >= u'A' && u <= u'Z');
        if (!ok)
            return QString();
    }
    return suffix;
}

// True when Windows would read `name` as a device: the part before the first dot (trailing spaces
// ignored, case ignored) is CON, PRN, AUX, NUL, CONIN$, CONOUT$, or COM/LPT with one digit (0-9,
// or a superscript 1-3). "NUL.txt" and "nul.tar.gz" are both NUL.
inline bool isWindowsDeviceName(const QString &name)
{
    const int dot = name.indexOf(QLatin1Char('.'));
    QString stem = dot < 0 ? name : name.left(dot);
    while (stem.endsWith(QLatin1Char(' ')))
        stem.chop(1);
    stem = stem.toUpper();
    if (stem == QLatin1String("CON") || stem == QLatin1String("PRN")
        || stem == QLatin1String("AUX") || stem == QLatin1String("NUL")
        || stem == QLatin1String("CONIN$") || stem == QLatin1String("CONOUT$"))
        return true;
    if (stem.size() == 4
        && (stem.startsWith(QLatin1String("COM")) || stem.startsWith(QLatin1String("LPT")))) {
        const char16_t d = stem.at(3).unicode();
        return (d >= u'0' && d <= u'9') || d == u'¹' || d == u'²' || d == u'³';
    }
    return false;
}

// The rules, in order: the last path component only (either separator); each of < > : " | ? *
// and every control character (below U+0020, and U+007F) becomes '_'; trailing dots and spaces
// go; nothing left (or only dots) is "download"; a device name takes a leading '_'.
inline QString safeDownloadName(const QString &serverName)
{
    QString n = serverName;
    const int cut = std::max(n.lastIndexOf(QLatin1Char('/')), n.lastIndexOf(QLatin1Char('\\')));
    if (cut >= 0)
        n = n.mid(cut + 1);
    for (QChar &c : n) {
        const char16_t u = c.unicode();
        if (u < 0x20 || u == 0x7F || u == u'<' || u == u'>' || u == u':' || u == u'"'
            || u == u'|' || u == u'?' || u == u'*')
            c = QLatin1Char('_');
    }
    while (n.endsWith(QLatin1Char('.')) || n.endsWith(QLatin1Char(' ')))
        n.chop(1);
    if (n.isEmpty())
        return QStringLiteral("download");
    if (isWindowsDeviceName(n))
        n.prepend(QLatin1Char('_'));
    return n;
}

// n == 0 is the name itself. The number goes before the LAST extension ("archive.tar (1).gz", as
// before); a name whose only dot leads it (".profile") has no extension.
inline QString numberedName(const QString &name, int n)
{
    if (n <= 0)
        return name;
    const QString tag = QStringLiteral(" (%1)").arg(n);
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0)
        return name + tag;
    return name.left(dot) + tag + name.mid(dot);
}

// Creates `dir`/`name`, or the first of "name (1)", "name (2)" ... that does not exist yet, and
// returns it open for writing. `name` must already be safe (safeDownloadName). Returns null when
// the folder refuses the file for any reason other than a name being taken, or after `maxTries`
// taken names; `path` is then the name that failed (empty only if `maxTries` < 1).
inline std::unique_ptr<QFile> createUniqueFile(const QString &dir, const QString &name,
                                               QString *path, int maxTries = 1000)
{
    path->clear();
    for (int n = 0; n < maxTries; ++n) {
        *path = dir + QLatin1Char('/') + numberedName(name, n);
        auto f = std::make_unique<QFile>(*path);
        if (f->open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return f;
        if (!QFileInfo::exists(*path))
            return nullptr;   // not a clash: the folder refused the file
    }
    return nullptr;
}

} // namespace talq
