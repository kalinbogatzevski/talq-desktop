#pragma once

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRectF>
#include <QColor>
#include <QString>
#include <QtMath>

// ── TalQ vector iconography ─────────────────────────────────────────────
//
// One shared icon set, drawn in a 24-unit viewBox and mapped into `box`.
// Stroked and tinted by the caller's pen colour so on/off state reads via
// colour + an optional diagonal "slash" — unlike Windows colour-emoji
// glyphs (Segoe UI Emoji) which ignore the QPainter pen entirely. This is
// the call bar's idiom, lifted here so the whole app speaks one language.
//
// Used by CallStage (control bar) and TalqIconButton (every icon button).

namespace VectorIcons {

inline void draw(QPainter &p, const QString &id, const QRectF &box,
                  const QColor &stroke, bool slash = false,
                  const QColor &slashBack = QColor())
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal s = qMin(box.width(), box.height()) / 24.0;
    p.translate(box.center());
    p.scale(s, s);
    p.translate(-12, -12);

    QPen pen(stroke, 2.0);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    if (id == "mic") {
        QPainterPath body;
        body.addRoundedRect(QRectF(9, 3, 6, 11), 3, 3);
        p.drawPath(body);
        QPainterPath arc;
        arc.moveTo(5.5, 10.5);
        arc.cubicTo(5.5, 17, 18.5, 17, 18.5, 10.5);
        p.drawPath(arc);
        p.drawLine(QPointF(12, 17), QPointF(12, 21));
        p.drawLine(QPointF(8.5, 21), QPointF(15.5, 21));
    } else if (id == "cam") {
        QPainterPath b; b.addRoundedRect(QRectF(3, 7, 12.5, 10), 2.2, 2.2);
        p.drawPath(b);
        QPainterPath lens;
        lens.moveTo(15.5, 10.2); lens.lineTo(21, 7);
        lens.lineTo(21, 17); lens.lineTo(15.5, 13.8); lens.closeSubpath();
        p.drawPath(lens);
    } else if (id == "share") {
        QPainterPath m; m.addRoundedRect(QRectF(3, 5, 18, 12), 2.2, 2.2);
        p.drawPath(m);
        p.drawLine(QPointF(12, 17), QPointF(12, 21));
        p.drawLine(QPointF(8.5, 21), QPointF(15.5, 21));
    } else if (id == "roster") {
        p.drawEllipse(QRectF(6, 5.5, 6, 6));
        QPainterPath sh; sh.moveTo(3.5, 19);
        sh.cubicTo(3.5, 12.5, 14.5, 12.5, 14.5, 19);
        p.drawPath(sh);
        p.drawEllipse(QRectF(14.5, 7, 4.6, 4.6));
        QPainterPath sh2; sh2.moveTo(16, 18.5);
        sh2.cubicTo(16, 14, 21.5, 13.8, 21.5, 18.5);
        p.drawPath(sh2);
    } else if (id == "telemetry") {
        p.drawLine(QPointF(6, 18), QPointF(6, 13));
        p.drawLine(QPointF(12, 18), QPointF(12, 8));
        p.drawLine(QPointF(18, 18), QPointF(18, 11));
        p.drawLine(QPointF(3.5, 20.5), QPointF(20.5, 20.5));
    } else if (id == "full") {
        const qreal L = 5;
        p.drawPolyline(QPolygonF({{3.0+L,4.0},{4.0,4.0},{4.0,4.0+L}}));
        p.drawPolyline(QPolygonF({{20.0-L,4.0},{20.0,4.0},{20.0,4.0+L}}));
        p.drawPolyline(QPolygonF({{4.0,20.0-L},{4.0,20.0},{4.0+L,20.0}}));
        p.drawPolyline(QPolygonF({{20.0-L,20.0},{20.0,20.0},{20.0,20.0-L}}));
    } else if (id == "end") {
        // Hang-up: a telephone handset — two ear/mouth cups on a CURVED grip —
        // tilted down onto the hook (the universal "end call"). The concave grip
        // (not a straight bar) is what makes it read as a receiver, not a dumbbell.
        p.translate(12, 12); p.rotate(135); p.translate(-12, -12);
        QPen hp(stroke, 4.2); hp.setCapStyle(Qt::RoundCap);
        p.setPen(hp); p.setBrush(Qt::NoBrush);
        QPainterPath grip;
        grip.moveTo(6.5, 10.0);
        grip.cubicTo(9, 13.8, 15, 13.8, 17.5, 10.0);
        p.drawPath(grip);
        p.setPen(Qt::NoPen); p.setBrush(stroke);
        p.drawEllipse(QPointF(6.5, 9.0), 4.0, 4.0);
        p.drawEllipse(QPointF(17.5, 9.0), 4.0, 4.0);
    } else if (id == "send") {
        // Paper-plane.
        QPainterPath t;
        t.moveTo(3, 12); t.lineTo(21, 4); t.lineTo(14, 21);
        t.lineTo(11.5, 13.5); t.closeSubpath();
        p.setPen(Qt::NoPen); p.setBrush(stroke); p.drawPath(t);
        QPen ln(slashBack.isValid() ? slashBack : stroke, 1.4);
        p.setPen(ln); p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(21, 4), QPointF(11.5, 13.5));
    } else if (id == "attach") {
        // Paper-clip.
        QPainterPath c;
        c.moveTo(17.5, 8);
        c.lineTo(9.5, 16);
        c.cubicTo(7.7, 17.8, 5, 15.1, 6.8, 13.3);
        c.lineTo(15.5, 4.6);
        c.cubicTo(18.4, 1.7, 22.8, 6.1, 19.9, 9);
        c.lineTo(10.6, 18.3);
        c.cubicTo(6.4, 22.5, 0.2, 16.3, 4.4, 12.1);
        c.lineTo(12, 4.6);
        p.drawPath(c);
    } else if (id == "emoji") {
        p.drawEllipse(QRectF(3.5, 3.5, 17, 17));
        p.setBrush(stroke); p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(8.3, 9, 2.0, 2.0));
        p.drawEllipse(QRectF(13.7, 9, 2.0, 2.0));
        p.setBrush(Qt::NoBrush); p.setPen(pen);
        QPainterPath sm; sm.moveTo(8, 14);
        sm.cubicTo(9.6, 17.4, 14.4, 17.4, 16, 14);
        p.drawPath(sm);
    } else if (id == "search") {
        p.drawEllipse(QRectF(4, 4, 12, 12));
        p.drawLine(QPointF(14.5, 14.5), QPointF(20.5, 20.5));
    } else if (id == "close") {
        p.drawLine(QPointF(5.5, 5.5), QPointF(18.5, 18.5));
        p.drawLine(QPointF(18.5, 5.5), QPointF(5.5, 18.5));
    } else if (id == "plus") {
        p.drawLine(QPointF(12, 4.5), QPointF(12, 19.5));
        p.drawLine(QPointF(4.5, 12), QPointF(19.5, 12));
    } else if (id == "back") {
        p.drawPolyline(QPolygonF({{14.5,5},{7.5,12},{14.5,19}}));
    } else if (id == "gear") {
        p.drawEllipse(QRectF(8.5, 8.5, 7, 7));
        for (int i = 0; i < 8; ++i) {
            const qreal a = i * M_PI / 4.0;
            const QPointF c(12, 12);
            p.drawLine(c + QPointF(qCos(a)*8.0,  qSin(a)*8.0),
                       c + QPointF(qCos(a)*10.5, qSin(a)*10.5));
        }
    } else if (id == "copy") {
        QPainterPath a; a.addRoundedRect(QRectF(8, 8, 12, 12), 2, 2);
        p.drawPath(a);
        QPainterPath b; b.moveTo(5, 16);
        b.lineTo(4, 16); b.lineTo(4, 4); b.lineTo(16, 4); b.lineTo(16, 5);
        p.drawPath(b);
    } else if (id == "forward") {
        p.drawPolyline(QPolygonF({{4,8},{12,8},{12,4},{20,11},{12,18},{12,14}}));
        QPainterPath tail; tail.moveTo(4, 8);
        tail.cubicTo(4, 18, 8, 20, 12, 20);
        p.setBrush(Qt::NoBrush); p.drawPath(tail);
    } else if (id == "belloff") {
        QPainterPath b;
        b.moveTo(7, 17);
        b.cubicTo(7, 11, 7.5, 6, 12, 6);
        b.cubicTo(16.5, 6, 17, 11, 17, 17);
        p.drawPath(b);
        p.drawLine(QPointF(4.5, 17), QPointF(19.5, 17));
        p.drawLine(QPointF(10, 20), QPointF(14, 20));
        // slash conveys "silent"
        QPen sl(stroke, 2.0); sl.setCapStyle(Qt::RoundCap);
        p.setPen(sl);
        p.drawLine(QPointF(4.5, 4.5), QPointF(19.5, 19.5));
    } else if (id == "trash") {
        p.drawLine(QPointF(4.5, 6.5), QPointF(19.5, 6.5));
        QPainterPath body; body.moveTo(6, 6.5);
        body.lineTo(7, 20.5); body.lineTo(17, 20.5); body.lineTo(18, 6.5);
        p.drawPath(body);
        p.drawLine(QPointF(9.5, 6.5), QPointF(10, 4));
        p.drawLine(QPointF(14.5, 6.5), QPointF(14, 4));
        p.drawLine(QPointF(10, 4), QPointF(14, 4));
    }

    if (slash) {
        // Standard disabled cut: a backing stroke in the cell colour, the
        // tinted stroke on top — the icon reads as visibly severed.
        p.setBrush(Qt::NoBrush);
        QPen bk(slashBack.isValid() ? slashBack : stroke, 5.0);
        bk.setCapStyle(Qt::RoundCap);
        p.setPen(bk);
        p.drawLine(QPointF(4.5, 4.5), QPointF(19.5, 19.5));
        QPen fr(stroke, 2.4); fr.setCapStyle(Qt::RoundCap);
        p.setPen(fr);
        p.drawLine(QPointF(4.5, 4.5), QPointF(19.5, 19.5));
    }
    p.restore();
}

} // namespace VectorIcons
