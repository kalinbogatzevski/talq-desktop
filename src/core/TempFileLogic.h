#pragma once

// The temp files TalQ itself creates in %TEMP%, and the one rule for removing the ones a session
// left behind.
//
// Three families, each created by TalQ and by nothing else this code can see:
//   talq_paste_<epoch-ms>.png            a pasted image, PNG-encoded for upload      (ComposerWidget)
//   voice-message-<yyyyMMdd-HHmmss>.mp3  a recorded voice note                       (MainWindow)
//   talq_upload_<token>[.ext]            a copy of a file that could not be opened in place
//                                        (MessageListModel; builds before 0.74.12 named it
//                                        talq_upload_<original name>, and those leftovers are
//                                        swept all the same)
// In a normal session each is removed as soon as its share succeeded, or when the user cancels the
// staged paste. What remains is what a crash, a refused upload or a quit mid-upload leaves -- and
// is, for the upload copy, a second copy of the user's own file. The startup sweep removes exactly
// those, and only those:
//   - by SHAPE, not by a loose prefix: a name has to match its family completely;
//   - only plain files directly in the folder (no directories, no links, no recursion);
//   - only when BOTH the modification and the creation time are a day old, so a file that was
//     copied or restored into %TEMP% (old mtime, new creation time) or touched recently (new
//     mtime) is never taken for ours;
//   - never on a guess: an unknown or future timestamp keeps the file.
// What it must never touch, and the test pins: the downloaded installer (TalQ-v<ver>-Setup.exe,
// talq-update*.exe -- UpdateChecker owns those), the self-test log, and anything else.

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QUuid>

#include <cstddef>
#include <string>

namespace talq {

enum class TempFileFamily {
    None,
    PastedImage,
    VoiceNote,
    UploadCopy,
};

namespace detail {
inline bool allDigits(const std::string &s, std::size_t from, std::size_t len)
{
    if (from + len > s.size()) return false;
    for (std::size_t i = from; i < from + len; ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}
inline bool startsWith(const std::string &s, const char *p)
{
    return s.rfind(p, 0) == 0;
}
inline bool endsWith(const std::string &s, const char *suffix)
{
    const std::string x(suffix);
    return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}
} // namespace detail

// `fileName` is the bare file name, no directory. Exact shapes; anything else is None.
inline TempFileFamily tempFileFamilyOf(const std::string &fileName)
{
    using namespace detail;
    static const char kPaste[]  = "talq_paste_";
    static const char kVoice[]  = "voice-message-";
    static const char kUpload[] = "talq_upload_";

    if (startsWith(fileName, kPaste) && endsWith(fileName, ".png")) {
        // <epoch milliseconds>: 13 digits today; a generous band so a shape change is not a leak.
        const std::size_t from = sizeof(kPaste) - 1;
        const std::size_t len  = fileName.size() - from - 4;
        if (len >= 10 && len <= 16 && allDigits(fileName, from, len))
            return TempFileFamily::PastedImage;
        return TempFileFamily::None;
    }
    if (startsWith(fileName, kVoice) && endsWith(fileName, ".mp3")) {
        // voice-message-20261010-142233.mp3
        const std::size_t from = sizeof(kVoice) - 1;
        if (fileName.size() == from + 8 + 1 + 6 + 4
            && allDigits(fileName, from, 8)
            && fileName[from + 8] == '-'
            && allDigits(fileName, from + 9, 6))
            return TempFileFamily::VoiceNote;
        return TempFileFamily::None;
    }
    if (startsWith(fileName, kUpload) && fileName.size() > sizeof(kUpload) - 1)
        return TempFileFamily::UploadCopy;
    return TempFileFamily::None;
}

// The name for the copy made in %TEMP% when a file cannot be opened where it is (a junction, a
// protected folder). Never reused and never built from the user's file name:
//   - QFile::copy refuses an existing destination, so the old fixed "talq_upload_<name>" made a
//     stale copy (a crashed session) or a second file of the same name fail the upload with
//     "Cannot open file";
//   - nothing reads the copy's name -- the server-side name comes from the ORIGINAL path -- so the
//     stem is dropped, which also takes MAX_PATH, non-ASCII and trailing-dot problems with it and
//     stops the user's file names lingering in %TEMP%. Only a short plain extension survives, for
//     anything that sniffs by it.
// `token` is random hex from the caller (no clock or RNG in here, so it stays testable); it is
// reduced to [0-9A-Za-z] and at most 32 characters, so the name cannot carry a path separator.
// The longest result is 12 + 32 + 1 + 8 = 53 characters.
inline std::string uploadCopyName(const std::string &token, const std::string &originalFileName)
{
    auto alnum = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    std::string tok;
    for (char c : token) {
        if (alnum(c)) tok.push_back(c);
        if (tok.size() >= 32) break;
    }
    if (tok.empty())
        tok = "0";

    std::string ext;
    const std::size_t dot = originalFileName.rfind('.');
    // dot > 0: a leading dot (".bashrc") is a name, not an extension.
    if (dot != std::string::npos && dot > 0 && dot + 1 < originalFileName.size()) {
        const std::string e = originalFileName.substr(dot + 1);
        bool plain = e.size() <= 8;
        for (char c : e) plain = plain && alnum(c);
        if (plain)
            ext = "." + e;
    }
    return "talq_upload_" + tok + ext;
}

// Copies `source` into `tempDir` under a fresh uploadCopyName and returns the copy's path, or an
// empty string with the reason in *why. This is the whole fallback of MessageListModel for a file
// that cannot be opened in place, kept here so it can be tested on a real folder.
//   - The token comes from here (a random UUID); `forcedToken` exists for tests only.
//   - A name that already exists is NEVER reused and NEVER removed: it may be another upload's
//     copy that is being read right now.
//   - After a failed copy the destination is removed: it did not exist a moment ago, so a partial
//     one can only be ours, and leaving it would sit in %TEMP% until the sweep.
inline QString makeUploadCopy(const QString &source, const QString &tempDir, QString *why = nullptr,
                              const std::string &forcedToken = std::string())
{
    const std::string token = forcedToken.empty()
        ? QUuid::createUuid().toString(QUuid::Id128).toStdString()
        : forcedToken;
    const QString dest = tempDir + QLatin1Char('/')
        + QString::fromStdString(uploadCopyName(token, QFileInfo(source).fileName().toStdString()));
    if (QFile::exists(dest)) {
        if (why) *why = QStringLiteral("a file with that name already exists");
        return QString();
    }
    QFile src(source);
    if (src.copy(dest)) {
        // The OS copy carries the source's READ-ONLY attribute over, and Windows will not delete a
        // read-only file: the copy of a read-only document could then never be removed, neither
        // by the upload paths (a bare QFile::remove) nor by the sweep, and every send would leave
        // a full copy in %TEMP%. The copy is ours, so it is made removable.
        QFile::setPermissions(dest, QFile::permissions(dest) | QFile::WriteOwner | QFile::WriteUser);
        return dest;
    }
    if (why) *why = src.errorString();
    QFile::remove(dest);
    return QString();
}

inline constexpr long long kTempFileMaxAgeSecs = 24LL * 60 * 60;

// BOTH clocks must be at least a day old. A stamp that is unknown (<= 0) or in the future (a clock
// that moved) keeps the file: deleting on a guess is the one mistake this cannot take back.
inline bool tempFileIsStale(long long nowSecs, long long modifiedSecs, long long createdSecs,
                            long long maxAgeSecs = kTempFileMaxAgeSecs)
{
    if (modifiedSecs <= 0 || createdSecs <= 0)
        return false;
    if (modifiedSecs > nowSecs || createdSecs > nowSecs)
        return false;
    return (nowSecs - modifiedSecs) >= maxAgeSecs && (nowSecs - createdSecs) >= maxAgeSecs;
}

struct TempSweepResult {
    int       removed = 0;   // files taken away
    int       failed  = 0;   // matched and stale, but could not be removed (in use, no permission)
    int       kept    = 0;   // matched a family but are not a day old yet
    qint64    bytes   = 0;   // size of what was removed
};

// One non-recursive pass over `dirPath`. `nowSecs` is a parameter so a test can age files without
// waiting a day.
inline TempSweepResult sweepStaleTempFiles(const QString &dirPath, qint64 nowSecs)
{
    TempSweepResult r;
    const QDir dir(dirPath);
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot | QDir::Hidden,
                          QDir::Unsorted);
    for (const QFileInfo &fi : entries) {
        if (!fi.isFile() || fi.isSymLink())
            continue;
        if (tempFileFamilyOf(fi.fileName().toStdString()) == TempFileFamily::None)
            continue;
        const QDateTime born = fi.birthTime();
        const qint64 created  = born.isValid() ? born.toSecsSinceEpoch() : 0;
        const qint64 modified = fi.lastModified().isValid() ? fi.lastModified().toSecsSinceEpoch() : 0;
        if (!tempFileIsStale(nowSecs, modified, created)) {
            ++r.kept;
            continue;
        }
        const qint64 size = fi.size();
        const QString path = fi.absoluteFilePath();
        bool gone = QFile::remove(path);
        if (!gone && tempFileFamilyOf(fi.fileName().toStdString()) == TempFileFamily::UploadCopy) {
            // Copies made by earlier builds of a read-only file inherited the attribute and could
            // not be deleted by anything. Only this family: a paste or a recording is written by
            // TalQ and never inherits one, so a read-only file of that name is somebody's.
            QFile::setPermissions(path, QFile::permissions(path) | QFile::WriteOwner | QFile::WriteUser);
            gone = QFile::remove(path);
        }
        if (gone) {
            ++r.removed;
            r.bytes += size;
        } else {
            ++r.failed;
        }
    }
    return r;
}

} // namespace talq
