#pragma once

// Where a voice message goes (MainWindow's VoiceRecorder wiring). Pure; test: voice-room-policy-test.
//
// A recording belongs to the room, and the topic inside it, that was open when the record button
// was pressed. Until 2026-11 nothing remembered that: the recording kept running across a room
// switch and, when stopped, was sent to whichever room was open by then; on Home the composer is
// hidden, so the microphone stayed open with no control at all.
//
// The rules:
//   - Leaving the room (another room, a topic in another room, Home) stops the recording and sends
//     it where it STARTED, and a toast names that room. A topic switch inside the same room is not
//     leaving: the record controls stay on screen, and the message still goes to the start topic.
//   - If the start room is gone (deleted, left, removed from the list), the recording is discarded,
//     and a toast says so.
//   - A recording under a second is a mis-click and is dropped without a word, as before.
//   - A cancelled recording never reaches any of this: VoiceRecorder::cancel emits no finished.

#include <QSet>
#include <QString>
#include <QtGlobal>

namespace talq::voice {

// Shorter than this is a mis-click, not a message: sending it would drop a 0-second bubble into
// somebody's chat that they cannot help but tap.
constexpr qint64 kMinVoiceMs = 1000;

// A room, and the topic inside it the composer posts to (0 = none / "All messages").
struct Place {
    QString token;
    int threadId = 0;
};

inline bool samePlace(const Place &a, const Place &b)
{
    return a.token == b.token && a.threadId == b.threadId;
}

// Does going to `toToken` (empty = Home) end a recording that started in `start`? Only a change of
// ROOM does. The room-selection handler re-fires on every sidebar refresh for the room already open,
// so this compares tokens and never reads "the handler fired" as leaving.
inline bool navigationEndsRecording(bool recording, const Place &start, const QString &toToken)
{
    return recording && toToken != start.token;
}

// Is the room a recording started in still one the user is in? The room list is the evidence, and an
// EMPTY list proves nothing: it is empty before the first load, and a refresh can answer [].
inline bool startRoomPresent(const QSet<QString> &listed, const QString &token)
{
    if (token.isEmpty()) return false;
    return listed.isEmpty() || listed.contains(token);
}

enum class Route {
    Drop,           // under a second: removed without a word
    Send,           // sent where it started, which is what is on screen: nothing to say
    SendAndSay,     // sent where it started, which is NOT what is on screen: a toast names the room
    DiscardAndSay,  // the room it started in is gone or was left: removed, and a toast says so
};

// What happens to a recording that has ended. `openNow` is what the user sees once this is done: the
// room and topic open for their own Stop, the place they are going to when a navigation ended it.
inline Route routeFinished(qint64 durationMs, const Place &start, const Place &openNow,
                           bool startPresent)
{
    if (durationMs < kMinVoiceMs) return Route::Drop;
    if (start.token.isEmpty() || !startPresent) return Route::DiscardAndSay;
    return samePlace(start, openNow) ? Route::Send : Route::SendAndSay;
}

// A room or topic name as a toast shows it: a server-supplied name can be up to 255 characters,
// and the toast is one line across the top of the window. Never splits a surrogate pair.
inline QString toastName(const QString &name, int maxChars = 60)
{
    const QString n = name.trimmed();
    if (maxChars < 1 || n.size() <= maxChars) return n;
    int cut = maxChars;
    if (n.at(cut - 1).isHighSurrogate()) --cut;
    return n.left(cut) + QChar(0x2026);
}

} // namespace talq::voice
