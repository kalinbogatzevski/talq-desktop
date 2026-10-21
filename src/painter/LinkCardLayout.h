#pragma once

// Geometry of a link-preview card (the box under a message that unfurls a URL).
//
// ONE place computes it, and both the layout pass (LayoutEngine, which needs the
// card's HEIGHT to size the bubble) and the painter (which needs every RECT and
// the exact lines of text) call it. They must never measure separately: a card
// whose layout wrapped the title onto two lines while the painter drew three
// would spill over the reactions bar -- the same drift the reaction pills had
// before ReactionLayout.h.
//
// Everything is relative to the card's own top-left, so a cached MessageLayout
// that is slid to a new y (ChatPainter's translateLayoutY) only has to move the
// card's outer rect.

#include "core/LinkPreviewLogic.h"
#include "painter/PainterTheme.h"

#include <QFont>
#include <QFontMetricsF>
#include <QRectF>
#include <QStringList>
#include <QTextLayout>
#include <QVector>

namespace talq {
namespace linkpreview {

constexpr qreal kCardMinWidth  = 300.0;   // the bubble is widened to at least this
constexpr qreal kCardMaxWidth  = 420.0;   // a card never stretches wider than this
constexpr qreal kCardBarWidth  = 3.0;
constexpr qreal kCardPadX      = 10.0;
constexpr qreal kCardPadY      = 8.0;
// Landscape, because the pictures on the web are banners: a square centre crop cut
// most logos in half.
constexpr qreal kCardThumbW    = 96.0;
constexpr qreal kCardThumbH    = 64.0;
constexpr qreal kCardThumbGap  = 10.0;
constexpr qreal kCardLineGap   = 2.0;
constexpr int   kCardTitleLines = 2;
constexpr int   kCardDescLines  = 2;
constexpr qreal kCardGapBelow   = 4.0;    // between a card and whatever follows it

struct CardGeometry {
    qreal height = 0;
    QRectF bar;      // the accent strip down the left edge
    QRectF site;
    QRectF title;
    QRectF desc;     // empty when the page gave no description
    QRectF thumb;    // empty when the card has no picture
};

// The pure arithmetic. The line counts and heights come from the font side
// (measureCard below), so this part is testable without a font database.
inline CardGeometry cardGeometry(qreal cardW, bool hasThumb,
                                 qreal siteLineH, qreal titleLineH, int titleLines,
                                 qreal descLineH, int descLines)
{
    CardGeometry g;
    const qreal textLeft  = kCardBarWidth + kCardPadX;
    const qreal textRight = cardW - kCardPadX
                          - (hasThumb ? kCardThumbW + kCardThumbGap : 0.0);
    const qreal textW = qMax<qreal>(0.0, textRight - textLeft);

    qreal y = kCardPadY;
    g.site = QRectF(textLeft, y, textW, siteLineH);
    y += siteLineH + kCardLineGap;
    g.title = QRectF(textLeft, y, textW, titleLines * titleLineH);
    y += titleLines * titleLineH;
    if (descLines > 0) {
        y += kCardLineGap;
        g.desc = QRectF(textLeft, y, textW, descLines * descLineH);
        y += descLines * descLineH;
    }
    y += kCardPadY;

    if (hasThumb) {
        g.thumb = QRectF(cardW - kCardPadX - kCardThumbW, kCardPadY,
                         kCardThumbW, kCardThumbH);
        y = qMax(y, kCardPadY * 2 + kCardThumbH);
    }
    g.height = y;
    g.bar = QRectF(0, 0, kCardBarWidth, g.height);
    return g;
}

struct CardFonts {
    QFont site, title, desc;
};

inline CardFonts cardFonts(const PainterTheme &theme)
{
    CardFonts f;
    f.site = theme.timeFont();
    f.site.setBold(true);
    f.title = theme.bodyFont();
    f.title.setBold(true);
    f.desc = theme.timeFont();
    return f;
}

// `text` wrapped to `width` in at most `maxLines` lines; if more text remains,
// the last line carries it, elided with an ellipsis. Returns exactly the lines
// to draw, so the line count used for the height IS the line count painted.
inline QStringList wrapLines(const QFont &font, const QString &text, qreal width, int maxLines)
{
    QStringList out;
    if (text.isEmpty() || maxLines <= 0 || width < 8.0)
        return out;
    QTextLayout tl(text, font);
    QTextOption opt;
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    tl.setTextOption(opt);
    const QFontMetricsF fm(font);
    tl.beginLayout();
    for (;;) {
        QTextLine line = tl.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        const int start = line.textStart();
        if (out.size() == maxLines - 1) {
            out << fm.elidedText(text.mid(start).trimmed(), Qt::ElideRight, width);
            break;
        }
        out << text.mid(start, line.textLength()).trimmed();
    }
    tl.endLayout();
    return out;
}

struct CardMeasure {
    CardGeometry g;
    QStringList titleLines;
    QStringList descLines;
};

// What the card looks like at `cardW`. Used by the layout pass for the height
// and by the painter for everything.
inline CardMeasure measureCard(const PainterTheme &theme, const ReadyCard &card, qreal cardW)
{
    const CardFonts f = cardFonts(theme);
    const QFontMetricsF fmSite(f.site), fmTitle(f.title), fmDesc(f.desc);

    const qreal textLeft  = kCardBarWidth + kCardPadX;
    const qreal textRight = cardW - kCardPadX
                          - (card.hasThumb ? kCardThumbW + kCardThumbGap : 0.0);
    const qreal textW = qMax<qreal>(0.0, textRight - textLeft);

    CardMeasure m;
    m.titleLines = wrapLines(f.title, card.data.title, textW, kCardTitleLines);
    m.descLines  = wrapLines(f.desc, card.data.description, textW, kCardDescLines);
    m.g = cardGeometry(cardW, card.hasThumb,
                       fmSite.lineSpacing(),
                       fmTitle.lineSpacing(), qMax<int>(1, int(m.titleLines.size())),
                       fmDesc.lineSpacing(), int(m.descLines.size()));
    return m;
}

} // namespace linkpreview
} // namespace talq
