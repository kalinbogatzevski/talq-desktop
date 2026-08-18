#include "PainterTheme.h"
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

// 8-color palette matching MessageBubble.qml authorColor()
static const QColor s_authorPalette[] = {
    QColor("#14b8a6"), QColor("#e07060"), QColor("#f0a050"), QColor("#5ec76a"),
    QColor("#9b7cd4"), QColor("#e87aae"), QColor("#50b8c8"), QColor("#5a9ecf")
};

// 6-color topic palette matching Theme.qml topicPalette
static const QColor s_topicPalette[] = {
    QColor("#14b8a6"), QColor("#e07060"), QColor("#f0a050"),
    QColor("#5ec76a"), QColor("#9b7cd4"), QColor("#e87aae")
};

namespace {
struct Palette {
    const char *bg, *side, *bg2, *surface, *sel, *hover, *line;
    const char *tx, *tx2, *tx3;
    const char *teal, *own, *amber, *clay, *green, *tealInk;
    int ambientAlpha;   // strength of the accent ambient behind the thread
    // Rich-text body tokens. The three dark themes keep the exact hexes that
    // used to be hardcoded in Message.cpp (they measure 4.5-13:1 there, so
    // there is nothing to fix and no reason to churn their look); only Paper
    // is re-derived, where the dark-tuned values were unreadable.
    const char *mention, *link, *codeBg;
};
// Ported verbatim from the approved mockup (mockups/talq-redesign.html).
// AA sweep 2026-08-18. PRODUCT.md commits to WCAG AA "across every theme" and
// adds that the commitment holds "even where they constrain the warm
// aesthetic". An audit of every token pair the painters actually put together
// found 40 failures. Three values move per dark theme, each by the SMALLEST
// step that clears 4.5:1 with headroom, hue and saturation preserved:
//
//   tx3  the muted tier (textTime + textMuted + systemMsg -- one value, three
//        roles) scored 2.4-4.5:1. Timestamps and system messages are content,
//        not decoration, so the 4.5 bar applies. Lifted only as far as the
//        CHROME grounds demand; the own-bubble is handled below instead, so
//        the muted tier stays clearly below tx2 and the hierarchy survives.
//   own  the sent-bubble fill. It is the one ground where a muted ink could
//        not be rescued without flattening tx2/tx3 together, so the fill
//        deepens instead. DESIGN.md asks that "'mine' reads by hue, not a
//        loud fill", and a deeper fill of the SAME hue is more on-brief, not
//        less; measured dE against the chat ground is 15.7-21.4, so "mine"
//        still reads at a glance. This one step also fixed mention, link and
//        tx2 on that surface at once.
//   codeBg  #1f2937 was a cool slate at hue 215 -- a flat No-Gray Rule
//        violation ("no neutral or cool gray exists in any theme"). Rebuilt
//        on each theme's own ground hue, so the code panel is now warm like
//        everything else and still reads as its own surface (dE 12.5-22.0).
//
// link also lifts slightly and is now one shared value across the three dark
// themes (it was already shared); mention needed no change anywhere.
const Palette kEmber{
    "#221c16","#1d1813","#281f18","#322820","#382c1f","#2b221a","#3a3027",
    "#f6f1e8","#b4ab9c","#a1988b",
    "#1ac2af","#18312d","#f3a948","#ec8a64","#5fce72","#0e1817", 13,
    "#14b8a6","#62a3d1","#16110d"};
const Palette kWarm{
    "#241d15","#1f1812","#2c2318","#382c1d","#3f311e","#2e2418","#43372a",
    "#f7f1e6","#bcae99","#a99e8b",
    "#1ecdb6","#17332d","#f6ad3e","#f08a62","#61d36f","#0e1817", 18,
    "#14b8a6","#62a3d1","#17120c"};
const Palette kVivid{
    "#271d12","#211810","#312517","#3d2e1b","#46341d","#342711","#4b3a23",
    "#fcf5e7","#c8b89a","#afa38a",
    "#21e3c8","#133630","#ffb84a","#ff9163","#66dd76","#06201c", 28,
    "#14b8a6","#62a3d1","#19120a"};
// Paper's three are re-derived, not inherited: measured against BOTH bubble
// fills this theme actually paints (bgSurface #fffdf5 for a peer's bubble,
// bgMessageOwn #d7ece6 for your own), the dark values scored 2.02-2.85:1 and
// the slate code fill 1.22:1. These clear 4.5:1 on both with headroom --
// mention 6.36/5.25, link 6.75/5.57, code text 13.98:1 -- while staying in
// the same teal/blue families and, for the code fill, in Paper's warm range
// instead of the cool slate that clashed with it anyway.
// Paper's four moves are all "darken for a light ground". tx3 for the same
// muted-tier reason as the dark themes (3.48:1). The other three are the
// semantic colours, which are painted as TEXT and not only as dots: amber and
// online carry the header's away/presence subtitle (HeaderPainter), danger
// carries the failed-send status and time (ChatPainter). On Paper they
// measured 2.40-3.23:1 -- the worst text in the app after the code panel.
// Darkening also strictly improves their OTHER role as badge/dot fills on a
// light ground, so no call site had to change and no token had to split.
const Palette kPaper{
    "#fbf6ed","#f3eddf","#efe8d8","#fffdf5","#e9e0cd","#efe8d8","#e1d8c4",
    "#1a1613","#5f5a52","#666055",
    "#0d9488","#d7ece6","#855615","#9e462d","#296d33","#fffdf5", 14,
    "#0c6a5f","#1d5f8a","#ece2cd"};

const Palette &paletteFor(PainterTheme::Theme t) {
    switch (t) {
    case PainterTheme::Theme::Ember: return kEmber;
    case PainterTheme::Theme::Warm:  return kWarm;
    case PainterTheme::Theme::Paper: return kPaper;
    case PainterTheme::Theme::Vivid: default: return kVivid;
    }
}

// WCAG 2.x relative luminance / contrast ratio (the standard formula: sRGB
// channels linearised, weighted 0.2126/0.7152/0.0722; contrast is the two
// luminances' (L+0.05) ratio, lighter over darker). Used by inkOn() to pick
// the readable ink per fill instead of one ink calibrated for a single hue.
double srgbChannelToLinear(int channel8)
{
    const double c = channel8 / 255.0;
    // Breakpoint deliberately 0.04045, not the 0.03928 the WCAG prose quotes:
    // 0.04045 is the real IEC 61966-2-1 sRGB breakpoint (where the linear and
    // gamma segments meet exactly), and the two values are output-identical
    // for every 8-bit channel anyway -- they only diverge for channel values
    // strictly between 10.0164 and 10.3148, an empty integer range. Not a typo.
    return c <= 0.04045 ? c / 12.92 : qPow((c + 0.055) / 1.055, 2.4);
}

double relativeLuminance(const QColor &c)
{
    return 0.2126 * srgbChannelToLinear(c.red())
         + 0.7152 * srgbChannelToLinear(c.green())
         + 0.0722 * srgbChannelToLinear(c.blue());
}

double contrastRatio(const QColor &a, const QColor &b)
{
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    const double hi = qMax(la, lb);
    const double lo = qMin(la, lb);
    return (hi + 0.05) / (lo + 0.05);
}

// Source-over composite of a translucent colour onto an opaque one, so a
// ground that is painted UNDER a wash can be measured as what the eye sees
// rather than as the bare fill.
QColor blendOver(const QColor &base, const QColor &over)
{
    const double a = over.alphaF();
    return QColor::fromRgbF(base.redF()   * (1 - a) + over.redF()   * a,
                            base.greenF() * (1 - a) + over.greenF() * a,
                            base.blueF()  * (1 - a) + over.blueF()  * a);
}
} // namespace

PainterTheme::PainterTheme(Theme t, qreal fontScale)
    : theme(t), m_fontScale(fontScale)
{
    const Palette &p = paletteFor(t);
    isLight = (t == Theme::Paper);

    bgPrimary    = QColor(p.bg);
    bgSidebar    = QColor(p.side);
    bgSecondary  = QColor(p.bg2);
    bgSurface    = QColor(p.surface);
    bgSelected   = QColor(p.sel);
    bgHover      = QColor(p.hover);
    bgMessageOwn = QColor(p.own);
    divider      = QColor(p.line);

    textPrimary   = QColor(p.tx);
    textSecondary = QColor(p.tx2);
    textTime      = QColor(p.tx3);
    textMuted     = QColor(p.tx3);
    systemMsg     = QColor(p.tx3);

    accent      = QColor(p.teal);
    unreadBadge = QColor(p.teal);
    controlInk  = QColor(p.tealInk);
    amber       = QColor(p.amber);
    online      = QColor(p.green);
    success     = QColor(p.green);
    danger      = QColor(p.clay);

    mention = QColor(p.mention);
    link    = QColor(p.link);
    codeBg  = QColor(p.codeBg);

    // Accent as TEXT: corrected against the worst ground it is painted on.
    // The sidebar rows are that worst case (the unread timestamp sits on
    // bgSelected / bgHover / bgSidebar), so correcting for bgSelected covers
    // the others too.
    accentText = readableOn(accent, bgSelected);

    // Author identity as TEXT, corrected against the ground the name is
    // ACTUALLY drawn on. That is not the bubble: LayoutEngine places
    // ml.nameRect at the current y and only THEN advances past it, so the
    // name sits strictly ABOVE bubbleRect and lands on bgPrimary. Correcting
    // against bgSurface (this code's first attempt) undershot on Paper, where
    // bgPrimary #fbf6ed is darker than bgSurface #fffdf5 -- all eight hues
    // measured 4.28-4.47 on the real ground while reading 4.52-4.73 on the
    // bubble they were tuned for, i.e. the fix missed the bar on the one
    // theme it was written for.
    //
    // The thread also carries the ambient accent wash behind it, so the true
    // worst case is bgPrimary composited with that wash at its peak alpha.
    // Correcting against both grounds in turn costs nothing: readableOn
    // returns its input untouched when it already clears the bar, and both
    // grounds sit on the same side of mid-luminance, so the second pass can
    // only continue in the same direction, never fight the first.
    glow = accent;                       // halo color for state
    ambient = accent;                    // soft tint behind the thread
    ambient.setAlpha(p.ambientAlpha);

    // MUST come after `ambient` is assigned, immediately above. Placing this
    // block earlier -- where the other derived tokens sit -- read a
    // default-constructed `ambient` and silently produced inks corrected
    // against a garbage ground: they measured 4.20-4.29 against the real one
    // instead of clearing 4.5. Nothing about the code LOOKED wrong; the
    // conformance test is what caught it. Keep derived-from-derived values
    // last, and keep them in dependency order.
    const QColor threadGround = blendOver(bgPrimary, ambient);
    for (int i = 0; i < 8; ++i) {
        // Correct against the bare ground first, then against the washed one.
        // readableOn returns its input untouched when it already clears the
        // bar, so the second pass only moves the hues the wash actually
        // threatens, and both grounds sit the same side of mid-luminance so
        // it can only continue in the same direction, never undo the first.
        QColor ink = readableOn(s_authorPalette[i], bgPrimary);
        m_authorInk[i] = readableOn(ink, threadGround);
    }

    fontSizeTiny   = qRound(11 * fontScale);
    fontSizeSmall  = qRound(12 * fontScale);
    fontSizeNormal = qRound(14 * fontScale);
    fontSizeLarge  = qRound(16 * fontScale);
}

// Compatibility shim: existing call sites pass a darkMode bool.
PainterTheme::PainterTheme(bool darkMode, qreal fontScale)
    : PainterTheme(darkMode ? Theme::Vivid : Theme::Paper, fontScale) {}

QColor PainterTheme::readableOn(const QColor &c, const QColor &ground, double minRatio)
{
    if (contrastRatio(c, ground) >= minRatio)
        return c;   // already fine -- never move a value that does not need it

    // Move lightness only. Which DIRECTION is decided by the ground, not by
    // the colour: on a light ground the only way up is darker, and vice
    // versa. Saturation is held so the hue keeps its identity and its
    // punch; dropping saturation would desaturate toward gray and walk
    // straight into the No-Gray Rule.
    // float, not qreal: Qt 6 changed the ...F colour accessors to float.
    float h = 0, s = 0, l = 0, a = 1;
    c.getHslF(&h, &s, &l, &a);
    const bool groundIsLight = relativeLuminance(ground) > 0.5;

    QColor best = c;
    double bestRatio = contrastRatio(c, ground);
    for (int i = 1; i <= 100; ++i) {
        const float step = i / 100.0f;
        const float nl = groundIsLight ? l - step : l + step;
        if (nl < 0.0f || nl > 1.0f)
            break;
        QColor cand = QColor::fromHslF(h < 0 ? 0.0f : h, s, nl, a);
        const double r = contrastRatio(cand, ground);
        if (r > bestRatio) { bestRatio = r; best = cand; }
        if (r >= minRatio)
            return cand;
    }
    // Ran out of lightness before reaching the bar (only possible for an
    // extreme ground): return the best we found rather than the original.
    return best;
}

QColor PainterTheme::authorInk(const QString &actorId) const
{
    // Shares authorPaletteIndex() with authorColor() rather than repeating the
    // hash. The first version of this function re-typed it from memory as
    // textbook djb2 (<<5) + hash, i.e. x33 -- but this app's identity hash is
    // (<<5) - hash, i.e. x31, matching Theme.qml/MessageBubble.qml. Roughly
    // seven of every eight actorIds landed on a DIFFERENT palette entry, so a
    // person's name and the avatar drawn beside it showed unrelated hues.
    // Nothing caught it, because either index yields a contrast-corrected
    // colour. One function, one hash, so the two cannot disagree again.
    return m_authorInk[authorPaletteIndex(actorId)];
}

QColor PainterTheme::authorInkAt(int index) const
{
    return m_authorInk[qBound(0, index, 7)];
}

int PainterTheme::authorPaletteIndex(const QString &actorId)
{
    // djb2 as this project has always computed it (x31), matching
    // Theme.qml stringHash / MessageBubble.qml authorColor.
    uint32_t hash = 5381;
    for (int i = 0; i < actorId.length(); ++i)
        hash = ((hash << 5) - hash) + actorId.at(i).unicode();
    return static_cast<int>(hash % 8);
}

QString PainterTheme::richTextStyleSheet() const
{
    // <code>/<pre> set BOTH fill and ink. Ink is not left to the paint-time
    // palette on purpose: the old markup set only the fill, and the ink that
    // fell through was textPrimary -- which is exactly how Paper ended up
    // painting near-black on a dark slate. Naming both keeps the pair
    // readable by construction rather than by coincidence of the two tokens.
    //
    // The `a` rule is likewise mandatory, not decorative. Qt resolves anchor
    // colour at setHtml time from the application palette, and MainWindow
    // deliberately repurposes QPalette::Link as a theme-token slot (it holds
    // theme.online, a presence green). Without this rule, stripping the old
    // inline link colour would silently render every message hyperlink green.
    return QStringLiteral(
        "pre { background-color:%1; color:%2; font-family:Consolas,monospace }"
        " code { background-color:%1; color:%2; font-family:Consolas,monospace }"
        " a { color:%3 }"
        " .mention { color:%4 }")
        .arg(codeBg.name(), textPrimary.name(), link.name(), mention.name());
}

QString PainterTheme::themeId(Theme t)
{
    switch (t) {
    case Theme::Ember: return QStringLiteral("ember");
    case Theme::Warm:  return QStringLiteral("warm");
    case Theme::Paper: return QStringLiteral("paper");
    case Theme::Vivid: default: return QStringLiteral("vivid");
    }
}

QString PainterTheme::themeLabel(Theme t)
{
    switch (t) {
    case Theme::Ember: return QStringLiteral("Ember");
    case Theme::Warm:  return QStringLiteral("Warm");
    case Theme::Paper: return QStringLiteral("Paper");
    case Theme::Vivid: default: return QStringLiteral("Vivid");
    }
}

PainterTheme::Theme PainterTheme::themeFromId(const QString &id, Theme fallback)
{
    if (id == QLatin1String("ember")) return Theme::Ember;
    if (id == QLatin1String("warm"))  return Theme::Warm;
    if (id == QLatin1String("vivid")) return Theme::Vivid;
    if (id == QLatin1String("paper")) return Theme::Paper;
    return fallback;
}

PainterTheme::Theme PainterTheme::cycle(Theme t)
{
    switch (t) {
    case Theme::Ember: return Theme::Warm;
    case Theme::Warm:  return Theme::Vivid;
    case Theme::Vivid: return Theme::Paper;
    case Theme::Paper: default: return Theme::Ember;
    }
}

QColor PainterTheme::authorColor(const QString &actorId)
{
    // The hash itself lives in authorPaletteIndex() so authorInk() cannot
    // drift from it (it once did -- see the note there).
    return s_authorPalette[authorPaletteIndex(actorId)];
}

int PainterTheme::authorPaletteSize()
{
    return static_cast<int>(sizeof(s_authorPalette) / sizeof(s_authorPalette[0]));
}

QColor PainterTheme::authorPaletteAt(int index)
{
    return s_authorPalette[qAbs(index) % authorPaletteSize()];
}

QColor PainterTheme::inkOn(const QColor &fill) const
{
    return contrastRatio(fill, controlInk) >= contrastRatio(fill, textPrimary)
        ? controlInk : textPrimary;
}

QColor PainterTheme::topicColor(int index)
{
    constexpr int N = sizeof(s_topicPalette) / sizeof(s_topicPalette[0]);
    return s_topicPalette[qAbs(index) % N];
}

int PainterTheme::topicPaletteSize()
{
    return static_cast<int>(sizeof(s_topicPalette) / sizeof(s_topicPalette[0]));
}

QImage PainterTheme::cropToCircle(const QImage &source, int size)
{
    QImage scaled = source.scaled(size, size, Qt::KeepAspectRatioByExpanding,
                                  Qt::SmoothTransformation);
    int cx = (scaled.width() - size) / 2;
    int cy = (scaled.height() - size) / 2;
    if (cx > 0 || cy > 0)
        scaled = scaled.copy(cx, cy, size, size);

    QImage result(size, size, QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);
    {
        QPainter painter(&result);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addEllipse(0, 0, size, size);
        painter.setClipPath(path);
        painter.drawImage(0, 0, scaled);
    }
    return result;
}

QString PainterTheme::formatRelativeTime(qint64 epochSecs)
{
    if (epochSecs <= 0)
        return {};

    QDateTime dt = QDateTime::fromSecsSinceEpoch(epochSecs);
    QDateTime now = QDateTime::currentDateTime();

    if (dt.date() == now.date())
        return dt.toString(QStringLiteral("HH:mm"));

    if (dt.date() == now.date().addDays(-1))
        return QStringLiteral("Yesterday");

    return dt.toString(QStringLiteral("dd MMM"));
}

QString PainterTheme::formatPreviewText(const QString &author, const QString &message)
{
    if (message.isEmpty())
        return {};
    // Collapse line breaks and tabs so the single-line preview shows the
    // first real content rather than bailing out at the first CRLF.
    QString flat = message;
    flat.replace(QLatin1Char('\r'), QLatin1Char(' '));
    flat.replace(QLatin1Char('\n'), QLatin1Char(' '));
    flat.replace(QLatin1Char('\t'), QLatin1Char(' '));
    while (flat.contains(QStringLiteral("  ")))
        flat.replace(QStringLiteral("  "), QStringLiteral(" "));
    flat = flat.trimmed();
    if (author.isEmpty())
        return flat;
    return author + QStringLiteral(": ") + flat;
}

static QFont interFont()
{
    // Inter is registered in main.cpp. If the registration failed for some
    // reason, Qt's default font is used as a silent fallback.
    QFont f(QStringLiteral("Inter"));
    f.setHintingPreference(QFont::PreferFullHinting);
    return f;
}

QFont PainterTheme::bodyFont() const
{
    QFont f = interFont();
    f.setPixelSize(fontSizeNormal);
    return f;
}

QFont PainterTheme::nameFont() const
{
    QFont f = interFont();
    f.setPixelSize(fontSizeSmall);
    f.setWeight(QFont::DemiBold);
    return f;
}

QFont PainterTheme::timeFont() const
{
    QFont f = interFont();
    f.setPixelSize(fontSizeTiny);
    return f;
}

QFont PainterTheme::systemFont() const
{
    // Two-Lever Rule: no italics for hierarchy. System messages are
    // differentiated by the muted systemMsg color + tiny size, not slant.
    QFont f = interFont();
    f.setPixelSize(fontSizeTiny);
    return f;
}

QFont PainterTheme::dateSepFont() const
{
    QFont f = interFont();
    f.setPixelSize(fontSizeTiny);
    f.setWeight(QFont::DemiBold);
    return f;
}

