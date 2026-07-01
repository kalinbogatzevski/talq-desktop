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
    // #5 — force a fresh tile layout + frame-cache flush. The owning CallWindow
    // calls this on a cross-monitor (DPI) move and after a fullscreen toggle,
    // where no resizeEvent is guaranteed even though the effective tile size /
    // backing-store devicePixelRatio changed — leaving the stage tile smashed
    // or dropped off-screen.
    void forceRelayout();

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
    // Idiot-proofing: a loud, plain-language banner shown when the local
    // camera can't be opened (missing / in use by another app / blocked by
    // OS privacy). Drawn outside the fading chrome so a non-technical user
    // is never left silently wondering why nobody can see them.
    void paintCameraBanner(QPainter &p, const PainterTheme &th);
    // Twin of paintCameraBanner for a microphone that won't open: the call
    // continues on silent audio, but the user is told nobody can hear them.
    void paintMicBanner(QPainter &p, const PainterTheme &th);
    // 0.52.5 — persistent amber chip when our OWN camera send quality is reduced
    // (software encoding / shed to the 480p floor under load), so the sender is
    // never silently stuck low. Reads CallManager::videoQualityNotice().
    void paintQualityNotice(QPainter &p, const PainterTheme &th);
    // 0.40.15 — split top chrome: paintInfoPills draws read-only telemetry
    // (codec/quality stat/RX) on the left; paintActionPills draws the
    // interactive QUALITY + BACKGROUND dropdown buttons on the right.
    void paintInfoPills(QPainter &p, const PainterTheme &th);
    void paintActionPills(QPainter &p, const PainterTheme &th);
    // Compute the top-right action-pill hit-rects (m_qualityPillRect/m_bgPillRect/
    // m_sharePillRect) WITHOUT painting, so the dropdown click registers even when
    // the call chrome is faded out (paintActionPills is skipped at alpha~0).
    void computeActionPillGeometry();
    void paintSharingBadge(QPainter &p, const PainterTheme &th);
    // Label for the receive-quality dropdown's HIGH entry, bucketed from the
    // PEER's peak decoded height (CallManager::peerPeakRxHeight) — the remote's
    // real top layer, not our own send setting. "High" alone until observed.
    QString highQualityLabel() const;
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
    // 0.41.1-beta — latest local-screen-share preview frame, pre-scaled
    // (height 360). Updated from the ScreenSharePipeline appsink tee
    // via rebindProviders. Empty while not screen-sharing.
    QImage m_selfScreenFrame;
    QRectF m_selfSharePipRect;
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
    // Last AUTO substream chosen per remote peer (sessionId -> 0/1/2), so
    // updateStreamQualities() can apply drop-hysteresis (pickSubstreamHysteretic)
    // and stop a tile parked on a size boundary from flapping the requested layer
    // every relayout. Keyed by sessionId; stale entries are harmless.
    QHash<QString,int> m_tileSubstream;

    // 0.40.15 — Status pill rect: doubles as the layout anchor for the
    // info pills (which start to its right) and as a hit rect. Always
    // visible while a call is up, so it has its own member rather than
    // living only inside m_topChromeRects.
    QRectF m_statusPillRect;
    // Left edge of the right-anchored action-button block, published by
    // paintActionPills each frame so paintInfoPills (drawn after it) can wrap
    // its telemetry tiles to a new row before they collide with the buttons on
    // a narrow window. Reset to width() each frame (no action pills = full width).
    qreal m_actionPillsLeft = 0;
    // Bottom Y of the top-chrome "anchor" row (status pill + action-button
    // block). When the window is too narrow to hold the status pill and the
    // action buttons side by side, paintActionPills drops the buttons to their
    // own row and this grows accordingly. Wrapped info-pill rows start BELOW
    // this so they never land on top of the action buttons.
    qreal m_chromeRowsBottom = 0;
    // Bottom Y actually reached by the info-pill tiles (incl. wrapped rows).
    // Published by paintInfoPills so the persistent "sharing" badge can sit
    // clear BELOW every top-chrome element instead of being overlapped by it.
    qreal m_infoPillsBottom = 0;

    // 0.40.15 — hit rects for the Quality / BG dropdown buttons (top-
    // right). Updated each paint by paintActionPills; consumed by
    // mousePressEvent + mouseMoveEvent for click + hover handling.
    // 0.41.1-beta — m_sharePillRect is the SHARE-quality dropdown,
    // only painted (and hit-tested) while screen sharing is live.
    QRectF m_qualityPillRect;
    QRectF m_bgPillRect;
    QRectF m_sharePillRect;

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
