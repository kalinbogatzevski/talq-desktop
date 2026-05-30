#pragma once
#include <QWidget>

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

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
};
