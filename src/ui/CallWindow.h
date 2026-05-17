#pragma once

#include <QWidget>
#include <QRect>
#include "core/CallManager.h"
#include "painter/PainterTheme.h"

class ApiClient;
class CallStage;

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

protected:
    void closeEvent(QCloseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;

private slots:
    void onCallState();

private:
    void toggleFullscreen();
    void pickShareTarget();

    CallManager *m_call;
    ApiClient *m_api;
    CallStage *m_stage = nullptr;
    bool m_fullscreen = false;
    bool m_pipDocked = false;
    QRect m_normalGeom;
};
