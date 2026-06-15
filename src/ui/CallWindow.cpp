#include "CallWindow.h"
#include "CallStage.h"
#include "SharePickerDialog.h"

#include <QVBoxLayout>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QMoveEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QTimer>
#include <QDebug>

CallWindow::CallWindow(CallManager *call, ApiClient *api, QWidget *parent)
    : QWidget(parent, Qt::Window)
    , m_call(call)
    , m_api(api)
{
    setWindowTitle(tr("Call"));
    // Floor the call window so the video stage can't be squeezed down to a
    // smashed/illegible tile (field report 2026-06-04). 640x480 keeps the
    // video + control bar usable on the smallest laptop screens.
    setMinimumSize(640, 480);
    resize(960, 600);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    m_stage = new CallStage(m_call, this);
    lay->addWidget(m_stage);

    connect(m_stage, &CallStage::requestToggleFullscreen, this, &CallWindow::toggleFullscreen);
    connect(m_stage, &CallStage::requestToggleShare, this, &CallWindow::pickShareTarget);
    connect(m_stage, &CallStage::requestOpenBackgroundSettings,
            this, &CallWindow::backgroundSettingsRequested);
    connect(m_call, &CallManager::stateChanged, this, &CallWindow::onCallState);

    onCallState();
}

void CallWindow::setTheme(PainterTheme::Theme t)
{
    if (m_stage) m_stage->setTheme(t);
}

void CallWindow::onCallState()
{
    const auto s = m_call->state();

    if (s == CallManager::Idle) {
        if (m_fullscreen) { m_fullscreen = false; }
        if (m_pipDocked) exitPipDock();
        hide();
        return;
    }

    if (s == CallManager::Incoming)
        m_call->setUserActionReady();

    if (!isVisible()) {
        if (QScreen *sc = QGuiApplication::primaryScreen()) {
            const QRect g = sc->availableGeometry();
            move(g.center() - rect().center());
        }
        show();
        raise();
        activateWindow();
        // #5 — cross-monitor (DPI) moves don't reliably fire a resizeEvent, so
        // the call tiles never re-lay-out and the stage tile is smashed/stretched
        // until something else triggers a resize. windowHandle() exists now that
        // we're shown; QWindow::screenChanged fires on every monitor cross.
        // UniqueConnection keeps it idempotent across show/hide cycles.
        if (QWindow *wh = windowHandle()) {
            connect(wh, &QWindow::screenChanged, this, [this](QScreen *) {
                if (m_stage) m_stage->forceRelayout();
            }, Qt::UniqueConnection);
        }
    }
}

void CallWindow::toggleFullscreen()
{
    if (m_pipDocked) exitPipDock();
    if (m_fullscreen) {
        qInfo() << "CallWindow: EXIT fullscreen";
        m_fullscreen = false;
        showNormal();
        if (m_normalGeom.isValid()) setGeometry(m_normalGeom);
    } else {
        QScreen *sc = screen();
        qInfo().nospace() << "CallWindow: ENTER fullscreen on screen "
                          << (sc ? sc->name() : QStringLiteral("?"))
                          << " " << (sc ? sc->geometry() : QRect())
                          << " dpr=" << (sc ? sc->devicePixelRatio() : 0.0);
        m_normalGeom = geometry();
        m_fullscreen = true;
        showFullScreen();
    }
    // #5 — the fullscreen/normal geometry settles asynchronously and a
    // resizeEvent isn't guaranteed to land with the final size, which can leave
    // the stage tile placed for the old geometry (it "drops" off-screen).
    // Re-lay-out on the next event-loop turn, against the settled size.
    QTimer::singleShot(0, this, [this] { if (m_stage) m_stage->forceRelayout(); });
}

void CallWindow::moveEvent(QMoveEvent *e)
{
    QWidget::moveEvent(e);
    // Log cross-display moves so the "video smashed after moving the call to
    // another monitor, then drops on fullscreen" report can be correlated in
    // the debug log against frame flow (CallStage::onFrame) + the fullscreen
    // toggle above. A DPI change between monitors is the prime suspect.
    QScreen *now = screen();
    if (now != m_lastScreen) {
        const QRect g = now ? now->geometry() : QRect();
        qInfo().nospace() << "CallWindow: moved to screen "
                          << (now ? now->name() : QStringLiteral("?"))
                          << " (" << g.width() << "x" << g.height()
                          << " dpr=" << (now ? now->devicePixelRatio() : 0.0) << ")"
                          << (m_lastScreen ? " — CROSS-DISPLAY MOVE" : "");
        m_lastScreen = now;
    }
}

void CallWindow::enterPipDock()
{
    if (m_pipDocked || m_call->state() == CallManager::Idle) return;
    if (m_fullscreen) toggleFullscreen();
    m_normalGeom = geometry();
    m_pipDocked = true;
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    const int w = 320, h = 190, m = 24;
    QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    setGeometry(avail.right()-w-m, avail.bottom()-h-m, w, h);
    show();
    raise();
}

void CallWindow::exitPipDock()
{
    if (!m_pipDocked) return;
    m_pipDocked = false;
    setWindowFlags(Qt::Window);
    if (m_normalGeom.isValid()) setGeometry(m_normalGeom);
    show();
    raise();
    activateWindow();
}

void CallWindow::mouseDoubleClickEvent(QMouseEvent *)
{
    if (m_pipDocked) { exitPipDock(); return; }
    toggleFullscreen();
}

void CallWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        if (m_fullscreen) { toggleFullscreen(); return; }
        if (m_pipDocked)  { exitPipDock(); return; }
        // Esc never silently kills a live call.
        e->ignore();
        return;
    }
    QWidget::keyPressEvent(e);
}

void CallWindow::closeEvent(QCloseEvent *e)
{
    // Closing the window ends the call (don't leave an orphan call running).
    if (m_call->state() != CallManager::Idle)
        m_call->hangUp();
    e->accept();
}

void CallWindow::pickShareTarget()
{
    if (m_call->isScreenSharing()) {
        m_call->stopScreenShare();
        return;
    }
    SharePickerDialog picker(this);
    if (picker.exec() != QDialog::Accepted) return;
    const auto target = picker.selectedTarget();
    if (target.type == ShareTarget::Window)
        m_call->startScreenShare(0, target.windowHandle);
    else
        m_call->startScreenShare(target.monitorIndex);
}
