#pragma once

#include <QRectF>
#include <QString>

class QPainter;

namespace EmojiTextRenderer {

// Draws `text` inside `origin`, substituting emoji clusters with Twemoji pixmaps.
// Non-emoji runs use the painter's current font/pen. Elides with `mode` if the
// total width would exceed origin.width(). Returns the x of the right edge drawn.
qreal drawElided(QPainter *p,
                 const QRectF &origin,
                 const QString &text,
                 Qt::TextElideMode mode = Qt::ElideRight);

} // namespace EmojiTextRenderer
