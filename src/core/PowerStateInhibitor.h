#pragma once

// Keeps Windows awake for the duration of a call.
//
// TalQ had NO power-state inhibition of any kind, so a screensaver engaging
// mid-call and disrupting it was expected behaviour rather than a glitch
// (field 2026-07-28).
//
// THREAD-SCOPED: SetThreadExecutionState applies to the CALLING THREAD. The
// request must be made and released on ONE thread, or the release silently
// fails to cancel a request registered on another. This class is main-thread
// only — CallManager owns it and drives it from call/camera/share state.
namespace talq {

class PowerStateInhibitor
{
public:
    ~PowerStateInhibitor();

    // inCall       — any active call: keep the SYSTEM awake.
    // needsDisplay — camera or a screen share is in use: also keep the DISPLAY
    //                on. An audio-only call has no reason to hold the screen
    //                lit, and doing so needlessly drains a laptop.
    // Idempotent: re-applying the same state is a no-op.
    void update(bool inCall, bool needsDisplay);

private:
    void apply(bool inCall, bool needsDisplay);
    bool m_inCall  = false;
    bool m_display = false;
};

} // namespace talq
