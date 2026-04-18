#include "EmojiTextRenderer.h"

#include "core/EmojiData.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPixmap>
#include <QTextBoundaryFinder>

namespace EmojiTextRenderer {

namespace {

struct Run {
    bool isEmoji;
    QString text;
    qreal width;
};

QVector<Run> buildRuns(const QString &text, qreal emojiSize, const QFontMetricsF &fm)
{
    QVector<Run> runs;
    QTextBoundaryFinder bf(QTextBoundaryFinder::Grapheme, text);
    int start = 0;
    int next = bf.toNextBoundary();
    QString currentText;
    while (next != -1) {
        QString cluster = text.mid(start, next - start);
        bool emoji = EmojiData::isEmojiCluster(cluster);
        if (emoji) {
            if (!currentText.isEmpty()) {
                runs.append({false, currentText, fm.horizontalAdvance(currentText)});
                currentText.clear();
            }
            runs.append({true, cluster, emojiSize});
        } else {
            currentText += cluster;
        }
        start = next;
        next = bf.toNextBoundary();
    }
    if (!currentText.isEmpty())
        runs.append({false, currentText, fm.horizontalAdvance(currentText)});
    return runs;
}

qreal totalWidth(const QVector<Run> &runs)
{
    qreal w = 0;
    for (const Run &r : runs) w += r.width;
    return w;
}

// Trim runs from the tail until total fits, replacing the last remaining run's
// text with an elided version that ends in "…".
void elide(QVector<Run> &runs, qreal maxWidth, const QFontMetricsF &fm)
{
    const QString ell = QStringLiteral("…");
    qreal ellW = fm.horizontalAdvance(ell);

    while (!runs.isEmpty() && totalWidth(runs) + ellW > maxWidth)
        runs.removeLast();

    // If we still don't fit, peel chars off the last text run.
    while (!runs.isEmpty()) {
        qreal sum = totalWidth(runs) + ellW;
        if (sum <= maxWidth) break;

        Run &last = runs.last();
        if (last.isEmoji || last.text.isEmpty()) {
            runs.removeLast();
            continue;
        }
        last.text.chop(1);
        last.width = fm.horizontalAdvance(last.text);
    }

    // Append ellipsis as its own text run (non-emoji).
    runs.append({false, ell, ellW});
}

} // namespace

qreal drawElided(QPainter *p, const QRectF &origin, const QString &text,
                 Qt::TextElideMode mode)
{
    QFontMetricsF fm(p->font());
    qreal emojiSize = fm.height();  // square emoji matching line height

    QVector<Run> runs = buildRuns(text, emojiSize, fm);
    if (totalWidth(runs) > origin.width()) {
        if (mode == Qt::ElideRight) {
            elide(runs, origin.width(), fm);
        }
        // ElideLeft / ElideMiddle / ElideNone — not used by sidebar/thread preview;
        // fall back to drawing as-is (may clip via painter clip rect).
    }

    qreal x = origin.left();
    qreal yBaseline = origin.center().y();
    for (const Run &r : runs) {
        if (r.isEmoji) {
            QPixmap pm = EmojiData::pixmapFor(r.text, int(emojiSize));
            if (!pm.isNull()) {
                qreal py = origin.center().y() - pm.height() / 2.0;
                p->drawPixmap(QPointF(x, py), pm);
            } else {
                // Fallback: draw the emoji glyph via system font.
                p->drawText(QPointF(x, yBaseline + fm.ascent() / 2.0 - 1.0), r.text);
            }
            x += r.width;
        } else {
            p->drawText(QRectF(x, origin.top(), r.width, origin.height()),
                        int(Qt::AlignLeft | Qt::AlignVCenter), r.text);
            x += r.width;
        }
    }
    return x;
}

} // namespace EmojiTextRenderer
