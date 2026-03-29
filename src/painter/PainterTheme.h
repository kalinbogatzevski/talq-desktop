#pragma once

#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QImage>
#include <QString>

/**
 * C++ mirror of Theme.qml colors/fonts/spacing for QPainter rendering.
 * Constructed with darkMode + fontScale, provides all visual constants.
 */
class PainterTheme
{
public:
    PainterTheme(bool darkMode = true, qreal fontScale = 1.0);

    // ── Backgrounds ──
    QColor bgPrimary;
    QColor bgSecondary;
    QColor bgSidebar;
    QColor bgSelected;
    QColor bgMessageOwn;
    QColor bgSurface;
    QColor bgHover;

    // ── Text ──
    QColor textPrimary;
    QColor textSecondary;
    QColor textTime;
    QColor textMuted;

    // ── Accents ──
    QColor accent;
    QColor unreadBadge;
    QColor online;
    QColor danger;
    QColor success;
    QColor systemMsg;

    // ── Borders ──
    QColor divider;

    // ── Font sizes (scaled) ──
    int fontSizeTiny;
    int fontSizeSmall;
    int fontSizeNormal;
    int fontSizeLarge;

    // ── Spacing ──
    static constexpr int spacingTiny = 4;
    static constexpr int spacingSmall = 8;
    static constexpr int spacingNormal = 12;
    static constexpr int spacingLarge = 16;
    static constexpr int spacingXLarge = 24;

    // ── Radii ──
    static constexpr int radiusSmall = 6;
    static constexpr int radiusNormal = 10;

    // ── Avatar ──
    static constexpr int avatarSize = 36;
    static constexpr int avatarGap = 8;

    // ── Layout ──
    static constexpr int messageSpacingGrouped = 6;
    static constexpr int messageSpacingNormal = 10;
    static constexpr int dateSepHeight = 40;
    static constexpr int datePillHeight = 26;

    // ── Author color from actorId (djb2 hash -> 8-color palette) ──
    static QColor authorColor(const QString &actorId);

    // ── Topic color from palette index (6-color palette) ──
    static QColor topicColor(int index);

    // ── Crop an image to a circle at the given size ──
    static QImage cropToCircle(const QImage &source, int size);

    // ── Format a Unix timestamp as "HH:mm", "Yesterday", or "dd MMM" ──
    static QString formatRelativeTime(qint64 epochSecs);

    // ── Build preview text from author + message ──
    static QString formatPreviewText(const QString &author, const QString &message);

    // ── Fonts ──
    QFont bodyFont() const;
    QFont nameFont() const;
    QFont timeFont() const;
    QFont systemFont() const;
    QFont dateSepFont() const;

private:
    qreal m_fontScale = 1.0;
};
