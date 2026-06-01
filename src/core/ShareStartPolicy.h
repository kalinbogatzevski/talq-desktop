#pragma once

// ShareStartPolicy — the pure decision logic for starting a screen share
// reliably. No Qt, no GStreamer: it takes events (pipeline started, ICE
// connected, media flowing, confirm timeout, release) and returns the action
// the caller (CallManager / ScreenSharePipeline owner) should take. Keeping it
// pure makes the "100% make sure we start" rule unit-testable without a real
// call (see tests/share_start_policy_test.cpp).
//
// Why it exists (researched against the spreed web client + Janus videoroom
// docs, 2026-05-31): the SDP answer is NOT proof a screen publish is live — it
// only means negotiation was accepted. The only publisher-observable proof that
// media is actually on the wire is ICE connected AND outbound RTP packets
// climbing. So a share is "Active" only when BOTH have happened. If they don't
// happen within a timeout, we tear down and retry a fresh pipeline (bounded),
// which is what removes the manual "wait and try again". And because the
// pipeline's GStreamer teardown is asynchronous (a detached NULL transition
// that can hold the d3d11 capture device for a few seconds), a start requested
// while we're still Stopping is QUEUED and fired on release — fixing the
// reported "wait several seconds between shares before a new one starts".

enum class ShareState {
    Idle,        // no share, ready to start
    Starting,    // pipeline asked to start, not yet PLAYING
    Confirming,  // PLAYING; waiting for ICE-connected AND media-flowing
    Active,      // confirmed live on the wire
    Stopping,    // tearing down (async); may have a queued start
};

enum class ShareAction {
    None,         // nothing for the caller to do
    StartNow,     // build + start a fresh pipeline now
    StartQueued,  // caller's start request was deferred until release
    Confirmed,    // share is now Active (caller can stop watchdogs)
    Retry,        // confirm failed; tear down (caller stops pipeline, awaits release)
    Fail,         // retries exhausted; tear down and surface a user-visible error
};

class ShareStartPolicy {
public:
    explicit ShareStartPolicy(int maxRetries = 2) : m_maxRetries(maxRetries) {}

    ShareState state() const { return m_state; }
    int retriesLeft() const { return m_retriesLeft; }

    // User (or a queued retry) asks to start sharing.
    ShareAction requestStart()
    {
        switch (m_state) {
        case ShareState::Idle:
            m_retriesLeft = m_maxRetries;
            m_iceOk = m_mediaOk = false;
            m_state = ShareState::Starting;
            return ShareAction::StartNow;
        case ShareState::Stopping:
            // Previous share still releasing its capture device — defer.
            m_queuedStart = true;
            return ShareAction::StartQueued;
        default:
            // Already starting/confirming/active: ignore (no double-start).
            return ShareAction::None;
        }
    }

    // Pipeline reached PLAYING — begin waiting for protocol confirmation.
    void onPipelineStarted()
    {
        if (m_state == ShareState::Starting) {
            m_iceOk = m_mediaOk = false;
            m_state = ShareState::Confirming;
        }
    }

    ShareAction onIceConnected()  { return confirmStep(m_iceOk); }
    ShareAction onMediaFlowing()  { return confirmStep(m_mediaOk); }

    // Confirmation didn't arrive in time.
    ShareAction onConfirmTimeout()
    {
        if (m_state != ShareState::Confirming)
            return ShareAction::None;
        if (m_retriesLeft > 0) {
            --m_retriesLeft;
            m_queuedStart = true;          // re-fire once teardown releases
            m_state = ShareState::Stopping;
            return ShareAction::Retry;
        }
        m_queuedStart = false;             // give up: tear down to Idle
        m_state = ShareState::Stopping;
        return ShareAction::Fail;
    }

    // User asks to stop a live (or confirming) share.
    ShareAction requestStop()
    {
        if (m_state == ShareState::Idle || m_state == ShareState::Stopping)
            return ShareAction::None;
        m_queuedStart = false;
        m_state = ShareState::Stopping;
        return ShareAction::None;
    }

    // Terminal-teardown escape hatch. Unconditionally returns the policy to
    // Idle (clearing any queued start + retry budget). The caller uses this when
    // it tears a share down WITHOUT a guaranteed release() — e.g. retries are
    // exhausted (Fail) and the pipeline is deleteLater()'d, which suppresses the
    // released() signal so onReleased() would never fire. Without this the policy
    // stays stuck in Stopping and every later start is queued forever ("queued
    // behind teardown"). Safe to call from any state.
    void reset()
    {
        m_state = ShareState::Idle;
        m_queuedStart = false;
        m_iceOk = m_mediaOk = false;
        m_retriesLeft = 0;
    }

    // The pipeline's async teardown finished (capture device released).
    ShareAction onReleased()
    {
        if (m_state != ShareState::Stopping)
            return ShareAction::None;
        if (m_queuedStart) {
            m_queuedStart = false;
            m_iceOk = m_mediaOk = false;
            m_state = ShareState::Starting;   // retry budget already decremented
            return ShareAction::StartNow;
        }
        m_state = ShareState::Idle;
        return ShareAction::None;
    }

private:
    // Mark one of the two confirmation conditions; Active only when both hold.
    ShareAction confirmStep(bool &flag)
    {
        if (m_state != ShareState::Confirming)
            return ShareAction::None;
        flag = true;
        if (m_iceOk && m_mediaOk) {
            m_state = ShareState::Active;
            return ShareAction::Confirmed;
        }
        return ShareAction::None;
    }

    ShareState m_state = ShareState::Idle;
    int  m_maxRetries;
    int  m_retriesLeft = 0;
    bool m_iceOk = false;
    bool m_mediaOk = false;
    bool m_queuedStart = false;
};
