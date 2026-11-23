#pragma once

#include "ui/ScreenFit.h"

#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QWidget>

// The widget half of ScreenFit.h, for a framed top-level window that has just been placed.
//
// setGeometry() and resize() place the CLIENT area and move() the FRAME's corner; the native
// title bar (with the close button) and the borders sit in the frame, which is only known once
// the native window is up. So this is called after show(): it takes the screen that holds the
// frame's centre (else the window's own screen, else the primary), shrinks the window if its
// frame is larger than that screen's work area, and moves the whole frame inside it. A window
// that is hidden, maximized, full screen or minimized is left alone: the window manager places
// those. ImageViewerDialog::keepFrameOnScreen() is the same rule for the photo viewer.
namespace talq::screenfit {

inline void keepWindowOnScreen(QWidget *w)
{
    if (!w || !w->isVisible() || w->isMaximized() || w->isFullScreen() || w->isMinimized())
        return;
    QRect fr = w->frameGeometry();
    const QScreen *s = QGuiApplication::screenAt(fr.center());
    if (!s) s = w->screen();
    if (!s) s = QGuiApplication::primaryScreen();
    if (!s)
        return;
    const QRect avail = s->availableGeometry();
    if (fr.width() > avail.width() || fr.height() > avail.height()) {
        const QSize chrome = fr.size() - w->size();
        w->resize(qMax(1, qMin(fr.width(),  avail.width())  - chrome.width()),
                  qMax(1, qMin(fr.height(), avail.height()) - chrome.height()));
        fr = w->frameGeometry();
    }
    const QPoint to = keepFrameOnScreen(fr, avail);
    if (to != fr.topLeft())
        w->move(to);             // move() takes the frame's corner
}

} // namespace talq::screenfit
