#include "CallWindow.h"
#include "CallStage.h"
#include "SharePickerDialog.h"

#include <QVBoxLayout>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QGuiApplication>
#include <QScreen>

CallWindow::CallWindow(CallManager *call, ApiClient *api, QWidget *parent)
    : QWidget(parent, Qt::Window)
    , m_call(call)
    , m_api(api)
{
    setWindowTitle(tr("Call"));
    setMinimumSize(360, 280);
    resize(960, 600);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    m_stage = new CallStage(m_call, this);
    lay->addWidget(m_stage);

    connect(m_stage, &CallStage::requestToggleFullscreen, this, &CallWindow::toggleFullscreen);
    connect(m_stage, &CallStage::requestToggleShare, this, &CallWindow::pickShareTarget);
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
    }
}

void CallWindow::toggleFullscreen()
{
    if (m_pipDocked) exitPipDock();
    if (m_fullscreen) {
        m_fullscreen = false;
        showNormal();
        if (m_normalGeom.isValid()) setGeometry(m_normalGeom);
    } else {
        m_normalGeom = geometry();
        m_fullscreen = true;
        showFullScreen();
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
