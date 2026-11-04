#include "FolderStore.h"

#include "core/FolderSyncLogic.h"
#include "core/TalqLog.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>

using talq::FolderLoadOutcome;
using talq::FolderResult;
using talq::FolderState;

namespace {

// Lowercase hex SHA-1, the form the folder-sync sidecar stores. A fingerprint for "is this the file
// I wrote", not a security measure.
QByteArray sha1Hex(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex();
}

} // namespace

FolderStore::FolderStore(QObject *parent)
    : QObject(parent)
{
}

FolderStore::~FolderStore()
{
    flush();
}

void FolderStore::setRootDir(const QString &dir)
{
    m_rootDir = dir;
}

QString FolderStore::rootDir() const
{
    if (m_rootDir.isEmpty()) {
        // Resolved HERE, not in the constructor: main() sets the organisation/application names
        // after some objects are built, and AppDataLocation is derived from them.
        m_rootDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                    + QStringLiteral("/folders");
    }
    return m_rootDir;
}

QString FolderStore::filePathFor(const QString &scopeKey) const
{
    // scopeKey was checked by isScopeKey() (16 lowercase hex), so it cannot carry a separator,
    // "..", a drive colon or a device name.
    return QDir(rootDir()).filePath(scopeKey + QStringLiteral(".json"));
}

// ---------------------------------------------------------------------------------------------
// attach / detach

bool FolderStore::attach(const QString &scopeKey)
{
    const bool wasAttached = m_attached;
    // Always drop the previous account first -- also when the new key turns out to be unusable.
    // Leaving the old scope attached on a refused attach is how one account's folders would end
    // up showing under another's.
    detachSilently();

    bool ok = false;
    if (!talq::isScopeKey(scopeKey))
        TWARN("FolderStore: refusing to attach to an invalid scope key (empty or not 16 hex characters)");
    else
        ok = load(scopeKey);

    if (wasAttached || ok)
        emit changed();
    return ok;
}

void FolderStore::detach()
{
    const bool wasAttached = m_attached;
    detachSilently();
    if (wasAttached)
        emit changed();
}

void FolderStore::detachSilently()
{
    if (!m_attached)
        return;
    flush();   // may emit saved() for the scope being left: m_scopeKey is still set until below
    m_attached = false;
    m_readOnly = false;
    m_readOnlyReason = None;
    m_dirty = false;
    m_scopeKey.clear();
    m_path.clear();
    m_state = FolderState();
    m_loadOutcome = FolderLoadOutcome::Detached;
    m_loadedSha.clear();
    m_fileSha.clear();
    m_localEditCount = 0;
    ++m_generation;   // a reply that was in flight for this attach is now stale
}

bool FolderStore::load(const QString &scopeKey)
{
    const QString path = filePathFor(scopeKey);
    FolderState loaded;
    ReadOnlyReason reason = None;
    // Worked out here and assigned to the members only at the end, so the two early "return false"
    // paths below leave nothing half-set.
    FolderLoadOutcome outcome = FolderLoadOutcome::Missing;
    QByteArray loadedSha;   // SHA-1 of the bytes read; stays empty only when there was no file
    QByteArray fileSha;     // SHA-1 of the file at the path now; empty when none is left there

    if (QFileInfo::exists(path)) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            // Present but unreadable (a lock, a permission): we cannot tell whether it is damaged,
            // and starting empty would let the next save replace it. Stay detached.
            TWARN("FolderStore: cannot read" << path << ":" << f.errorString());
            return false;
        }
        // One byte past the cap is enough to tell "too big" without reading an arbitrary file whole.
        const QByteArray bytes = f.read(talq::kMaxFolderFileBytes + 1);
        const bool readFailed = f.error() != QFileDevice::NoError;
        f.close();   // Windows will not rename a file that is still open
        if (readFailed) {
            TWARN("FolderStore: read error on" << path);
            return false;
        }

        // Hashed as read, before decoding. A zero-byte file is a file: it hashes to the SHA-1 of
        // empty input, which is NOT the empty string a missing file leaves in loadedSha.
        loadedSha = sha1Hex(bytes);
        fileSha = loadedSha;

        const talq::DecodeResult d = talq::decodeFolders(bytes);
        switch (d.status) {
        case talq::DecodeStatus::Ok:
            loaded = d.state;
            outcome = d.repaired ? FolderLoadOutcome::Repaired : FolderLoadOutcome::Ok;
            if (d.repaired)
                TWARN("FolderStore: dropped unusable entries while loading" << path);
            break;
        case talq::DecodeStatus::FutureVersion:
            // Written by a newer build (the user went back to an older one). Keep the file exactly
            // as it is; show no folders; refuse every change.
            reason = NewerFormat;
            outcome = FolderLoadOutcome::ReadOnlyLoad;
            TWARN("FolderStore:" << path << "is a newer format; folders are read-only in this version");
            break;
        case talq::DecodeStatus::Corrupt:
            if (setAsideCorruptFile(path)) {
                outcome = FolderLoadOutcome::SetAside;
                fileSha.clear();   // the path is empty now; the bytes live on under .corrupt-<time>
                TWARN("FolderStore:" << path << "is damaged; it was renamed and folders start empty");
            } else {
                // Could not move it out of the way, and the next save would replace it.
                reason = CouldNotSetAside;
                outcome = FolderLoadOutcome::ReadOnlyLoad;
                TWARN("FolderStore:" << path << "is damaged and could not be renamed; folders are read-only");
            }
            break;
        }
    }

    m_scopeKey = scopeKey;
    m_path = path;
    m_state = loaded;
    m_readOnly = reason != None;
    m_readOnlyReason = reason;
    m_dirty = false;
    m_attached = true;
    m_loadOutcome = outcome;
    m_loadedSha = loadedSha;
    m_fileSha = fileSha;
    m_localEditCount = 0;
    ++m_generation;   // a new attach: whatever a caller remembered about an earlier one is stale
    return true;
}

// <name>.corrupt-<yyyyMMddHHmmss>, with -2, -3, ... appended on a clash. QFile::rename never
// overwrites, so an earlier set-aside file is safe even if the loop gave up.
bool FolderStore::setAsideCorruptFile(const QString &path)
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss"));
    QString target = path + QStringLiteral(".corrupt-") + stamp;
    for (int n = 2; QFileInfo::exists(target) && n < 1000; ++n)
        target = path + QStringLiteral(".corrupt-") + stamp + QLatin1Char('-') + QString::number(n);
    if (QFileInfo::exists(target))
        return false;
    return QFile::rename(path, target);
}

// ---------------------------------------------------------------------------------------------
// writing

bool FolderStore::save()
{
    if (!m_attached || m_readOnly)
        return false;   // belt and braces: a read-only store must never reach the disk

    const QString dir = rootDir();
    if (!QDir().mkpath(dir)) {
        TWARN("FolderStore: cannot create" << dir << "- folders are kept in memory and saved on the next change");
        return false;
    }
    const QByteArray bytes = talq::encodeFolders(m_state);
    QSaveFile f(m_path);   // temp file in the same directory, renamed over the target on commit()
    if (!f.open(QIODevice::WriteOnly)) {
        TWARN("FolderStore: cannot write" << m_path << ":" << f.errorString()
              << "- folders are kept in memory and saved on the next change");
        return false;
    }
    if (f.write(bytes) != bytes.size()) {
        const QString why = f.errorString();
        f.cancelWriting();
        TWARN("FolderStore: short write to" << m_path << ":" << why);
        return false;
    }
    if (!f.commit()) {
        TWARN("FolderStore: cannot replace" << m_path << ":" << f.errorString()
              << "- folders are kept in memory and saved on the next change");
        return false;
    }
    m_dirty = false;
    ++m_saveCount;
    // The ONE place every successful write passes: the hash is of the exact bytes just committed,
    // and the signal goes out last, after the store is consistent, and only for a write that
    // happened. A slot must only record (see the header), so copying the scope is belt and braces
    // against one that does not: the argument stays valid whatever happens to the member.
    const QByteArray sha = sha1Hex(bytes);
    m_fileSha = sha;
    const QString scope = m_scopeKey;
    emit saved(scope, sha);
    return true;
}

void FolderStore::flush()
{
    if (m_attached && !m_readOnly && m_dirty)
        save();
}

bool FolderStore::forceSave()
{
    if (!m_attached || m_readOnly)
        return false;
    return save();   // m_dirty is cleared only by a write that succeeded; a failure leaves it as it was
}

// ---------------------------------------------------------------------------------------------
// mutators

FolderResult FolderStore::apply(const std::function<FolderResult(FolderState &)> &fn)
{
    if (!m_attached)
        return FolderResult::NotAttached;
    if (m_readOnly)
        return FolderResult::ReadOnly;
    const FolderResult r = fn(m_state);
    if (r == FolderResult::Ok) {
        ++m_generation;       // the state changed, whether or not the write below succeeds
        ++m_localEditCount;   // a user mutation: apply() is the only caller that counts
        m_dirty = true;       // stays set if the save fails, so flush() and the next change retry
        save();
        emit changed();
        emit localEdited();   // after changed(); nothing else emits it
    }
    return r;
}

FolderStore::ApplyResult FolderStore::applyMerged(const QString &scopeKey, quint64 expectedGeneration,
                                                  const FolderState &merged)
{
    // The order is part of the contract (spec, "Store changes"): a call that is refused for an
    // earlier reason must not be told about a later one, and a refusal touches nothing -- no write,
    // no backup, no signal, no counter.
    if (!m_attached)
        return ApplyResult::NotAttached;
    if (scopeKey != m_scopeKey)
        return ApplyResult::Stale;       // the reply belongs to an account that is no longer attached
    if (m_readOnly)
        return ApplyResult::ReadOnly;
    if (m_generation != expectedGeneration)
        return ApplyResult::Stale;       // the user (or an attach) moved on while the network call was out

    // The caps are checked on their own because sanitiseFolderState() caps the folders but not the
    // filings; the sanitise pass then refuses anything it would change (a bad id or token, an orphan
    // filing, an unnormalised name). A merged state is never repaired here: the caller's merge is wrong.
    if (merged.folders.size() > talq::kMaxFolders || merged.assign.size() > talq::kMaxAssignments)
        return ApplyResult::Invalid;
    FolderState probe = merged;
    if (talq::sanitiseFolderState(probe))
        return ApplyResult::Invalid;

    if (talq::sameFolderState(m_state, merged))
        return ApplyResult::Unchanged;

    // Decided against the state as it is NOW, before it is replaced, and the copy is taken before
    // anything is written: afterwards the file already holds the merge.
    if (talq::removesFoldersOrFilings(m_state, merged))
        makePreSyncCopy();

    m_state = merged;
    ++m_generation;   // the state changed, whether or not the write below succeeds
    m_dirty = true;   // stays set if the save fails, so flush() and the next change retry
    const bool written = save();   // emits saved() from its success branch
    emit changed();   // never localEdited(): a remote apply must not schedule an upload
    return written ? ApplyResult::Applied : ApplyResult::AppliedNotSaved;
}

// D32: ONE rolling copy of the file, <file path>.pre-sync, written with QSaveFile (an atomic replace)
// so an older copy is replaced in one step and is never lost to a copy that then fails. It is the
// bytes of the FILE, read from disk: when an earlier write failed memory is ahead of the disk, and
// the disk is what the apply is about to replace. Best effort: no file, an empty file (it would
// replace a good older copy with nothing), an unreadable or oversized file, or a copy that cannot be
// written is logged and the apply goes ahead (a backup failure must not block a sync).
void FolderStore::makePreSyncCopy()
{
    if (!QFileInfo::exists(m_path))
        return;   // nothing on disk to keep (a state that never reached it, or a file that went away)

    QFile in(m_path);
    if (!in.open(QIODevice::ReadOnly)) {
        TWARN("FolderStore: cannot read" << m_path << "for the .pre-sync copy:" << in.errorString()
              << "- applying without one");
        return;
    }
    // One byte past the cap is enough to tell "too big" without reading an arbitrary file whole.
    const QByteArray bytes = in.read(talq::kMaxFolderFileBytes + 1);
    const bool readFailed = in.error() != QFileDevice::NoError;
    in.close();
    if (readFailed || bytes.size() > talq::kMaxFolderFileBytes) {
        TWARN("FolderStore: cannot copy" << m_path << "to .pre-sync (unreadable or larger than any file of ours) - applying without one");
        return;
    }
    if (bytes.isEmpty()) {
        // Same as no file (R13): an empty copy is worthless, and writing it would replace the one
        // older copy that may still be worth something. Not writing is the safe choice.
        TWARN("FolderStore:" << m_path << "is empty; not copying it over the .pre-sync copy - applying without a new one");
        return;
    }

    const QString target = preSyncPath();
    QSaveFile out(target);   // temp file in the same directory, renamed over the target on commit()
    if (!out.open(QIODevice::WriteOnly)) {
        TWARN("FolderStore: cannot write" << target << ":" << out.errorString() << "- applying without a backup");
        return;
    }
    if (out.write(bytes) != bytes.size()) {
        const QString why = out.errorString();
        out.cancelWriting();
        TWARN("FolderStore: short write to" << target << ":" << why << "- applying without a backup");
        return;
    }
    if (!out.commit())
        TWARN("FolderStore: cannot replace" << target << ":" << out.errorString() << "- applying without a backup");
}

FolderResult FolderStore::createFolder(const QString &name, QString *outId)
{
    if (outId)
        outId->clear();
    return apply([&](FolderState &s) {
        return talq::createFolder(s, name, QRandomGenerator::global()->generate64(), outId);
    });
}

FolderResult FolderStore::renameFolder(const QString &id, const QString &name)
{
    return apply([&](FolderState &s) { return talq::renameFolder(s, id, name); });
}

FolderResult FolderStore::deleteFolder(const QString &id)
{
    return apply([&](FolderState &s) { return talq::deleteFolder(s, id); });
}

FolderResult FolderStore::moveFolder(const QString &id, int delta)
{
    return apply([&](FolderState &s) { return talq::moveFolder(s, id, delta); });
}

FolderResult FolderStore::assign(const QString &token, const QString &folderId)
{
    return apply([&](FolderState &s) { return talq::assignToken(s, token, folderId); });
}

FolderResult FolderStore::setCollapsed(const QString &id, bool collapsed)
{
    return apply([&](FolderState &s) { return talq::setFolderCollapsed(s, id, collapsed); });
}

FolderResult FolderStore::setOtherCollapsed(bool collapsed)
{
    return apply([&](FolderState &s) { return talq::setOtherCollapsed(s, collapsed); });
}
