// Unit test for the auto-substream policy (#18). Pure function, no Qt
// runtime needed beyond qreal — runs in milliseconds. Exits non-zero on
// any assertion miss so the suite turns red.

#include "ui/SubstreamPolicy.h"

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void check(const char *what, int got, int want)
{
    if (got == want) {
        std::printf("  PASS  %-50s expected=%d got=%d\n", what, want, got);
    } else {
        std::printf("  FAIL  %-50s expected=%d got=%d\n", what, want, got);
        ++failures;
    }
}

} // namespace

int main()
{
    std::printf("===== #18 substream policy =====\n");

    // (A) Auto path (override < 0): tile-size thresholds.
    check("auto h=100 -> LOW",  pickSubstream(-1, 100.0, false), 0);
    check("auto h=239 -> LOW",  pickSubstream(-1, 239.99, false), 0);
    check("auto h=240 -> MED",  pickSubstream(-1, 240.0, false), 1);
    check("auto h=300 -> MED",  pickSubstream(-1, 300.0, false), 1);
    check("auto h=479 -> MED",  pickSubstream(-1, 479.99, false), 1);
    check("auto h=480 -> HIGH", pickSubstream(-1, 480.0, false), 2);
    check("auto h=720 -> HIGH", pickSubstream(-1, 720.0, false), 2);

    // (B) Stage tile forces HIGH regardless of size — common in the
    //     side-rail layout where the stage tile is much smaller than 480px
    //     but still wants the best layer.
    check("stage h=80  -> HIGH", pickSubstream(-1, 80.0,  true), 2);
    check("stage h=240 -> HIGH", pickSubstream(-1, 240.0, true), 2);
    check("stage h=480 -> HIGH", pickSubstream(-1, 480.0, true), 2);

    // (C) Manual override (#8 Quality chip) beats tile-size.
    check("force LOW on big tile",   pickSubstream(0, 720.0, false), 0);
    check("force MED on tiny tile",  pickSubstream(1, 90.0,  false), 1);
    check("force HIGH on tiny tile", pickSubstream(2, 90.0,  false), 2);
    check("force LOW on stage",      pickSubstream(0, 720.0, true),  0);

    // (D) Out-of-range override = ignored (defensive — UI cycles 0..2).
    //     The function falls back to auto so the SFU doesn't see garbage.
    check("override=5 ignored, auto on small tile", pickSubstream(5, 100.0, false), 0);

    std::printf("\n%s: %d failure(s)\n",
                failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
