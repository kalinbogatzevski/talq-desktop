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

PainterTheme::PainterTheme(bool darkMode, qreal fontScale)
    : m_fontScale(fontScale)
{
    // ── Backgrounds — warm dispatch, paper-like depth ──
    bgPrimary    = darkMode ? QColor("#141210") : QColor("#fbf9f5");
    bgSecondary  = darkMode ? QColor("#1a1613") : QColor("#f3f0e9");
    bgSidebar    = darkMode ? QColor("#18140f") : QColor("#ede9e1");
    bgSelected   = darkMode ? QColor("#2a211a") : QColor("#ded8cb");
    bgMessageOwn = darkMode ? QColor("#1c3330") : QColor("#d4ebe7");
    bgSurface    = darkMode ? QColor("#221d19") : QColor("#ffffff");
    bgHover      = darkMode ? QColor("#241f1a") : QColor("#ebe6dd");

    // ── Text — warmer cream on dark ──
    textPrimary   = darkMode ? QColor("#f4efe6") : QColor("#1a1613");
    textSecondary = darkMode ? QColor("#a8a096") : QColor("#65605a");
    textTime      = darkMode ? QColor("#7a726a") : QColor("#8e887f");
    textMuted     = darkMode ? QColor("#5a5348") : QColor("#b0aca5");

    // ── Accents — teal primary, amber secondary for emphasis ──
    accent      = darkMode ? QColor("#14b8a6") : QColor("#0d9488");
    unreadBadge = darkMode ? QColor("#14b8a6") : QColor("#0d9488");
    online      = QColor("#5ec76a");
    danger      = QColor("#e8866b");
    success     = QColor("#5ec76a");
    systemMsg   = darkMode ? QColor("#7a726a") : QColor("#8e887f");

    // ── Borders — warm-tinted so panels don't read as gray ──
    divider = darkMode ? QColor("#2a241f") : QColor("#e8e2d6");

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
    QFont f = interFont();
    f.setPixelSize(fontSizeTiny);
    f.setItalic(true);
    return f;
}

QFont PainterTheme::dateSepFont() const
{
    QFont f = interFont();
    f.setPixelSize(fontSizeTiny);
    f.setWeight(QFont::DemiBold);
    return f;
}
