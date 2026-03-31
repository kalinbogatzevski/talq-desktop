#include "PainterTheme.h"
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

// 8-color palette matching MessageBubble.qml authorColor()
static const QColor s_authorPalette[] = {
    QColor("#2ec4b6"), QColor("#e07060"), QColor("#f0a050"), QColor("#5ec76a"),
    QColor("#9b7cd4"), QColor("#e87aae"), QColor("#50b8c8"), QColor("#5a9ecf")
};

// 6-color topic palette matching Theme.qml topicPalette
static const QColor s_topicPalette[] = {
    QColor("#2ec4b6"), QColor("#e07060"), QColor("#f0a050"),
    QColor("#5ec76a"), QColor("#9b7cd4"), QColor("#e87aae")
};

PainterTheme::PainterTheme(bool darkMode, qreal fontScale)
    : m_fontScale(fontScale)
{
    // ── Backgrounds ──
    bgPrimary    = darkMode ? QColor("#121210") : QColor("#ffffff");
    bgSecondary  = darkMode ? QColor("#1a1a16") : QColor("#f5f5f2");
    bgSidebar    = darkMode ? QColor("#181814") : QColor("#f0f0ed");
    bgSelected   = darkMode ? QColor("#33332e") : QColor("#e4e4de");
    bgMessageOwn = darkMode ? QColor("#1a302e") : QColor("#d4f0ed");
    bgSurface    = darkMode ? QColor("#222220") : QColor("#ffffff");
    bgHover      = darkMode ? QColor("#2c2c28") : QColor("#eeeee8");

    // ── Text ──
    textPrimary   = darkMode ? QColor("#e4e0da") : QColor("#1a1a16");
    textSecondary = darkMode ? QColor("#8a8680") : QColor("#6b6860");
    textTime      = darkMode ? QColor("#6a665e") : QColor("#9a968e");
    textMuted     = darkMode ? QColor("#5a5850") : QColor("#b0aca5");

    // ── Accents ──
    accent      = darkMode ? QColor("#2ec4b6") : QColor("#1aab9d");
    unreadBadge = darkMode ? QColor("#2ec4b6") : QColor("#1aab9d");
    online      = QColor("#5ec76a");
    danger      = QColor("#e06060");
    success     = QColor("#5ec76a");
    systemMsg   = darkMode ? QColor("#6a665e") : QColor("#9a968e");

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
    // djb2 hash -- matches Theme.qml stringHash / MessageBubble.qml authorColor
    uint32_t hash = 5381;
    for (int i = 0; i < actorId.length(); ++i) {
        hash = ((hash << 5) - hash) + actorId.at(i).unicode();
    }
    int idx = static_cast<int>(hash % 8);
    return s_authorPalette[idx];
}

QColor PainterTheme::topicColor(int index)
{
    constexpr int N = sizeof(s_topicPalette) / sizeof(s_topicPalette[0]);
    return s_topicPalette[qAbs(index) % N];
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
    if (author.isEmpty())
        return message;
    return author + QStringLiteral(": ") + message;
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
