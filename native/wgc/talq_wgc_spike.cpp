/*
 * talq_wgc_spike.cpp -- minimal end-to-end validation of the WGC capture path.
 * Captures ONE frame of a window (foreground by default, or an HWND passed as
 * argv[1] in hex/dec) via talq_wgc, writes it to wgc_frame.bmp, and exits.
 * Proves the toolchain + WGC capture before wiring it into TalQ's pipeline.
 *
 *   build.bat  ->  talq_wgc_spike.exe
 *   talq_wgc_spike.exe            (captures the foreground window)
 *   talq_wgc_spike.exe 0x00080ABC (captures a specific HWND)
 */
#include "talq_wgc.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

struct Cap {
    std::atomic<bool>    got{false};
    std::vector<uint8_t> bgra;
    int w{0}, h{0}, stride{0};
};

static void on_frame(void *user, const uint8_t *bgra, int w, int h, int stride)
{
    Cap *c = static_cast<Cap *>(user);
    bool expected = false;
    if (!c->got.compare_exchange_strong(expected, true)) return;  /* first only */
    c->w = w; c->h = h; c->stride = stride;
    c->bgra.assign(bgra, bgra + static_cast<size_t>(stride) * h);
}

/* Write a 32-bit BGRA top-down buffer as a bottom-up 32bpp BMP. */
static bool save_bmp(const char *path, const Cap &c)
{
    const int rowBytes = c.w * 4;
    const uint32_t pixBytes = static_cast<uint32_t>(rowBytes) * c.h;

#pragma pack(push, 1)
    struct { uint16_t bfType; uint32_t bfSize; uint16_t r1, r2; uint32_t bfOff; } fh{};
    struct { uint32_t sz; int32_t w, h; uint16_t planes, bpp; uint32_t comp, img;
             int32_t xppm, yppm; uint32_t clrUsed, clrImp; } ih{};
#pragma pack(pop)

    fh.bfType = 0x4D42;                       /* 'BM' */
    fh.bfOff  = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOff + pixBytes;
    ih.sz = sizeof(ih); ih.w = c.w; ih.h = c.h; ih.planes = 1; ih.bpp = 32;
    ih.comp = 0; ih.img = pixBytes;

    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    /* BMP is bottom-up; source is top-down with possible row padding (stride). */
    for (int y = c.h - 1; y >= 0; --y)
        fwrite(c.bgra.data() + static_cast<size_t>(y) * c.stride, 1, rowBytes, f);
    fclose(f);
    return true;
}

int main(int argc, char **argv)
{
    if (!talq_wgc_available()) { printf("WGC NOT available on this OS\n"); return 2; }

    HWND hwnd = GetForegroundWindow();
    if (argc > 1) hwnd = reinterpret_cast<HWND>(
                      static_cast<uintptr_t>(strtoull(argv[1], nullptr, 0)));
    if (!hwnd || !IsWindow(hwnd)) { printf("bad HWND %p\n", (void *)hwnd); return 3; }

    char title[256] = {0};
    GetWindowTextA(hwnd, title, sizeof(title) - 1);
    printf("capturing hwnd=%p  \"%s\"\n", (void *)hwnd, title);

    Cap cap;
    talq_wgc_session *s = talq_wgc_start_window(hwnd, on_frame, &cap, /*show_border*/0);
    if (!s) { printf("talq_wgc_start_window FAILED\n"); return 4; }

    for (int i = 0; i < 300 && !cap.got.load(); ++i) Sleep(10);  /* up to 3 s */
    talq_wgc_stop(s);

    if (!cap.got.load()) { printf("no frame arrived\n"); return 5; }
    printf("got frame %dx%d stride=%d (%zu bytes)\n",
           cap.w, cap.h, cap.stride, cap.bgra.size());
    if (save_bmp("wgc_frame.bmp", cap)) printf("wrote wgc_frame.bmp\n");
    else { printf("BMP write failed\n"); return 6; }
    return 0;
}
