#include "CallStage.h"
#include "core/VideoFrameProvider.h"
#include "painter/VectorIcons.h"
#include "ui/SubstreamPolicy.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QFontMetrics>
#include <QScreen>
#include <QWindow>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QToolTip>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QtMath>

// ── small helpers ───────────────────────────────────────────────────────
namespace {
QString initials(const QString &name)
{
    const QStringList parts = name.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return QStringLiteral("?");
    QString s = parts.first().left(1).toUpper();
    if (parts.size() > 1) s += parts.last().left(1).toUpper();
    return s;
}
QString fmtDuration(int sec)
{
    int h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
    if (h > 0) return QString::asprintf("%d:%02d:%02d", h, m, s);
    return QString::asprintf("%d:%02d", m, s);
}
QFont monoFont(int px)
{
    QFont f(QStringLiteral("Consolas"));
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(px);
    f.setWeight(QFont::DemiBold);
    return f;
}
} // namespace

CallStage::CallStage(CallManager *call, QWidget *parent)
    : QWidget(parent), m_call(call)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_idleTimer.start();

    connect(m_call, &CallManager::stateChanged, this, [this]{
        if (m_call->state() == CallManager::Idle) {
            m_camFrame.clear(); m_scrFrame.clear();
            m_tileSubstream.clear();   // fresh hysteresis seed per call; no cross-call leak
        }
        // Keep the control bar (with the Leave/Cancel button) reachable while
        // reconnecting. Auto-hide only runs in Active, but if the chrome had
        // already faded when ICE failed, force it back so the user can always
        // cancel a stuck reconnect.
        if (m_call->state() == CallManager::Reconnecting) {
            m_controlsVisible = true;
            m_idleTimer.restart();
        }
        relayout(); update();
    });
    connect(m_call, &CallManager::durationChanged, this, [this]{ update(); });
    connect(m_call, &CallManager::videoQualityNoticeChanged, this, [this]{ update(); });  // 0.52.5 chip
    connect(m_call, &CallManager::callStatsChanged, this, [this]{
        // Sample the outbound bitrate into a ring buffer (~1 s cadence, the
        // callStats tick) so the telemetry panel can draw a live sparkline.
        // Sample even when the panel is closed so history exists on open.
        m_bwHistory.push_back((float)m_call->txBitrateMbps());
        while (m_bwHistory.size() > kBwHistoryMax) m_bwHistory.pop_front();
        if (m_telemetryOpen) update();
    });
    connect(m_call, &CallManager::participantsChanged, this, [this]{ relayout(); update(); });
    connect(m_call, &CallManager::participantAdded, this, [this]{ rebindProviders(); relayout(); update(); });
    // 0.41.1-beta — when our screen share starts/stops, rebind the
    // local-screen preview provider connection and relayout to place
    // (or remove) the screen-share self-PiP rect.
    connect(m_call, &CallManager::screenShareChanged, this,
            [this]{ rebindProviders(); relayout(); update(); });
    connect(m_call, &CallManager::participantRemoved, this, [this](const QString &){
        purgeStaleFrames();    // drop departed peers' cached frames (a raw
        rebindProviders();     // pointer key could otherwise collide with a
        relayout(); update();  // recycled CallParticipant address)
    });

    // One coalesced ~30fps tick: drives video repaint, the breathing glow,
    // and control-bar auto-hide. Cheaper than update()-per-frame-per-tile.
    m_tick = new QTimer(this);
    m_tick->setInterval(33);
    connect(m_tick, &QTimer::timeout, this, [this]{
        if (!reducedMotion()) m_glowPhase += 0.06;
        // 0.40.15 — 5 s idle (was 3.5 s). The chrome was disappearing
        // too aggressively for a manual hover/inspect workflow; this
        // lets the eye settle on the chips before they fade.
        if (m_controlsVisible && m_call->state() == CallManager::Active
            && !m_menuOpen
            && m_idleTimer.elapsed() > 5000) {
            m_controlsVisible = false;
            setCursor(Qt::BlankCursor);
        }
        // Ease the top-chrome + control-bar alpha toward visible/hidden.
        // ~250 ms fade (step 0.13/frame at 30 fps). Reduced-motion users
        // get an instant snap so we don't introduce gratuitous animation.
        const double target = m_controlsVisible ? 1.0 : 0.0;
        if (reducedMotion()) {
            m_chromeAlpha = target;
        } else if (qAbs(m_chromeAlpha - target) > 1e-3) {
            const double step = 0.13;
            m_chromeAlpha = (m_chromeAlpha < target)
                ? qMin(target, m_chromeAlpha + step)
                : qMax(target, m_chromeAlpha - step);
        }
        update();
    });
    m_tick->start();

    rebindProviders();
    relayout();
}

CallStage::~CallStage() = default;

void CallStage::setTheme(PainterTheme::Theme t)
{
    if (m_themeId == t) return;
    m_themeId = t;
    update();
}

bool CallStage::reducedMotion() const
{
    return QSettings("TalQ", "TalQ").value("reduceMotion", false).toBool();
}

// ── provider binding ────────────────────────────────────────────────────
void CallStage::rebindProviders()
{
    // Drop stale connections, (re)connect every participant's camera+screen.
    for (const auto &c : m_conns) QObject::disconnect(c);
    m_conns.clear();

    const auto parts = m_call->participants();
    for (CallParticipant *p : parts) {
        // React to a provider pointer swap / any field change on this peer.
        m_conns << connect(p, &CallParticipant::videoProvidersChanged,
                           this, [this]{ rebindProviders(); update(); });
        m_conns << connect(p, &CallParticipant::changed, this, [this]{ update(); });
        if (auto *cam = p->camera())
            m_conns << connect(cam, &VideoFrameProvider::imageReady, this,
                [this, p](const QImage &img){ onFrame(p, false, img); });
        else
            m_camFrame.remove(p);  // camera stream ended → drop cached frame
        if (auto *scr = p->screen())
            m_conns << connect(scr, &VideoFrameProvider::imageReady, this,
                [this, p](const QImage &img){ onFrame(p, true, img); });
        else
            // Screen share ended while participant stays in the call. Drop
            // the cached last frame so the tile renders empty instead of
            // freezing on the prior share's last image. Mirrors upstream
            // Talk's ScreenShare.vue `srcObject = null` clear on
            // unshareScreen; see [[project_talq_upstream_screenshare]].
            m_scrFrame.remove(p);
    }
    // 0.41.1-beta — local screen-share self-preview. Independent of the
    // per-participant remote screen providers above: this is OUR
    // outgoing share, tapped off the ScreenSharePipeline appsink tee.
    if (auto *sp = m_call->localScreenPreviewProvider()) {
        m_conns << connect(sp, &VideoFrameProvider::imageReady, this,
            [this](const QImage &img) {
                // 0.53.0 — scale to 720 (was 360): the self-share can now be the
                // full STAGE tile (self-view), not just a thumbnail, so it needs the
                // extra resolution to read crisply. Never upscale a smaller capture.
                m_selfScreenFrame = img.height() > 720
                    ? img.scaledToHeight(720, Qt::SmoothTransformation)
                    : img;
                update();
            });
    } else {
        m_selfScreenFrame = QImage();
    }
    update();
}

void CallStage::purgeStaleFrames()
{
    QSet<CallParticipant*> live;
    const auto parts = m_call->participants();
    live.reserve(parts.size());
    for (CallParticipant *p : parts) live.insert(p);
    for (auto it = m_camFrame.begin(); it != m_camFrame.end(); ) {
        if (live.contains(it.key())) ++it;
        else it = m_camFrame.erase(it);
    }
    for (auto it = m_scrFrame.begin(); it != m_scrFrame.end(); ) {
        if (live.contains(it.key())) ++it;
        else it = m_scrFrame.erase(it);
    }
    for (auto it = m_micLvl.begin(); it != m_micLvl.end(); ) {
        if (live.contains(it.key())) ++it;
        else it = m_micLvl.erase(it);
    }
}

void CallStage::onFrame(CallParticipant *p, bool screen, const QImage &img)
{
    // Diagnostic: confirm the render path actually receives remote frames
    // (first few + every 100th). Cheap, gated to the debug log only.
    static QHash<CallParticipant*, int> s_dbgCount;
    int &n = s_dbgCount[p];
    if (++n <= 3 || n % 100 == 0)
        qDebug() << "CallStage::onFrame" << (p && p->isSelf() ? "SELF" : "REMOTE")
                 << (screen ? "screen" : "camera") << img.size()
                 << "frame#" << n << "part=" << (void*)p;
    if (img.isNull() || img.width() <= 32) return;       // skip MCU 16x16 dummy
    // 0.52.12 — a real SELF CAMERA frame proves the camera recovered. Clear any
    // stale "Your camera isn't available" notice: it was raised on an earlier
    // cameraError and is otherwise only cleared on a fresh call or a manual
    // re-toggle, so after an internal retry succeeded it lingered on screen (and
    // over a screen share). Idempotent — only fires while the flag is still set.
    if (p && p->isSelf() && !screen && m_call && m_call->isCameraUnavailable())
        m_call->cameraFrameConfirmed();
    // Pre-scale to the widget bound so paint is a plain blit (perf guardrail).
    // SCREEN frames take the QUALITY path: FastTransformation is nearest-
    // neighbour, which DROPS pixel rows outright — on downscaled screen text
    // that reads exactly as "interlaced / detail missing vertically / fonts
    // unreadable" (Kalin→Ilko field 2026-07-08; worse the BIGGER the source,
    // so "even native res looked bad"). And the cap was in LOGICAL pixels: on
    // a hi-DPI viewer the tile then re-upscaled the decimated image by the
    // device-pixel-ratio (decimate-then-blur). So for screens: cap in PHYSICAL
    // pixels + SmoothTransformation. Shares are mostly static (few frames/s),
    // so the smooth filter's extra ms never hits the hot path — the 30 fps
    // camera tiles keep the cheap Fast blit (motion masks it).
    QImage f = img;
    QSize cap = size().isValid() ? size() : QSize(1280, 720);
    if (screen) {
        const qreal dpr = devicePixelRatioF();
        cap = QSize(qRound(cap.width() * dpr), qRound(cap.height() * dpr));
    }
    if (img.width() > cap.width() || img.height() > cap.height())
        f = img.scaled(cap, Qt::KeepAspectRatio,
                       screen ? Qt::SmoothTransformation : Qt::FastTransformation);
    (screen ? m_scrFrame : m_camFrame)[p] = f;
    // tick timer coalesces the actual repaint
}

// ── layout ──────────────────────────────────────────────────────────────
CallParticipant *CallStage::stageSource(bool *isScreen) const
{
    *isScreen = false;
    const auto parts = m_call->participants();
    // Skip self here — self is always index 0 (CallManager prepends it), so an
    // unguarded scan would match OUR OWN active share before ever reaching a
    // remote peer's, permanently winning the stage even after a peer starts
    // sharing too (backlog bug: a peer's incoming share never displayed while
    // we were already sharing our own screen). The self-share fallback below
    // already implements the correct "peer's share wins the stage" precedent
    // this loop must respect.
    for (CallParticipant *p : parts)
        if (!p->isSelf() && p->screenSharing() && p->screen()) { *isScreen = true; return p; }
    // 0.53.0 — Zoom-style self-view: when WE are sharing and no PEER is, put OUR
    // OWN share on the stage (rendered from the local capture preview tap), so the
    // presenter sees what they're broadcasting full-size — the same view the remote
    // gets — instead of only a small thumbnail. A peer's share still wins the stage
    // (their content is what the call is watching); ours then falls back to the PiP.
    if (m_call->isScreenSharing() && m_call->localScreenPreviewProvider()
        && m_call->selfParticipant()) {
        // 0.53.x — self-view on the stage for BOTH window AND monitor shares, so
        // the local-share UI matches a peer-share / p2p call (consistent, per
        // Kalin). The monitor-share hall-of-mirrors (sharing the screen TalQ is
        // on → WGC captures TalQ showing the capture → feedback freeze) is solved
        // in paintTile by drawing a static "You're sharing this screen" placeholder
        // instead of the live recapture — NOT by hiding the self-view (the blunt
        // 0.53.1 window-only gate this replaces).
        *isScreen = true;
        return m_call->selfParticipant();
    }
    if (m_pinned) return m_pinned;
    CallParticipant *speaker = nullptr, *firstRemote = nullptr;
    for (CallParticipant *p : parts) {
        if (p->isSelf()) continue;
        if (!firstRemote) firstRemote = p;
        if (p->speaking() && !speaker) speaker = p;
    }
    if (speaker) return speaker;
    if (firstRemote) return firstRemote;
    return parts.isEmpty() ? nullptr : parts.first();
}

QVector<CallStage::Tile> CallStage::computeLayout() const
{
    QVector<Tile> tiles;
    const auto parts = m_call->participants();
    QList<CallParticipant*> remotes;
    for (CallParticipant *p : parts) if (!p->isSelf()) remotes << p;

    const QRectF area = rect().adjusted(0, 0, 0, 0);

    // Multi-peer GROUP screen-share receive: 2+ remote peers sharing their
    // screen CONCURRENTLY. The subscribe layer already keeps one independent
    // SubscribePipeline + VideoFrameProvider per remote sharer regardless of
    // count (CallManager::m_screenSubscribers, keyed by sessionId — the same
    // machinery an ordinary 3+-way camera group call already relies on), so
    // every concurrent share is already being received and decoded. The
    // single-stage-source design below (stageSource()) only ever surfaces
    // ONE of them (first match) and silently drops the rest from view. This
    // branch is a purely ADDITIVE generalization: give every concurrently-
    // sharing peer its own tile in a grid across the stage area instead of
    // discarding all but one. It only engages for 2+ simultaneous shares —
    // the 0-or-1-sharer path (the heavily field-hardened common case) below
    // is completely untouched. Deliberately NOT gated on m_pinned: stageSource()
    // already lets any single active share win over a manual pin unconditionally
    // (the sharing-participant loop runs before the pin check) — a pin only ever
    // governs stage selection while nobody is sharing. Matching that precedent,
    // 2+ concurrent shares also win over a stale pin; it resumes governing the
    // stage once every share ends.
    QList<CallParticipant*> sharers;
    for (CallParticipant *p : remotes)
        if (p->screenSharing() && p->screen()) sharers << p;
    if (sharers.size() >= 2) {
        const qreal m = 14, gap = 10, railW = 196;
        QRectF stageR(area.left()+m, area.top()+m,
                      area.width()-2*m - (railW+gap),
                      area.height()-2*m);
        const int cnt = sharers.size();
        const int cols = qCeil(qSqrt(double(cnt)));
        const int rows = qCeil(double(cnt) / cols);
        const qreal cw = (stageR.width() - (cols-1)*gap) / cols;
        const qreal ch = (stageR.height() - (rows-1)*gap) / rows;
        for (int i = 0; i < cnt; ++i) {
            int r = i / cols, c = i % cols;
            Tile t; t.p = sharers[i]; t.isStage = true; t.isScreen = true;
            t.rect = QRectF(stageR.left() + c*(cw+gap), stageR.top() + r*(ch+gap), cw, ch);
            tiles << t;
        }
        // Rail: EVERYONE (self included) as a small camera tile — same "group
        // interface, never a stray self-PiP" rule the single-share rail below
        // follows. Identical sizing/overflow math to that rail so the two
        // paths look consistent to the eye.
        qreal x = stageR.right()+gap;
        qreal railTh = 132, ty = area.top()+m;
        int maxVisible = qMax(1, int((area.height()-2*m+gap) / (railTh+gap)));
        int shown = qMin(parts.size(), maxVisible);
        bool overflow = parts.size() > maxVisible;
        int real = overflow ? shown-1 : shown;
        for (int i = 0; i < real; ++i) {
            Tile t; t.p = parts[i];
            t.rect = QRectF(x, ty + i*(railTh+gap), railW, railTh);
            tiles << t;
        }
        if (overflow) {
            Tile t; t.p = nullptr;   // sentinel: +N tile
            t.rect = QRectF(x, ty + real*(railTh+gap), railW, railTh);
            tiles << t;
        }
        return tiles;
    }

    bool isScreen = false;
    CallParticipant *src = stageSource(&isScreen);

    // A group screen share must always show the GROUP interface: the screen on
    // the stage and EVERYONE (incl. self) as member tiles in the rail — never a
    // stray self-PiP. Decide rail-vs-PiP from the share FLAG (stable the instant
    // sharing starts), NOT from isScreen (which is true only once the screen
    // provider has DECODED a frame, so it flickers self out to a PiP during a
    // share's startup/rebuild/frame-gap — the wrong-interface bug Kalin hit in a
    // 3-way call while Pavel was sharing). isScreen still drives what the STAGE
    // renders; shareActive drives the layout MODE.
    bool shareActive = isScreen || m_call->isScreenSharing();
    if (!shareActive)
        for (CallParticipant *p : parts)
            if (p->screenSharing()) { shareActive = true; break; }

    const int n = remotes.size();
    const bool evenGallery = !shareActive && n >= 2 && n <= 4 && !m_pinned;

    if (evenGallery) {
        // Equal warm grid of everyone (self included), speaker gets the glow.
        QList<CallParticipant*> all = { };
        for (CallParticipant *p : parts) all << p;
        int cnt = all.size();
        int cols = qCeil(qSqrt(double(cnt)));
        int rows = qCeil(double(cnt) / cols);
        qreal gap = 10, m = 14;
        qreal cw = (area.width() - 2*m - (cols-1)*gap) / cols;
        qreal ch = (area.height() - 2*m - (rows-1)*gap) / rows;
        for (int i = 0; i < cnt; ++i) {
            int r = i / cols, c = i % cols;
            Tile t; t.p = all[i];
            t.rect = QRectF(m + c*(cw+gap), m + r*(ch+gap), cw, ch);
            tiles << t;
        }
        return tiles;
    }

    // Stage + rail (1:1, medium, large, or any screen share).
    if (!src) return tiles;
    QList<CallParticipant*> railList;
    for (CallParticipant *p : parts) {
        if (p == src && !isScreen) continue;          // src is the stage
        // Self is a floating PiP overlay normally, BUT when a screen share
        // is active anywhere in the call (self or peer) the stage is the
        // screen content and the rail carries the participant tiles. In
        // that mode the self-camera belongs in the rail (becomes the "You"
        // tile, no floating overlay), so it doesn't obscure shared content.
        // Keyed on shareActive (flag) not isScreen (frame-decoded) so self
        // stays a rail member throughout a share, never flickering to a PiP.
        // Self floats as a corner PiP ONLY in a 1:1 stage (n==1). In any GROUP
        // stage+rail layout (a pinned speaker, or 5+ peers) self joins the rail as
        // the "You" tile so it matches the other member tiles instead of floating
        // mis-sized over the rail in the bottom-right corner (field bug). During a
        // screen share self is already a rail member (shareActive handled above).
        if (p->isSelf() && !shareActive && n < 2) continue;   // floating self-PiP only in 1:1
        railList << p;
    }
    const bool hasRail = !railList.isEmpty();
    qreal m = 14, gap = 10;
    qreal railW = hasRail ? 196 : 0;
    QRectF stageR(area.left()+m, area.top()+m,
                  area.width()-2*m - (hasRail ? railW+gap : 0),
                  area.height()-2*m);
    Tile st; st.p = src; st.rect = stageR; st.isStage = true; st.isScreen = isScreen;
    tiles << st;

    if (hasRail) {
        qreal x = stageR.right()+gap;
        qreal th = 132, ty = area.top()+m;
        int maxVisible = qMax(1, int((area.height()-2*m+gap) / (th+gap)));
        int shown = qMin(railList.size(), maxVisible);
        // If overflow, last visible slot becomes a "+N" tile.
        bool overflow = railList.size() > maxVisible;
        int real = overflow ? shown-1 : shown;
        for (int i = 0; i < real; ++i) {
            Tile t; t.p = railList[i];
            t.rect = QRectF(x, ty + i*(th+gap), railW, th);
            tiles << t;
        }
        if (overflow) {
            Tile t; t.p = nullptr;   // sentinel: +N tile
            t.rect = QRectF(x, ty + real*(th+gap), railW, th);
            tiles << t;
        }
    }
    return tiles;
}

void CallStage::relayout()
{
    m_tiles = computeLayout();
    buildButtons();
    // self-PiP rect. Self counts as "already a tile" (→ no PiP) ONLY in
    // mediaPhase, since that's the only phase tiles are painted. During the
    // centered calling/connecting screen computeLayout() still emits a
    // throwaway self stage-tile (no remote to put on stage yet) that's never
    // drawn — it must not suppress the self-preview PiP.
    const auto st = m_call->state();
    int rmt = 0; for (auto *q : m_call->participants()) if (!q->isSelf()) ++rmt;
    const bool mediaPhase =
        (st == CallManager::Active || st == CallManager::Connecting)
        && rmt > 0 && !m_tiles.isEmpty();
    bool selfIsTile = false;
    if (mediaPhase)
        for (const Tile &t : m_tiles) if (t.p && t.p->isSelf()) selfIsTile = true;
    // Mixed-DPI drag-smash guard (backlog item): relayout() can also be
    // triggered mid-gesture by resizeEvent() / forceRelayout() — the latter
    // fires unconditionally from CallWindow::onScreenChanged() whenever the
    // call window crosses to a different-DPI monitor. If that recomputes
    // m_pipRect from the NEW width()/height() while the user is mid-drag on
    // the self PiP, the tile jumps: the next mouseMoveEvent then applies
    // m_dragOff (captured at mouse-press against the OLD logical-pixel grid)
    // on top of the freshly-snapped rect. Leave m_pipRect alone while
    // dragging — mouseReleaseEvent's corner-snap + relayout() already
    // re-places it correctly once the gesture ends. Only defensively clamp
    // it on-widget in case a shrink-on-DPI-change happened mid-drag.
    if (!m_draggingPip) {
        if (!selfIsTile && m_call->selfParticipant()) {
            qreal w = qBound(140.0, width()*0.18, 240.0), h = w*9.0/16.0, m = 18;
            qreal x = (m_pipCorner % 2 == 0) ? m : width()-w-m;
            qreal y = (m_pipCorner < 2)      ? m : height()-h-m-72; // clear control bar
            m_pipRect = QRectF(x, y, w, h);
        } else {
            m_pipRect = QRectF();
        }
    } else if (!m_pipRect.isNull()) {
        const qreal maxX = qMax(0.0, width()  - m_pipRect.width());
        const qreal maxY = qMax(0.0, height() - m_pipRect.height());
        QPointF tl = m_pipRect.topLeft();
        tl.setX(qBound(0.0, tl.x(), maxX));
        tl.setY(qBound(0.0, tl.y(), maxY));
        m_pipRect.moveTopLeft(tl);
    }

    // 0.41.1-beta — second small PiP for the local screen share so the
    // user can SEE what they're broadcasting (Zoom/Teams/Meet/Telegram
    // consensus: never hide the camera PiP for the share — both stay).
    // Anchor to the corner opposite the camera PiP so they don't stack.
    // 0.53.0 — when OUR share is the STAGE (self-view; no peer is sharing) the big
    // stage tile already shows it, so the small self-share PiP is redundant — drop
    // it. It still appears when a PEER's share holds the stage and we're ALSO sharing
    // (so we can watch theirs while keeping an eye on our own outgoing share).
    // Same mid-drag guard as the self-PiP above: only the camera PiP
    // (m_pipRect) is directly dragged, but its corner/size drive this one
    // too (opposite corner), so freeze it in lockstep to avoid the pair
    // visibly popping to inconsistent positions mid-gesture.
    if (!m_draggingPip) {
        bool selfShareOnStage = false;
        for (const Tile &t : m_tiles)
            if (t.isStage && t.isScreen && t.p && t.p->isSelf()) selfShareOnStage = true;
        if (m_call->isScreenSharing() && m_call->localScreenPreviewProvider()
            && !selfShareOnStage) {
            const qreal w = qBound(140.0, width() * 0.16, 220.0);
            const qreal h = w * 9.0 / 16.0;
            const qreal m = 18;
            const int   oppCorner = m_pipCorner ^ 1;          // flip horizontal
            qreal x = (oppCorner % 2 == 0) ? m : width()  - w - m;
            qreal y = (oppCorner <  2)    ? m : height() - h - m - 72;
            m_selfSharePipRect = QRectF(x, y, w, h);
        } else {
            m_selfSharePipRect = QRectF();
        }
    }

    updateStreamQualities();
}

void CallStage::updateStreamQualities()
{
    // #132 simulcast: request the substream that matches how big each
    // remote peer is rendered (upstream spreed's tile-size policy). The
    // SFU adapts DOWN on its own, so this is an upper bound. CallManager
    // dedupes, so calling on every relayout is cheap. Screen-share is
    // single-layer and skipped.
    if (!m_call) return;
    for (const Tile &t : m_tiles) {
        if (!t.p || t.p->isSelf() || t.isScreen) continue;
        const QString sid = t.p->sessionId();
        // Drop-hysteresis on the AUTO selection: a tile hovering on a size
        // boundary won't flip the requested layer (and force an SFU switch +
        // keyframe) every relayout. Manual picks bypass it (override wins inside
        // pickSubstreamHysteretic). prev = last layer chosen for this peer.
        const int substream = pickSubstreamHysteretic(
            m_qualityOverride, t.rect.height(), t.isStage,
            m_tileSubstream.value(sid, -1));
        m_tileSubstream[sid] = substream;
        // m_qualityOverride >= 0 means the user manually picked a quality —
        // pin it past the auto load controller; -1 (Auto) lets it govern.
        m_call->requestPeerVideoQuality(sid, substream,
                                        /*manual=*/m_qualityOverride >= 0);
    }
}

// ── painting ────────────────────────────────────────────────────────────
void CallStage::paintEvent(QPaintEvent *)
{
    // One-time hook (the window handle exists once we're painting): force a
    // repaint when the call window moves to another display, so a self MONITOR
    // share flips placeholder<->live even for a perfectly static shared screen
    // that delivers no new capture frames. Harmless when not sharing (the
    // re-eval is a no-op). update() re-runs paintTile → re-checks window()->screen().
    if (!m_winScreenHooked) {
        if (QWidget *w = window()) {
            if (QWindow *wh = w->windowHandle()) {
                connect(wh, &QWindow::screenChanged, this, [this]{ update(); });
                m_winScreenHooked = true;
            }
        }
    }
    PainterTheme th(m_themeId, 1.0);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.fillRect(rect(), th.bgPrimary);

    // Top-chrome hit rects: cleared every frame, re-populated by each
    // paint helper that draws into the top row. Anything not drawn this
    // frame correctly falls out of double-click-guard hit testing.
    m_topChromeRects.clear();
    // No action buttons until paintActionPills runs this frame → info pills may
    // use the full width; paintActionPills lowers this to the button block's left.
    m_actionPillsLeft = width();
    m_chromeRowsBottom = 40.0;   // status-pill row bottom (14 + 26) by default
    m_infoPillsBottom  = 0.0;
    m_qualityPillRect = QRectF();
    m_bgPillRect      = QRectF();

    const auto state = m_call->state();
    const auto parts = m_call->participants();
    int remotes = 0; for (auto *q : parts) if (!q->isSelf()) remotes++;

    const bool mediaPhase =
        (state == CallManager::Active || state == CallManager::Connecting)
        && remotes > 0
        && !m_tiles.isEmpty();

    if (state == CallManager::Incoming || state == CallManager::Outgoing
        || (!mediaPhase && state != CallManager::Idle)) {
        paintCentered(p, th);
    } else if (mediaPhase) {
        for (const Tile &t : m_tiles)
            paintTile(p, t, th, t.isStage || m_tiles.size() <= 4);
        // self picture-in-picture
        if (!m_pipRect.isNull() && m_call->selfParticipant()) {
            Tile s; s.p = m_call->selfParticipant(); s.rect = m_pipRect;
            paintTile(p, s, th, false);
        }
        // 0.41.1-beta — local screen-share self-preview tile.
        if (!m_selfSharePipRect.isNull()) {
            const QRectF r = m_selfSharePipRect;
            const qreal rd = 13;
            // bg-surface card frame so the preview reads as a tile, not
            // a floater
            QColor face = th.bgSurface; face.setAlphaF(0.9);
            p.setBrush(face); p.setPen(QPen(th.danger, 1.3));
            p.drawRoundedRect(r, rd, rd);
            const QRectF inner = r.adjusted(2, 2, -2, -2);
            if (!m_selfScreenFrame.isNull()) {
                const QImage &img = m_selfScreenFrame;
                const qreal sx = inner.width()  / img.width();
                const qreal sy = inner.height() / img.height();
                const qreal s  = qMin(sx, sy);
                const QSizeF target(img.width() * s, img.height() * s);
                const QPointF off(inner.center() - QPointF(target.width()/2,
                                                            target.height()/2));
                p.drawImage(QRectF(off, target), img);
            }
            // Small label so it's clear what this tile is, even when
            // the preview frame is empty/black for the first ~200 ms.
            QFont lf = monoFont(8); lf.setBold(true);
            p.setFont(lf);
            QColor labelBg = th.danger; labelBg.setAlphaF(0.92);
            QFontMetrics lfm(lf);
            const QString label = tr("SHARING");
            const QRectF lbl(r.left() + 6, r.top() + 6,
                              lfm.horizontalAdvance(label) + 12, 16);
            p.setBrush(labelBg); p.setPen(Qt::NoPen);
            p.drawRoundedRect(lbl, 5, 5);
            p.setPen(th.controlInk);
            p.drawText(lbl, Qt::AlignCenter, label);
        }
        paintStatusPill(p, th);
        // Camera-unavailable banner: painted OUTSIDE the fading chrome block
        // below so it stays put the whole time the problem is live.
        paintCameraBanner(p, th);
        paintMicBanner(p, th);
        paintQualityNotice(p, th);   // sender-visible degraded-send chip (not chrome-faded)
        if (m_telemetryOpen) paintTelemetry(p, th);
        // Top info/action chrome + bottom control bar fade together.
        // When fully hidden we skip painting (and skip appending hit
        // rects), so clicks fall through to the bare video surface and
        // double-click toggles fullscreen.
        if (m_chromeAlpha > 1e-3) {
            p.save();
            p.setOpacity(m_chromeAlpha);
            // Action buttons FIRST: they publish m_actionPillsLeft +
            // m_chromeRowsBottom so the info pills below wrap before colliding
            // with them (and drop BELOW them) on a narrow window.
            paintActionPills(p, th);
            paintInfoPills(p, th);
            paintControlBar(p, th);
            p.restore();
        }
        // Persistent "you're sharing your screen" badge — painted LAST, at full
        // opacity, on its OWN row BELOW the top-chrome block (status + info +
        // action rows). Previously it shared the top row and the info/action
        // pills (drawn after it) painted ON TOP, so on a narrow window it sat
        // "under" them. Now it always has a clear row and stays visible while
        // the chrome fades.
        paintSharingBadge(p, th);
    }
}

QImage CallStage::avatarDisc(const QString &id, const QString &name,
                             int size, const PainterTheme &th) const
{
    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter g(&img);
    g.setRenderHint(QPainter::Antialiasing);
    g.setBrush(PainterTheme::authorColor(id)); g.setPen(Qt::NoPen);
    g.drawEllipse(QRectF(0.5, 0.5, size-1.0, size-1.0));
    QFont f = th.nameFont(); f.setPixelSize(int(size*0.36)); f.setWeight(QFont::DemiBold);
    g.setFont(f); g.setPen(th.controlInk);
    g.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, initials(name));
    return img;
}

void CallStage::paintTile(QPainter &p, const Tile &t, const PainterTheme &th, bool large)
{
    const qreal r = 13;
    QRectF rc = t.rect;

    if (!t.p) {  // "+N more" overflow sentinel
        int hidden = 0;
        const auto parts = m_call->participants();
        for (auto *q : parts) if (!q->isSelf()) hidden++;
        // Non-rail tiles = every stage tile (normally 1; a multi-peer
        // screen-share grid can put several sharers on "stage" at once) plus
        // this sentinel itself. Counting it explicitly (rather than the old
        // hardcoded "-2", which silently assumed exactly one stage tile)
        // keeps the "+N more" count correct in both layouts.
        int stageTiles = 0;
        for (const Tile &tt : m_tiles) if (tt.isStage) ++stageTiles;
        hidden = qMax(0, hidden - (m_tiles.size() - stageTiles - 1));
        p.setBrush(th.bgSurface); p.setPen(Qt::NoPen);
        p.drawRoundedRect(rc, r, r);
        p.setPen(th.textSecondary); p.setFont(th.nameFont());
        p.drawText(rc, Qt::AlignCenter, QStringLiteral("+%1 more").arg(hidden));
        return;
    }
    CallParticipant *cp = t.p;
    const bool speaking = cp->speaking() && !cp->audioMuted();

    // Active-speaker frame: a clear border around whoever is speaking (was a
    // faint glow that was easy to miss). It breathes gently; on reduced-motion
    // it's a steady solid frame. Drawn just outside the tile so it reads as a
    // frame, not an inner ring. AMBER, not the teal accent: the accent reads
    // too close to the green screen-share border (ShareOverlay's #2EA043), and
    // users couldn't tell "speaking" from "sharing" at a glance. Amber is
    // themed per-palette, warm ("voice"), and unambiguous against both.
    if (speaking) {
        qreal pulse = reducedMotion() ? 1.0 : (0.78 + 0.22*qSin(m_glowPhase));
        QColor fr = th.amber; fr.setAlphaF(qBound(0.0, 0.9 * pulse, 1.0));
        p.setPen(QPen(fr, 3.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(rc.adjusted(-1.5,-1.5,1.5,1.5), r+2, r+2);
    }

    QPainterPath clip; clip.addRoundedRect(rc, r, r);
    p.save();
    p.setClipPath(clip);
    p.fillRect(rc, th.bgSidebar);   // warm ground, never black

    // 0.53.0 — our OWN screen on the stage (self-view) renders from the local
    // capture preview (m_selfScreenFrame), not a per-participant screen provider
    // (self has none). A peer's screen still comes from m_scrFrame.
    // Hall-of-mirrors guard: a self MONITOR share on the stage must NOT render
    // the live capture (WGC re-captures TalQ showing it → feedback freeze). Draw
    // a static placeholder instead (window shares are safe — they capture a
    // specific window, not the screen showing TalQ).
    // Hall-of-mirrors guard — but ONLY when the call window sits on the very
    // monitor we're sharing (WGC re-captures TalQ showing the share → feedback
    // freeze). If the window has been dragged to a DIFFERENT display, the live
    // self-share is safe AND wanted, so render it. Re-checked every repaint (the
    // capture keeps delivering frames), so moving the window across displays
    // flips it live. Unknown / out-of-range share index → keep the placeholder.
    bool selfMonitorShare =
        cp->isSelf() && t.isScreen && !m_call->screenShareIsWindow();
    if (selfMonitorShare) {
        const QList<QScreen *> scrs = QApplication::screens();
        const int shareIdx = m_call->shareMonitorIndex();
        QWidget *topWin = window();
        QScreen *winScreen = topWin ? topWin->screen() : nullptr;
        if (winScreen && shareIdx >= 0 && shareIdx < scrs.size()
            && scrs[shareIdx] != winScreen)
            selfMonitorShare = false;   // sharing a DIFFERENT monitor → show live
    }
    const QImage &frame = t.isScreen
        ? (cp->isSelf() ? m_selfScreenFrame : m_scrFrame.value(cp))
        : m_camFrame.value(cp);
    const bool showVideo = !frame.isNull()
        && (t.isScreen || (!cp->videoMuted()))
        && !selfMonitorShare;
    if (showVideo) {
        QSize sc = frame.size().scaled(rc.size().toSize(),
                       t.isScreen ? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding);
        QRectF dst(rc.center().x()-sc.width()/2.0, rc.center().y()-sc.height()/2.0,
                   sc.width(), sc.height());
        if (cp->isSelf() && !t.isScreen) {           // mirror own camera
            p.save(); p.translate(dst.center());
            p.scale(-1, 1); p.translate(-dst.center());
            p.drawImage(dst, frame); p.restore();
        } else {
            p.drawImage(dst, frame);
        }
    } else {
        int d = int(qMin(rc.width(), rc.height()) * (large ? 0.30 : 0.42));
        d = qBound(40, d, 132);
        p.drawImage(QPointF(rc.center().x()-d/2.0, rc.center().y()-d/2.0),
                    avatarDisc(cp->sessionId(), cp->displayName(), d, th));

        // No silent black: always say WHY there's no picture. The
        // Reconnecting/Failed scrim below owns those states; this caption
        // covers the normal ones (camera starting / off / waiting,
        // screen share starting).
        const bool reconn = cp->connState() == CallParticipant::Reconnecting
                         || cp->connState() == CallParticipant::Failed;
        if (!reconn) {
            QString cap;
            if (selfMonitorShare) {
                // Deliberate placeholder (not "starting"): we're actively sharing
                // this monitor but must not render the live recapture here.
                cap = tr("You're sharing this screen");
            } else if (t.isScreen) {
                // Provider exists (cp->screen()) but no frame yet → the
                // share is being negotiated. Tell the viewer instead of
                // showing a silent avatar tile (#134 UX gap).
                cap = cp->isSelf() ? tr("Starting screen share…")
                                   : tr("Starting remote screen share…");
            } else if (cp->isSelf() && m_call->isCameraUnavailable()) {
                // Idiot-proofing: never sit silently on "Starting camera...".
                // The device couldn't open (missing / busy / OS-blocked); the
                // top banner carries the full how-to-fix text, this is the
                // tile-level echo.
                cap = tr("Camera unavailable");
            } else if (cp->isSelf()) {
                cap = m_call->isCameraOn() ? tr("Starting camera…")
                                           : tr("Camera off");
            } else if (cp->videoMuted()) {
                cap = tr("Camera off");
            } else if (cp->connState() == CallParticipant::Connecting
                       && m_call->state() != CallManager::Active) {
                // Only say "Connecting" while the CALL is still being set up.
                // Once it's established, a peer with no video yet is "waiting
                // for video", never "connecting" -- the call is already up.
                cap = tr("Connecting…");
            } else {
                cap = tr("Waiting for video…");
            }
            QFont cf = th.systemFont();
            p.setFont(cf);
            p.setPen(th.textSecondary);
            QRectF capR(rc.left()+8,
                        rc.center().y() + d/2.0 + 10,
                        rc.width()-16, 20);
            p.drawText(capR, Qt::AlignHCenter|Qt::AlignTop,
                       QFontMetrics(cf).elidedText(cap, Qt::ElideRight,
                                                   int(capR.width())));
        }
    }

    // Reconnecting scrim (warm ladder, not a black overlay).
    if (cp->connState() == CallParticipant::Reconnecting
        || cp->connState() == CallParticipant::Failed) {
        QColor s = th.bgPrimary; s.setAlphaF(0.55);
        p.fillRect(rc, s);
        p.setPen(th.textSecondary); p.setFont(th.systemFont());
        p.drawText(rc, Qt::AlignCenter,
            cp->connState() == CallParticipant::Failed ? tr("Connection lost")
                                                       : tr("Reconnecting…"));
    }
    p.restore();

    // name plate + mute + connection LED
    QString label = cp->isSelf() ? tr("You") : cp->displayName();
    if (label.isEmpty()) label = tr("Participant");
    QFont nf = th.systemFont();
    p.setFont(nf);
    QFontMetrics fm(nf);
    int tw = fm.horizontalAdvance(label);
    // Live mic meter sits in the plate (legacy feature: see your mic work).
    // Hidden when muted (the ✕ already says "no audio").
    const bool showMeter = !cp->audioMuted();
    const qreal meterW = showMeter ? 46.0 : 0.0;
    // Per-participant signal-quality bars (Zoom/Teams-style). Remote peers
    // only -- self has no inbound receive stream to measure -- and only once
    // a stats tick has actually classified this peer (signalQuality() < 0
    // means "no data yet", e.g. before the subscriber's first poll lands).
    const bool showBars = !cp->isSelf() && cp->signalQuality() >= 0;
    constexpr qreal kBarW = 3.0, kBarGap = 2.0;
    constexpr qreal kBarsW = 3*kBarW + 2*kBarGap;      // 13
    const qreal barsSlot = showBars ? kBarsW + 6.0 : 0.0;   // + trailing gap to the LED
    QRectF plate(rc.left()+10, rc.bottom()-30,
                 qMin<qreal>(tw+52+meterW+barsSlot, rc.width()-20), 22);
    QColor pb = th.bgPrimary; pb.setAlphaF(0.5);
    p.setBrush(pb); p.setPen(Qt::NoPen);
    p.drawRoundedRect(plate, 7, 7);
    qreal tx = plate.left()+10;
    if (showBars) {
        // 3 bars in an ascending ladder (like a phone signal-strength icon),
        // filled left-to-right proportional to SignalQualityPolicy's
        // committed level (0=Lost..3=Good). Lost draws all 3 bars dim/grey
        // rather than a loud red spike -- it reads as "no signal", matching
        // the greyed-out Reconnecting/Failed scrim semantics elsewhere in
        // this function, not an alarming error state.
        const int lvl = cp->signalQuality();
        const QColor barColor = lvl >= 3 ? th.success
                               : lvl == 2 ? th.amber
                               : lvl == 1 ? th.danger
                               : th.textSecondary;
        const qreal baseY = plate.center().y() + 5.0;
        p.setPen(Qt::NoPen);
        for (int b = 0; b < 3; ++b) {
            const qreal h = 4.0 + b*3.0;   // 4 / 7 / 10 px tall ascending ladder
            QRectF bar(tx + b*(kBarW+kBarGap), baseY - h, kBarW, h);
            QColor c = barColor;
            if (b >= lvl) c.setAlphaF(lvl == 0 ? 0.30 : 0.22);  // unfilled bar
            p.setBrush(c);
            p.drawRoundedRect(bar, 1, 1);
        }
        tx += kBarsW + 6.0;
    }
    // LED
    QColor led = cp->connState()==CallParticipant::Connected ? th.online
               : cp->connState()==CallParticipant::Failed    ? th.danger
               : th.amber;
    p.setBrush(led); p.drawEllipse(QRectF(tx, plate.center().y()-3, 6, 6));
    tx += 13;
    if (cp->audioMuted()) {
        p.setPen(th.danger);
        p.drawText(QRectF(tx, plate.top(), 16, plate.height()),
                   Qt::AlignVCenter|Qt::AlignLeft, QStringLiteral("✕"));
        tx += 16;
    }
    p.setPen(th.textPrimary);
    p.drawText(QRectF(tx, plate.top(),
                      plate.right()-tx-6-meterW, plate.height()),
               Qt::AlignVCenter|Qt::AlignLeft, fm.elidedText(label, Qt::ElideRight,
               int(plate.right()-tx-6-meterW)));

    if (showMeter) {
        // Fast attack, slow decay → reads like a real VU; perceptual curve
        // so quiet speech is still visible. Data, so shown even in
        // reduced-motion (no decoration, just the level).
        qreal target = qPow(qBound(0.0, cp->audioLevel(), 1.0), 0.6);
        qreal &s = m_micLvl[cp];
        s += (target - s) * (target > s ? 0.6 : 0.16);
        const QRectF track(plate.right()-meterW, plate.center().y()-2.5,
                           meterW-10, 5);
        QColor tb = th.textPrimary; tb.setAlphaF(0.18);
        p.setBrush(tb); p.setPen(Qt::NoPen);
        p.drawRoundedRect(track, 2.5, 2.5);
        if (s > 0.02) {
            QRectF fillR(track.left(), track.top(),
                         track.width() * qBound(0.0, s, 1.0), track.height());
            // Mic level stays GREEN (a VU meter reads as "audio present/healthy").
            // The "who is speaking" cue lives on the active-speaker FRAME (amber),
            // not the meter — colouring the meter amber too was redundant and
            // read as a warning. Frame = identity, meter = level.
            p.setBrush(th.online);
            p.drawRoundedRect(fillR, 2.5, 2.5);
        }
    }

    if (t.isScreen) {
        p.setPen(th.accent); p.setFont(th.timeFont());
        p.drawText(rc.adjusted(12, 8, -12, 0), Qt::AlignTop|Qt::AlignLeft,
                   tr("%1 is sharing").arg(cp->isSelf() ? tr("You") : cp->displayName()));
    }
}

void CallStage::paintCentered(QPainter &p, const PainterTheme &th)
{
    const auto state = m_call->state();
    QString name = m_call->remotePeerName();
    if (name.isEmpty()) name = tr("Call");
    const QString id = m_call->remotePeerId().isEmpty() ? name : m_call->remotePeerId();
    int d = 112;
    p.drawImage(QPointF(width()/2.0-d/2.0, height()/2.0-d-30),
                avatarDisc(id, name, d, th));

    p.setPen(th.textPrimary);
    QFont nf = th.nameFont(); nf.setPixelSize(th.fontSizeLarge+4); nf.setWeight(QFont::DemiBold);
    p.setFont(nf);
    p.drawText(QRectF(0, height()/2.0-6, width(), 34), Qt::AlignHCenter, name);

    // 0.40.15 — never leak internal statusDetail strings ("Publisher ICE
    // connected", "Fetching servers", "Joining room") into the user-
    // facing sub-line. They were useful when this surface doubled as a
    // dev console, but now the Mission Control telemetry chips + log
    // panel carry diagnostics. Sub-line stays a calm, friendly phrase.
    QString sub = state == CallManager::Incoming ? tr("Incoming call")
                : state == CallManager::Outgoing ? tr("Calling…")
                : state == CallManager::Connecting ? tr("Connecting…")
                                                   : tr("Waiting for others to join");
    p.setPen(th.textSecondary); p.setFont(th.systemFont());
    p.drawText(QRectF(0, height()/2.0+30, width(), 22), Qt::AlignHCenter, sub);

    // Incoming → Accept / Decline. Else → status pill + control bar.
    if (state == CallManager::Incoming) {
        m_buttons.clear();
        const qreal bw = 132, bh = 46, gap = 16, cy = height()/2.0 + 84;
        p.setFont(th.nameFont());
        if (m_call->callHasVideo()) {
            // Video call → let the callee choose: answer WITH video, answer
            // audio-only, or decline. Three buttons centered.
            const qreal total = bw*3 + gap*2;
            qreal x = width()/2.0 - total/2.0;
            QRectF vid(x, cy, bw, bh);             x += bw + gap;
            QRectF aud(x, cy, bw, bh);             x += bw + gap;
            QRectF dec(x, cy, bw, bh);
            p.setBrush(th.accent); p.setPen(Qt::NoPen);
            p.drawRoundedRect(vid, 10, 10);
            p.setPen(th.controlInk); p.drawText(vid, Qt::AlignCenter, tr("Video"));
            p.setBrush(Qt::NoBrush); p.setPen(QPen(th.accent, 1.3));
            p.drawRoundedRect(aud, 10, 10);
            p.setPen(th.accent); p.drawText(aud, Qt::AlignCenter, tr("Audio"));
            p.setBrush(Qt::NoBrush); p.setPen(QPen(th.danger, 1.3));
            p.drawRoundedRect(dec, 10, 10);
            p.setPen(th.danger); p.drawText(dec, Qt::AlignCenter, tr("Decline"));
            m_buttons.push_back({QStringLiteral("accept-video"), vid, {}, tr("Video"), false, false});
            m_buttons.push_back({QStringLiteral("accept"),       aud, {}, tr("Audio"), false, false});
            m_buttons.push_back({QStringLiteral("decline"),      dec, {}, tr("Decline"), false, true});
        } else {
            QRectF acc(width()/2.0-bw-gap/2, cy, bw, bh);
            QRectF dec(width()/2.0+gap/2, cy, bw, bh);
            p.setBrush(th.accent); p.setPen(Qt::NoPen);
            p.drawRoundedRect(acc, 10, 10);
            p.setPen(th.controlInk);
            p.drawText(acc, Qt::AlignCenter, tr("Accept"));
            p.setBrush(Qt::NoBrush); p.setPen(QPen(th.danger, 1.3));
            p.drawRoundedRect(dec, 10, 10);
            p.setPen(th.danger);
            p.drawText(dec, Qt::AlignCenter, tr("Decline"));
            m_buttons.push_back({QStringLiteral("accept"), acc, {}, tr("Accept"), false, false});
            m_buttons.push_back({QStringLiteral("decline"), dec, {}, tr("Decline"), false, true});
        }
        // #13: pre-answer self-preview. CallManager starts a standalone
        // camera→appsink pipeline for incoming VIDEO calls and feeds frames
        // into the self participant's camera provider. Paint it as the PiP
        // (same rect the media-phase PiP uses) so the callee can see
        // themselves before answering.
        if (auto *self = m_call->selfParticipant();
            self && self->camera() && !m_pipRect.isNull()) {
            Tile s; s.p = self; s.rect = m_pipRect;
            paintTile(p, s, th, false);
        }
    } else {
        // Self-preview PiP shown immediately while calling/connecting, in
        // the exact corner it keeps once connected (no jump on transition).
        // Hidden entirely if the camera is off — on the calling screen an
        // empty "Camera off" box is just noise, so toggling cam off here
        // dismisses the PiP (the ~30fps tick repaints within a frame).
        if (m_call->isCameraOn() && !m_pipRect.isNull()
            && m_call->selfParticipant()) {
            Tile s; s.p = m_call->selfParticipant(); s.rect = m_pipRect;
            paintTile(p, s, th, false);
        }
        paintStatusPill(p, th);
        // Top info chips also belong here so the user can read the
        // negotiated codec / quality / RX as soon as media starts to
        // arrive — even before the stage flips into the multi-tile
        // mediaPhase layout. They share the chromeAlpha fade with the
        // control bar so they auto-hide together.
        if (m_chromeAlpha > 1e-3) {
            p.save();
            p.setOpacity(m_chromeAlpha);
            paintInfoPills(p, th);
            paintControlBar(p, th);
            p.restore();
        }
    }
}

void CallStage::buildButtons()
{
    m_buttons.clear();
    if (m_call->state() == CallManager::Incoming) return;

    // Only surface controls that mean something *right now*. Before the
    // call connects there is nothing to share into, no telemetry yet, and
    // no roster; the roster is also pointless in a 1:1/P2P call (it'd just
    // list you and the one callee). mic/cam/fullscreen/hang-up always apply.
    const bool active = m_call->state() == CallManager::Active;
    int remotes = 0;
    for (auto *q : m_call->participants()) if (!q->isSelf()) ++remotes;
    const bool group = remotes >= 2;

    QStringList ctl = {"mic", "cam"};
    if (active)          ctl << "share" << "telemetry";
    if (active && group) ctl << "roster";
    ctl << "full";

    // Don't strand an open panel for a control we just hid (e.g. a group
    // call shrinking to 1:1 with the roster still toggled open).
    if (!ctl.contains("telemetry")) m_telemetryOpen = false;
    if (!ctl.contains("roster"))    m_rosterOpen = false;

    // Segmented-pill layout: the applicable controls share one continuous
    // strip; hang-up is a detached red pill set apart so it can't be
    // misclicked. Geometry derives from ctl.size() so the pill re-centres
    // as controls appear/disappear across call phases.
    const qreal cellW = 56, cellH = 50, pad = 8, endW = 58, gap = 14;
    const qreal pillW  = ctl.size()*cellW + 2*pad;
    const qreal total  = pillW + gap + endW;
    qreal x = (width()-total)/2.0;
    const qreal y = height()-cellH-26;

    auto add = [&](const QString &id, const QRectF &r){
        Btn b; b.id = id; b.rect = r;
        if (id=="mic")        { b.on = !m_call->isMuted();
                                b.tip = b.on ? tr("Mute") : tr("Unmute"); }
        else if (id=="cam")   { b.on = m_call->isCameraOn();
                                b.tip = b.on ? tr("Turn camera off") : tr("Turn camera on"); }
        else if (id=="share") { b.on = m_call->isScreenSharing();
                                b.tip = b.on ? tr("Stop sharing") : tr("Share screen"); }
        else if (id=="telemetry"){ b.on = m_telemetryOpen;
                                b.tip = b.on ? tr("Hide telemetry") : tr("Telemetry"); }
        else if (id=="roster"){ b.on = m_rosterOpen;
                                b.tip = b.on ? tr("Hide participants") : tr("Participants"); }
        else if (id=="full")  { b.tip = tr("Fullscreen"); }
        else if (id=="end")   { b.danger = true; b.tip = tr("Leave call"); }
        m_buttons << b;
    };

    qreal cx = x + pad;
    for (const QString &id : ctl) { add(id, QRectF(cx, y, cellW, cellH)); cx += cellW; }
    add("end", QRectF(x + pillW + gap, y, endW, cellH));
}

void CallStage::paintControlBar(QPainter &p, const PainterTheme &th)
{
    if (m_buttons.isEmpty()) return;
    p.setRenderHint(QPainter::Antialiasing, true);

    auto mix = [](const QColor &a, const QColor &b, qreal t) {
        return QColor::fromRgbF(a.redF()  +(b.redF()  -a.redF())  *t,
                                a.greenF()+(b.greenF()-a.greenF())*t,
                                a.blueF() +(b.blueF() -a.blueF()) *t);
    };
    const QColor pillSolid = mix(th.bgSecondary, th.textPrimary, 0.0); // = bgSecondary
    const QColor clayChip  = mix(th.bgSecondary, th.danger, 0.22);
    const QColor clayInk   = th.danger.lighter(135);
    const QColor grnChip   = mix(th.bgSecondary, th.accent, 0.20);
    const QColor grnInk    = th.accent.lighter(122);

    // ── the segmented strip (all non-danger cells) ──
    QRectF pill;
    const Btn *endBtn = nullptr;
    for (const Btn &b : m_buttons) {
        if (b.danger) { endBtn = &b; continue; }
        pill = pill.isNull() ? b.rect : pill.united(b.rect);
    }
    pill = pill.adjusted(-8, -3, 8, 3);
    QColor barBg = th.bgSecondary; barBg.setAlphaF(0.94);
    p.setBrush(barBg); p.setPen(QPen(th.divider, 1));
    p.drawRoundedRect(pill, pill.height()/2.0, pill.height()/2.0);

    int idx = 0, nCtl = m_buttons.size() - (endBtn ? 1 : 0);
    for (const Btn &b : m_buttons) {
        if (b.danger) continue;
        const bool hover = (b.id == m_hoverBtn);
        const bool off   = (b.id=="mic"||b.id=="cam") && !b.on;
        const bool act   = (b.id=="share"||b.id=="telemetry"||b.id=="roster") && b.on;

        QColor chip; QColor ink = th.textPrimary; QColor slashBack = pillSolid;
        if (off)      { chip = clayChip; ink = clayInk; slashBack = clayChip; }
        else if (act) { chip = grnChip;  ink = grnInk; }

        const QRectF chipR = b.rect.adjusted(3, 5, -3, -5);
        if (chip.isValid()) {
            if (hover) chip = chip.lighter(116);
            p.setBrush(chip); p.setPen(Qt::NoPen);
            p.drawRoundedRect(chipR, 12, 12);
        } else if (hover) {
            QColor wash = th.accent; wash.setAlphaF(0.13);
            p.setBrush(wash); p.setPen(Qt::NoPen);
            p.drawRoundedRect(chipR, 12, 12);
        } else if (idx > 0) {
            // subtle separator between two plain cells
            const Btn &prev = m_buttons[idx-1];
            const bool prevPlain = !((prev.id=="mic"||prev.id=="cam")&&!prev.on)
                                && !((prev.id=="share"||prev.id=="telemetry"||prev.id=="roster")&&prev.on)
                                && prev.id != m_hoverBtn;
            if (prevPlain) {
                QColor d = th.divider; d.setAlphaF(0.6);
                p.setPen(QPen(d, 1));
                p.drawLine(QPointF(b.rect.left(), b.rect.top()+12),
                           QPointF(b.rect.left(), b.rect.bottom()-12));
            }
        }
        const qreal isz = qMin(b.rect.width(), b.rect.height()) * 0.46;
        QRectF ib(0, 0, isz, isz); ib.moveCenter(b.rect.center());
        VectorIcons::draw(p, b.id, ib, ink, off, slashBack);
        idx++;
    }

    // ── detached hang-up pill ──
    if (endBtn) {
        const bool hv = (endBtn->id == m_hoverBtn);
        // 0.52.11 — the hang-up pill gets its OWN clear red, not th.danger (which
        // is the palette "clay" and reads orange). A universal end-call red so the
        // button is unmistakable as "leave the call".
        const QColor hangupRed(0xE2, 0x3B, 0x33);
        QColor f = hv ? hangupRed.lighter(112) : hangupRed;
        p.setBrush(f); p.setPen(Qt::NoPen);
        p.drawRoundedRect(endBtn->rect, endBtn->rect.height()/2.0,
                          endBtn->rect.height()/2.0);
        const qreal isz = qMin(endBtn->rect.width(), endBtn->rect.height())*0.5;
        QRectF ib(0,0,isz,isz); ib.moveCenter(endBtn->rect.center());
        VectorIcons::draw(p, "end", ib, th.controlInk, false, f);
    }

    // ── themed tooltip for the hovered control ──
    if (!m_hoverBtn.isEmpty()) {
        for (const Btn &b : m_buttons) {
            if (b.id != m_hoverBtn || b.tip.isEmpty()) continue;
            QFont tf = monoFont(11); p.setFont(tf);
            QFontMetrics fm(tf);
            const qreal tw = fm.horizontalAdvance(b.tip) + 22;
            const qreal th_ = 26;
            qreal tx = b.rect.center().x() - tw/2.0;
            tx = qBound(8.0, tx, width()-tw-8.0);
            const qreal ty = b.rect.top() - th_ - 10;
            QRectF tip(tx, ty, tw, th_);
            QColor bg = th.bgSecondary; bg.setAlphaF(0.98);
            p.setBrush(bg); p.setPen(QPen(th.divider, 1));
            p.drawRoundedRect(tip, 8, 8);
            // little downward pointer toward the button
            qreal px = qBound(tip.left()+12, b.rect.center().x(), tip.right()-12);
            QPainterPath tri;
            tri.moveTo(px-6, tip.bottom());
            tri.lineTo(px+6, tip.bottom());
            tri.lineTo(px,   tip.bottom()+6);
            tri.closeSubpath();
            p.setBrush(bg); p.setPen(Qt::NoPen); p.drawPath(tri);
            p.setPen(th.textPrimary);
            p.drawText(tip, Qt::AlignCenter, b.tip);
            break;
        }
    }
}

void CallStage::paintStatusPill(QPainter &p, const PainterTheme &th)
{
    // Aggregate state → the one signal colour. Whole-call Reconnecting (our
    // publisher media path being rebuilt) OR any per-peer subscriber reconnect
    // both light the pill red with "RECONNECTING…".
    const auto st = m_call->state();
    bool reconnecting = (st == CallManager::Reconnecting), degraded = false;
    for (auto *cp : m_call->participants()) {
        if (cp->isSelf()) continue;
        if (cp->connState()==CallParticipant::Reconnecting
            || cp->connState()==CallParticipant::Failed) reconnecting = true;
        // Once the call is established, a peer's "Connecting" substream state
        // must not paint the pill amber -- the call IS up. Only pre-Active
        // states drive the connecting/degraded indicator.
        if (cp->connState()==CallParticipant::Connecting
            && st != CallManager::Active) degraded = true;
    }
    QColor dot = reconnecting ? th.danger
               : (degraded || st==CallManager::Connecting || st==CallManager::Outgoing)
                 ? th.amber : th.accent;
    // 0.40.15 — Mission Control lingo. The home's status pill reads
    // "● ALL SYSTEMS NOMINAL"; we mirror that pattern here so walking
    // sidebar → call doesn't change visual vocabulary. "LIVE" becomes
    // "IN CALL · mm:ss" (noun phrasing + bullet separator), and the
    // transient states get a trailing ellipsis to signal motion.
    QString word = reconnecting ? tr("RECONNECTING…")
                 : st==CallManager::Outgoing ? tr("CALLING…")
                 : st==CallManager::Connecting ? tr("CONNECTING…")
                 : tr("IN CALL");
    QString dur = st==CallManager::Active
                    ? QStringLiteral("  ·  ")+fmtDuration(m_call->callDuration())
                    : QString();
    QString text = word + dur;

    // 0.41.5-beta — bumped 9 → 11 px + 20 → 26 px height for 2K
    // legibility. Plus the LED gets a contrasting dark ring + a
    // never-fully-transparent floor so it stays visible on light
    // backgrounds (was washing out during the breathing pulse).
    QFont f = monoFont(11); f.setBold(true);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
    p.setFont(f);
    QFontMetrics fm(f);
    QRectF pill(16, 14, fm.horizontalAdvance(text) + 38, 26);

    // Card-style background (semi-opaque bg-surface) so the pill stays
    // readable when the call surface behind it is bright (1.0 video).
    QColor face = th.bgSurface; face.setAlphaF(0.88);
    QColor border = dot; border.setAlphaF(0.85);
    p.setBrush(face);
    p.setPen(QPen(border, 1.2));
    p.drawRoundedRect(pill, 13, 13);

    qreal pulse = reducedMotion() ? 0.85 : qMax(0.55, 0.55 + 0.45 * qSin(m_glowPhase));
    QColor d = dot; d.setAlphaF(pulse);
    const QRectF ledRect(pill.left() + 11, pill.center().y() - 3.5, 7, 7);
    // Dark contrast ring so the LED reads against light/white video.
    QColor ledRing = th.controlInk; ledRing.setAlphaF(0.7);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(ledRing, 1.0));
    p.drawEllipse(ledRect.adjusted(-0.5, -0.5, 0.5, 0.5));
    p.setBrush(d); p.setPen(Qt::NoPen);
    p.drawEllipse(ledRect);
    p.setPen(dot);
    p.drawText(pill.adjusted(25, 0, -12, 0), Qt::AlignVCenter|Qt::AlignLeft, text);

    m_statusPillRect = pill;
    m_topChromeRects.append(pill);
}

void CallStage::paintCameraBanner(QPainter &p, const PainterTheme &th)
{
    // Idiot-proofing: when our own camera can't be opened we must NOT fail
    // silently. A persistent, plain-language banner near the top tells the
    // user their camera isn't being sent and exactly how to fix it. No
    // jargon -- the people running TalQ may not know what a "capture device"
    // is. Drawn every frame this state is live (not gated by chrome fade).
    if (!m_call || !m_call->isCameraUnavailable()) return;

    const QString title = tr("Your camera isn't available");
    const QString hint  = tr("Others can't see you. Close any app that might "
                             "be using the camera, or allow camera access in "
                             "Windows Settings > Privacy > Camera, then turn "
                             "your camera off and on again.");

    QFont tf = th.systemFont(); tf.setBold(true);
    QFont hf = th.systemFont();
    QFontMetrics tfm(tf), hfm(hf);

    const qreal pad   = 14.0;
    const qreal bw    = qMin<qreal>(width() - 40.0, 560.0);
    const qreal textX = pad + 6.0;          // +6 clears the warning stripe
    const qreal textW = bw - textX - pad;
    const QRect hintR = hfm.boundingRect(QRect(0, 0, int(textW), 1000),
                                         Qt::TextWordWrap, hint);
    const qreal bh = pad + tfm.height() + 4.0 + hintR.height() + pad;
    const qreal bx = (width() - bw) / 2.0;
    const qreal by = 52.0;                   // below the top status/info pills
    const QRectF banner(bx, by, bw, bh);

    QColor face = th.bgSurface; face.setAlphaF(0.96);
    QColor edge = th.amber;     edge.setAlphaF(0.90);
    p.setBrush(face);
    p.setPen(QPen(edge, 1.4));
    p.drawRoundedRect(banner, 12, 12);

    // Left warning stripe so it reads as "attention" at a glance.
    QPainterPath clip; clip.addRoundedRect(banner, 12, 12);
    p.save();
    p.setClipPath(clip);
    QColor stripe = th.amber; stripe.setAlphaF(0.92);
    p.fillRect(QRectF(banner.left(), banner.top(), 4.0, banner.height()), stripe);
    p.restore();

    p.setFont(tf); p.setPen(th.textPrimary);
    p.drawText(QRectF(banner.left()+textX, banner.top()+pad, textW, tfm.height()),
               Qt::AlignLeft|Qt::AlignVCenter, title);
    p.setFont(hf); p.setPen(th.textSecondary);
    p.drawText(QRectF(banner.left()+textX, banner.top()+pad+tfm.height()+4.0,
                      textW, hintR.height()),
               Qt::TextWordWrap, hint);
}

void CallStage::paintMicBanner(QPainter &p, const PainterTheme &th)
{
    // Idiot-proofing twin of paintCameraBanner: when our microphone can't be
    // opened the publisher falls back to silent audio so the call survives —
    // but the user must be TOLD, in plain language, that nobody can hear them
    // and how to fix it. If the camera banner is also up, this stacks beneath
    // it instead of overlapping.
    if (!m_call || !m_call->isMicUnavailable()) return;

    const QString title = tr("Your microphone isn't available");
    const QString hint  = tr("Others can't hear you. Close any app that might "
                             "be using the microphone, or allow microphone "
                             "access in Windows Settings > Privacy > "
                             "Microphone, then end and rejoin the call.");

    QFont tf = th.systemFont(); tf.setBold(true);
    QFont hf = th.systemFont();
    QFontMetrics tfm(tf), hfm(hf);

    const qreal pad   = 14.0;
    const qreal bw    = qMin<qreal>(width() - 40.0, 560.0);
    const qreal textX = pad + 6.0;          // +6 clears the warning stripe
    const qreal textW = bw - textX - pad;
    const QRect hintR = hfm.boundingRect(QRect(0, 0, int(textW), 1000),
                                         Qt::TextWordWrap, hint);
    const qreal bh = pad + tfm.height() + 4.0 + hintR.height() + pad;
    const qreal bx = (width() - bw) / 2.0;
    // Sit below the camera banner when both failures are live (rare but real
    // on a machine with neither device available); otherwise take the same
    // top slot the camera banner uses.
    const qreal by = m_call->isCameraUnavailable() ? 52.0 + bh + 10.0 : 52.0;
    const QRectF banner(bx, by, bw, bh);

    QColor face = th.bgSurface; face.setAlphaF(0.96);
    QColor edge = th.amber;     edge.setAlphaF(0.90);
    p.setBrush(face);
    p.setPen(QPen(edge, 1.4));
    p.drawRoundedRect(banner, 12, 12);

    QPainterPath clip; clip.addRoundedRect(banner, 12, 12);
    p.save();
    p.setClipPath(clip);
    QColor stripe = th.amber; stripe.setAlphaF(0.92);
    p.fillRect(QRectF(banner.left(), banner.top(), 4.0, banner.height()), stripe);
    p.restore();

    p.setFont(tf); p.setPen(th.textPrimary);
    p.drawText(QRectF(banner.left()+textX, banner.top()+pad, textW, tfm.height()),
               Qt::AlignLeft|Qt::AlignVCenter, title);
    p.setFont(hf); p.setPen(th.textSecondary);
    p.drawText(QRectF(banner.left()+textX, banner.top()+pad+tfm.height()+4.0,
                      textW, hintR.height()),
               Qt::TextWordWrap, hint);
}

// 0.40.15 — the top chrome is split into two distinct surfaces.
//
//   INFO pills (left, after the status pill): codec/HW-SW, live quality
//   stat, RX resolution. Quiet telemetry, no interaction.
//
//   ACTION pills (top-right): Quality override + Background mode. Click
//   opens a dropdown; hover surfaces a native tooltip.
void CallStage::paintQualityNotice(QPainter &p, const PainterTheme &th)
{
    // 0.52.5 — persistent sender-visible chip: our OWN camera SEND quality is
    // reduced (software encoding, or shed to the 480p floor under load). The
    // sender must SEE why their video is limited — the field bug was a silent
    // collapse to 180p with no cue. Compact amber pill at top-center; stacks
    // below the camera/mic failure banners on the rare both-at-once case.
    if (!m_call || m_call->videoQualityNotice().isEmpty()) return;
    const QString text = m_call->videoQualityNotice();

    QFont f = th.systemFont(); f.setBold(true);
    p.setFont(f);
    QFontMetrics fm(f);
    const qreal padH = 14.0, dot = 8.0, gap = 8.0, h = 30.0;
    const qreal w = padH + dot + gap + fm.horizontalAdvance(text) + padH;
    qreal y = 52.0;                                   // top slot, like the banners
    if (m_call->isCameraUnavailable()) y += 78.0;     // stack below a camera banner
    if (m_call->isMicUnavailable())    y += 78.0;     // and/or a mic banner
    const QRectF pill((width() - w) / 2.0, y, w, h);

    QColor face = th.bgSurface; face.setAlphaF(0.96);
    QColor edge = th.amber;     edge.setAlphaF(0.85);
    p.setBrush(face); p.setPen(QPen(edge, 1.4));
    p.drawRoundedRect(pill, 15, 15);
    p.setBrush(th.amber); p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(pill.left() + padH, pill.center().y() - dot / 2.0, dot, dot));
    p.setPen(th.textPrimary);
    p.drawText(pill.adjusted(padH + dot + gap, 0, -padH, 0),
               Qt::AlignVCenter | Qt::AlignLeft, text);
}

void CallStage::paintInfoPills(QPainter &p, const PainterTheme &th)
{
    // 0.40.15 — Mission Control telemetry tile vocabulary, laid out on
    // the same row as the status pill so the full "what's happening"
    // line reads left-to-right:
    //   status · CODEC · QUALITY · RX
    // Per tile: [ ●led · KEY (mono caption, textTime) · VAL (mono bold) ]
    const QString enc = m_call->activeVideoEncoder();
    if (enc.isEmpty()) return;

    // 0.41.5-beta — chips bumped to status-pill scale (26 h, 9/11 mono)
    // for 1440p legibility. Field report: 7/9 px was unreadable on 2K
    // panels at default Windows scaling.
    QFont keyF = monoFont(9);  keyF.setBold(true);
    keyF.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    QFont valF = monoFont(11); valF.setBold(true);
    QFontMetrics keyFm(keyF), valFm(valF);

    const qreal padL     = 11.0;
    const qreal padR     = 12.0;
    const qreal dotW     = 7.0;
    const qreal dotGap   = 8.0;
    const qreal keyValGap = 10.0;
    const qreal tileH    = 26.0;
    const qreal radius   = 11.0;
    const qreal gap      = 8.0;
    QColor face = th.bgSurface; face.setAlphaF(0.88);

    qreal x = m_statusPillRect.right() + gap;
    qreal y = m_statusPillRect.top();
    bool  onRow0 = true;                              // row shared with status/actions
    const qreal rowStartX = m_statusPillRect.left();  // wrapped rows left-justify here
    const qreal rowGap    = 6.0;

    auto drawTile = [&](const QString &key, const QString &val,
                        const QColor &led) {
        const qreal w = padL + dotW + dotGap
                      + keyFm.horizontalAdvance(key) + keyValGap
                      + valFm.horizontalAdvance(val) + padR;
        // Wrap when this tile would overlap the action-button block (row 0,
        // boundary = m_actionPillsLeft) or run off the right edge (wrapped rows
        // own the full width). A row's FIRST tile never wraps, so a tile wider
        // than the row can't loop forever. The FIRST wrap drops BELOW the whole
        // anchor block (m_chromeRowsBottom) so info never lands on top of the
        // action buttons — even when they were dropped to their own row.
        const qreal rowRight = onRow0 ? (m_actionPillsLeft - gap)
                                      : (width() - 16.0);
        if (x > rowStartX + 0.5 && x + w > rowRight) {
            y = onRow0 ? (m_chromeRowsBottom + rowGap) : (y + tileH + rowGap);
            x = rowStartX;
            onRow0 = false;
        }
        QRectF tile(x, y, w, tileH);
        p.setBrush(face);
        p.setPen(QPen(th.divider, 1.0));
        p.drawRoundedRect(tile, radius, radius);

        p.setBrush(led); p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(tile.left() + padL,
                             tile.center().y() - dotW/2.0,
                             dotW, dotW));

        qreal cx = tile.left() + padL + dotW + dotGap;
        p.setFont(keyF);
        p.setPen(th.textTime);
        p.drawText(QRectF(cx, tile.top(),
                          keyFm.horizontalAdvance(key), tile.height()),
                   Qt::AlignVCenter | Qt::AlignLeft, key);
        cx += keyFm.horizontalAdvance(key) + keyValGap;
        p.setFont(valF);
        p.setPen(th.textPrimary);
        p.drawText(QRectF(cx, tile.top(),
                          valFm.horizontalAdvance(val), tile.height()),
                   Qt::AlignVCenter | Qt::AlignLeft, val);
        x = tile.right() + gap;
        m_infoPillsBottom = qMax(m_infoPillsBottom, tile.bottom());
        m_topChromeRects.append(tile);
    };

    const bool    hw    = m_call->activeVideoEncoderIsHw();
    const QString codec = enc.section(QStringLiteral(" · "), 0, 0);
    const QString val1  = codec + (hw ? QStringLiteral(" · HW")
                                       : QStringLiteral(" · SW"));
    drawTile(QStringLiteral("CODEC"), val1, hw ? th.success : th.amber);

    // Call MODE pill: media is forwarded through the MCU (SFU).
    drawTile(QStringLiteral("MODE"), QStringLiteral("MCU"), th.amber);

    // QUALITY chip: live readout of the substream the primary remote is
    // forwarding. Distinct from the QUALITY DROPDOWN — the dropdown is
    // what we REQUEST, this chip is what we GET. They agree when the
    // SFU honours the request; they diverge when the SFU adapts down,
    // which is exactly the moment a stats chip earns its place.
    int sub = -1;
    if (m_qualityOverride >= 0) {
        sub = m_qualityOverride;
    } else {
        QString bestSid; qreal bestH = 0; bool bestIsStage = false;
        for (const Tile &t : m_tiles) {
            if (!t.p || t.p->isSelf() || t.isScreen) continue;
            if (t.isStage) { bestIsStage = true; bestH = t.rect.height(); bestSid = t.p->sessionId(); break; }
            if (t.rect.height() > bestH) { bestH = t.rect.height(); bestSid = t.p->sessionId(); }
        }
        // Reflect what we ACTUALLY requested for the primary tile (the hysteretic
        // value stored in updateStreamQualities), not a fresh non-hysteretic
        // recompute — otherwise inside the drop-hold band the chip would read e.g.
        // MED while we are still requesting (and the SFU forwarding) HIGH.
        if (!bestSid.isEmpty())
            sub = m_tileSubstream.value(bestSid, pickSubstream(-1, bestH, bestIsStage));
    }
    // #76 -- during a remote screen share the sharer pins their camera to the
    // LOW simulcast layer, so the camera-substream QUALITY pill would read a
    // misleading "LOW". Show the SHARE's exact received resolution instead —
    // that's the content the viewer is actually watching.
    const QString shareRes = m_call->hasRemoteScreen() ? m_call->remoteScreenResolutionLabel()
                                                       : QString();
    if (!shareRes.isEmpty()) {
        drawTile(QStringLiteral("SHARE Q"), shareRes, th.success);
    } else if (sub >= 0 && sub <= 2) {
        static const char *const kQualityLabels[] = { "LOW", "MED", "HIGH" };
        const QColor qLed = (sub == 2) ? th.success
                           : (sub == 1) ? th.amber : th.danger;
        drawTile(QStringLiteral("QUALITY"),
                 QString::fromLatin1(kQualityLabels[sub]), qLed);
    }

    const QString rx = m_call->activeRxResolution();
    if (!rx.isEmpty()) {
        drawTile(QStringLiteral("RX"), rx, th.textTime);
    }
}

QString CallStage::highQualityLabel() const
{
    // The receive-quality dropdown controls which of the REMOTE's simulcast
    // layers the SFU forwards, so its HIGH label must reflect the REMOTE's top
    // layer — not our own "Maximum send resolution" (the old bug: a 1080p local
    // setting showed "High (1080p)" even when the peer capped at 720p / shed
    // HIGH). Bucket the peak height we've actually decoded from the peer.
    const int h = m_call ? m_call->peerPeakRxHeight() : 0;
    if (h <= 0)    return tr("High");          // not observed yet — claim nothing
    if (h >= 1800) return tr("High (4K)");
    if (h >= 1260) return tr("High (2K)");
    if (h >= 900)  return tr("High (1080p)");
    if (h >= 630)  return tr("High (720p)");
    if (h >= 450)  return tr("High (540p)");
    if (h >= 270)  return tr("High (360p)");
    return tr("High (180p)");
}

void CallStage::computeActionPillGeometry()
{
    // GEOMETRY ONLY (no painting) for the top-right action buttons
    // (QUALITY / BACKGROUND / SHARE), so the click hit-rects are valid even when
    // the chrome is faded out (paintActionPills is skipped at alpha~0). Without
    // this, the quality-dropdown click hit a NULL rect and the menu never opened
    // (field: "the dropdown did nothing"). paintActionPills calls this then draws
    // into the SAME member rects, so the drawn buttons and the hit-rects can't
    // drift. Keep the constants/labels in lockstep with paintActionPills.
    const QString highLabel = highQualityLabel();
    const QString kLowLabel = QStringLiteral("Low (180p)");
    const QString kMedLabel = QStringLiteral("Medium (360p)");
    const QString qVal = (m_qualityOverride < 0)
        ? tr("AUTO")
        : (m_qualityOverride == 0 ? kLowLabel
           : m_qualityOverride == 1 ? kMedLabel
           : highLabel).toUpper();

    QSettings bgSet("TalQ", "TalQ");
    bgSet.beginGroup("Talk/Backgrounds");
    const bool    bgOn   = bgSet.value("virtualBackgroundEnabled", false).toBool();
    const QString bgType = bgSet.value("virtualBackgroundType", "blur").toString();
    bgSet.endGroup();
    const QString bgVal = !bgOn
        ? tr("OFF")
        : (bgType == QLatin1String("image") ? tr("IMAGE") : tr("BLUR"));

    QFont keyF = monoFont(9);  keyF.setBold(true);
    keyF.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    QFont valF = monoFont(11); valF.setBold(true);
    QFontMetrics keyFm(keyF), valFm(valF);

    const qreal padL = 11.0, padR = 10.0, keyValGap = 10.0;
    const qreal caretW = 9.0, caretGap = 7.0, btnH = 26.0, gap = 8.0;

    const QString qKey  = QStringLiteral("QUALITY");
    const QString bgKey = QStringLiteral("BACKGROUND");
    const QString shKey = QStringLiteral("SHARE");

    auto btnWidth = [&](const QString &key, const QString &val) {
        return padL + keyFm.horizontalAdvance(key) + keyValGap
             + valFm.horizontalAdvance(val) + caretGap + caretW + padR;
    };

    const bool showShare = m_call->isScreenSharing();
    static const char *const kShLabels[] = { "720P", "1080P", "1440P", "NATIVE" };
    QString shVal;
    if (showShare) {
        const int lv = qBound(0, m_call->screenShareQuality(), 3);
        shVal = QString::fromLatin1(kShLabels[lv]);
    }

    const qreal qW  = btnWidth(qKey,  qVal);
    const qreal bgW = btnWidth(bgKey, bgVal);
    const qreal shW = showShare ? btnWidth(shKey, shVal) : 0.0;
    const qreal rowRight = width() - 16.0;
    const qreal rowTop   = 14.0;
    QRectF bgBtn(rowRight - bgW,          rowTop, bgW, btnH);
    QRectF qBtn (bgBtn.left() - gap - qW, rowTop, qW,  btnH);
    QRectF shBtn;
    if (showShare) shBtn = QRectF(qBtn.left() - gap - shW, rowTop, shW, btnH);

    // Narrow-window guard: if the right-anchored button block would overlap the
    // left-anchored status pill, drop the WHOLE block to its own row under the
    // status pill (m_statusPillRect is current within a paint frame; in relayout/
    // click it's last-frame's, fine for hit-testing).
    const qreal blockLeft0  = showShare ? shBtn.left() : qBtn.left();
    const qreal statusRight = m_statusPillRect.isValid() ? m_statusPillRect.right() : 0.0;
    bool actionsDropped = false;
    if (blockLeft0 < statusRight + gap) {
        const qreal newTop = (m_statusPillRect.isValid() ? m_statusPillRect.bottom()
                                                          : 40.0) + 6.0;
        const qreal dy = newTop - rowTop;
        bgBtn.translate(0, dy);
        qBtn.translate(0, dy);
        if (showShare) shBtn.translate(0, dy);
        actionsDropped = true;
    }

    m_actionPillsLeft = actionsDropped ? width()
                                       : (showShare ? shBtn.left() : qBtn.left());
    const qreal statusBottom = m_statusPillRect.isValid() ? m_statusPillRect.bottom() : 40.0;
    const qreal actionBottom = (showShare ? shBtn.bottom() : bgBtn.bottom());
    m_chromeRowsBottom = qMax(statusBottom, actionBottom);

    m_qualityPillRect = qBtn;
    m_bgPillRect      = bgBtn;
    m_sharePillRect   = showShare ? shBtn : QRectF();
}

void CallStage::paintActionPills(QPainter &p, const PainterTheme &th)
{
    // 0.40.15 — action BUTTONS in the top-right; click opens a QMenu. The hit-
    // rects come from computeActionPillGeometry() (also called from mousePressEvent
    // so the dropdown works while chrome is faded); here we just DRAW into them.
    computeActionPillGeometry();

    // Display values — deterministic from the same state computeActionPillGeometry
    // used, so positions never drift from the drawn content.
    const QString highLabel = highQualityLabel();
    const QString kLowLabel = QStringLiteral("Low (180p)");
    const QString kMedLabel = QStringLiteral("Medium (360p)");
    const QString qVal = (m_qualityOverride < 0)
        ? tr("AUTO")
        : (m_qualityOverride == 0 ? kLowLabel
           : m_qualityOverride == 1 ? kMedLabel
           : highLabel).toUpper();
    const bool qActive = (m_qualityOverride >= 0);

    QSettings bgSet("TalQ", "TalQ");
    bgSet.beginGroup("Talk/Backgrounds");
    const bool    bgOn   = bgSet.value("virtualBackgroundEnabled", false).toBool();
    const QString bgType = bgSet.value("virtualBackgroundType", "blur").toString();
    bgSet.endGroup();
    const QString bgVal = !bgOn
        ? tr("OFF")
        : (bgType == QLatin1String("image") ? tr("IMAGE") : tr("BLUR"));

    const bool showShare = m_call->isScreenSharing();
    static const char *const kShLabels[] = { "720P", "1080P", "1440P", "NATIVE" };
    QString shVal;
    if (showShare)
        shVal = QString::fromLatin1(kShLabels[qBound(0, m_call->screenShareQuality(), 3)]);

    QFont keyF = monoFont(9);  keyF.setBold(true);
    keyF.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    QFont valF = monoFont(11); valF.setBold(true);
    QFontMetrics keyFm(keyF), valFm(valF);

    const qreal padL     = 11.0;
    const qreal padR     = 10.0;
    const qreal keyValGap = 10.0;
    const qreal caretW   = 9.0;
    const qreal radius   = 11.0;

    const QString qKey  = QStringLiteral("QUALITY");
    const QString bgKey = QStringLiteral("BACKGROUND");
    const QString shKey = QStringLiteral("SHARE");

    auto drawButton = [&](const QRectF &rect, const QString &key,
                          const QString &val, bool active, bool hovered) {
        // 0.40.15 — hover state is now the accent-coloured border alone
        // (no 1.5-px scale-up). The frame is enough; the micro-zoom was
        // jittery alongside the menu open/close.
        const QRectF r = rect;
        QColor face   = th.bgSurface; face.setAlphaF(active ? 0.95 : 0.88);
        QColor border = hovered ? th.accent
                       : (active ? th.accent : th.divider);
        if (active && !hovered) border.setAlphaF(0.75);
        p.setBrush(face);
        p.setPen(QPen(border, hovered ? 1.6 : 1.3));
        p.drawRoundedRect(r, radius, radius);

        qreal cx = r.left() + padL;
        p.setFont(keyF);
        p.setPen(th.textTime);
        p.drawText(QRectF(cx, r.top(), keyFm.horizontalAdvance(key), r.height()),
                   Qt::AlignVCenter | Qt::AlignLeft, key);
        cx += keyFm.horizontalAdvance(key) + keyValGap;
        p.setFont(valF);
        p.setPen(active || hovered ? th.textPrimary : th.textSecondary);
        p.drawText(QRectF(cx, r.top(), valFm.horizontalAdvance(val), r.height()),
                   Qt::AlignVCenter | Qt::AlignLeft, val);

        const qreal caretCx = r.right() - padR - caretW / 2.0;
        const qreal caretCy = r.center().y();
        QPainterPath caret;
        caret.moveTo(caretCx - caretW/2.0, caretCy - 2);
        caret.lineTo(caretCx + caretW/2.0, caretCy - 2);
        caret.lineTo(caretCx,              caretCy + 3);
        caret.closeSubpath();
        QColor caretC = active || hovered ? th.textPrimary : th.textSecondary;
        p.setBrush(caretC); p.setPen(Qt::NoPen);
        p.drawPath(caret);
    };

    // Draw into the hit-rects computeActionPillGeometry() just set (m_actionPillsLeft
    // and m_chromeRowsBottom were published there too).
    drawButton(m_qualityPillRect, qKey,  qVal,  qActive, m_hoverPill == QStringLiteral("quality"));
    drawButton(m_bgPillRect,      bgKey, bgVal, bgOn,    m_hoverPill == QStringLiteral("bg"));
    if (showShare) {
        drawButton(m_sharePillRect, shKey, shVal, true,
                   m_hoverPill == QStringLiteral("share"));
    }
    m_topChromeRects.append(m_qualityPillRect);
    m_topChromeRects.append(m_bgPillRect);
    if (showShare) m_topChromeRects.append(m_sharePillRect);
}

void CallStage::paintSharingBadge(QPainter &p, const PainterTheme &th)
{
    // Persistent top-center indicator while WE are sharing our screen, so
    // the publisher always knows the share is live (#3 — previously there
    // was no local cue). Pulsing red dot + label; click-to-stop is the
    // existing "share" control-bar toggle.
    if (!m_call->isScreenSharing()) return;

    const QString text = tr("You're sharing your screen");
    QFont f = monoFont(12); f.setBold(true);
    p.setFont(f);
    QFontMetrics fm(f);
    // Chrome around the text inside the pill: 30px on the left (red dot + gap,
    // see the adjusted() draw rect below) + 10px on the right. Add slack on top
    // of that so the inner text rect is WIDER than the glyph advance — a bold
    // font's right-side bearing plus QRectF→device-pixel rounding was clipping
    // the final character ("…your scree" with the n cut off).
    qreal pillW = fm.horizontalAdvance(text) + 30 + 10 + 14;
    // Sit on a clear row BELOW the top-chrome block (status pill, info pills incl.
    // any wrapped rows, and the action buttons) so the badge can't be overlapped
    // by them on a narrow window. m_chromeRowsBottom/m_infoPillsBottom are
    // published earlier this frame by paintActionPills/paintInfoPills.
    const qreal badgeY = qMax(m_chromeRowsBottom, m_infoPillsBottom) + 8.0;
    QRectF pill((width() - pillW) / 2.0, badgeY, pillW, 28);

    QColor bg = th.bgSecondary; bg.setAlphaF(0.92);
    QColor border = th.danger;  border.setAlphaF(0.65);
    p.setBrush(bg); p.setPen(QPen(border, 1.5));
    p.drawRoundedRect(pill, 14, 14);

    // Live red dot.
    p.setBrush(th.danger); p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(pill.left() + 14, pill.center().y() - 4, 8, 8));

    p.setPen(th.textPrimary);
    p.drawText(pill.adjusted(30, 0, -10, 0),
               Qt::AlignVCenter | Qt::AlignLeft, text);
}

void CallStage::paintTelemetry(QPainter &p, const PainterTheme &th)
{
    // "Mission Control" telemetry: a header, a live outbound-bandwidth
    // sparkline gauge, a 2-up grid of metric cards, per-participant
    // subsystem chips, and a compact stats footer — all QPainter, theme-
    // driven, over a semi-transparent ground so the call stays visible.
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal w = qBound(300.0, width()*0.32, 400.0);
    const QRectF panel(width()-w, 0, w, height());
    QColor bg = th.bgSecondary; bg.setAlphaF(0.82);
    p.setBrush(bg); p.setPen(Qt::NoPen);
    p.fillRect(panel, bg);
    p.setPen(QPen(th.divider, 1));
    p.drawLine(panel.topLeft(), panel.bottomLeft());

    const qreal pad = 18.0;
    const qreal x0 = panel.left() + pad;
    const qreal innerW = w - pad*2;
    qreal y = 26;

    // ── Header ──
    {
        qreal pulse = reducedMotion() ? 1.0 : (0.5 + 0.5*qSin(m_glowPhase));
        QColor dotc = th.success; dotc.setAlphaF(pulse);
        p.setBrush(dotc); p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(x0, y-8, 8, 8));
        p.setPen(th.textSecondary); p.setFont(monoFont(12));
        p.drawText(QPointF(x0+16, y), QStringLiteral("MISSION CONTROL"));
        const int secs = m_call->callDuration();
        if (secs > 0) {
            const QString dur = QStringLiteral("%1:%2")
                .arg(secs/60).arg(secs%60, 2, 10, QChar('0'));
            p.setPen(th.textTime);
            p.drawText(QRectF(x0, y-12, innerW, 16), Qt::AlignRight|Qt::AlignVCenter, dur);
        }
        y += 18;
    }

    // ── Bandwidth gauge + sparkline ──
    {
        const qreal cardH = 76;
        QRectF card(x0, y, innerW, cardH);
        QColor cbg = th.bgPrimary; cbg.setAlphaF(0.5);
        p.setBrush(cbg); p.setPen(QPen(th.divider, 1));
        p.drawRoundedRect(card, 10, 10);

        const QString bw = m_call->streamBandwidthLabel();
        p.setPen(th.textTime); p.setFont(monoFont(10));
        p.drawText(QPointF(card.left()+12, card.top()+18), QStringLiteral("OUTBOUND"));
        p.setPen(bw.isEmpty() ? th.textSecondary : th.success);
        p.setFont(monoFont(20));
        p.drawText(QPointF(card.left()+12, card.top()+44),
                   bw.isEmpty() ? QStringLiteral("—") : bw);

        // Sparkline across the lower portion of the card.
        if (m_bwHistory.size() >= 2) {
            QRectF spark(card.left()+12, card.bottom()-22, card.width()-24, 16);
            float mx = 0.01f;
            for (float v : m_bwHistory) mx = qMax(mx, v);
            QPainterPath path;
            const int n = m_bwHistory.size();
            for (int i = 0; i < n; ++i) {
                const qreal px = spark.left() + spark.width() * i / (n-1);
                const qreal py = spark.bottom() - spark.height() * (m_bwHistory[i]/mx);
                if (i == 0) path.moveTo(px, py); else path.lineTo(px, py);
            }
            QColor line = th.success; line.setAlphaF(0.85);
            p.setBrush(Qt::NoBrush); p.setPen(QPen(line, 1.5));
            p.drawPath(path);
        }
        y += cardH + 12;
    }

    // ── Metric cards (2-up grid) ──
    QString enc = m_call->activeVideoEncoder();
    QString dec = m_call->activeVideoDecoder();
    QString rx  = m_call->activeRxResolution();
    QString txRes = m_call->isScreenSharing() ? QStringLiteral("screen")
                  : m_call->isCameraOn()      ? QStringLiteral("1280×720")
                                              : QString();
    struct Metric { QString k, v; QColor c; };
    const QVector<Metric> metrics = {
        { "CODEC",   m_call->activeVideoCodec().isEmpty() ? "—" : m_call->activeVideoCodec(), th.textPrimary },
        { "ENCODER", enc.isEmpty() ? "—" : enc.section(QStringLiteral(" · "), 1, 1),
                     enc.isEmpty() ? th.textSecondary : (m_call->activeVideoEncoderIsHw() ? th.success : th.amber) },
        { "TX RES",  txRes.isEmpty() ? "—" : txRes, th.textPrimary },
        { "RX RES",  rx.isEmpty() ? "—" : rx, rx.isEmpty() ? th.textSecondary : th.textPrimary },
        { "DECODER", dec.isEmpty() ? "—" : dec, dec=="Software" ? th.amber : (dec.isEmpty()?th.textSecondary:th.success) },
        { "PEER",    m_call->remotePeerClient().isEmpty() ? "—" : m_call->remotePeerClient(), th.textPrimary },
    };
    {
        const qreal gap = 8, cardW = (innerW - gap)/2.0, cardH = 46;
        for (int i = 0; i < metrics.size(); ++i) {
            const qreal cx = x0 + (i % 2) * (cardW + gap);
            if (i % 2 == 0 && i > 0) y += cardH + gap;
            QRectF card(cx, y, cardW, cardH);
            QColor cbg = th.bgPrimary; cbg.setAlphaF(0.5);
            p.setBrush(cbg); p.setPen(QPen(th.divider, 1));
            p.drawRoundedRect(card, 8, 8);
            p.setPen(th.textTime); p.setFont(monoFont(9));
            p.drawText(QPointF(card.left()+10, card.top()+16), metrics[i].k);
            p.setPen(metrics[i].c); p.setFont(monoFont(12));
            QString v = p.fontMetrics().elidedText(metrics[i].v, Qt::ElideRight, (int)card.width()-18);
            p.drawText(QPointF(card.left()+10, card.top()+36), v);
        }
        y += cardH + 16;
    }

    // ── Routing: the HPB (signaling) + TURN relay this call actually selected ──
    {
        p.setPen(th.textSecondary); p.setFont(monoFont(10));
        p.drawText(QPointF(x0, y), QStringLiteral("ROUTING")); y += 18;
        auto row = [&](const QString &k, const QString &v) {
            p.setPen(th.textTime); p.setFont(monoFont(10));
            p.drawText(QPointF(x0, y), k);
            const QString vv = v.isEmpty() ? QStringLiteral("—") : v;
            p.setPen(v.isEmpty() ? th.textSecondary : th.textPrimary); p.setFont(monoFont(10));
            p.drawText(QRectF(x0 + 46, y - 12, innerW - 46, 16), Qt::AlignLeft | Qt::AlignVCenter,
                       p.fontMetrics().elidedText(vv, Qt::ElideMiddle, int(innerW) - 50));
            y += 18;
        };
        auto withRtt = [](const QString &host, int rtt) -> QString {
            if (host.isEmpty()) return QString();
            return rtt >= 0 ? QStringLiteral("%1  ·  %2 ms").arg(host).arg(rtt) : host;
        };
        row(QStringLiteral("HPB"),  withRtt(m_call->selectedSignalingLabel(), m_call->selectedSignalingRttMs()));
        row(QStringLiteral("TURN"), withRtt(m_call->selectedTurnLabel(),      m_call->selectedTurnRttMs()));
        y += 8;
    }

    // ── Subsystems: per-participant connection chips ──
    p.setPen(th.textSecondary); p.setFont(monoFont(10));
    p.drawText(QPointF(x0, y), QStringLiteral("SUBSYSTEMS")); y += 20;
    for (auto *cp : m_call->participants()) {
        if (cp->isSelf()) continue;
        const bool ok   = cp->connState()==CallParticipant::Connected;
        const bool fail = cp->connState()==CallParticipant::Failed;
        QColor c = ok ? th.success : fail ? th.danger : th.amber;
        QRectF chip(x0, y-12, innerW, 26);
        QColor cbg = c; cbg.setAlphaF(0.10);
        p.setBrush(cbg); p.setPen(QPen(c, 1)); p.drawRoundedRect(chip, 8, 8);
        p.setBrush(c); p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(chip.left()+10, chip.center().y()-3.5, 7, 7));
        p.setPen(th.textPrimary); p.setFont(monoFont(11));
        p.drawText(QPointF(chip.left()+26, chip.center().y()+4), cp->displayName().left(18));
        p.setPen(c);
        p.drawText(chip.adjusted(0,0,-10,0), Qt::AlignRight|Qt::AlignVCenter,
                   ok ? "LIVE" : fail ? "FAIL" : "…");
        y += 32;
    }

    // ── Stats footer (compact mono) ──
    y += 6;
    p.setPen(th.textSecondary); p.setFont(monoFont(10));
    p.drawText(QPointF(x0, y), QStringLiteral("FLIGHT LOG")); y += 18;
    p.setPen(th.textTime); p.setFont(monoFont(10));
    for (const QString &ln : m_call->callStats().split('\n', Qt::SkipEmptyParts)) {
        p.drawText(QRectF(x0, y-12, innerW, 16), Qt::TextSingleLine,
                   p.fontMetrics().elidedText(ln, Qt::ElideRight, (int)innerW));
        y += 18;
        if (y > height()-16) break;
    }
}

// ── input ───────────────────────────────────────────────────────────────
void CallStage::pokeControls()
{
    m_idleTimer.restart();
    if (!m_controlsVisible) { m_controlsVisible = true; relayout(); }
    unsetCursor();
}

QString CallStage::hitButton(const QPointF &pos) const
{
    for (const Btn &b : m_buttons)
        if (b.rect.adjusted(-4,-4,4,4).contains(pos)) return b.id;
    return {};
}

void CallStage::mouseMoveEvent(QMouseEvent *e)
{
    if (m_draggingPip) {
        m_pipRect.moveTopLeft(e->position() - m_dragOff);
        update();
        return;
    }
    // Track which control-bar button is under the cursor for hover feedback.
    const QString hov = m_controlsVisible ? hitButton(e->position()) : QString();
    m_hoverBtn = hov;
    // 0.40.15 — action-pill hover (Quality / BG top-right).
    // 0.41.1-beta — SHARE quality button joins, only when sharing.
    QString hovPill;
    if (!m_qualityPillRect.isNull() && m_qualityPillRect.contains(e->position()))
        hovPill = QStringLiteral("quality");
    else if (!m_bgPillRect.isNull() && m_bgPillRect.contains(e->position()))
        hovPill = QStringLiteral("bg");
    else if (!m_sharePillRect.isNull() && m_sharePillRect.contains(e->position()))
        hovPill = QStringLiteral("share");
    if (hovPill != m_hoverPill) {
        m_hoverPill = hovPill;
        if (hovPill == QStringLiteral("quality")) {
            const QString highLab = highQualityLabel();
            const QString cur = (m_qualityOverride < 0) ? QStringLiteral("Auto")
                : (m_qualityOverride == 0 ? QStringLiteral("Low (180p)")
                   : m_qualityOverride == 1 ? QStringLiteral("Medium (360p)") : highLab);
            QToolTip::showText(e->globalPosition().toPoint(),
                tr("Receive quality: %1\nClick to pick the substream the SFU forwards.").arg(cur),
                this);
        } else if (hovPill == QStringLiteral("bg")) {
            QSettings bgSet("TalQ", "TalQ");
            bgSet.beginGroup("Talk/Backgrounds");
            const bool    on   = bgSet.value("virtualBackgroundEnabled", false).toBool();
            const QString type = bgSet.value("virtualBackgroundType", "blur").toString();
            bgSet.endGroup();
            const QString cur = !on ? tr("Off")
                              : (type == QLatin1String("image") ? tr("Image") : tr("Blur"));
            QToolTip::showText(e->globalPosition().toPoint(),
                tr("Background: %1\nClick to cycle Off → Blur → Image.\nRight-click to open the full picker.").arg(cur),
                this);
        } else if (hovPill == QStringLiteral("share")) {
            static const char *const kShareLabels[] = { "720p", "1080p", "1440p", "Native" };
            const int lv = qBound(0, m_call->screenShareQuality(), 3);
            QToolTip::showText(e->globalPosition().toPoint(),
                tr("Screen-share quality: %1\nClick to change.")
                    .arg(QString::fromLatin1(kShareLabels[lv])),
                this);
        } else {
            QToolTip::hideText();
        }
    }
    // Cursor: pointing-hand over any clickable target (control button OR
    // action pill).
    setCursor((!hov.isEmpty() || !hovPill.isEmpty())
              ? Qt::PointingHandCursor : Qt::ArrowCursor);
    pokeControls();
    update();
}

void CallStage::mousePressEvent(QMouseEvent *e)
{
    pokeControls();
    // Refresh the top-right action-pill hit-rects synchronously so a click lands
    // even when the chrome had faded out (paintActionPills was skipped → the rects
    // were NULL → the quality dropdown was swallowed; field: "the dropdown did
    // nothing"). GATED on the SAME media-phase predicate paintEvent uses to draw
    // those buttons — otherwise a top-right click on a ringing/idle screen (no
    // buttons drawn) would open a phantom Quality/Background menu.
    {
        const auto st = m_call->state();
        int remotes = 0;
        for (auto *q : m_call->participants()) if (!q->isSelf()) ++remotes;
        const bool mediaPhase =
            (st == CallManager::Active || st == CallManager::Connecting)
            && remotes > 0 && !m_tiles.isEmpty();
        if (mediaPhase) computeActionPillGeometry();
    }
    const QString id = hitButton(e->position());

    // Right-click on the share segment while sharing → quality menu
    // (live re-share at the new cap). Any other right-click is ignored
    // so it can't accidentally trigger a control action.
    if (e->button() == Qt::RightButton) {
        if (id == "share" && m_call->isScreenSharing())
            showScreenShareQualityMenu(e->globalPosition().toPoint());
        else if (!m_qualityPillRect.isNull()
                 && m_qualityPillRect.contains(e->position())) {
            // Right-click on the Quality chip resets to Auto.
            m_qualityOverride = -1;
            updateStreamQualities();
            update();
        } else if (!m_bgPillRect.isNull()
                   && m_bgPillRect.contains(e->position())) {
            // Right-click on the BG chip jumps to Settings → Audio & Video
            // (where the full picker + blur slider + image browser live).
            emit requestOpenBackgroundSettings();
        }
        return;
    }

    // 0.40.15 — Quality / Background dropdowns share orchestration:
    // anchor below the button, pin the chrome visible while exec()
    // blocks, restore the idle timer afterwards. Each caller just
    // populates the QMenu with its checkable entries.
    auto openDropdown = [&](const QRectF &anchor, auto populate) {
        QMenu menu(this);
        populate(menu);
        const QPoint origin = mapToGlobal(QPoint(int(anchor.left()),
                                                  int(anchor.bottom() + 4)));
        m_menuOpen = true;
        pokeControls();
        menu.exec(origin);
        m_menuOpen = false;
        pokeControls();
    };

    // 0.40.15 — Quality button: left-click opens a dropdown of all
    // options. Previous behavior cycled Auto->L->M->H, which was
    // confusing (no way back without three more clicks).
    if (e->button() == Qt::LeftButton
        && !m_qualityPillRect.isNull()
        && m_qualityPillRect.contains(e->position())) {
        // The HIGH entry's resolution tracks the REMOTE peer's actual top
        // layer (peak decoded height), not our own send setting — so a peer
        // that capped to 720p / shed HIGH no longer shows a phantom 1080p.
        const QString highLabel = highQualityLabel();
        openDropdown(m_qualityPillRect, [this, highLabel](QMenu &menu) {
            auto add = [&](const QString &label, int ov) {
                QAction *act = menu.addAction(label);
                act->setCheckable(true);
                act->setChecked(m_qualityOverride == ov);
                connect(act, &QAction::triggered, this, [this, ov]() {
                    m_qualityOverride = ov;
                    updateStreamQualities();
                    update();
                });
            };
            add(tr("Auto"),         -1);
            menu.addSeparator();
            add(tr("Low (180p)"),    0);
            add(tr("Medium (360p)"), 1);
            add(highLabel,           2);
        });
        return;
    }

    // 0.41.1-beta — SHARE quality button: dropdown only present while
    // screen-sharing. Same options as the right-click menu on the
    // bottom share button (720p / 1080p / 1440p / Native).
    if (e->button() == Qt::LeftButton
        && !m_sharePillRect.isNull()
        && m_sharePillRect.contains(e->position())) {
        const int cur = qBound(0, m_call->screenShareQuality(), 3);
        openDropdown(m_sharePillRect, [this, cur](QMenu &menu) {
            static const char *const kLabels[] = {
                "720p", "1080p (Full HD)", "1440p (2K)", "Native (4K-capable)"
            };
            for (int i = 0; i < 4; ++i) {
                QAction *act = menu.addAction(QString::fromLatin1(kLabels[i]));
                act->setCheckable(true);
                act->setChecked(i == cur);
                const int lv = i;
                connect(act, &QAction::triggered, this, [this, lv, cur]() {
                    if (lv != cur) m_call->setScreenShareQuality(lv);
                });
            }
        });
        return;
    }

    // 0.40.15 — Background button: dropdown menu Off/Blur/Image + a
    // "more…" entry that opens the full picker. Right-click still
    // jumps to the picker for power users.
    if (e->button() == Qt::LeftButton
        && !m_bgPillRect.isNull()
        && m_bgPillRect.contains(e->position())) {
        QSettings s("TalQ", "TalQ");
        s.beginGroup("Talk/Backgrounds");
        const bool    curOn   = s.value("virtualBackgroundEnabled", false).toBool();
        const QString curType = s.value("virtualBackgroundType", "blur").toString();
        s.endGroup();
        openDropdown(m_bgPillRect, [this, curOn, curType](QMenu &menu) {
            auto apply = [this](bool on, const QString &type) {
                QSettings s2("TalQ", "TalQ");
                s2.beginGroup("Talk/Backgrounds");
                s2.setValue("virtualBackgroundEnabled", on);
                if (!type.isEmpty()) s2.setValue("virtualBackgroundType", type);
                s2.endGroup();
                m_call->applyBackgroundSettings();
                update();
            };
            auto add = [&](const QString &label, bool on, const QString &type, bool checked) {
                QAction *act = menu.addAction(label);
                act->setCheckable(true);
                act->setChecked(checked);
                connect(act, &QAction::triggered, this, [apply, on, type]() {
                    apply(on, type);
                });
            };
            add(tr("Off"),   false, QString(),  !curOn);
            add(tr("Blur"),  true,  "blur",      curOn && curType == "blur");
            add(tr("Image"), true,  "image",     curOn && curType == "image");
            menu.addSeparator();
            QAction *picker = menu.addAction(tr("Open background settings…"));
            connect(picker, &QAction::triggered, this,
                    &CallStage::requestOpenBackgroundSettings);
        });
        return;
    }

    if (!id.isEmpty()) {
        if (id=="mic")        m_call->toggleMute();
        else if (id=="cam")   m_call->toggleCamera();
        else if (id=="share") emit /*window picks target*/ requestToggleShare();
        else if (id=="telemetry") { m_telemetryOpen=!m_telemetryOpen; update(); }
        else if (id=="roster")    { m_rosterOpen=!m_rosterOpen; update(); }
        else if (id=="full")  emit requestToggleFullscreen();
        else if (id=="end")   m_call->hangUp();
        else if (id=="accept-video") m_call->acceptCall(true);
        else if (id=="accept")  m_call->acceptCall(false);
        else if (id=="decline") m_call->declineCall();
        return;
    }
    if (!m_pipRect.isNull() && m_pipRect.contains(e->position())) {
        m_draggingPip = true;
        m_dragOff = e->position() - m_pipRect.topLeft();
    }
}

void CallStage::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_draggingPip) {
        m_draggingPip = false;
        // snap to nearest corner
        bool right = m_pipRect.center().x() > width()/2.0;
        bool bottom = m_pipRect.center().y() > height()/2.0;
        m_pipCorner = (bottom?2:0) + (right?1:0);
        relayout(); update();
        return;
    }
    // Click a tile: a RAIL tile pins it to the stage; clicking the PINNED stage
    // (a camera, not a screen share) UNPINS back to the gallery. Without this a
    // pinned speaker has no rail tile left to click, so there was no way back to
    // multi-view (field bug: "no way to return to multi-view").
    for (const Tile &t : m_tiles) {
        if (!t.p || !t.rect.contains(e->position())) continue;
        if (t.isStage) {
            if (m_pinned && t.p == m_pinned && !t.isScreen) {
                m_pinned = nullptr;            // unpin → back to gallery
                relayout(); update();
            }
            return;   // a non-pinned / screen-share stage: leave for double-click-fullscreen
        }
        m_pinned = (m_pinned == t.p) ? nullptr : t.p;   // rail tile → pin/unpin
        relayout(); update();
        return;
    }
}

void CallStage::mouseDoubleClickEvent(QMouseEvent *e)
{
    // A double-click on the control bar, top-row chrome, action buttons,
    // or the draggable self-PiP must NOT toggle fullscreen. Rapidly
    // clicking a control — e.g. the camera switch, or the Quality
    // dropdown — otherwise registers every other click as a double-
    // click and bounces the window in and out of fullscreen. Only the
    // bare video surface goes fullscreen on double-click.
    const auto pos = e->position();
    const bool onControl = m_controlsVisible && !hitButton(pos).isEmpty();
    const bool onPip     = !m_pipRect.isNull() && m_pipRect.contains(pos);
    bool onChrome = false;
    for (const QRectF &r : m_topChromeRects) {
        if (r.contains(pos)) { onChrome = true; break; }
    }
    if (onControl || onPip || onChrome) {
        pokeControls();
        return;
    }
    emit requestToggleFullscreen();
}

void CallStage::showScreenShareQualityMenu(const QPoint &globalPos)
{
    // Themed by AppStyle's app-wide QMenu sheet. Heap-alloc + DeleteOnClose
    // so the lifetime ends with the popup (no modal blocking call).
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto *group = new QActionGroup(menu);
    group->setExclusive(true);
    const QStringList labels = {
        tr("720p"), tr("1080p"), tr("1440p"), tr("Native")
    };
    const int cur = m_call->screenShareQuality();
    for (int i = 0; i < labels.size(); ++i) {
        QAction *a = menu->addAction(labels[i]);
        a->setCheckable(true);
        a->setChecked(i == cur);
        a->setData(i);
        group->addAction(a);
    }
    connect(menu, &QMenu::triggered, this, [this, cur](QAction *picked) {
        if (!picked) return;
        const int lv = picked->data().toInt();
        if (lv != cur) m_call->setScreenShareQuality(lv);
    });
    menu->popup(globalPos);
}

void CallStage::leaveEvent(QEvent *)
{
    // Don't strand the user with a hidden bar + blank cursor when the
    // pointer leaves while idle; restore both so re-entry is never blind.
    if (!m_hoverBtn.isEmpty()) { m_hoverBtn.clear(); setCursor(Qt::ArrowCursor); }
    if (!m_hoverPill.isEmpty()) { m_hoverPill.clear(); QToolTip::hideText(); }
    pokeControls();
    update();
}

void CallStage::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_M: m_call->toggleMute(); break;
    case Qt::Key_V: m_call->toggleCamera(); break;
    case Qt::Key_S: emit requestToggleShare(); break;
    case Qt::Key_F: emit requestToggleFullscreen(); break;
    case Qt::Key_T: m_telemetryOpen = !m_telemetryOpen; update(); break;
    default: QWidget::keyPressEvent(e); return;
    }
    pokeControls();
}

void CallStage::resizeEvent(QResizeEvent *)
{
    relayout();
}

void CallStage::forceRelayout()
{
    // Frames are pre-scaled to the OLD size() cap in onFrame(); after a DPI or
    // geometry change that cached image is the wrong resolution and gets
    // stretched into the new tile rect (the "smashed" tile, #5). Drop the
    // caches so the next live frame repopulates at the new tile size, then
    // recompute the layout for the current geometry.
    m_camFrame.clear();
    m_scrFrame.clear();
    relayout();
    update();
}
