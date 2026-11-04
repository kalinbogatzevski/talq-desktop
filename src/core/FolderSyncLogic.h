#pragma once

// FolderSyncLogic -- the pure half of folder sync: how the shared remote file is read, how two
// folder states are compared and diffed, the three-way merge of base, local and remote, and the
// sidecar that records the base together with the rule for when that base may be believed. Qt Core
// value types only: no I/O, no clock, no network, no QObject, so a test can pin every rule and the
// controller carries none of them. See tests/folder_sync_logic_test.cpp.
// Spec: docs/superpowers/specs/2026-11-03-folder-sync-design.md
//
// Every function takes SANITISED states (sanitiseFolderState changed nothing): a state read from the
// local file was repaired at load, and a remote body that needed repair never reaches a comparison
// because decodeRemote calls it Unreadable.
//
// There is deliberately no operator== on Folder or FolderState: a free one in namespace talq could
// collide with a future member or another header's, so the comparison is the named function
// sameFolderState.

#include "core/FolderLogic.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QChar>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <cmath>
#include <utility>

namespace talq {

// The "v" of the SHARED file, separate from kFolderFormatVersion (the local file). Any additive
// change to the remote body bumps it: a writer rebuilds the file from FolderState, so a key it does
// not know is dropped on its next write, and an older sync build must pause on a newer "v" instead.
constexpr int kFolderSyncRemoteVersion = 1;

// The sidecar (<scopeKey>.sync.json) is read with this limit; over it is Unreadable (an untrusted
// base, the same outcome as no sidecar).
constexpr qint64 kMaxSidecarBytes = 8 * 1024 * 1024;

// "Empty" is no folders and no filings. otherCollapsed alone does not make a state non-empty: it is
// a display flag with nothing in it to lose.
inline bool isEmptyFolderState(const FolderState &s)
{
    return s.folders.isEmpty() && s.assign.isEmpty();
}

// Order, ids, names (exact: a case-only rename is a difference), collapsed flags, otherCollapsed
// and the filing map. Folder is compared field by field here; it has no operator== on purpose.
inline bool sameFolderState(const FolderState &a, const FolderState &b)
{
    if (a.otherCollapsed != b.otherCollapsed || a.folders.size() != b.folders.size())
        return false;
    for (int i = 0; i < a.folders.size(); ++i) {
        const Folder &x = a.folders.at(i);
        const Folder &y = b.folders.at(i);
        if (x.id != y.id || x.name != y.name || x.collapsed != y.collapsed)
            return false;
    }
    return a.assign == b.assign;
}

// What changed between two states, in the terms a notice and a backup decision need. A folder is
// its id: a name is not identity.
struct StateDiff {
    int foldersAdded = 0;         // ids in `to` that `from` lacks
    int foldersRemoved = 0;       // ids in `from` that `to` lacks
    int foldersRenamed = 0;       // ids in both whose name differs (exact: a case-only rename counts)
    int chatsChanged = 0;         // tokens whose folder differs; unfiled is the folder "none", so a
                                  // chat filed, unfiled or moved counts once, and so does one that
                                  // fell out of a folder because the folder was deleted
    bool orderChanged = false;    // the folders present on BOTH sides are in a different relative
                                  // order; an add, a remove or an append alone does not set it
    bool collapsedChanged = false;  // a folder present on both sides changed its collapsed flag, or
                                    // the built-in "Other" section did

    // Worth telling the user about. A collapse is a view setting, not a change to their folders, so
    // collapsedChanged alone is NOT visible (the owner's Q2 iii baseline).
    bool visible() const
    {
        return foldersAdded > 0 || foldersRemoved > 0 || foldersRenamed > 0 || chatsChanged > 0
            || orderChanged;
    }
};

inline StateDiff diffStates(const FolderState &from, const FolderState &to)
{
    StateDiff d;

    QHash<QString, const Folder *> fromById;
    for (const Folder &f : from.folders)
        fromById.insert(f.id, &f);

    QSet<QString> toIds;
    QStringList commonInToOrder;
    for (const Folder &f : to.folders) {
        toIds.insert(f.id);
        const auto it = fromById.constFind(f.id);
        if (it == fromById.constEnd()) {
            ++d.foldersAdded;
            continue;
        }
        commonInToOrder.append(f.id);
        if (it.value()->name != f.name)
            ++d.foldersRenamed;
        if (it.value()->collapsed != f.collapsed)
            d.collapsedChanged = true;
    }

    QStringList commonInFromOrder;
    for (const Folder &f : from.folders) {
        if (toIds.contains(f.id))
            commonInFromOrder.append(f.id);
        else
            ++d.foldersRemoved;
    }

    d.orderChanged = commonInFromOrder != commonInToOrder;
    if (from.otherCollapsed != to.otherCollapsed)
        d.collapsedChanged = true;

    QSet<QString> tokens;
    for (auto it = from.assign.constBegin(); it != from.assign.constEnd(); ++it)
        tokens.insert(it.key());
    for (auto it = to.assign.constBegin(); it != to.assign.constEnd(); ++it)
        tokens.insert(it.key());
    for (const QString &t : std::as_const(tokens)) {
        if (folderOf(from, t) != folderOf(to, t))
            ++d.chatsChanged;
    }
    return d;
}

// Would going from `from` to `to` take something away: a folder id of `from` that `to` lacks, or a
// chat filed in `from` that is unfiled (or absent) in `to`. A rename, a collapse, a reorder, a move
// to another folder and every addition are not removals. The caller keeps a backup before applying
// a state for which this is true.
inline bool removesFoldersOrFilings(const FolderState &from, const FolderState &to)
{
    QSet<QString> toIds;
    for (const Folder &f : to.folders)
        toIds.insert(f.id);
    for (const Folder &f : from.folders) {
        if (!toIds.contains(f.id))
            return true;
    }
    for (auto it = from.assign.constBegin(); it != from.assign.constEnd(); ++it) {
        if (folderOf(to, it.key()).isEmpty())
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// The remote file

enum class RemoteKind {
    Absent,      // zero bytes, or only whitespace (after an optional BOM): nobody wrote anything yet
    Present,     // a readable file of a version this build knows, nothing repaired; `state` is set
    Newer,       // "v" above kFolderSyncRemoteVersion: a newer TalQ wrote it; never write it back
    Unreadable   // not a file this build can trust: never merge it, never overwrite it
};

struct RemoteDecode {
    RemoteKind kind = RemoteKind::Absent;
    FolderState state;   // meaningful only for Present; empty otherwise
};

// A wrapper over decodeFolders(bytes, kFolderSyncRemoteVersion) with the remote's own readings.
//  - Absent is not "an empty state". decodeFolders calls a zero-byte body an empty state, but for
//    the REMOTE it means nobody has written yet (like a 404); reading it as "everyone deleted
//    everything" would wipe every computer. Only a readable file WITH a "v" is evidence of that
//    (`{"v":1}` is Present with an empty state).
//  - A body that decodeFolders had to REPAIR is Unreadable. It always runs sanitiseFolderState, so
//    51 to 1000 folders, a bad or duplicate id, an invalid token, an orphan assignment or an
//    unnormalised name decode as Ok with `repaired` set; merging that would silently cut the file
//    down, and the cycle would then upload the cut-down copy.
//  - The size is judged here, before anything else, rather than trusted to decodeFolders, and a
//    huge all-whitespace body is Unreadable, not Absent.
inline RemoteDecode decodeRemote(const QByteArray &bytes)
{
    RemoteDecode out;
    if (bytes.size() > kMaxFolderFileBytes) {
        out.kind = RemoteKind::Unreadable;
        return out;
    }

    // The same optional UTF-8 byte-order mark decodeFolders skips: a BOM alone is an empty file,
    // not a readable one.
    QByteArrayView body(bytes);
    if (body.startsWith("\xEF\xBB\xBF"))
        body = body.sliced(3);
    if (body.trimmed().isEmpty())
        return out;

    DecodeResult d = decodeFolders(bytes, kFolderSyncRemoteVersion);
    switch (d.status) {
    case DecodeStatus::Ok:
        if (d.repaired) {
            out.kind = RemoteKind::Unreadable;
        } else {
            out.kind = RemoteKind::Present;
            out.state = std::move(d.state);
        }
        break;
    case DecodeStatus::FutureVersion:
        out.kind = RemoteKind::Newer;
        break;
    case DecodeStatus::Corrupt:
        out.kind = RemoteKind::Unreadable;
        break;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// The three-way merge (spec "Merge model", Steps 1 to 6; worked examples E1 to E14 and E24)
//
//   base   = the state at this PC's last successful sync, or the EMPTY state when the base is not
//            trusted (D12);
//   local  = this PC's state now;   remote = the shared file's state now.
//
// A folder is its id, a filed chat is its token. There is no timestamp, counter or tombstone: a
// deletion is known only because the base has a folder and one side does not (D9), so an untrusted
// (empty) base makes the merge a union and a deleted folder may come back -- the safe direction.
// No wall clock is read: "the merging PC wins" means the PC that syncs second, as the server's
// If-Match sequence ordered it.

enum class MergePrefer {
    Local,    // the base is TRUSTED: on a real conflict the merging PC wins
    Remote    // the base is NOT trusted (the caller passes the empty state): the remote wins
};

enum class MergeStatus {
    Ok,                   // `merged` is the state to apply and, if it differs, upload
    TooManyFolders,       // the result would hold more than kMaxFolders folders: refused
    TooManyAssignments,   // the result would hold more than kMaxAssignments filed chats: refused
    Invalid               // the result failed the sanitise self-check (an input that was not
                          // sanitised, or a bug in here): refused
};

struct MergeResult {
    MergeStatus status = MergeStatus::Ok;
    FolderState merged;       // Ok only. A refused merge carries the empty state and zero counts:
                              // there is nothing to apply and nothing to upload.
    int foldersPaired = 0;    // local folders matched to a remote folder by name (Step 1)
    int foldersDropped = 0;   // folders in base AND local that the remote lacks (Step 2): what the
                              // REMOTE takes away from this PC. A folder this PC deleted itself is
                              // never counted, so a PC cannot brake on its own deletion (D29).
};

namespace folder_sync_detail {

// id -> index into s.folders.
inline QHash<QString, int> folderIndexById(const FolderState &s)
{
    QHash<QString, int> idx;
    idx.reserve(s.folders.size());
    for (int i = 0; i < s.folders.size(); ++i)
        idx.insert(s.folders.at(i).id, i);
    return idx;
}

inline QStringList folderIdsInOrder(const FolderState &s)
{
    QStringList ids;
    ids.reserve(s.folders.size());
    for (const Folder &f : s.folders)
        ids.append(f.id);
    return ids;
}

// The folder `token` is filed under in `s`, or EMPTY when unfiled. folderOf's meaning (an orphan is
// unfiled) without its linear scan: `ids` is s's own folderIndexById.
inline QString filingIn(const FolderState &s, const QHash<QString, int> &ids, const QString &token)
{
    const auto it = s.assign.constFind(token);
    if (it == s.assign.constEnd() || !ids.contains(it.value()))
        return QString();
    return it.value();
}

// Step 3's rule for one field. `hasBase`: the base has an opinion on this field (for a folder's
// own fields, the folder is in the base; for the section flag otherCollapsed, the base is trusted).
template <typename T>
inline T pick3(bool hasBase, const T &b, const T &l, const T &r, MergePrefer prefer)
{
    if (l == r)
        return l;
    if (hasBase && l == b)
        return r;   // only the remote changed it
    if (hasBase && r == b)
        return l;   // only the local changed it
    return prefer == MergePrefer::Local ? l : r;   // both changed it, or there is no base to judge by
}

// Step 4's "effective" value of one side. A side that has the chat unfiled while the base's folder
// is gone from that side did not unfile it: deleting the folder did. That counts as "unchanged".
inline QString effectiveFiling(const QString &b, const QString &side,
                               const QHash<QString, int> &sideIds)
{
    if (!b.isEmpty() && side.isEmpty() && !sideIds.contains(b))
        return b;
    return side;
}

// Step 5: place sideOrder[at] immediately before the nearest FOLLOWING folder of the same side's
// list that is already placed; append it when none follows. (Anchoring on the PRECEDING folder was
// rejected: it drags a new last folder up behind a folder the other PC moved to the top.)
inline void placeBeforeFollowing(QStringList &out, QSet<QString> &placed,
                                 const QStringList &sideOrder, int at)
{
    const QString &id = sideOrder.at(at);
    for (int j = at + 1; j < sideOrder.size(); ++j) {
        if (placed.contains(sideOrder.at(j))) {
            out.insert(out.indexOf(sideOrder.at(j)), id);
            placed.insert(id);
            return;
        }
    }
    out.append(id);
    placed.insert(id);
}

} // namespace folder_sync_detail

// Every input must be SANITISED (sanitiseFolderState changes nothing): a local state that needed it
// was repaired at load, and a remote that needed it is Unreadable and never gets here (D27).
// `prefer == Local` is how the caller says the base is trusted (D10).
//
// The base/prefer contract (ruling R10): prefer Remote means the base is untrusted and MUST be the
// empty state; a non-empty base with prefer Remote is outside the contract and idempotence (E20)
// does not hold there. chooseBase hands over the empty state whenever it distrusts the base, so the
// controller never makes that call; some tests do, on purpose (the rule with prefer Remote, and the
// pinned boundary case in E20), which is why it is not asserted here.
//
// The result is never truncated to fit: over a cap it is REFUSED (D13), because sanitiseFolderState
// would silently drop the folders past 50 together with their chats.
inline MergeResult mergeFolderStates(const FolderState &base, const FolderState &local,
                                     const FolderState &remote, MergePrefer prefer)
{
    using namespace folder_sync_detail;
    const bool preferLocal = prefer == MergePrefer::Local;
    MergeResult result;

    const QHash<QString, int> baseIds = folderIndexById(base);
    const QHash<QString, int> remoteIds = folderIndexById(remote);

    // Step 1, pairing (D11). A folder is NEW on a side when the base lacks its id. Walking the local
    // new folders that the remote lacks by id, in local order, each is paired with the unpaired
    // remote new folder (that the local lacks by id) whose case-folded name is equal and whose id is
    // the SMALLEST (plain string comparison). Not the first in remote order (ruling R6): Step 5 can
    // put the remote-new folders in another relative order in the merged state, and a pairing keyed
    // on position would then pair twins differently when that state is merged again (E20). The
    // paired local folder is replaced IN PLACE by a copy of the remote one (remote id, name casing
    // and collapsed flag), and its chats are re-pointed to the remote id; from here on the pair is
    // one folder present on both sides with equal fields. `loc` is that re-pointed local state (L').
    FolderState loc = local;
    {
        const QHash<QString, int> localIdsBefore = folderIndexById(local);
        QVector<int> remoteNew;      // indices into remote.folders
        QStringList remoteNewFolded;
        for (int i = 0; i < remote.folders.size(); ++i) {
            const Folder &f = remote.folders.at(i);
            if (!baseIds.contains(f.id) && !localIdsBefore.contains(f.id)) {
                remoteNew.append(i);
                remoteNewFolded.append(f.name.toCaseFolded());
            }
        }
        QVector<bool> taken(remoteNew.size(), false);
        QHash<QString, QString> repoint;   // old local id -> remote id
        for (Folder &f : loc.folders) {
            if (baseIds.contains(f.id) || remoteIds.contains(f.id))
                continue;
            const QString folded = f.name.toCaseFolded();
            int best = -1;
            for (int k = 0; k < remoteNew.size(); ++k) {
                if (taken.at(k) || remoteNewFolded.at(k) != folded)
                    continue;
                if (best < 0
                    || remote.folders.at(remoteNew.at(k)).id
                           < remote.folders.at(remoteNew.at(best)).id)
                    best = k;
            }
            if (best < 0)
                continue;
            taken[best] = true;
            const Folder &match = remote.folders.at(remoteNew.at(best));
            repoint.insert(f.id, match.id);
            f = match;
            ++result.foldersPaired;
        }
        if (!repoint.isEmpty()) {
            for (auto it = loc.assign.begin(); it != loc.assign.end(); ++it) {
                const auto to = repoint.constFind(it.value());
                if (to != repoint.constEnd())
                    it.value() = to.value();
            }
        }
    }
    const QHash<QString, int> localIds = folderIndexById(loc);

    // Step 2, which folders survive. In base and on both sides: survives. In base and on ONE side:
    // the other side deleted it, and a deletion beats an edit, so it is dropped (counted only when
    // it is the REMOTE that lacks it). In base only: dropped. Not in base (new on either or both
    // sides): survives.
    QSet<QString> survivors;
    survivors.reserve(base.folders.size() + loc.folders.size() + remote.folders.size());
    for (const Folder &f : base.folders) {
        const bool inLocal = localIds.contains(f.id);
        const bool inRemote = remoteIds.contains(f.id);
        if (inLocal && inRemote)
            survivors.insert(f.id);
        else if (inLocal)
            ++result.foldersDropped;
    }
    for (const Folder &f : loc.folders) {
        if (!baseIds.contains(f.id))
            survivors.insert(f.id);
    }
    for (const Folder &f : remote.folders) {
        if (!baseIds.contains(f.id))
            survivors.insert(f.id);
    }

    // Step 3, fields. A survivor on both sides takes pick3 per field; a survivor on one side only
    // is taken as that side has it.
    const auto mergedFolder = [&](const QString &id) -> Folder {
        const auto li = localIds.constFind(id);
        const auto ri = remoteIds.constFind(id);
        if (ri == remoteIds.constEnd())
            return loc.folders.at(li.value());
        if (li == localIds.constEnd())
            return remote.folders.at(ri.value());
        const Folder &l = loc.folders.at(li.value());
        const Folder &r = remote.folders.at(ri.value());
        const auto bi = baseIds.constFind(id);
        const bool hasBase = bi != baseIds.constEnd();
        const Folder b = hasBase ? base.folders.at(bi.value()) : Folder();
        Folder m;
        m.id = id;
        m.name = pick3(hasBase, b.name, l.name, r.name, prefer);
        m.collapsed = pick3(hasBase, b.collapsed, l.collapsed, r.collapsed, prefer);
        return m;
    };

    FolderState merged;
    merged.otherCollapsed = pick3(preferLocal, base.otherCollapsed, loc.otherCollapsed,
                                  remote.otherCollapsed, prefer);

    // Step 4, filing, per token in the union of the keys of base, local and remote. An untrusted
    // (empty) base has every chat unfiled, which makes these rules a union.
    QSet<QString> tokens;
    tokens.reserve(base.assign.size() + loc.assign.size() + remote.assign.size());
    for (auto it = base.assign.constBegin(); it != base.assign.constEnd(); ++it)
        tokens.insert(it.key());
    for (auto it = loc.assign.constBegin(); it != loc.assign.constEnd(); ++it)
        tokens.insert(it.key());
    for (auto it = remote.assign.constBegin(); it != remote.assign.constEnd(); ++it)
        tokens.insert(it.key());
    merged.assign.reserve(tokens.size());
    for (const QString &token : std::as_const(tokens)) {
        const QString b = filingIn(base, baseIds, token);
        const QString l = filingIn(loc, localIds, token);
        const QString r = filingIn(remote, remoteIds, token);
        QString chosen;
        bool conflict = false;
        if (l == r)
            chosen = l;
        else if (effectiveFiling(b, l, localIds) == b)
            chosen = r;   // the local did not change it
        else if (effectiveFiling(b, r, remoteIds) == b)
            chosen = l;   // the remote did not change it
        else {
            conflict = true;
            chosen = preferLocal ? l : r;
        }
        // A chosen FOLDER that did not survive: in a conflict, the other side's folder if that
        // survived; otherwise the chat is unfiled, exactly as deleteFolder returns a deleted
        // folder's chats to Other. ("Unfiled" is not a folder: when it is chosen, it stands.)
        if (!chosen.isEmpty() && !survivors.contains(chosen)) {
            const QString &other = preferLocal ? r : l;
            chosen = (conflict && !other.isEmpty() && survivors.contains(other)) ? other : QString();
        }
        if (!chosen.isEmpty())
            merged.assign.insert(token, chosen);
    }

    // Step 5, order. `common` = the survivors in base (by Step 2 they are on both sides). Their
    // order: the shared one if both sides agree; else the side that changed it; else prefer's.
    // Then the new survivors: first every one the remote has (remote-only, pairs, and ids new on
    // both sides), in remote order, each placed by the REMOTE list; then the local-only ones, in
    // local order, each placed by the (paired) LOCAL list. Every id not in the base survived
    // Step 2.
    const QStringList baseOrder = folderIdsInOrder(base);
    const QStringList localOrder = folderIdsInOrder(loc);
    const QStringList remoteOrder = folderIdsInOrder(remote);
    const auto commonIn = [&](const QStringList &order) {
        QStringList rel;
        for (const QString &id : order) {
            if (baseIds.contains(id) && survivors.contains(id))
                rel.append(id);
        }
        return rel;
    };
    const QStringList relB = commonIn(baseOrder);
    const QStringList relL = commonIn(localOrder);
    const QStringList relR = commonIn(remoteOrder);
    QStringList order;
    if (relL == relR)
        order = relL;
    else if (relL == relB)
        order = relR;
    else if (relR == relB)
        order = relL;
    else
        order = preferLocal ? relL : relR;

    QSet<QString> placed(order.cbegin(), order.cend());
    for (int i = 0; i < remoteOrder.size(); ++i) {
        if (!baseIds.contains(remoteOrder.at(i)))
            placeBeforeFollowing(order, placed, remoteOrder, i);
    }
    for (int i = 0; i < localOrder.size(); ++i) {
        const QString &id = localOrder.at(i);
        if (!baseIds.contains(id) && !remoteIds.contains(id))
            placeBeforeFollowing(order, placed, localOrder, i);
    }
    merged.folders.reserve(order.size());
    for (const QString &id : std::as_const(order))
        merged.folders.append(mergedFolder(id));

    // Step 6, caps and self-check. Never truncate: over a cap, or a result sanitiseFolderState
    // would change, is refused with nothing to apply.
    const auto refused = [](MergeStatus status) {
        MergeResult r;
        r.status = status;
        return r;
    };
    if (merged.folders.size() > kMaxFolders)
        return refused(MergeStatus::TooManyFolders);
    if (merged.assign.size() > kMaxAssignments)
        return refused(MergeStatus::TooManyAssignments);
    FolderState selfCheck = merged;
    if (sanitiseFolderState(selfCheck))
        return refused(MergeStatus::Invalid);

    result.merged = std::move(merged);
    return result;
}

// ---------------------------------------------------------------------------------------------
// The sidecar (spec D8, "Sidecar format")
//
//   { "v": 1,
//     "localSha": "<40 lowercase hex, or the empty string when no local file existed (D31)>",
//     "base": "<the text encodeFolders() writes for the base state, as a JSON string>" }
//
// `base` is a string holding the local file's own codec output, read back by decodeFolders: there is
// no second codec and the caps are the same. The sidecar is DERIVED data (the state at this PC's last
// successful sync and a fingerprint of the local file as this build last wrote it), so every reading
// that is not a clean one only costs trust: the base is thrown away and the merge is a union (D12).

// The sidecar's own "v", separate from kFolderFormatVersion and kFolderSyncRemoteVersion.
constexpr int kFolderSyncSidecarVersion = 1;

enum class SidecarStatus {
    Ok,           // readable, a version this build knows, nothing repaired; `sidecar` is set
    Absent,       // there is no sidecar file. Set by the file layer, NEVER by decodeSidecar: the bytes
                  // of a file that exists are never "absent"
    Unreadable,   // there, but not something to believe: over the cap, not JSON, a wrong shape, a
                  // sha that is not a sha, no usable base, or a base that decodes only after repair
    Newer         // "v" above kFolderSyncSidecarVersion: a newer TalQ wrote it
};

struct Sidecar {
    QByteArray localSha;   // 40 lowercase hex digits, or empty (D31)
    FolderState base;
};

// `sidecar` is meaningful only when status is Ok; otherwise it is EMPTY (no sha, the empty state),
// so a caller that forgot to look at the status still cannot believe a base.
struct SidecarDecode {
    SidecarStatus status = SidecarStatus::Unreadable;
    Sidecar sidecar;
};

namespace folder_sync_detail {

// Exactly 40 lowercase hex digits, checked by hand. Not a "^...$" pattern: `$` also matches before
// a trailing newline, so "<40 digits>\n" would pass (the trap TalkToken.h documents for tokens).
inline bool isSha1Hex(const QString &s)
{
    if (s.size() != 40)
        return false;
    for (const QChar c : s) {
        const char16_t u = c.unicode();
        if (!((u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f')))
            return false;
    }
    return true;
}

} // namespace folder_sync_detail

// `localShaHex` is written as given: decodeSidecar is the gate, and a value it would refuse reads back
// as Unreadable, which is the safe direction.
inline QByteArray encodeSidecar(const FolderState &base, const QByteArray &localShaHex)
{
    QJsonObject root;
    root.insert(QStringLiteral("v"), kFolderSyncSidecarVersion);
    root.insert(QStringLiteral("localSha"), QString::fromLatin1(localShaHex));
    root.insert(QStringLiteral("base"), QString::fromUtf8(encodeFolders(base)));
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

// Never returns Absent. In order:
//  - over kMaxSidecarBytes, not JSON, or a root that is not an object: Unreadable. (Zero bytes is
//    "not JSON" here: a file that exists and holds nothing is not one this build wrote.)
//  - "v" missing, not a number, not an integer, not finite, or below 1: Unreadable; above
//    kFolderSyncSidecarVersion: Newer. The version is judged BEFORE any other key, because a newer
//    writer may have changed what the other keys are.
//  - "localSha" not a string, or a string that is neither empty nor 40 lowercase hex: Unreadable.
//  - "base" not a string; a BLANK string (decodeFolders reads blank bytes as an empty state, which
//    would hand a damaged sidecar a believed, empty base); text decodeFolders calls Corrupt or
//    FutureVersion (the inner "v" is the local format's, so that is "no usable base", not Newer); or
//    text it had to REPAIR (D27, as for the remote): Unreadable.
inline SidecarDecode decodeSidecar(const QByteArray &bytes)
{
    SidecarDecode out;   // Unreadable, nothing in it, until every check has passed
    if (bytes.size() > kMaxSidecarBytes)
        return out;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return out;
    const QJsonObject root = doc.object();

    const QJsonValue v = root.value(QStringLiteral("v"));
    if (!v.isDouble())
        return out;
    const double version = v.toDouble();
    // The finiteness test is belt and braces: Qt's parser already refuses 1e400 as an illegal
    // number, but floor(inf) == inf, so a parser that returned infinity would read as Newer.
    if (!std::isfinite(version) || version != std::floor(version) || version < 1)
        return out;
    if (version > kFolderSyncSidecarVersion) {
        out.status = SidecarStatus::Newer;
        return out;
    }

    const QJsonValue shaV = root.value(QStringLiteral("localSha"));
    if (!shaV.isString())
        return out;
    const QString sha = shaV.toString();
    if (!sha.isEmpty() && !folder_sync_detail::isSha1Hex(sha))
        return out;

    const QJsonValue baseV = root.value(QStringLiteral("base"));
    if (!baseV.isString())
        return out;
    const QByteArray baseText = baseV.toString().toUtf8();
    QByteArrayView body(baseText);
    if (body.startsWith("\xEF\xBB\xBF"))
        body = body.sliced(3);
    if (body.trimmed().isEmpty())
        return out;
    DecodeResult d = decodeFolders(baseText);
    if (d.status != DecodeStatus::Ok || d.repaired)
        return out;

    out.status = SidecarStatus::Ok;
    out.sidecar.localSha = sha.toLatin1();
    out.sidecar.base = std::move(d.state);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Trusting the base (spec D12, "Trusting the base"; worked examples E15 to E17)
//
// The base is used for one thing: telling "the other side deleted it" from "this side never had
// it". A base believed wrongly turns an empty or damaged local state into a deletion on every PC,
// so every rule here leans one way: when in doubt the base is thrown away (the EMPTY state), the
// merge becomes a union, and the worst outcome is a deleted folder that comes back.

enum class BaseWhy {
    Trusted,                 // no rule fired
    NoSidecar,               // rule 1: there is no sidecar
    SidecarUnreadable,       // rule 1: it is there but unreadable, or its base needed repair
    SidecarNewer,            // rule 1: a newer TalQ wrote it
    LocalLoadNotClean,       // rule 2: the local file did not load cleanly
    LocalChangedByOther,     // rule 3: the loaded bytes are not the ones this build last wrote
    EmptyLocalWithoutEdit    // rule 4: the within-session brake
};

struct BaseInputs {
    SidecarStatus sidecarStatus = SidecarStatus::Absent;
    Sidecar sidecar;                 // read only when sidecarStatus is Ok
    FolderLoadOutcome outcome = FolderLoadOutcome::Detached;   // how the store's attach found the file
    QByteArray loadedSha;            // hex SHA-1 of the bytes the store loaded at attach; empty when
                                     // there were none (Missing, SetAside)
    bool localEmpty = false;         // the local state is empty NOW (isEmptyFolderState)
    int localEditCount = 0;          // user mutations this session
    bool stateUnchangedSinceAttach = true;   // the store's generation() still equals its value at attach
};

// `base` is the sidecar's base when `trusted`, and the EMPTY state otherwise (otherCollapsed
// included), so a caller that merges with it can never act on a base it was told not to believe.
// A trusted base merges with MergePrefer::Local, an untrusted one with MergePrefer::Remote (D10).
struct BaseChoice {
    bool trusted = false;
    FolderState base;
    BaseWhy why = BaseWhy::NoSidecar;
};

// The first rule that fires decides, in this order:
//  1. no sidecar, an unreadable or newer one (a base that decodes only after repair was already
//     made Unreadable by decodeSidecar);
//  2. the local file did not load cleanly: EVERY outcome but Ok, including Detached and
//     ReadOnlyLoad (a Missing or SetAside file is an empty state that must not read as a deletion;
//     an empty localSha, which records "no file existed", never overrides this);
//  3. the loaded bytes' sha is not the sidecar's localSha: someone else wrote the file after this
//     build did (an older build, a hand edit, a restore, a truncation). A load that is Ok yet has no
//     sha at all is an inconsistent input and does not match anything, an empty localSha included;
//  4. the within-session brake: the local state is empty, the base is not, there was no user edit
//     this session, and the state moved since attach. A MATCHING localSha with an UNCHANGED attach
//     state exempts it (E15): the empty file found at attach is the one this build wrote after the
//     user's deliberate delete, so "delete my last folder, quit, restart" ends deleted. Rules 1 to 3
//     are judged once, at attach; rule 4 every cycle.
inline BaseChoice chooseBase(const BaseInputs &in)
{
    const auto distrust = [](BaseWhy why) {
        BaseChoice c;   // trusted = false, the empty state
        c.why = why;
        return c;
    };

    // Rule 1. Anything but Ok is distrusted, including a status this switch does not name.
    if (in.sidecarStatus != SidecarStatus::Ok) {
        switch (in.sidecarStatus) {
        case SidecarStatus::Unreadable:
            return distrust(BaseWhy::SidecarUnreadable);
        case SidecarStatus::Newer:
            return distrust(BaseWhy::SidecarNewer);
        default:
            return distrust(BaseWhy::NoSidecar);
        }
    }
    // Rule 2.
    if (in.outcome != FolderLoadOutcome::Ok)
        return distrust(BaseWhy::LocalLoadNotClean);
    // Rule 3.
    if (in.loadedSha.isEmpty() || in.loadedSha != in.sidecar.localSha)
        return distrust(BaseWhy::LocalChangedByOther);
    // Rule 4.
    if (in.localEmpty && !isEmptyFolderState(in.sidecar.base) && in.localEditCount == 0
        && !in.stateUnchangedSinceAttach)
        return distrust(BaseWhy::EmptyLocalWithoutEdit);

    BaseChoice c;
    c.trusted = true;
    c.base = in.sidecar.base;
    c.why = BaseWhy::Trusted;
    return c;
}

} // namespace talq
