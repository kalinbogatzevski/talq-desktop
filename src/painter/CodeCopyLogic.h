#pragma once

// Code-block copy chip -- the SINGLE source of truth for where the chip sits
// beside a fenced code block, when it exists at all, and what counts as a click
// on it.
//
// WHY THIS IS ONE FILE: the chip is painted over the message body by
// ChatPainter::paintCodeChips while hitTestAt, the press/release handlers, the
// hover state and the tooltip must all agree on exactly the same disc. This is
// the rule ReactionLayout.h and JumpToBottomLogic.h apply for the same reason:
// paint and hit-test once computed their own rects and drifted apart.
//
// WHY A GUTTER, NOT AN OVERLAY. The first version drew the chip over the block's
// top-right corner. A <pre> has no right padding and a bubble shrink-wraps to
// its widest line (contentW = idealWidth + 2), so for a short command -- the
// commonest code block -- the opaque disc covered the last three characters of
// the very line it copies, and a press there was claimed by the chip instead of
// selecting text. CSS padding is ignored for blocks in Qt, so room cannot be
// reserved INSIDE the panel; a right MARGIN can, and it is counted by
// idealWidth (the bubble grows to include it) and is excluded from the painted
// fill (measured, Qt 6.10.1). So `pre` gets `margin-right: kCodeChipGutter` in
// PainterTheme::richTextStyleSheet, the panel is that much narrower than the
// text column, and the chip sits in the strip that is left. It never overlaps a
// character, and it exists whether or not the chip is currently offered, so a
// pending message and its echo have the same width.
//
// Measurement of the block's rect is deliberately NOT here (it comes from the
// QTextDocument's layout); the chip is derived from a plain rectangle, so the
// geometry is testable without a font stack and cannot come out differently at
// the call sites.
namespace talq {

// Diameter matches the 22-24px icon buttons elsewhere (info-card copy button is
// 22px, hover bar 28px).
constexpr double kCodeChipDiameter = 24.0;
// Gap above the chip when the block is tall enough to hold it.
constexpr double kCodeChipInset = 6.0;
// Width of the strip reserved to the right of every code block, in px. Also the
// `margin-right` the style sheet gives a <pre> -- ONE number for both.
constexpr double kCodeChipGutter = 30.0;
static_assert(kCodeChipGutter >= kCodeChipDiameter,
              "the gutter must be able to hold the chip");

// How long the chip shows its "copied" tick.
constexpr int kCodeChipCopiedMs = 1500;

struct CodeChipParams {
    double diameter = kCodeChipDiameter;
    double inset    = kCodeChipInset;
    double gutter   = kCodeChipGutter;
};

struct CodeChipRect {
    double x = 0, y = 0, w = 0, h = 0;
    bool   visible = false;
    double cx() const { return x + w / 2.0; }
    double cy() const { return y + h / 2.0; }
};

// Where the viewport's top edge is, in the same space as the block's rectangle.
// When it is known, a block taller than the viewport keeps its chip on screen
// instead of leaving it behind at the block's (scrolled-away) top. Only the top
// edge matters: the chip sits at the top of the block, so it is the block's top
// that scrolls away; the viewport's bottom edge never hides it.
struct CodeChipView {
    bool   known = false;
    double top = 0;
};

// (bx,by,bw,bh) is the code block's PANEL rectangle (the painted fill, margin
// excluded) in whatever space the caller works in; the result is in the same
// space.
//
//  * Centred in the gutter to the right of the panel, so it is never over text.
//  * Aligned to the block's top, like every copy-code control people already
//    know, so it never drifts down a tall block...
//  * ...unless that top is scrolled out of view. A block taller than the
//    viewport (a long log, a stack trace) would otherwise have no reachable
//    chip at all once its top edge has gone. With `view` known the chip follows
//    the top of the viewport down the block (staying `inset` below it), and
//    stops at the block's own last inset: it never leaves the block, and it is
//    where it always was whenever the block's top is on screen.
//  * A one-line block is ~17px tall, shorter than the disc. It is then centred
//    on the block and overhangs it a few pixels above and below rather than
//    being clipped; the ring is contrast-scored on the bubble ground (see the
//    conformance test), which is what it overhangs onto.
//  * Hidden only for a degenerate block (no height).
inline CodeChipRect layoutCodeChip(double bx, double by, double bw, double bh,
                                   const CodeChipParams &p = {},
                                   const CodeChipView &view = {})
{
    CodeChipRect r;
    if (bh <= 0.0 || bw <= 0.0)
        return r;

    r.visible = true;
    r.w = r.h = p.diameter;
    r.x = bx + bw + (p.gutter - p.diameter) / 2.0;
    if (bh >= p.diameter + 2.0 * p.inset) {
        const double lo = by + p.inset;                          // the block's top
        const double hi = by + bh - p.diameter - p.inset;        // its last position (>= lo)
        r.y = lo;
        if (view.known) {
            const double follow = view.top + p.inset;
            r.y = follow > lo ? (follow < hi ? follow : hi) : lo;
        }
    } else {
        r.y = by + (bh - p.diameter) / 2.0;
    }
    return r;
}

// Is (px,py) on the chip? A circle, like the jump control: the corners of the
// bounding box are NOT a hit, because that is the shape that is painted.
inline bool codeChipHit(const CodeChipRect &r, double px, double py)
{
    if (!r.visible) return false;
    const double rad = r.w / 2.0;
    const double dx = px - r.cx();
    const double dy = py - r.cy();
    return dx * dx + dy * dy <= rad * rad;
}

// Whether chips are offered on a row at all. One predicate, shared by paint,
// hit-test, hover and tooltip, so the pointer can never turn into a hand over a
// chip that is not drawn. It is the hover bar's own rule: nothing on system
// rows, on a message that has not been delivered (its id is still negative and
// its text may yet change), or while whole-message selection owns clicks.
inline bool codeChipsOffered(bool selectionMode, bool isSystem, bool sending,
                             bool failed, int messageId)
{
    return !selectionMode && !isSystem && !sending && !failed && messageId > 0;
}

} // namespace talq
