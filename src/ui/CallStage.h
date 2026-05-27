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
    // #20 — right-click on the Background chip jumps to Settings →
    // Audio & Video where the full picker + blur slider + image
    // browser live. The window owns the dialog (MainWindow), so this
    // signal asks it to open Settings to the right tab.
    void requestOpenBackgroundSettings();

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
    struct Btn  { QString id; QRectF rect; QString glyph; QString tip; bool on = false; bool danger = false; };

    void rebindProviders();
    void purgeStaleFrames();      // drop cached frames for departed peers
    void onFrame(CallParticipant *p, bool screen, const QImage &img);
    void relayout();
    void updateStreamQualities();  // #132: per-tile-size simulcast substream request
    QVector<Tile> computeLayout() const;
    CallParticipant *stageSource(bool *isScreen) const;
    void paintTile(QPainter &p, const Tile &t, const PainterTheme &th, bool large);
    void paintControlBar(QPainter &p, const PainterTheme &th);
    void paintStatusPill(QPainter &p, const PainterTheme &th);
    // 0.40.15 — split top chrome: paintInfoPills draws read-only telemetry
    // (codec/quality stat/RX) on the left; paintActionPills draws the
    // interactive QUALITY + BACKGROUND dropdown buttons on the right.
    void paintInfoPills(QPainter &p, const PainterTheme &th);
    void paintActionPills(QPainter &p, const PainterTheme &th);
    void paintSharingBadge(QPainter &p, const PainterTheme &th);
    void paintTelemetry(QPainter &p, const PainterTheme &th);
    void paintCentered(QPainter &p, const PainterTheme &th); // incoming/outgoing/alone
    void buildButtons();
    QString hitButton(const QPointF &pos) const;
    void pokeControls();              // show control bar, restart idle timer
    // Right-click on the share segment while sharing → quality menu
    // (720p / 1080p / 1440p / Native) → CallManager::setScreenShareQuality
    // does a live re-share at the new cap. The picker dialog still owns
    // the pre-share initial pick.
    void showScreenShareQualityMenu(const QPoint &globalPos);
    bool reducedMotion() const;
    QImage avatarDisc(const QString &id, const QString &name, int size, const PainterTheme &th) const;

    CallManager *m_call;
    PainterTheme::Theme m_themeId = PainterTheme::Theme::Vivid;

    // Per-participant scaled frame cache (pre-scaled in onFrame, cheap paint)
    QHash<CallParticipant*, QImage> m_camFrame;
    QHash<CallParticipant*, QImage> m_scrFrame;
    // Smoothed mic level per participant (fast attack / slow decay) so the
    // name-plate meter reads like a real VU, not a jittery raw value.
    QHash<CallParticipant*, qreal>  m_micLvl;
    QVector<QMetaObject::Connection> m_conns;

    QVector<Tile> m_tiles;
    QVector<Btn> m_buttons;
    QPointer<CallParticipant> m_pinned;       // manual stage override
    bool m_controlsVisible = true;
    QElapsedTimer m_idleTimer;
    QTimer *m_tick = nullptr;                  // ~30fps repaint + idle/glow
    bool m_telemetryOpen = false;
    bool m_rosterOpen = false;
    // Outbound-bitrate ring buffer (Mbps) for the telemetry sparkline.
    static constexpr int kBwHistoryMax = 60;
    QVector<float> m_bwHistory;
    QString m_hoverBtn;                        // control-bar button under cursor
    // 0.40.15 — smooth fade for the top-row chrome (info chips + action
    // buttons) and the bottom control bar, tied to m_controlsVisible.
    // 1.0 = fully visible, 0.0 = fully hidden; the tick eases ~250 ms.
    // Status pill stays at 1.0 always per Mission Control "calm glance".
    double m_chromeAlpha = 1.0;

    // self-PiP drag
    int m_pipCorner = 3;                       // 0..3 TL,TR,BL,BR (default BR)
    bool m_draggingPip = false;
    QPointF m_dragOff;
    QRectF m_pipRect;

    double m_glowPhase = 0.0;

    // #8 manual stream-quality selector: -1=Auto (tile-size driven by
    // updateStreamQualities), 0=Low/180p, 1=Med/360p, 2=High/720p.
    int m_qualityOverride = -1;

    // 0.40.15 — Status pill rect: doubles as the layout anchor for the
    // info pills (which start to its right) and as a hit rect. Always
    // visible while a call is up, so it has its own member rather than
    // living only inside m_topChromeRects.
    QRectF m_statusPillRect;

    // 0.40.15 — hit rects for the Quality / BG dropdown buttons (top-
    // right). Updated each paint by paintActionPills; consumed by
    // mousePressEvent + mouseMoveEvent for click + hover handling.
    QRectF m_qualityPillRect;
    QRectF m_bgPillRect;

    // 0.40.15 — every rect painted in the top chrome row (status pill,
    // info chips, action buttons). The double-click handler iterates
    // this to suppress fullscreen-toggle on chip clicks; cleared at the
    // start of every paintEvent and appended-to by each top-chrome
    // paint helper.
    QVector<QRectF> m_topChromeRects;

    // 0.40.15 — true while a Quality/BG dropdown menu is open. Used to
    // pin the chrome visible (skip the idle auto-hide) so the menu
    // doesn't fade out from under the cursor while it's on screen.
    bool m_menuOpen = false;

    // 0.40.15 — action-pill hover. "quality", "bg", or empty. Drives
    // the accent-border + native QToolTip on the hovered action chip.
    QString m_hoverPill;
};
