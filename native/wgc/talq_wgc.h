/*
 * talq_wgc.h -- C ABI for single-WINDOW capture via Windows Graphics Capture
 * (WGC).
 *
 * Why this exists: capturing one application window (not the whole monitor)
 * requires WGC, whose WinRT headers ship only with the MSVC/Win11-SDK
 * toolchain -- the MSYS2/MinGW GStreamer TalQ links against does NOT have them,
 * so GStreamer's d3d11screencapturesrc has no window-capture and DXGI desktop
 * duplication is monitor-only. So this tiny piece is built SEPARATELY with
 * MSVC (cl.exe) and loaded by the MinGW app at runtime.
 *
 * Boundary discipline (critical): ONLY this plain C interface crosses the
 * MSVC<->MinGW boundary. No C++ types, no STL, no GstBuffer, no ownership of
 * heap objects passed across. Frame bytes are delivered by value into a
 * caller-owned copy inside the callback. That keeps the C++ ABI / CRT mismatch
 * from ever mattering.
 *
 * TalQ side (MinGW): for a window share it LoadLibrary's talq_wgc.dll, starts
 * a capture on the target HWND, and in the frame callback pushes the BGRA into
 * a GStreamer appsrc -> videoconvert -> the existing scale/encode/webrtc chain.
 */
#ifndef TALQ_WGC_H
#define TALQ_WGC_H

#include <stdint.h>
#ifdef _WIN32
#  include <windows.h>   /* HWND */
#endif

/* Export when building the DLL (define TALQ_WGC_BUILDING), import otherwise.
 * The MinGW app loads the functions via GetProcAddress (which needs the DLL to
 * export them), so the export is required regardless of how callers link. */
#if defined(_WIN32)
#  ifdef TALQ_WGC_BUILDING
#    define TALQ_WGC_API __declspec(dllexport)
#  else
#    define TALQ_WGC_API __declspec(dllimport)
#  endif
#else
#  define TALQ_WGC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-frame callback. `bgra` is tightly-or-padded BGRA8, top-down, `stride`
 * bytes per row (>= width*4). The buffer is valid ONLY for the duration of the
 * call -- copy what you need before returning. Invoked on a WGC pool thread
 * (NOT the caller's thread); keep it short and re-entrant.
 */
typedef void (*talq_wgc_frame_cb)(void *user, const uint8_t *bgra,
                                  int width, int height, int stride);

/* Opaque capture session. */
typedef struct talq_wgc_session talq_wgc_session;

/*
 * 1 if WGC window-capture is usable on this OS (Windows 10 1903 / build 18362+
 * for GraphicsCaptureItem; border control needs Win11), else 0. Call before
 * attempting a capture so the app can fall back / disable the window option.
 */
TALQ_WGC_API int talq_wgc_available(void);

/*
 * Start capturing window `hwnd`. `cb`/`user` receive each frame. `show_border`
 * controls the Win11 yellow capture border (1 = show, 0 = hide where the OS
 * allows; ignored before Win11 where it is unavoidable).
 *
 * Returns an opaque session, or NULL on failure (bad/again-protected window,
 * WGC unavailable, D3D device creation failed).
 */
TALQ_WGC_API talq_wgc_session *talq_wgc_start_window(HWND hwnd,
                                                    talq_wgc_frame_cb cb, void *user,
                                                    int show_border);

/*
 * Start capturing an entire MONITOR `mon` (HMONITOR). Same callback/lifetime
 * contract as talq_wgc_start_window. This exists because GStreamer's DXGI
 * Desktop Duplication path (d3d11screencapturesrc) fails with
 * DXGI_ERROR_UNSUPPORTED on hybrid-GPU laptops when the app runs on a different
 * adapter than the one driving the display; WGC has no such adapter constraint.
 *
 * Returns an opaque session, or NULL on failure.
 */
TALQ_WGC_API talq_wgc_session *talq_wgc_start_monitor(HMONITOR mon,
                                                     talq_wgc_frame_cb cb, void *user,
                                                     int show_border);

/*
 * Stop and free the session. Safe to call exactly once with a non-NULL handle;
 * blocks until the frame pool has drained so no callback fires after return
 * (so the app can free `user` afterwards without a race). NULL is a no-op.
 */
TALQ_WGC_API void talq_wgc_stop(talq_wgc_session *s);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* TALQ_WGC_H */
