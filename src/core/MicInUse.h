#pragma once

#include <QString>

namespace talq {

// True when another application is capturing from a microphone right now (a
// Zoom/Teams/Meet call). TalQ then plays no notification sound and no incoming
// ring; popups still show. See MicInUsePolicy.h for the rule and why.
//
// Cheap (one WASAPI session enumeration, a few ms), so callers ask at the moment
// they would make a sound instead of polling. `who`, when given, receives the
// capturing process's executable name for the log. Always false off Windows or
// if the audio stack can't be queried -- failing open to TalQ's normal sounds.
bool anotherAppUsingMic(QString *who = nullptr);

} // namespace talq
