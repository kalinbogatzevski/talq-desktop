#pragma once
#include <atomic>

// Bounded hand-off of decoded video frames to the Qt main thread.
//
// THE BUG THIS EXISTS TO PREVENT (field, 2026-07-27). Every decoded frame used
// to be handed to the main thread with a bare Qt::QueuedConnection. That has NO
// backpressure: each post becomes a queued event owning a full GstSample —
// ~3 MB for one 1080p frame. While the main thread keeps up this is invisible.
// When it does NOT keep up, the queue grows without limit.
//
// That is exactly what happened to a WeakIgpu box (Intel UHD 620, `GPU accel:
// "Software only"`) in a two-screen-share call: it was decoding an incoming
// 1080p share in SOFTWARE (avdec_h264) while also previewing its own 720p share
// and its own camera, with the CPU saturated. Its RSS went 55 MB -> 2 GB in
// about three and a half minutes and the process died on a GLib OOM abort. The
// allocation that finally failed was 3,686,543 bytes — one 1080p frame. The
// per-widget memory accounting stayed flat throughout, because none of this was
// in the UI: it was frames piling up in the event queue.
//
// THE FIX. Live video is inherently droppable — only the newest frame is worth
// showing, and a frame the main thread has not reached yet is already stale. So
// cap the number of frames in flight and DROP rather than queue. The worst case
// becomes a visible frame-rate dip instead of an out-of-memory crash.
//
// Each stream owns its own gate, so one struggling stream cannot starve another.
namespace talq {

class FrameHandoffGate
{
public:
    // One frame being consumed by the main thread, plus one queued behind it.
    // Enough to absorb ordinary scheduling jitter without dropping; small
    // enough that the ceiling is a couple of frames per stream rather than
    // "however many the producer managed to emit".
    static constexpr int kMaxInFlight = 2;

    // Called on the streaming thread BEFORE posting. Returns false when the
    // main thread is already behind — the caller must then unref the sample and
    // drop it. Never blocks: blocking here would stall the GStreamer pipeline,
    // trading a memory problem for a media one.
    bool tryAcquire()
    {
        int cur = m_inFlight.load(std::memory_order_relaxed);
        while (cur < kMaxInFlight) {
            if (m_inFlight.compare_exchange_weak(cur, cur + 1,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed))
                return true;
            // cur was refreshed by the failed exchange; re-test the bound.
        }
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Called once the main thread has consumed the frame. Must pair with every
    // tryAcquire() that returned true, or the gate closes permanently and the
    // stream goes black.
    void release() { m_inFlight.fetch_sub(1, std::memory_order_acq_rel); }

    long long dropped() const { return m_dropped.load(std::memory_order_relaxed); }
    int inFlight()      const { return m_inFlight.load(std::memory_order_relaxed); }

    // True on the 1st, 2nd, 4th, 8th… drop. Lets a caller log that it is
    // dropping without turning a sustained stall into a log flood — the drop
    // rate matters, and doubling steps convey it in a handful of lines.
    static bool isLogWorthy(long long dropCount)
    {
        return dropCount > 0 && (dropCount & (dropCount - 1)) == 0;
    }

private:
    std::atomic<int>       m_inFlight{0};
    std::atomic<long long> m_dropped{0};
};

} // namespace talq
