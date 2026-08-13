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
};
// Ported verbatim from the approved mockup (mockups/talq-redesign.html).
const Palette kEmber{
    "#221c16","#1d1813","#281f18","#322820","#382c1f","#2b221a","#3a3027",
    "#f6f1e8","#b4ab9c","#8d8273",
    "#1ac2af","#20413b","#f3a948","#ec8a64","#5fce72","#0e1817", 13};
const Palette kWarm{
    "#241d15","#1f1812","#2c2318","#382c1d","#3f311e","#2e2418","#43372a",
    "#f7f1e6","#bcae99","#948770",
    "#1ecdb6","#224c43","#f6ad3e","#f08a62","#61d36f","#0e1817", 18};
const Palette kVivid{
    "#271d12","#211810","#312517","#3d2e1b","#46341d","#342711","#4b3a23",
    "#fcf5e7","#c8b89a","#9c8c6d",
    "#21e3c8","#1f5a4f","#ffb84a","#ff9163","#66dd76","#06201c", 28};
const Palette kPaper{
    "#fbf6ed","#f3eddf","#efe8d8","#fffdf5","#e9e0cd","#efe8d8","#e1d8c4",
    "#1a1613","#5f5a52","#7c7568",
    "#0d9488","#d7ece6","#c8821f","#c75a3a","#3a9b48","#fffdf5", 14};

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

    glow = accent;                       // halo color for state
    ambient = accent;                    // soft tint behind the thread
    ambient.setAlpha(p.ambientAlpha);

    fontSizeTiny   = qRound(11 * fontScale);
    fontSizeSmall  = qRound(12 * fontScale);
    fontSizeNormal = qRound(14 * fontScale);
    fontSizeLarge  = qRound(16 * fontScale);
}

// Compatibility shim: existing call sites pass a darkMode bool.
PainterTheme::PainterTheme(bool darkMode, qreal fontScale)
    : PainterTheme(darkMode ? Theme::Vivid : Theme::Paper, fontScale) {}

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
    // djb2 hash -- matches Theme.qml stringHash / MessageBubble.qml authorColor
    uint32_t hash = 5381;
    for (int i = 0; i < actorId.length(); ++i) {
        hash = ((hash << 5) - hash) + actorId.at(i).unicode();
    }
    int idx = static_cast<int>(hash % 8);
    return s_authorPalette[idx];
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

