#pragma once

#include <QWidget>
#include <QRect>
#include "core/CallManager.h"
#include "painter/PainterTheme.h"

class ApiClient;
class CallStage;
class CallPipWidget;
class QScreen;
class QMoveEvent;

/**
 * Top-level call window. Thin shell around CallStage: owns window state
 * (normal / fullscreen / compact PiP dock), self-shows and self-hides off
 * CallManager state (as the old CallDialog did), forwards the theme, and
 * owns the screen-source picker. No chrome of its own; the surface paints
 * everything on the warm theme.
 */
class CallWindow : public QWidget
{
    Q_OBJECT

public:
    CallWindow(CallManager *call, ApiClient *api, QWidget *parent = nullptr);

    void setTheme(PainterTheme::Theme t);

    // Compact always-on-top corner dock, so a call survives the user
    // navigating back to chat. Idempotent.
    void enterPipDock();
    void exitPipDock();
    bool isPipDocked() const { return m_pipDocked; }
    // Live window state, NOT the cached m_fullscreen latch: an OS-driven exit
    // (Win+D / minimize / a system fullscreen-off) leaves m_fullscreen stale,
    // which would wrongly suppress the PiP dock. QWidget::isFullScreen() reflects
    // the real state. (m_fullscreen stays as the internal toggle latch only.)
    bool isFullscreen() const { return isFullScreen(); }

signals:
    // 0.40.15 — "Open background settings…" entry in the BACKGROUND
    // dropdown (CallStage) bubbles up to MainWindow, which owns
    // SettingsDialog. The dialog opens to its Audio & Video tab (the
    // home of the blur slider + image picker).
    void backgroundSettingsRequested();

protected:
    void closeEvent(QCloseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    // Instrumentation for the "video smashed after dragging the call between
    // displays, then drops on fullscreen" report: logs cross-monitor moves so
    // the next debug log can be correlated against frame flow + the fullscreen
    // toggle (2026-06-04).
    void moveEvent(QMoveEvent *) override;
    // PiP-dock-on-drop: minimizing a live call (title-bar minimize, or the
    // classic Windows drag-the-titlebar-onto-the-taskbar "drop" gesture)
    // would otherwise vanish the call to the taskbar with no visual trace.
    // Catch the minimize transition and dock to the compact corner PiP
    // instead, same contract as the conversation-switch dock.
    void changeEvent(QEvent *) override;

private slots:
    void onCallState();

private:
    void toggleFullscreen();
    void onScreenChanged();   // #5 — re-lay-out the stage on a cross-DPI monitor move
    void pickShareTarget();

    CallManager *m_call;
    ApiClient *m_api;
    CallStage *m_stage = nullptr;
    CallPipWidget *m_pipWidget = nullptr;
    bool m_fullscreen = false;
    bool m_pipDocked = false;
    QRect m_normalGeom;
    QScreen *m_lastScreen = nullptr;   // for cross-display-move logging
};
