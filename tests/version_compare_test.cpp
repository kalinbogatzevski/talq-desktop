// Unit test for the auto-updater's version-newer logic (#19). A false
// answer here either strands users on broken builds (false negative) or
// downgrades them (false positive), so the comparator must be airtight.
// Pure C++, no Qt — builds + runs in well under a second.

#include "core/VersionCompare.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(const char *what, bool got, bool want)
{
    if (got == want) {
        std::printf("  PASS  %-58s expected=%d got=%d\n", what, want, got);
    } else {
        std::printf("  FAIL  %-58s expected=%d got=%d\n", what, want, got);
        ++failures;
    }
}

} // namespace

int main()
{
    using talq::versionNewer;

    std::printf("===== #19 version-newer comparator =====\n");

    // Equal versions — should never be reported as newer.
    check("0.37.3 vs 0.37.3 -> not newer", versionNewer("0.37.3", "0.37.3"), false);
    check("0.0.0  vs 0.0.0  -> not newer", versionNewer("0.0.0",  "0.0.0"),  false);

    // Plain patch bump.
    check("0.37.4 vs 0.37.3 -> newer",     versionNewer("0.37.4", "0.37.3"), true);
    check("0.37.3 vs 0.37.4 -> not newer", versionNewer("0.37.3", "0.37.4"), false);

    // Minor bump.
    check("0.38.0 vs 0.37.9 -> newer",     versionNewer("0.38.0", "0.37.9"), true);
    check("0.37.9 vs 0.38.0 -> not newer", versionNewer("0.37.9", "0.38.0"), false);

    // Major bump.
    check("1.0.0  vs 0.99.99 -> newer",    versionNewer("1.0.0",  "0.99.99"), true);

    // Missing trailing segments → treated as 0.
    check("0.37   vs 0.37.0  -> not newer (== 0.37.0)", versionNewer("0.37", "0.37.0"), false);
    check("0.37.1 vs 0.37    -> newer",                 versionNewer("0.37.1", "0.37"), true);

    // Double-digit segments — the classic "0.10 < 0.9 if string-compared"
    // trap. Must be numeric.
    check("0.10.0 vs 0.9.99  -> newer (10 > 9)", versionNewer("0.10.0", "0.9.99"), true);
    check("0.9.99 vs 0.10.0  -> not newer",      versionNewer("0.9.99", "0.10.0"), false);

    // Three-digit segments. Real release candidates.
    check("1.2.100 vs 1.2.99 -> newer", versionNewer("1.2.100", "1.2.99"), true);

    // Zero edge cases.
    check("0.0.1  vs 0.0.0  -> newer",     versionNewer("0.0.1", "0.0.0"), true);
    check("0      vs 0.0.0  -> not newer", versionNewer("0",     "0.0.0"), false);

    // Malformed candidate (no digits) — non-numeric prefix segments parse
    // to 0, so this should not be "newer than" a real version.
    check("garbage vs 0.37.3 -> not newer", versionNewer("garbage", "0.37.3"), false);
    check("0.37.3 vs garbage -> newer",     versionNewer("0.37.3", "garbage"), true);

    // Empty candidate.
    check("empty  vs 0.0.1  -> not newer", versionNewer("", "0.0.1"), false);
    check("0.0.1  vs empty  -> newer",     versionNewer("0.0.1", ""), true);

    std::printf("\n%s: %d failure(s)\n",
                failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
