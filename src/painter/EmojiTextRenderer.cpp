#include "EmojiTextRenderer.h"

#include <QFontMetricsF>
#include <QPainter>

namespace EmojiTextRenderer {

qreal drawElided(QPainter *p, const QRectF &origin, const QString &text,
                 Qt::TextElideMode mode)
{
    // Stub — identical to QPainter::drawText with elision. Task 2 replaces this.
    QFontMetricsF fm(p->font());
    QString elided = fm.elidedText(text, mode, origin.width());
    p->drawText(origin, int(Qt::AlignLeft | Qt::AlignVCenter), elided);
    return origin.left() + fm.horizontalAdvance(elided);
}

} // namespace EmojiTextRenderer
