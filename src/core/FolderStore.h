#pragma once
// The persistence half of sidebar folders: one FolderState per Nextcloud account, loaded from and
// saved to one small JSON file on this PC with QSaveFile (an atomic replace). The data rules live in
// FolderLogic.h; this class only owns the state, the file and its signals (changed, localEdited,
// saved). It never talks to any server. Folders are local by default; with the opt-in folder sync
// switch on, FolderSyncController reads this store's facts, keeps a copy in the user's own Nextcloud
// Files (never the Talk server) and hands a merge back through applyMerged(). The local file stays
// byte for byte what 0.79.4 writes; the sync files (<key>.sync.json, <key>.probed) are not this
// class's.
// Spec: docs/superpowers/specs/2026-11-02-sidebar-folders-design.md; sync:
// docs/superpowers/specs/2026-11-03-folder-sync-design.md
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
//    folder, collapsing an already-collapsed folder): no write, no changed();
//  - take a merge from another computer it cannot vouch for: applyMerged() refuses a scope or a
//    generation that has moved on, a state over the caps or one that would need repairing, and
//    (like every mutator) a read-only store. Before it applies a merge that removes a folder or a
//    filing it TRIES to keep one rolling <file>.pre-sync copy of the file, best effort: there is no
//    copy when no file exists, when the file is empty, over 4 MiB or unreadable, or when the copy
//    cannot be written (logged, and the apply goes ahead).

#include "core/FolderLogic.h"

#include <QByteArray>
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
    // Emits changed() once (when it attached, or when it detached a previous scope). Flushing the
    // previous scope may emit saved() for THAT scope first. Never emits localEdited(). Records the
    // load outcome and the hashes below, resets localEditCount() and bumps generation().
    bool attach(const QString &scopeKey);

    // Flush, then clear the in-memory state and forget the scope. The file stays on disk, so the
    // layout comes back at the next login. Emits changed() if it was attached. The flush may emit
    // saved() for the scope being left; a detach bumps generation().
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

    // Write the current state once, even when nothing is pending. For a file that loaded as
    // Repaired or SetAside, whose damage an Unchanged mutation would never rewrite (folder-sync
    // spec D30). false when detached or read-only (nothing is written) and when the write fails;
    // a failed forceSave leaves isDirty() as it was, because a clean store's file still holds
    // whatever it held before. Emits saved() on success. Changes neither generation() nor
    // localEditCount(), and emits neither changed() nor localEdited(): the state did not change.
    bool forceSave();

    // Test/diagnostic seams: unsaved changes exist (a write failed), and how many writes have
    // succeeded since construction.
    bool isDirty() const { return m_dirty; }
    int saveCount() const { return m_saveCount; }

    // ----- Facts for the folder-sync controller (inert for a store nobody syncs) ------------------
    // How attach() found the file. Detached while no account is attached (also after a refused or
    // unreadable attach). Missing = no file; Ok = valid, nothing dropped; Repaired = valid but
    // entries were dropped or cleaned; SetAside = damaged and renamed away; ReadOnlyLoad = a newer
    // format, or damaged and could not be renamed (readOnlyReason() says which).
    talq::FolderLoadOutcome loadOutcome() const { return m_loadOutcome; }

    // Hex SHA-1 of the bytes attach() read, a zero-byte file included (the hash of empty input).
    // Empty ONLY when the outcome is Missing (and Detached). For a damaged file that was set aside
    // it is the hash of the bytes that were read (at most kMaxFolderFileBytes + 1 of them). Fixed
    // for the whole attach: later writes do not change it.
    QByteArray loadedSha() const { return m_loadedSha; }

    // Hex SHA-1 of the file now at the path, as this build last read or wrote it: set by attach(),
    // then by every successful write. Empty while no file exists, which includes after a damaged
    // file was set aside. It is NOT "the sha1Hex that saved() last carried": right after attach()
    // no saved() has fired for that attach yet (this is then the hash attach() read), after a
    // set-aside attach it is empty, and after a scope switch the last saved() named another scope.
    // A diagnostic and test seam only: production code that needs the hash of what THIS build
    // wrote must take it from saved(), never read it back from here.
    QByteArray fileSha() const { return m_fileSha; }

    // Bumped by every mutation that changed the state (apply() and applyMerged(), also when the
    // write then failed: memory did change), by every successful attach() and by detach() of an
    // attached store. It never goes back, not even across detach and attach, so a number
    // remembered across an async gap names one particular state of one particular attach.
    // (attach() onto an attached store detaches first, so it bumps twice.)
    quint64 generation() const { return m_generation; }

    // How many user mutations changed the state since the last attach (apply() only; applyMerged()
    // is a remote merge and does not count). Zero after attach and while detached.
    int localEditCount() const { return m_localEditCount; }

    // ----- Applying a merge that came from another computer (folder sync) ------------------------
    enum class ApplyResult {
        Applied,          // the state was replaced and the file written
        AppliedNotSaved,  // the state was replaced in memory, the write failed: the store is dirty
        Unchanged,        // already equal to the current state: no write, no signal, no generation bump.
                          // It says nothing about the DISK: the store may still be dirty from an
                          // earlier failed write (isDirty() tells), so it is not "saved"
        Stale,          // another scope is attached, or generation() moved since the caller looked
        NotAttached,
        ReadOnly,
        Invalid           // over the caps, or sanitiseFolderState() would change it: refused whole
    };

    // Replace the whole state with `merged`, which the sync controller computed from a snapshot it
    // took at `expectedGeneration` for `scopeKey`. Refusals, checked in this order, touch nothing
    // (no write, no backup, no signal, no counter): NotAttached; a scope other than the attached one
    // is Stale; ReadOnly; generation() != expectedGeneration is Stale (the user edited while the
    // network call was out: the caller merges again, so a mid-flight edit is never clobbered);
    // more than kMaxFolders folders or kMaxAssignments filings, or a state sanitiseFolderState()
    // would change, is Invalid; sameFolderState() is Unchanged (no write, whatever the disk holds).
    // Otherwise, when the merge removes a folder or a filing (talq::removesFoldersOrFilings), the
    // file now on disk is first copied to preSyncPath(), best effort (no file, an empty, oversized
    // or unreadable file, or a failed copy means no backup: it is logged and does not stop the
    // apply); then the state is replaced, generation() is bumped, the store is marked dirty,
    // the file is written (saved() fires from inside on success) and changed() is emitted. It
    // never emits localEdited() and never counts as a local edit: only apply() starts an upload.
    // AppliedNotSaved keeps the merged state in memory and the store dirty; flush() or the next
    // change writes it, exactly as for apply().
    // WHAT THE CALLER MAY RECORD. Whatever the result, Unchanged included, the caller must not
    // write the sidecar (a base or a localSha) while isDirty() is true: the state it would
    // describe is not on disk, and after a restart the file would read as a local deletion of
    // whatever the merge added (spec E23). An Unchanged right after an AppliedNotSaved is the
    // realistic case: the next poll merges the same state, finds nothing to do, and the store is
    // still dirty. The caller's in-memory base may move; only writing the sidecar is wrong.
    ApplyResult applyMerged(const QString &scopeKey, quint64 expectedGeneration, const talq::FolderState &merged);

    // <file path>.pre-sync: the ONE rolling copy of the file as it was before the last apply that
    // removed a folder or a filing (a hand-recovery path, nothing reads it back). Empty while
    // detached. It exists only after such an apply, and only if the copy could be made: it is best
    // effort (none when no file existed, when the file was empty, over 4 MiB or unreadable, or when
    // the copy could not be written). An apply that only adds or edits makes none.
    QString preSyncPath() const { return m_attached ? m_path + QStringLiteral(".pre-sync") : QString(); }

signals:
    // After every successful mutation (apply() and applyMerged()), attach and detach. The state is
    // already updated and saved (or the save has failed and been logged) when this fires.
    void changed();

    // After changed(), for a user mutation (apply()) only: never for attach, detach, flush(),
    // forceSave() or applyMerged(). A listener that schedules an upload of the folders listens to
    // this and not to changed(), which is what keeps a remote apply from scheduling an upload.
    void localEdited();

    // After EVERY successful write of the file: a mutation (apply() or applyMerged()), forceSave(),
    // a flush() retry after a failed write, and the flush attach() and detach() do for the scope
    // they are leaving (that is why it names the scope: it can be the previous one). sha1Hex is
    // the hex SHA-1 of the exact bytes that were written. Never emitted for a failed write. For a
    // mutation it fires from inside the mutator, before changed(). Also fires from the destructor's
    // flush: a listener must not be a receiver that is already gone by then.
    // A slot must only RECORD (scope, sha). It must not attach() or detach() this store, nor
    // mutate it: detachSilently() flushes and then clears the state, so a re-entrant attach() from
    // inside a flush would be wiped by the detach that is running around it.
    void saved(const QString &scopeKey, const QByteArray &sha1Hex);

private:
    talq::FolderResult apply(const std::function<talq::FolderResult(talq::FolderState &)> &fn);
    bool load(const QString &scopeKey);
    bool setAsideCorruptFile(const QString &path);
    void makePreSyncCopy();
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
    talq::FolderLoadOutcome m_loadOutcome = talq::FolderLoadOutcome::Detached;
    QByteArray m_loadedSha;        // hex SHA-1 of the bytes attach() read; empty only for Missing/Detached
    QByteArray m_fileSha;          // hex SHA-1 of the file at m_path as last read or written; empty if none
    quint64 m_generation = 0;
    int m_localEditCount = 0;
};
