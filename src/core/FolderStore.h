#pragma once
// The persistence half of sidebar folders: one FolderState per Nextcloud account, loaded from and
// saved to one small JSON file with QSaveFile (an atomic replace). The data rules live in
// FolderLogic.h; this class only owns the state, the file and the "changed" notification.
// Spec: docs/superpowers/specs/2026-11-02-sidebar-folders-design.md
//
// GUI-THREAD ONLY. Nothing here is locked; the sidebar reads state() synchronously from paint
// code, which is the reason this is a file and not the SQLite cache (a worker-thread database
// would make the first paint wait for, or flash without, the folders).
//
// WHY EVERY MUTATION SAVES IMMEDIATELY. There is no debounce: the shutdown watchdog calls
// _Exit(0) six seconds after quit, and the updater closes the app with /CLOSEAPPLICATIONS, so a
// timer that has not fired yet is a lost folder. A state this small writes in well under a
// millisecond. flush() exists for the one case where a write FAILED (disk full, antivirus lock)
// and the in-memory state is ahead of the disk.
//
// WHAT IT NEVER DOES
//  - attach to an account it cannot name: an empty/invalid scope key is refused AND detaches the
//    previous account first, so one account's folders can never show under another's;
//  - overwrite a file it did not understand: a corrupt file is RENAMED (<name>.corrupt-<time>),
//    a file written by a newer build is held read-only and never written back, and if the file
//    cannot even be set aside the store goes read-only rather than write over it;
//  - write while read-only, from flush(), detach() or the destructor either;
//  - touch disk for an unchanged mutation (renaming to the same name, filing under the same
//    folder, collapsing an already-collapsed folder): no write, no changed().

#include "core/FolderLogic.h"

#include <QObject>
#include <QString>

#include <functional>

class FolderStore : public QObject
{
    Q_OBJECT
public:
    explicit FolderStore(QObject *parent = nullptr);
    ~FolderStore() override;   // flush()

    // Where the per-account files live. Default: QStandardPaths::AppDataLocation + "/folders",
    // resolved on first use (not at construction) so it is computed AFTER the application and
    // organisation names are set. Tests point it at a QTemporaryDir.
    void setRootDir(const QString &dir);
    QString rootDir() const;

    // Load <root>/<scopeKey>.json. Any previous scope is flushed and detached first. The key must
    // be talq::accountScopeKey()'s output (16 lowercase hex); anything else -- an empty key above
    // all -- is refused: returns false and the store stays detached.
    //   missing file     -> a fresh empty state, true (nothing is created until the first change)
    //   unreadable file  -> false, detached (never guess, never overwrite)
    //   corrupt file     -> renamed to <name>.corrupt-<yyyyMMddHHmmss> (a counter is appended on a
    //                       clash; never overwritten, never deleted), empty state, true
    //   newer-format file-> held read-only with an EMPTY state, true; every mutator then refuses
    //                       with ReadOnly and nothing is ever written
    //                       (readOnlyReason() == NewerFormat)
    //   corrupt AND it cannot be renamed (held open by another program)
    //                    -> the same read-only, empty state, true
    //                       (readOnlyReason() == CouldNotSetAside)
    // Emits changed() once (when it attached, or when it detached a previous scope).
    bool attach(const QString &scopeKey);

    // Flush, then clear the in-memory state and forget the scope. The file stays on disk, so the
    // layout comes back at the next login. Emits changed() if it was attached.
    void detach();

    // Why a store is read-only. The two causes need different words for the user and different
    // remedies (update TalQ, versus close whatever holds the file and restart), so the cause is
    // kept, not just the fact. FolderResult::ReadOnly still covers both.
    enum ReadOnlyReason {
        None,               // writable (or detached)
        NewerFormat,        // the file was written by a newer build
        CouldNotSetAside    // a damaged file that could not be renamed out of the way
    };

    bool isAttached() const { return m_attached; }
    bool isReadOnly() const { return m_readOnly; }
    ReadOnlyReason readOnlyReason() const { return m_readOnlyReason; }
    const talq::FolderState &state() const { return m_state; }
    QString scopeKey() const { return m_scopeKey; }

    // Mutators. Ok = changed (saved, then changed() emitted -- also when the save itself failed,
    // because the in-memory state did change). Unchanged = nothing to do: no write, no signal.
    // NotAttached / ReadOnly are the store's own refusals; everything else is FolderLogic's.
    talq::FolderResult createFolder(const QString &name, QString *outId = nullptr);
    talq::FolderResult renameFolder(const QString &id, const QString &name);
    talq::FolderResult deleteFolder(const QString &id);
    talq::FolderResult moveFolder(const QString &id, int delta);
    talq::FolderResult assign(const QString &token, const QString &folderId /* empty = unfile */);
    talq::FolderResult setCollapsed(const QString &id, bool collapsed);
    talq::FolderResult setOtherCollapsed(bool collapsed);

    // Write now if the last write failed (or never happened). A no-op when clean, detached or
    // read-only.
    void flush();

    // Test/diagnostic seams: unsaved changes exist (a write failed), and how many writes have
    // succeeded since construction.
    bool isDirty() const { return m_dirty; }
    int saveCount() const { return m_saveCount; }

signals:
    // After every successful mutation, attach and detach. The state is already updated and saved
    // (or the save has failed and been logged) when this fires.
    void changed();

private:
    talq::FolderResult apply(const std::function<talq::FolderResult(talq::FolderState &)> &fn);
    bool load(const QString &scopeKey);
    bool setAsideCorruptFile(const QString &path);
    bool save();
    QString filePathFor(const QString &scopeKey) const;
    void detachSilently();

    mutable QString m_rootDir;     // resolved lazily by rootDir()
    QString m_scopeKey;
    QString m_path;                // <root>/<scopeKey>.json while attached
    talq::FolderState m_state;
    bool m_attached = false;
    bool m_readOnly = false;
    ReadOnlyReason m_readOnlyReason = None;   // None exactly when !m_readOnly
    bool m_dirty = false;
    int m_saveCount = 0;
};
