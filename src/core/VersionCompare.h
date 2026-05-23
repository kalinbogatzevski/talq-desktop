#pragma once

// VersionCompare — header-only, no Qt deps. The auto-updater asks "is the
// candidate version newer than what we're running?"; that test must be
// rock-solid or users either stay on a broken build (false negative) or
// downgrade themselves (false positive). Pure function so it can be
// unit-tested in milliseconds without spinning up QNetworkAccessManager.
//
// Semantics: dotted decimal segments compared numerically, left-to-right.
// Missing trailing segments are treated as 0 ("0.37" == "0.37.0"). The
// parser matches the original QString::toInt(&ok)-based path for the
// clean dotted-decimal inputs the manifest emits (our only real input);
// embedded non-digit characters (e.g. "1.2.3-rc1") and leading whitespace
// are tolerated more leniently than the original — we ignore those chars
// rather than failing the whole segment to 0. This is a deliberate
// behavioural choice (more forgiving), not a literal re-implementation.

#include <cstring>
#include <string>
#include <vector>

namespace talq {

inline std::vector<int> parseVersionSegments(const std::string &v)
{
    std::vector<int> out;
    int cur = 0;
    bool inDigit = false;
    bool seenAnyDigit = false;
    for (char c : v) {
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            inDigit = true;
            seenAnyDigit = true;
        } else if (c == '.') {
            // segment break; if the segment had no digits we still push 0
            // (matches QString::toInt's ok=false → out.push_back(0) path).
            out.push_back(seenAnyDigit ? cur : 0);
            cur = 0;
            inDigit = false;
            seenAnyDigit = false;
        }
        // Any other char (e.g. "-beta") is ignored mid-segment; trailing
        // non-digit suffixes don't change the integer value of the prefix.
    }
    out.push_back(seenAnyDigit ? cur : 0);
    return out;
}

inline bool versionNewer(const std::string &candidate, const std::string &current)
{
    const auto a = parseVersionSegments(candidate);
    const auto b = parseVersionSegments(current);
    const size_t n = a.size() > b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const int va = i < a.size() ? a[i] : 0;
        const int vb = i < b.size() ? b[i] : 0;
        if (va != vb) return va > vb;
    }
    return false;
}

inline bool versionNewer(const char *candidate, const char *current)
{
    return versionNewer(std::string(candidate ? candidate : ""),
                        std::string(current   ? current   : ""));
}

} // namespace talq
