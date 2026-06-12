#pragma once

#include <atomic>

// ---------------------------------------------------------------------------
// Live leak-hunt counters (0.51.x OOM during video calls, ~100MB/s).
//
// Every per-frame video sample is pulled on a GStreamer streaming thread and
// handed to the Qt main thread via QMetaObject::invokeMethod(..., QueuedConnection)
// — an UNBOUNDED cross-thread post. If the main thread can't drain at camera fps,
// those posts (each holding a full GstSample / decoded frame) pile up in the Qt
// event queue → exactly this leak. These global atomics are bumped at the POST
// site (++Posted) and inside the delivered lambda (++Delivered); the [LEAK]
// heartbeat in CallManager::onLoadTick logs them once a second. If (Posted -
// Delivered) climbs into the hundreds/thousands during a real call, the
// cross-thread pileup is the leak and the fix is to coalesce (post at most one
// in-flight frame, drop intermediates). If the gauges stay bounded but RSS still
// climbs, the leak is inside a GStreamer element (watch bgQ bytes instead).
//
// Temporary diagnostic — remove once the leak is fixed.
// ---------------------------------------------------------------------------
namespace talq { namespace leak {
    inline std::atomic<long long> previewPosted{0};     // local self-preview frames posted to main
    inline std::atomic<long long> previewDelivered{0};  // …and actually consumed by main
    inline std::atomic<long long> subPosted{0};         // remote subscriber frames posted to main
    inline std::atomic<long long> subDelivered{0};      // …and actually consumed by main
    inline std::atomic<long long> bgPassThrough{0};     // camera frames pushed through the BG bridge
}}
