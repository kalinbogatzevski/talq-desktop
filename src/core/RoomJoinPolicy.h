#pragma once

// RoomJoinPolicy -- pure decision for SignalingClient::joinRoom(): should a join
// of the room we are already joining wait for the one in flight?
//
// joinRoom() POSTs participants/active, which mints a FRESH Nextcloud session and
// retires the previous one, then sends the WS "room" message under it. A second
// join of the same room issued before the first is acked therefore does not
// "also" join: it replaces the session the first one is about to bind, the HPB
// answers the first with no_such_room, and the room is only really joined after
// the second round trip. The existing guard (SignalingClient::joinRoom) skips a
// redundant join only once the room is ACKED; this covers the window before that.
//
// Field 2026-10-29, Kalin's Zenbook: he opened the chat with Petia and pressed
// Call within seconds. startCall() saw currentRoom() == token (set optimistically
// by the chat's join) but roomJoinAcked() still false, so ensureSignalingRoomJoined
// issued a second join — two participants/active POSTs 0.7 s apart, a no_such_room
// error, and the ack landing 1.3 s after the first join's response.
//
// Time-bounded: a join that died silently (no ack, no failure) must never be able
// to swallow a retry, so after kRoomJoinCoalesceMs a new join goes out as before.

namespace talq {

constexpr long long kRoomJoinCoalesceMs = 8000;

// force          : the caller insists (reconnect re-join, rejoinRoomThenRepost)
// sameRoom       : the requested token is the room already being joined
// authenticated  : the signaling session is up (a join needs it)
// acked          : the HPB already confirmed membership (handled by the older guard)
// pendingAgeMs   : age of the unanswered join of that room, -1 when none is in flight
inline bool coalesceRoomJoin(bool force, bool sameRoom, bool authenticated, bool acked,
                             long long pendingAgeMs)
{
    return !force && sameRoom && authenticated && !acked
           && pendingAgeMs >= 0 && pendingAgeMs < kRoomJoinCoalesceMs;
}

} // namespace talq
