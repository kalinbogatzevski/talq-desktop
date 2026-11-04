#pragma once

// FolderSyncMeta -- the file I/O for folder sync's two small files, which sit beside the local folders
// file in the folders directory: the sidecar <scopeKey>.sync.json (the merge base plus a fingerprint of
// the local file, encoded and decoded by the pure code in FolderSyncLogic.h) and the capability-probe
// marker <scopeKey>.probed. Qt Core only. See tests/folder_sync_meta_test.cpp.
// Spec: docs/superpowers/specs/2026-11-03-folder-sync-design.md, "Where the merge metadata lives (D8)",
// "Sidecar format" and "Keeping localSha current (D25)"; D17 for what the marker means.
//
// Names. The local file <scopeKey>.json is written by FolderStore, which also makes
// <scopeKey>.json.pre-sync and <scopeKey>.json.corrupt-<stamp> from that path. Both names here end in
// ".sync.json" / ".probed" right after the key, so none of them can equal, or start with,
// "<scopeKey>.json." -- and an older build, which builds the one path <scopeKey>.json, never sees them.
// Nothing here ever reads, writes or removes any of the store's files.
//
// Callers pass a scope key that already passed talq::isScopeKey (16 lowercase hex): the key becomes a
// file name, exactly as in FolderStore::filePathFor. An empty root or key is refused here as well (no
// path, so nothing is read, written or removed) rather than falling back to the working directory.
//
// Every write is a QSaveFile (a temp file in the same directory, renamed over the target on commit)
// with no direct-write fallback, so a reader sees the old file or the whole new one, never a part, and
// a failed write leaves neither a partial file nor a temp file. A function returns true only for a
// committed write.
//
// The sidecar is DERIVED data: a missing, damaged or newer one only costs trust (the base is dropped
// and the merge is a union), and the controller overwrites it. The marker is per install and per
// scope, deliberately not in QSettings.

#include "core/FolderSyncLogic.h"

#include <QByteArray>
#include <QString>

namespace talq::foldersyncmeta {

// The marker is about 25 bytes; this is how much of it is ever read (one byte more is read, to tell
// "too big" from "exactly the cap").
constexpr qint64 kMaxProbeMarkerBytes = 4096;

// <rootDir>/<scopeKey>.sync.json and <rootDir>/<scopeKey>.probed. Empty when rootDir or scopeKey is empty.
QString sidecarPath(const QString &rootDir, const QString &scopeKey);
QString probeMarkerPath(const QString &rootDir, const QString &scopeKey);

// `sidecar` is meaningful only when status is Ok. For Absent, Unreadable and Newer it is EMPTY (no
// sha, the empty state), so a caller that forgot to look at the status still cannot believe a base.
struct SidecarRead {
    SidecarStatus status = SidecarStatus::Absent;
    Sidecar sidecar;
};

// No file: Absent. A file that exists but cannot be opened or read, or is over kMaxSidecarBytes:
// Unreadable (at most kMaxSidecarBytes + 1 bytes are read, so a huge file is never read whole into
// memory). Anything else is decodeSidecar's verdict (Ok, Unreadable or Newer).
SidecarRead readSidecar(const QString &rootDir, const QString &scopeKey);

// Atomic. Creates the directory. True only when the new file was committed. `localShaHex` is empty
// (no local file existed, D31) or exactly 40 lowercase hex digits: anything else is a bug in the
// producer, caught by a Q_ASSERT in development. (A release build writes it as given, which reads back
// as Unreadable -- the safe direction -- but that is a deleted folder coming back on every start, the
// failure D30 treats as a defect, so the producer must be right.)
bool writeSidecar(const QString &rootDir, const QString &scopeKey, const FolderState &base,
                  const QByteArray &localShaHex);

// True ONLY for the JSON object { "v": 1, "probedOk": true } (those two keys and no others, the
// number 1, the boolean true). Absent, unreadable, over the cap, not JSON, any other shape or value:
// false, which means "not probed" and costs one probe, the safe direction.
bool probeMarkerPresent(const QString &rootDir, const QString &scopeKey);

// Atomic. Creates the directory. True only when the marker was committed.
bool writeProbeMarker(const QString &rootDir, const QString &scopeKey);

// True when the marker is gone afterwards, including when there never was one. False when something
// is still there (a directory in its place, a file that could not be removed).
bool removeProbeMarker(const QString &rootDir, const QString &scopeKey);

} // namespace talq::foldersyncmeta
