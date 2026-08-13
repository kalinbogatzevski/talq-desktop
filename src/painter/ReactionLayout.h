#pragma once
#include <vector>

// Reaction pill geometry — the SINGLE source of truth for both painting and
// hit-testing.
//
// THE BUG THIS EXISTS TO PREVENT: painting and hit-testing each computed pill
// rects independently and disagreed three ways — the hit-test measured the
// whole "emoji count" token with one font and added 14, while the painter
// measured emoji and count with two different fonts and added padding; the
// hit-test assumed a 22px height while the painter used the bar's; and the
// painter stopped drawing when the bar overflowed while the hit-test kept
// walking. A wide or multi-emoji reaction therefore had clickable area that
// drifted off the pill the user could see, and clicks could land on pills that
// were never drawn at all.
//
// Text measurement stays with the caller (it needs QFontMetrics and the
// theme's fonts); only the arithmetic lives here, which is what makes it
// testable without a font stack.
namespace talq {

// Measured text widths for one pill, in device-independent pixels.
struct ReactionMetrics {
    int emojiWidth = 0;
    int countWidth = 0;
};

// Where one pill goes. `visible` is false once the bar has overflowed —
// both the painter and the hit-test must honour it, or they diverge again.
struct ReactionRect {
    double x = 0;
    double width = 0;
    bool   visible = false;
};

struct ReactionLayoutParams {
    double barLeft = 0;
    double barRight = 0;
    double padX = 6;            // horizontal padding inside a pill
    double emojiCountGap = 3;   // between the emoji and its count
    double pillGap = 4;         // between adjacent pills
};

// Returns exactly one rect per input pill, in order, so callers can index by
// reaction number. Rects past the overflow point are returned with
// visible=false rather than omitted.
inline std::vector<ReactionRect>
layoutReactionPills(const std::vector<ReactionMetrics> &pills,
                    const ReactionLayoutParams &p)
{
    std::vector<ReactionRect> out;
    out.reserve(pills.size());

    double x = p.barLeft;
    bool overflowed = false;
    for (const ReactionMetrics &m : pills) {
        if (overflowed) { out.push_back(ReactionRect{}); continue; }

        const double w = p.padX + m.emojiWidth + p.emojiCountGap
                       + m.countWidth + p.padX;
        if (x + w > p.barRight) {
            overflowed = true;
            out.push_back(ReactionRect{});
            continue;
        }
        ReactionRect r;
        r.x = x;
        r.width = w;
        r.visible = true;
        out.push_back(r);
        x += w + p.pillGap;
    }
    return out;
}

} // namespace talq
