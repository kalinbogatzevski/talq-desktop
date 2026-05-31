#pragma once
#include <QWidget>
#include <QColor>
#include <QtGlobal>

// A frameless, click-through, always-on-top overlay that paints a coloured
// border around a monitor to signal an active screen-share (the Zoom green /
// Teams red "you are sharing this screen" indicator). It is input-transparent
// so it never intercepts clicks, never takes focus, and on Windows 10 2004+ is
// excluded from screen capture so the shared peer never sees the border itself.
class ShareOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit ShareOverlay(QWidget *parent = nullptr);

    // Frame the monitor at `monitorIndex` (index into QApplication::screens()).
    // Out-of-range falls back to the primary screen.
    void showForMonitor(int monitorIndex);

    // #72 (subtler / optional) — style + gating exposed as pure statics so
    // the headless overlay test can assert them and CallManager can decide
    // whether to show the border without constructing the widget.
    static int borderWidthPx();   // pen width in logical px (thin: 2)
    static QColor borderColor();  // semi-transparent "sharing" green
    // Show the monitor border only when the user setting is ON *and* this is
    // a full-monitor share. Window shares (windowHandle != 0) never get a
    // frame — that matches Zoom's per-app share, the original request.
    static bool shouldShowForShare(bool borderEnabled, quintptr windowHandle);

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
};
