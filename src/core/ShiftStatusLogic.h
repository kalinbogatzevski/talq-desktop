#pragma once

// Colleague shift status -- what the client does with the ERP's answer.
//
// TalQ deliberately knows NOTHING about rosters, working hours, breaks or
// public holidays. A site's ERP resolves all of that and sends back a closed
// state plus a display label:
//
//   {"statuses": {"<user-id>": {"state":"on_shift","label":"On shift"}}}
//
// That split is not incidental. TalQ has no permission model and cannot have
// one -- it renders what arrives and cannot tell "you may not see this" from
// "there is nothing to see". So every policy decision belongs to the server,
// and Unknown is deliberately indistinguishable from "we never asked": a
// refused request, a timeout and an unmapped colleague all draw the same
// nothing. That is what makes the feature safe to fail.
//
// What is left here is genuinely the client's job, and each piece of it is a
// mistake somebody would otherwise make later:
//   - degrade a state this build has never heard of, instead of blanking;
//   - keep a server-supplied label from breaking the layout;
//   - decide which surfaces draw which states (a long list marks only the
//     exception -- see shiftMarksExceptionInList);
//   - assemble a batch that cannot grow unbounded because a sidebar got long.

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace talq {

enum class ShiftState {
    Unknown,   // no ERP link, ambiguous link, no permission, or any error
    OnShift,
    OnBreak,
    OffShift,
};

inline const char *shiftStateName(ShiftState s)
{
    switch (s) {
    case ShiftState::OnShift:  return "OnShift";
    case ShiftState::OnBreak:  return "OnBreak";
    case ShiftState::OffShift: return "OffShift";
    case ShiftState::Unknown:  break;
    }
    return "Unknown";
}

// Exact match only -- no trimming, no case folding. A newer server adding a
// state must degrade to Unknown on an older client rather than blanking the
// chip or guessing at a near-miss, and Unknown already has a safe rendering.
// Being strict here also means a malformed payload cannot accidentally land on
// a real state.
inline ShiftState shiftStateFromWire(const std::string &state)
{
    if (state == "on_shift")  return ShiftState::OnShift;
    if (state == "on_break")  return ShiftState::OnBreak;
    if (state == "off_shift") return ShiftState::OffShift;
    return ShiftState::Unknown;
}

// Unknown draws nothing, anywhere.
inline bool shiftStateIsDrawable(ShiftState s)
{
    return s != ShiftState::Unknown;
}

// MARK THE EXCEPTION, NOT THE NORM.
//
// During the working day OnShift is what you expect, so badging it on nearly
// every row in the sidebar would be pure noise -- and noise on every row
// teaches people to stop seeing the rows that matter. In a list, only the
// exceptions are marked. The chat header, where one colleague already has your
// attention and there is room for words, still says it explicitly.
inline bool shiftMarksExceptionInList(ShiftState s)
{
    return s == ShiftState::OnBreak || s == ShiftState::OffShift;
}

inline constexpr int kMaxShiftBatch = 100;          // a request may never exceed this
inline constexpr long long kShiftTtlMs = 120000;    // 2 min; shift state moves in minutes

// A clock that jumps backwards must not pin an entry as "fresh" forever, so a
// stamp in the future counts as stale rather than as very recent.
inline bool shiftEntryIsStale(long long fetchedAtMs, long long nowMs)
{
    if (fetchedAtMs <= 0) return true;      // never fetched
    if (nowMs < fetchedAtMs) return true;   // clock went backwards
    return (nowMs - fetchedAtMs) >= kShiftTtlMs;
}

// Dedupe, drop empties, preserve first-seen order, clamp.
//
// The cap is not politeness. A long sidebar plus an open group room could
// otherwise put an unbounded name list into a single request; the server
// rejects anything larger, so clamping here turns a would-be 400 into a
// slightly-short answer that still renders.
inline std::vector<std::string> buildShiftBatch(const std::vector<std::string> &uids)
{
    const std::size_t cap = static_cast<std::size_t>(kMaxShiftBatch);
    std::vector<std::string> out;
    out.reserve(uids.size() < cap ? uids.size() : cap);
    for (const std::string &u : uids) {
        if (u.empty()) continue;
        if (std::find(out.begin(), out.end(), u) != out.end()) continue;
        out.push_back(u);
        if (out.size() >= cap) break;
    }
    return out;
}

inline constexpr std::size_t kMaxShiftLabelChars = 24;

// The server owns the wording, so "On shift" can become "Rostered" without a
// desktop release. The client owns everything that can break the layout:
// control characters (one newline turns a single-line chip into two) and
// length.
//
// This clamp lives in the tested header rather than in a painter on purpose.
// The CTI card's equivalent guarantees live in its renderer, which means every
// new consumer of that payload silently re-inherits the risk of losing them.
inline std::string sanitizeShiftLabel(const std::string &label)
{
    std::string out;
    out.reserve(label.size() < kMaxShiftLabelChars ? label.size() : kMaxShiftLabelChars);
    for (char c : label) {
        const unsigned char u = static_cast<unsigned char>(c);
        out.push_back((u < 0x20 || u == 0x7f) ? ' ' : c);
        if (out.size() >= kMaxShiftLabelChars) break;
    }
    return out;
}

} // namespace talq
