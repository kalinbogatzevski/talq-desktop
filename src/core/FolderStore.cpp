#include "FolderStore.h"

#include "core/TalqLog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>

using talq::FolderResult;
using talq::FolderState;

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
    flush();
    m_attached = false;
    m_readOnly = false;
    m_readOnlyReason = None;
    m_dirty = false;
    m_scopeKey.clear();
    m_path.clear();
    m_state = FolderState();
}

bool FolderStore::load(const QString &scopeKey)
{
    const QString path = filePathFor(scopeKey);
    FolderState loaded;
    ReadOnlyReason reason = None;

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

        const talq::DecodeResult d = talq::decodeFolders(bytes);
        switch (d.status) {
        case talq::DecodeStatus::Ok:
            loaded = d.state;
            if (d.repaired)
                TWARN("FolderStore: dropped unusable entries while loading" << path);
            break;
        case talq::DecodeStatus::FutureVersion:
            // Written by a newer build (the user went back to an older one). Keep the file exactly
            // as it is; show no folders; refuse every change.
            reason = NewerFormat;
            TWARN("FolderStore:" << path << "is a newer format; folders are read-only in this version");
            break;
        case talq::DecodeStatus::Corrupt:
            if (setAsideCorruptFile(path)) {
                TWARN("FolderStore:" << path << "is damaged; it was renamed and folders start empty");
            } else {
                // Could not move it out of the way, and the next save would replace it.
                reason = CouldNotSetAside;
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
    return true;
}

void FolderStore::flush()
{
    if (m_attached && !m_readOnly && m_dirty)
        save();
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
        m_dirty = true;   // stays set if the save fails, so flush() and the next change retry
        save();
        emit changed();
    }
    return r;
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
