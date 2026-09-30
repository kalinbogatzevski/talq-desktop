#pragma once

// MicInUsePolicy -- is ANOTHER app using the microphone right now? No Qt, no
// Windows -- see tests/mic_in_use_policy_test.cpp. MicInUse.cpp gathers the
// capture sessions (WASAPI IAudioSessionManager2 on every active capture
// endpoint) and hands them here.
//
// Why (Kalin, 2026-09-30): in a Zoom call on the office laptop, TalQ's message
// chimes and a loud incoming-call ring went straight into his headphones. While
// another app holds the mic TalQ plays no sound at all -- the desktop popup and
// the call window still appear. Being Away alone must NOT silence TalQ: auto-away
// from inactivity is exactly when a ring has to be heard across the room.
//
// The live session list, not the ConsentStore registry history: that is a usage
// log (LastUsedTimeStart/Stop) written by the privacy subsystem, and the live
// session state is what "in a call right now" actually means.

#include <vector>

namespace talq {

struct CaptureSession {
    unsigned long pid = 0;   // owning process; 0 = system / unattributed
    bool active = false;     // AudioSessionStateActive: capturing right now
};

// True when some OTHER process is actively capturing. Our own capture (TalQ in
// its own call) never counts, nor do inactive sessions (an app that opened the
// mic once and idles) or the unattributed system session.
inline bool otherAppCapturing(const std::vector<CaptureSession> &sessions,
                              unsigned long selfPid)
{
    for (const CaptureSession &s : sessions)
        if (s.active && s.pid != 0 && s.pid != selfPid)
            return true;
    return false;
}

} // namespace talq
