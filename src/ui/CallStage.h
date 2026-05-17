#pragma once

#include <QWidget>
#include <QHash>
#include <QImage>
#include <QPointer>
#include <QElapsedTimer>
#include "core/CallManager.h"
#include "painter/PainterTheme.h"

class CallParticipant;

/**
 * The in-call surface, rendered entirely with QPainter on the warm theme
 * ladder (no #000 letterbox, no cool gray, accent is the one signal).
 *
 * One adaptive continuum, not three modes: it computes a stage source
 * (a shared screen, else the active speaker, else the sole remote) plus
 * a participant rail from participant count + window size. Self is a
 * draggable PiP in stage modes, an ordinary tile in the even gallery.
 *
 * Mission Control = calm glance: one breathing status pill always on, a
 * per-tile connection LED, and a summonable telemetry drawer (default
 * hidden). The control bar auto-hides on idle. Honors reduced motion.
 */
class CallStage : public QWidget
{
    Q_OBJECT

public:
    explicit CallStage(CallManager *call, QWidget *parent = nullptr);
    ~CallStage() override;

    void setTheme(PainterTheme::Theme t);
    bool telemetryOpen() const { return m_telemetryOpen; }

signals:
    void requestToggleFullscreen();
    void requestToggleShare();    // window owns the screen-source picker

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

private:
    struct Tile { QPointer<CallParticipant> p; QRectF rect; bool isStage = false; bool isScreen = false; };
    struct Btn  { QString id; QRectF rect; QString glyph; bool on = false; bool danger = false; };

    void rebindProviders();
    void purgeStaleFrames();      // drop cached frames for departed peers
    void onFrame(CallParticipant *p, bool screen, const QImage &img);
    void relayout();
    QVector<Tile> computeLayout() const;
    CallParticipant *stageSource(bool *isScreen) const;
    void paintTile(QPainter &p, const Tile &t, const PainterTheme &th, bool large);
    void paintControlBar(QPainter &p, const PainterTheme &th);
    void paintStatusPill(QPainter &p, const PainterTheme &th);
    void paintTelemetry(QPainter &p, const PainterTheme &th);
    void paintCentered(QPainter &p, const PainterTheme &th); // incoming/outgoing/alone
    void buildButtons();
    QString hitButton(const QPointF &pos) const;
    void pokeControls();              // show control bar, restart idle timer
    bool reducedMotion() const;
    QImage avatarDisc(const QString &id, const QString &name, int size, const PainterTheme &th) const;

    CallManager *m_call;
    PainterTheme::Theme m_themeId = PainterTheme::Theme::Vivid;

    // Per-participant scaled frame cache (pre-scaled in onFrame, cheap paint)
    QHash<CallParticipant*, QImage> m_camFrame;
    QHash<CallParticipant*, QImage> m_scrFrame;
    QVector<QMetaObject::Connection> m_conns;

    QVector<Tile> m_tiles;
    QVector<Btn> m_buttons;
    QPointer<CallParticipant> m_pinned;       // manual stage override
    bool m_controlsVisible = true;
    QElapsedTimer m_idleTimer;
    QTimer *m_tick = nullptr;                  // ~30fps repaint + idle/glow
    bool m_telemetryOpen = false;
    bool m_rosterOpen = false;

    // self-PiP drag
    int m_pipCorner = 3;                       // 0..3 TL,TR,BL,BR (default BR)
    bool m_draggingPip = false;
    QPointF m_dragOff;
    QRectF m_pipRect;

    double m_glowPhase = 0.0;
};
