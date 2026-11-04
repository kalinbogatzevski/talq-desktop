#include "core/FolderSyncMeta.h"

#include "core/TalqLog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>

namespace talq::foldersyncmeta {

namespace {

// The file names, appended to the scope key. Neither begins with ".json", so neither can be the local
// file or a name FolderStore derives from it (.json.pre-sync, .json.corrupt-<stamp>).
const QString &sidecarSuffix()
{
    static const QString s = QStringLiteral(".sync.json");
    return s;
}

const QString &probeMarkerSuffix()
{
    static const QString s = QStringLiteral(".probed");
    return s;
}

QString pathFor(const QString &rootDir, const QString &scopeKey, const QString &suffix)
{
    // Refused, not defaulted: QDir("") is the working directory, and a hidden ".sync.json" there is the
    // worst place for a sidecar to turn up.
    if (rootDir.isEmpty() || scopeKey.isEmpty())
        return QString();
    return QDir(rootDir).filePath(scopeKey + suffix);
}

// Reads at most `cap + 1` bytes of the file at `path`: one byte past the cap is enough to tell "too
// big" without reading an arbitrary file whole. False when the file cannot be opened or read.
bool readBounded(const QString &path, qint64 cap, QByteArray *bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        TWARN("FolderSyncMeta: cannot read" << path << ":" << f.errorString());
        return false;
    }
    *bytes = f.read(cap + 1);
    const bool readFailed = f.error() != QFileDevice::NoError;
    if (readFailed)
        TWARN("FolderSyncMeta: read error on" << path << ":" << f.errorString());
    f.close();
    return !readFailed;
}

// An atomic replace of `path`: a temp file in the same directory, renamed over the target on commit().
// QSaveFile's direct-write fallback is off (the default), so there is no mode in which the target is
// opened and truncated in place. True only for a committed write; on any failure the previous file is
// untouched and no temp file is left.
bool writeAtomically(const QString &path, const QByteArray &bytes)
{
    if (path.isEmpty()) {
        TWARN("FolderSyncMeta: refusing to write a file with no root directory or no scope key");
        return false;
    }
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir)) {
        TWARN("FolderSyncMeta: cannot create" << dir);
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        TWARN("FolderSyncMeta: cannot write" << path << ":" << f.errorString());
        return false;
    }
    if (f.write(bytes) != bytes.size()) {
        const QString why = f.errorString();
        f.cancelWriting();
        TWARN("FolderSyncMeta: short write to" << path << ":" << why);
        return false;
    }
    if (!f.commit()) {
        TWARN("FolderSyncMeta: cannot replace" << path << ":" << f.errorString());
        return false;
    }
    return true;
}

} // namespace

QString sidecarPath(const QString &rootDir, const QString &scopeKey)
{
    return pathFor(rootDir, scopeKey, sidecarSuffix());
}

QString probeMarkerPath(const QString &rootDir, const QString &scopeKey)
{
    return pathFor(rootDir, scopeKey, probeMarkerSuffix());
}

// ---------------------------------------------------------------------------------------------
// the sidecar

SidecarRead readSidecar(const QString &rootDir, const QString &scopeKey)
{
    SidecarRead out;   // Absent, nothing in it
    const QString path = sidecarPath(rootDir, scopeKey);
    if (path.isEmpty() || !QFileInfo::exists(path))
        return out;

    // From here a file is there. Anything short of a clean decode is Unreadable and carries nothing:
    // the base is thrown away and the merge is a union (D12), which is the safe direction.
    out.status = SidecarStatus::Unreadable;
    QByteArray bytes;
    if (!readBounded(path, kMaxSidecarBytes, &bytes))
        return out;

    const SidecarDecode d = decodeSidecar(bytes);   // judges the size too: cap + 1 bytes is over
    out.status = d.status;
    if (d.status == SidecarStatus::Ok)
        out.sidecar = d.sidecar;
    return out;
}

bool writeSidecar(const QString &rootDir, const QString &scopeKey, const FolderState &base,
                  const QByteArray &localShaHex)
{
    // A producer bug, not user data: the sha comes from FolderStore's saved() signal (a SHA-1 hex
    // digest) or is empty (D31). Anything else reads back Unreadable, so a base would never be
    // trusted again and a folder the user deleted would return on every start (D30).
    Q_ASSERT(localShaHex.isEmpty() || folder_sync_detail::isSha1Hex(QString::fromLatin1(localShaHex)));

    return writeAtomically(sidecarPath(rootDir, scopeKey), encodeSidecar(base, localShaHex));
}

// ---------------------------------------------------------------------------------------------
// the probe marker

bool probeMarkerPresent(const QString &rootDir, const QString &scopeKey)
{
    const QString path = probeMarkerPath(rootDir, scopeKey);
    if (path.isEmpty() || !QFileInfo::exists(path))
        return false;

    QByteArray bytes;
    if (!readBounded(path, kMaxProbeMarkerBytes, &bytes) || bytes.size() > kMaxProbeMarkerBytes)
        return false;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    const QJsonObject o = doc.object();

    // Exactly { "v": 1, "probedOk": true }: those two keys, that number, that boolean.
    if (o.size() != 2)
        return false;
    const QJsonValue v = o.value(QStringLiteral("v"));
    const QJsonValue ok = o.value(QStringLiteral("probedOk"));
    return v.isDouble() && v.toDouble() == 1.0 && ok.isBool() && ok.toBool();
}

bool writeProbeMarker(const QString &rootDir, const QString &scopeKey)
{
    QJsonObject o;
    o.insert(QStringLiteral("v"), 1);
    o.insert(QStringLiteral("probedOk"), true);
    return writeAtomically(probeMarkerPath(rootDir, scopeKey), QJsonDocument(o).toJson(QJsonDocument::Indented));
}

bool removeProbeMarker(const QString &rootDir, const QString &scopeKey)
{
    const QString path = probeMarkerPath(rootDir, scopeKey);
    if (path.isEmpty() || !QFileInfo::exists(path))
        return true;   // nothing there: it is gone, which is all the caller asked for
    if (QFile::remove(path))
        return true;
    // Not removed by us; it may still have gone (another instance, a race). Judge by what is there now.
    if (!QFileInfo::exists(path))
        return true;
    TWARN("FolderSyncMeta: cannot remove" << path);
    return false;
}

} // namespace talq::foldersyncmeta
