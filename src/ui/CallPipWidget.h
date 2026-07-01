#pragma once

#include <QWidget>
#include <QRectF>
#include <QImage>
#include <QPointer>
#include <QVector>
#include <QMetaObject>
#include "core/CallManager.h"
#include "painter/PainterTheme.h"

class CallParticipant;

/**
 * Minimal always-on-top PiP dock content, shown by CallWindow while
 * PiP-docked (see CallWindow::enterPipDock — triggered by navigating to a
 * different conversation, OR by minimizing/"dropping" the live call window,
 * which would otherwise vanish it to the taskbar mid-call). Deliberately
 * NOT the full CallStage crammed into a small box: just the three things
 * that matter in a glance — a small video/avatar thumbnail, a mute-state
 * indicator, and a hang-up button.
 *
 * Also owns dragging the compact dock around the desktop: CallStage's own
 * mouse handling is for the draggable self-view tile INSIDE the stage (a
 * different concept), which would otherwise swallow attempts to drag the
 * frameless PiP window itself. Releasing ("dropping") a drag here snaps the
 * top-level window to the nearest screen corner.
 */
class CallPipWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CallPipWidget(CallManager *call, QWidget *parent = nullptr);

    void setTheme(PainterTheme::Theme t);

signals:
    // Double-click anywhere except the hang-up button restores the full
    // call window (CallWindow::exitPipDock).
    void restoreRequested();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;

private:
    void rebindProvider();
    void onFrame(const QImage &img);
    QImage avatarDisc(const QString &id, const QString &name, int size, const PainterTheme &th) const;
    // Prefer showing the other party (mirrors what you'd want a glance-
    // reminder of); falls back to yourself when alone in the room.
    CallParticipant *bestParticipant() const;
    // Move the top-level (frameless) window to whichever screen corner is
    // nearest its current center — the "dock" half of "PiP-dock-on-drop".
    void snapToNearestCorner();

    CallManager *m_call;
    PainterTheme::Theme m_themeId = PainterTheme::Theme::Vivid;
    QVector<QMetaObject::Connection> m_conns;
    QPointer<CallParticipant> m_shown;
    QImage m_thumb;                 // last camera frame from m_shown, unscaled

    QRectF m_hangupRect;            // hit rect, recomputed each paint

    // Whole-window drag state (dragging THIS widget drags window()).
    bool m_pressOnHangup = false;
    bool m_dragging = false;
    bool m_dragMoved = false;
    QPoint m_dragStartGlobalPos;
    QPoint m_dragStartWinPos;
};
