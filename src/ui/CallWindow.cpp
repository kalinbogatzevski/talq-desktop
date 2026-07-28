#include "CallWindow.h"
#include "CallStage.h"
#include "CallPipWidget.h"
#include "SharePickerDialog.h"

#include <QVBoxLayout>
#include <QCloseEvent>
#include <QEvent>
#include <QKeyEvent>
#include <QMoveEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QTimer>
#include <QDebug>

// Clamp an entire rect inside one screen's available work area so no pixel can
// land in a multi-monitor gap or off an edge. The screen is resolved from the
// rect's centre (robust to gaps between displays), then the rect is shrunk to
// fit if larger than the work area and shifted fully inside it.
static QRect clampToWorkArea(QRect target)
{
    QScreen *s = QGuiApplication::screenAt(target.center());
    if (!s) s = QGuiApplication::primaryScreen();
    if (!s) return target;
    const QRect a = s->availableGeometry();
    target.setSize(target.size().boundedTo(a.size()));
    const int x = qBound(a.left(), target.left(), a.right()  - target.width()  + 1);
    const int y = qBound(a.top(),  target.top(),  a.bottom() - target.height() + 1);
    target.moveTopLeft(QPoint(x, y));
    return target;
}

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
    // Minimal PiP-dock content (mute chip + hang-up + a small thumbnail) —
    // deliberately NOT the full CallStage squeezed into a corner box. Hidden
    // until enterPipDock() swaps it in; the hidden widget takes no layout
    // space so this doesn't affect the normal/fullscreen sizing above.
    m_pipWidget = new CallPipWidget(m_call, this);
    lay->addWidget(m_pipWidget);
    m_pipWidget->hide();

    connect(m_stage, &CallStage::requestToggleFullscreen, this, &CallWindow::toggleFullscreen);
    connect(m_stage, &CallStage::requestToggleShare, this, &CallWindow::pickShareTarget);
    connect(m_stage, &CallStage::requestOpenBackgroundSettings,
            this, &CallWindow::backgroundSettingsRequested);
    connect(m_pipWidget, &CallPipWidget::restoreRequested, this, &CallWindow::exitPipDock);
    connect(m_pipWidget, &CallPipWidget::minimizeRequested, this, &CallWindow::minimizeDock);
    connect(m_call, &CallManager::stateChanged, this, &CallWindow::onCallState);

    onCallState();
}

void CallWindow::setTheme(PainterTheme::Theme t)
{
    if (m_stage) m_stage->setTheme(t);
    if (m_pipWidget) m_pipWidget->setTheme(t);
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
        // NB: Qt::UniqueConnection requires a pointer-to-member-function slot —
        // with a lambda it hits a Q_ASSERT and aborts debug builds (and is wrong
        // in release too). So we route through the onScreenChanged() member.
        if (QWindow *wh = windowHandle())
            connect(wh, &QWindow::screenChanged, this,
                    &CallWindow::onScreenChanged, Qt::UniqueConnection);
    }
}

void CallWindow::onScreenChanged()
{
    // #5 — the window crossed to a monitor with (possibly) different DPI; a
    // resizeEvent isn't guaranteed, so force the stage to re-lay-out + flush its
    // frame caches.
    if (m_stage) m_stage->forceRelayout();
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
    // A minimize-triggered dock (see changeEvent) arrives still minimized;
    // restore it first so the flag/geometry change below actually shows.
    if (isMinimized()) setWindowState(windowState() & ~Qt::WindowMinimized);
    if (m_fullscreen) toggleFullscreen();
    m_normalGeom = geometry();
    m_pipDocked = true;
    if (m_stage) m_stage->hide();
    if (m_pipWidget) m_pipWidget->show();
    // Frameless + always-on-top, but a NORMAL taskbar window (NOT Qt::Tool): the
    // dock keeps its own taskbar button so it can be minimized to / restored from
    // the taskbar on its own, one click, independent of the main window. (Qt::Tool
    // is precisely what suppresses the taskbar button, which made a minimized dock
    // unrestorable except via the main window -- Kalin 2026-07-10.)
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    const int w = 320, h = 190, m = 24;
    // The normal call window floors at 640x480 (setMinimumSize in the ctor, so
    // the video stage can't be smashed). That floor also silently CLAMPS this
    // dock's setGeometry(...,320,190) back UP to 640x480 while the top-left
    // stays computed for a 320x190 box -> the dock overflowed its monitor by
    // (640-320)=320px right and (480-190)=290px bottom, landing half off-screen
    // (field 2026-07-09: docked into a multi-monitor gap). Drop the floor to the
    // dock size here; exitPipDock() restores 640x480.
    setMinimumSize(w, h);
    // Dock relative to whichever screen the window is CURRENTLY on, not
    // unconditionally the primary one -- on a multi-monitor setup, docking
    // against primaryScreen() snaps the window across to a different
    // monitor than the one the user was actually looking at, which reads
    // as "the call window randomly jumped/moved" (field: a 4-monitor
    // layout with gaps between displays made this especially jarring).
    QScreen *scr = this->screen();
    if (!scr) scr = QGuiApplication::primaryScreen();
    const QRect avail = scr->availableGeometry();
    // Clamp the whole rect into the work area so no pixel lands in a gap/edge.
    setGeometry(clampToWorkArea(QRect(avail.right()-w-m, avail.bottom()-h-m, w, h)));
    show();
    raise();
}

void CallWindow::exitPipDock()
{
    if (!m_pipDocked) return;
    m_pipDocked = false;
    if (m_pipWidget) m_pipWidget->hide();
    if (m_stage) m_stage->show();
    setMinimumSize(640, 480);   // restore the normal-window floor dropped in enterPipDock()
    setWindowFlags(Qt::Window);
    if (m_normalGeom.isValid()) setGeometry(clampToWorkArea(m_normalGeom));
    show();
    raise();
    activateWindow();
}

void CallWindow::minimizeDock()
{
    // "Minimize the dock entirely." The docked window is a NORMAL taskbar window
    // (enterPipDock deliberately does NOT set Qt::Tool), so a native minimize sends
    // it to its OWN taskbar button -- one click there restores just the dock,
    // independent of the main TalQ window (Kalin 2026-07-10: the tray-hide approach
    // forced an awkward main-window-toggle dance). changeEvent's minimize->dock
    // interceptor is guarded on !m_pipDocked, so it stays inert here and this is a
    // real minimize rather than a re-dock. Restore is native (click the taskbar
    // button); nothing else to wire.
    if (!m_pipDocked || m_call->state() == CallManager::Idle) return;
    showMinimized();
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

void CallWindow::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    // PiP-dock-on-drop: a minimize (title-bar button, taskbar, or dragging the
    // title bar down and dropping it onto the taskbar — all end in the same
    // WindowMinimized state) would otherwise silently vanish a live call to
    // the taskbar. Dock it to the compact corner PiP instead, deferred a tick
    // so the flag/geometry change in enterPipDock() doesn't race the OS's own
    // in-flight minimize transition.
    if (e->type() == QEvent::WindowStateChange && isMinimized()
        && !m_pipDocked && m_call->state() != CallManager::Idle) {
        QTimer::singleShot(0, this, [this] { enterPipDock(); });
    }
}

void CallWindow::closeEvent(QCloseEvent *e)
{
    // X must NEVER end the call. It used to call hangUp(), so one stray click
    // dropped every participant (field 2026-07-28).
    //
    // The original reasoning was sound — a hidden window must not leave a call
    // running invisibly with a live microphone — but the answer is the PiP
    // dock, not hanging up: it keeps a thumbnail, a mute chip and a hang-up
    // button on screen, so the call cannot run unnoticed and ending it stays
    // one click away. Hang-up is now explicit-only.
    if (m_call->state() != CallManager::Idle) {
        e->ignore();          // we are docking, not closing — keep the window alive
        if (!m_pipDocked) enterPipDock();
        return;
    }
    e->accept();
}

void CallWindow::pickShareTarget()
{
    if (m_call->isScreenSharing()) {
        m_call->stopScreenShare();
        return;
    }
    SharePickerDialog picker(this);
    // If the call ends (either side hangs up) while the source picker is still
    // open, dismiss it -- otherwise the chooser lingers on screen after the call
    // is gone (field 2026-07-10). callEnded fires synchronously from
    // CallManager::teardown() for BOTH local hangUp() and peer onPeerHungUp(), and
    // is still delivered inside picker.exec()'s nested loop; reject() unwinds it.
    // The connection auto-disconnects when the stack-local picker is destroyed, so
    // repeated shares never stack handlers.
    connect(m_call, &CallManager::callEnded, &picker, &QDialog::reject);
    if (picker.exec() != QDialog::Accepted) return;
    const auto target = picker.selectedTarget();
    // Both call sites must carry the presentation flag — missing either leaves
    // one share type silently ignoring the toggle.
    const bool presentation = picker.presentationMode();
    if (target.type == ShareTarget::Window)
        m_call->startScreenShare(0, target.windowHandle, presentation);
    else
        m_call->startScreenShare(target.monitorIndex, 0, presentation);
}
