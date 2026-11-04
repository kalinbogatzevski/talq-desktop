#pragma once

#include <QMargins>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QtGlobal>

// Fitting a dialog to the screen it is on. Pure arithmetic, so the frame a real title bar adds
// (about 31 px at the top and 8 px at the other edges on Windows at 200 %) can be tested even
// where the platform has none: the offscreen platform fakes a 2 px frame.
//
// A dialog's size is its CLIENT size; the work area (QScreen::availableGeometry) has to hold the
// FRAME, which is that plus the title bar and borders.
namespace talq::screenfit {

// The client size to give a dialog: what it wants, but no more than the work area leaves after
// the frame. The layout's minimum still wins over the screen, because Qt would enforce it anyway;
// a dialog that must always fit has to keep that minimum small (ConversationInfoDialog scrolls
// its body for that reason).
inline QSize fitClientSize(const QSize &wanted, const QSize &minimum, const QRect &available,
                           const QMargins &frame)
{
    const int maxW = available.width() - frame.left() - frame.right();
    const int maxH = available.height() - frame.top() - frame.bottom();
    return QSize(qMax(minimum.width(), qMin(wanted.width(), maxW)),
                 qMax(minimum.height(), qMin(wanted.height(), maxH)));
}

// Where a frame's top-left corner goes so the whole frame is inside the work area. A frame that
// already is stays put; one larger than the area keeps its top-left corner (and so the title bar)
// on screen.
inline QPoint keepFrameOnScreen(const QRect &frame, const QRect &available)
{
    QPoint p = frame.topLeft();
    if (p.x() + frame.width() > available.x() + available.width())
        p.setX(available.x() + available.width() - frame.width());
    if (p.y() + frame.height() > available.y() + available.height())
        p.setY(available.y() + available.height() - frame.height());
    if (p.x() < available.x()) p.setX(available.x());
    if (p.y() < available.y()) p.setY(available.y());
    return p;
}

} // namespace talq::screenfit
