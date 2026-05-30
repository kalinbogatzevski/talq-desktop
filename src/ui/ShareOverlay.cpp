#include "ui/ShareOverlay.h"

#include <QApplication>
#include <QScreen>
#include <QPainter>
#include <QPaintEvent>
#include <QShowEvent>

#ifdef Q_OS_WIN
#include <windows.h>
// WDA_EXCLUDEFROMCAPTURE was added in the Windows 10 2004 SDK; define it if the
// toolchain's headers predate that so the build still works (the call then
// no-ops on older Windows, which renders the overlay black in captures instead
// of fully hiding it — acceptable).
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#endif

namespace {
// Sharing-active indicator colour (a confident green, the near-universal
// "live / sharing" convention) and the border geometry.
constexpr int kBorderPx = 4;
constexpr int kRadiusPx = 10;
inline QColor shareColor() { return QColor(0x2E, 0xA0, 0x43); }
} // namespace

ShareOverlay::ShareOverlay(QWidget *parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                          | Qt::Tool | Qt::WindowTransparentForInput)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);   // never steal focus on show
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setObjectName(QStringLiteral("ShareOverlay"));
}

void ShareOverlay::showForMonitor(int monitorIndex)
{
    const QList<QScreen *> screens = QApplication::screens();
    if (screens.isEmpty())
        return;
    if (monitorIndex < 0 || monitorIndex >= screens.size())
        monitorIndex = 0;
    QScreen *scr = screens.at(monitorIndex);

    // Qt 6 setGeometry works in logical (device-independent) pixels, matching
    // QScreen::geometry(), so the overlay lines up across mixed-DPI monitors
    // without manual devicePixelRatio juggling.
    setScreen(scr);
    setGeometry(scr->geometry());
    show();
    raise();
}

void ShareOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal half = kBorderPx / 2.0;
    const QRectF r = QRectF(rect()).adjusted(half, half, -half, -half);
    QPen pen(shareColor(), kBorderPx);
    pen.setJoinStyle(Qt::MiterJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, kRadiusPx, kRadiusPx);
}

void ShareOverlay::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
#ifdef Q_OS_WIN
    if (HWND hwnd = reinterpret_cast<HWND>(winId())) {
        // Keep the border out of the captured stream so the peer never sees it.
        SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
        // Belt-and-braces no-activate / tool-window so it never grabs focus or
        // shows in the alt-tab list, complementing Qt::WindowTransparentForInput.
        const LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        SetWindowLongPtr(hwnd, GWL_EXSTYLE,
                         ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT);
    }
#endif
}
