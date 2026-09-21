#pragma once
#include <algorithm>
#include <string>

// Jump-to-bottom control -- the SINGLE source of truth for where the button and
// its count badge sit, when they show, what the count says, and what counts as
// a click on them.
//
// WHY THIS IS ONE FILE: the control floats over the message rows at a fixed
// spot in the VIEWPORT, so ChatPainter::paintEvent draws it while hitTestAt and
// the mouse handlers must agree on exactly the same disc and pill. ReactionLayout.h
// exists because paint and hit-test once computed pill rects separately and
// drifted apart; this applies the same rule before the drift can happen.
//
// Text measurement is deliberately NOT here. The badge is sized from its
// character count, not from font metrics, so the geometry is testable without a
// font stack AND cannot come out differently at the two call sites.
namespace talq {

// A rectangle in viewport coordinates (0,0 = top-left of the chat widget).
struct JumpRect {
    double x = 0, y = 0, w = 0, h = 0;
    bool   isNull() const { return w <= 0 || h <= 0; }
    double right()  const { return x + w; }
    double bottom() const { return y + h; }
    double cx()     const { return x + w / 2.0; }
    double cy()     const { return y + h / 2.0; }
};

// Bigger than the hover bar's 28px round buttons on purpose: this is the one
// control reached for from anywhere in the list, and at 28px it read as
// decoration rather than as something to press.
constexpr double kJumpDiameter     = 36.0;
// The painted scrollbar's grab strip: ChatPainter claims a left press in the
// rightmost this-many px as a scrollbar grab. Defined HERE, next to the inset
// that depends on it, and used by ChatPainter::mousePressEvent, so the two
// cannot drift apart.
constexpr double kScrollbarGrabStrip = 14.0;
// Gap from the widget's right edge to the disc. It clears the grab strip with
// 6px to spare, so a press meant for the scrollbar can never land here.
constexpr double kJumpRightInset   = 20.0;
static_assert(kJumpRightInset >= kScrollbarGrabStrip + 4.0,
              "the jump control must stay clear of the scrollbar's grab strip");
constexpr double kJumpBottomInset  = 14.0;
// Each character past the first widens the badge by this much.
constexpr double kJumpBadgeDigitW  = 6.0;

struct JumpToBottomLayout {
    bool        visible = false;
    JumpRect    button;      // the disc; null while hidden
    JumpRect    badge;       // count pill straddling the disc's top edge; null at count 0
    std::string badgeText;   // "" | "1".."99" | "99+"
};

// The count as drawn: nothing at 0, capped at 99+.
inline std::string jumpBadgeText(int n)
{
    if (n <= 0)  return {};
    if (n > 99)  return "99+";
    return std::to_string(n);
}

// Whether the control shows at all. `atBottom` is the view's own answer
// (ChatPainter::atBottom), passed in so its slop lives in exactly one place.
//
// The rule is deliberately just "not on the newest message", with no distance
// threshold and no dependence on new arrivals: the control is there however the
// user came to be looking at older content -- the wheel, the scrollbar, or being
// taken to an old message by a quote click or a search hit -- so getting back
// never needs the scroll.
inline bool jumpToBottomVisible(bool atBottom, bool selectionMode)
{
    return !atBottom && !selectionMode;
}

// `badgeHeight` is PainterTheme::badgeHeight, passed in so this header does not
// carry a second copy of the constant.
inline JumpToBottomLayout layoutJumpToBottom(double viewW, double viewH, bool visible,
                                             int newBelow, double badgeHeight)
{
    JumpToBottomLayout L;
    // Too small to hold the control without clipping it or its badge: hide it
    // rather than draw a broken one.
    if (!visible
        || viewW < kJumpRightInset + kJumpDiameter
        || viewH < kJumpBottomInset + kJumpDiameter + badgeHeight / 2.0)
        return L;

    L.visible = true;
    L.button  = { viewW - kJumpRightInset - kJumpDiameter,
                  viewH - kJumpBottomInset - kJumpDiameter,
                  kJumpDiameter, kJumpDiameter };

    L.badgeText = jumpBadgeText(newBelow);
    if (!L.badgeText.empty()) {
        const double w = badgeHeight
                       + kJumpBadgeDigitW * double(L.badgeText.size() - 1);
        L.badge = { L.button.cx() - w / 2.0,
                    L.button.y - badgeHeight / 2.0,   // centred on the disc's top edge
                    w, badgeHeight };
    }
    return L;
}

// Is (px,py) on the control? The disc is a circle and the badge a stadium, so
// the corners of their bounding boxes are NOT hits -- that is the shape that is
// painted.
inline bool jumpToBottomHit(const JumpToBottomLayout &L, double px, double py)
{
    if (!L.visible) return false;

    const double r  = L.button.w / 2.0;
    const double dx = px - L.button.cx();
    const double dy = py - L.button.cy();
    if (dx * dx + dy * dy <= r * r) return true;

    if (L.badge.isNull()) return false;
    // Stadium: inside when the distance to the segment joining its two end-cap
    // centres is within the cap radius.
    const double br  = L.badge.h / 2.0;
    const double nx  = std::min(std::max(px, L.badge.x + br), L.badge.right() - br);
    const double bdx = px - nx;
    const double bdy = py - L.badge.cy();
    return bdx * bdx + bdy * bdy <= br * br;
}

// ── The new-message count ────────────────────────────────────────────────
//
// The count is DERIVED, not incremented: it is how many messages from other
// people sit newer than the newest one the user had on screen the last time the
// view was at the bottom. Deriving it means no signal has to be caught -- a
// context-window jump, a history prepend, a re-sort and a poll batch all leave
// it right without any of them knowing it exists -- and it clears itself the
// moment the view reaches the bottom by any route.
//
// Rows are OLDEST-first (the order of ChatPainter::m_layouts) and `factAt(i)`
// returns the JumpRowFacts of row i.
struct JumpRowFacts {
    int  id     = 0;       // <= 0 is a pending send that has no server id yet
    bool own    = false;
    bool system = false;
};

// Newest real (server) id, skipping pending sends at the end. 0 when there is none.
template <typename FactAt>
inline int newestRealId(int rowCount, FactAt factAt)
{
    for (int i = rowCount - 1; i >= 0; --i) {
        const int id = factAt(i).id;
        if (id > 0) return id;
    }
    return 0;
}

// `seenNewestId` <= 0 means "not known yet": no count is ever guessed. Own
// messages and system rows never count -- a reply you sent from your phone or a
// "so-and-so joined" line is not something waiting for you.
template <typename FactAt>
inline int countNewBelow(int rowCount, FactAt factAt, int seenNewestId)
{
    if (seenNewestId <= 0) return 0;
    int n = 0;
    for (int i = rowCount - 1; i >= 0; --i) {
        const JumpRowFacts f = factAt(i);
        // Ids ascend toward the newest row, so the first already-seen real id
        // ends the walk: it costs the number of NEW rows, not the history length.
        if (f.id > 0 && f.id <= seenNewestId) break;
        if (f.id <= 0 || f.own || f.system) continue;
        ++n;
    }
    return n;
}

} // namespace talq
