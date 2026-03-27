#include "PainterTheme.h"
#include <QtMath>

// 8-color palette matching MessageBubble.qml authorColor()
static const QColor s_authorPalette[] = {
    QColor("#2ec4b6"), QColor("#e07060"), QColor("#f0a050"), QColor("#5ec76a"),
    QColor("#9b7cd4"), QColor("#e87aae"), QColor("#50b8c8"), QColor("#5a9ecf")
};

PainterTheme::PainterTheme(bool darkMode, qreal fontScale)
    : m_fontScale(fontScale)
{
    // ── Backgrounds ──
    bgPrimary    = darkMode ? QColor("#121210") : QColor("#ffffff");
    bgSecondary  = darkMode ? QColor("#1a1a16") : QColor("#f5f5f2");
    bgMessageOwn = darkMode ? QColor("#1a302e") : QColor("#d4f0ed");
    bgSurface    = darkMode ? QColor("#222220") : QColor("#ffffff");
    bgHover      = darkMode ? QColor("#2c2c28") : QColor("#eeeee8");

    // ── Text ──
    textPrimary   = darkMode ? QColor("#e4e0da") : QColor("#1a1a16");
    textSecondary = darkMode ? QColor("#8a8680") : QColor("#6b6860");
    textTime      = darkMode ? QColor("#6a665e") : QColor("#9a968e");
    textMuted     = darkMode ? QColor("#5a5850") : QColor("#b0aca5");

    // ── Accents ──
    accent    = darkMode ? QColor("#2ec4b6") : QColor("#1aab9d");
    danger    = QColor("#e06060");
    success   = QColor("#5ec76a");
    systemMsg = darkMode ? QColor("#6a665e") : QColor("#9a968e");

    // ── Borders ──
    divider = darkMode ? QColor("#222220") : QColor("#eeeee8");

    // ── Font sizes ──
    fontSizeTiny   = qRound(11 * fontScale);
    fontSizeSmall  = qRound(12 * fontScale);
    fontSizeNormal = qRound(14 * fontScale);
    fontSizeLarge  = qRound(16 * fontScale);
}

QColor PainterTheme::authorColor(const QString &actorId)
{
    // djb2 hash — matches Theme.qml stringHash / MessageBubble.qml authorColor
    int hash = 0;
    for (int i = 0; i < actorId.length(); ++i) {
        hash = ((hash << 5) - hash) + actorId.at(i).unicode();
    }
    int idx = qAbs(hash) % 8;
    return s_authorPalette[idx];
}

QFont PainterTheme::bodyFont() const
{
    QFont f;
    f.setPixelSize(fontSizeNormal);
    return f;
}

QFont PainterTheme::nameFont() const
{
    QFont f;
    f.setPixelSize(fontSizeSmall);
    f.setWeight(QFont::DemiBold);
    return f;
}

QFont PainterTheme::timeFont() const
{
    QFont f;
    f.setPixelSize(fontSizeTiny);
    return f;
}

QFont PainterTheme::systemFont() const
{
    QFont f;
    f.setPixelSize(fontSizeTiny);
    f.setItalic(true);
    return f;
}

QFont PainterTheme::dateSepFont() const
{
    QFont f;
    f.setPixelSize(fontSizeTiny);
    f.setWeight(QFont::DemiBold);
    return f;
}
