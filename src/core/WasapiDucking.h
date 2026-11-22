#pragma once

namespace talq {

// Opt this process out of Windows' "communications" stream auto-ducking so
// TalQ's call audio is NOT attenuated when another application opens a
// communications stream (e.g. an incoming call in another app ringing in the
// background). Operates on the process's audio session on the default render
// device via IAudioSessionControl2::SetDuckingPreference(TRUE), matching what
// other call apps do. Best-effort and silent on failure; idempotent (safe to call
// once per call start). Compiles to a no-op on non-Windows builds.
void disableCommunicationsDucking();

} // namespace talq
