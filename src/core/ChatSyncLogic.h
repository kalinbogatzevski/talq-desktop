#pragma once
// Pure, Qt-free chat-sync invariants, extracted so the bug-prone logic is
// unit-testable in isolation (mirrors VersionCompare.h). Callers pass
// primitives; there are no model/network/Qt dependencies here. Each function
// encodes one invariant that, when violated, produced a shipped 0.41.9 bug —
// see docs/superpowers/specs/2026-05-29-chat-sync-hardening-design.md.
namespace talq {

// H1 — forward-only read marker. Returns true iff posting `lastId` as the
// per-user server read marker would ADVANCE it (never regress it).
// knownServerRead is the freshest per-user read marker the client knows
// (ConversationListModel::lastReadMessageForToken). A device that is behind
// (stale cache / generation-race) or has a topic/thread filter active computes
// a lastId LOWER than the true newest; without this guard markAsRead would
// drag the server marker backward and the next /room refresh would re-inflate
// the server-authoritative unread badge (bugs 2/5/6). MUST gate markAsRead
// only — never markAsUnread, which deliberately posts a lower id.
inline bool shouldPostReadMarker(int lastId, int knownServerRead)
{
    return lastId > 0 && lastId > knownServerRead;
}

// H2 — reconcile index validity. Returns true iff `idx` is a usable index
// into a container of size `size` AND still points at the message id we
// expected (idAtIdx == expectedId). refreshLatest() builds an id->index map
// before its loop, then replaceTempByReferenceId() removes a row mid-loop and
// invalidates every cached index; accessing m_messages[idx] with a stale idx
// is an out-of-bounds read+write (heap corruption / crash in Release) — bug 7.
// Validate with this before every cached-index access.
inline bool reconcileIndexValid(int idx, int size, int idAtIdx, int expectedId)
{
    return idx >= 0 && idx < size && idAtIdx == expectedId;
}

// H5 — newest-first ordering that respects optimistic temps. A pending
// optimistic message carries a NEGATIVE id (a decrementing counter, so a later
// send is MORE negative). It is the NEWEST message and must sort to the FRONT,
// but a plain `a.id > b.id` sinks negatives to the OLDEST slot — that reorders
// (or buries) the user's just-sent message and corrupts the oldest-cursor
// (m_oldestMessageId becomes a negative temp id). Returns true iff message id
// `a` should sort BEFORE `b` in a newest-first list (front = newest = bottom of
// the view). Used by enforceNewestFirstInvariant + the refreshLatest merge so
// both share one ordering authority. Bug: "send two messages, the first one
// disappears/jumps", 2026-06-04.
inline bool messageSortsBefore(int a, int b)
{
    const bool aPending = a < 0, bPending = b < 0;
    if (aPending != bPending) return aPending;  // any pending temp is newer than any real
    if (aPending)             return a < b;     // both temp: more-negative = later send = newer
    return a > b;                               // both real: higher server id = newer
}

// True iff the pair (prevId then curId) is already in newest-first order, i.e.
// curId is NOT newer than prevId. Lets the O(n) ordered-check share the exact
// ordering used by the sort, so the check and the sort can never disagree.
inline bool messagePairOrdered(int prevId, int curId)
{
    return !messageSortsBefore(curId, prevId);
}

// H4 — peer-version freshness. Returns true iff a cached peer-version value
// last observed at lastSeenMs is still within windowMs of nowMs. A
// lastSeenMs of 0 (never stamped) is treated as stale. Outside the window the
// caller should show no version chip rather than a confidently-wrong old
// number (bug 3: home 0.28.3 vs office 0.40.x for the same peer).
inline bool isPeerVersionFresh(long long lastSeenMs, long long nowMs, long long windowMs)
{
    return lastSeenMs > 0 && (nowMs - lastSeenMs) <= windowMs;
}

} // namespace talq
