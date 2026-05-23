#include "CallStage.h"
#include "core/VideoFrameProvider.h"
#include "painter/VectorIcons.h"
#include "ui/SubstreamPolicy.h"

#include <QAction>
#include <QActionGroup>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
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
        if (m_call->state() == CallManager::Idle) { m_camFrame.clear(); m_scrFrame.clear(); }
        relayout(); update();
    });
    connect(m_call, &CallManager::durationChanged, this, [this]{ update(); });
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
        if (m_controlsVisible && m_call->state() == CallManager::Active
            && m_idleTimer.elapsed() > 3500) {
            m_controlsVisible = false;
            setCursor(Qt::BlankCursor);
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
    // Pre-scale to the widget bound so paint is a plain blit (perf guardrail).
    QImage f = img;
    const QSize cap = size().isValid() ? size() : QSize(1280, 720);
    if (img.width() > cap.width() || img.height() > cap.height())
        f = img.scaled(cap, Qt::KeepAspectRatio, Qt::FastTransformation);
    (screen ? m_scrFrame : m_camFrame)[p] = f;
    // tick timer coalesces the actual repaint
}

// ── layout ──────────────────────────────────────────────────────────────
CallParticipant *CallStage::stageSource(bool *isScreen) const
{
    *isScreen = false;
    const auto parts = m_call->participants();
    for (CallParticipant *p : parts)
        if (p->screenSharing() && p->screen()) { *isScreen = true; return p; }
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
    bool isScreen = false;
    CallParticipant *src = stageSource(&isScreen);

    const int n = remotes.size();
    const bool evenGallery = !isScreen && n >= 2 && n <= 4 && !m_pinned;

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
        if (p->isSelf() && !isScreen) continue;       // PiP only when no screen share
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
    if (!selfIsTile && m_call->selfParticipant()) {
        qreal w = qBound(140.0, width()*0.18, 240.0), h = w*9.0/16.0, m = 18;
        qreal x = (m_pipCorner % 2 == 0) ? m : width()-w-m;
        qreal y = (m_pipCorner < 2)      ? m : height()-h-m-72; // clear control bar
        m_pipRect = QRectF(x, y, w, h);
    } else {
        m_pipRect = QRectF();
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
        const int substream = pickSubstream(m_qualityOverride,
                                            t.rect.height(), t.isStage);
        m_call->requestPeerVideoQuality(t.p->sessionId(), substream);
    }
}

// ── painting ────────────────────────────────────────────────────────────
void CallStage::paintEvent(QPaintEvent *)
{
    PainterTheme th(m_themeId, 1.0);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.fillRect(rect(), th.bgPrimary);

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
        paintStatusPill(p, th);
        paintCodecPill(p, th);
        paintSharingBadge(p, th);
        if (m_telemetryOpen) paintTelemetry(p, th);
        if (m_controlsVisible) paintControlBar(p, th);
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
        hidden = qMax(0, hidden - (m_tiles.size() - 2));
        p.setBrush(th.bgSurface); p.setPen(Qt::NoPen);
        p.drawRoundedRect(rc, r, r);
        p.setPen(th.textSecondary); p.setFont(th.nameFont());
        p.drawText(rc, Qt::AlignCenter, QStringLiteral("+%1 more").arg(hidden));
        return;
    }
    CallParticipant *cp = t.p;
    const bool speaking = cp->speaking() && !cp->audioMuted();

    // Speaking halo: the one sanctioned state glow (skip if reduced motion).
    if (speaking) {
        qreal pulse = reducedMotion() ? 0.5 : (0.5 + 0.5*qSin(m_glowPhase));
        QColor g = th.glow; g.setAlphaF(0.35 + 0.35*pulse);
        p.setPen(QPen(g, 3));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(rc.adjusted(-1.5,-1.5,1.5,1.5), r+2, r+2);
    }

    QPainterPath clip; clip.addRoundedRect(rc, r, r);
    p.save();
    p.setClipPath(clip);
    p.fillRect(rc, th.bgSidebar);   // warm ground, never black

    const QImage &frame = t.isScreen ? m_scrFrame.value(cp) : m_camFrame.value(cp);
    const bool showVideo = !frame.isNull()
        && (t.isScreen || (!cp->videoMuted()));
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
            if (t.isScreen) {
                // Provider exists (cp->screen()) but no frame yet → the
                // share is being negotiated. Tell the viewer instead of
                // showing a silent avatar tile (#134 UX gap).
                cap = cp->isSelf() ? tr("Starting screen share…")
                                   : tr("Starting remote screen share…");
            } else if (cp->isSelf()) {
                cap = m_call->isCameraOn() ? tr("Starting camera…")
                                           : tr("Camera off");
            } else if (cp->videoMuted()) {
                cap = tr("Camera off");
            } else if (cp->connState() == CallParticipant::Connecting) {
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
    QRectF plate(rc.left()+10, rc.bottom()-30,
                 qMin<qreal>(tw+52+meterW, rc.width()-20), 22);
    QColor pb = th.bgPrimary; pb.setAlphaF(0.5);
    p.setBrush(pb); p.setPen(Qt::NoPen);
    p.drawRoundedRect(plate, 7, 7);
    qreal tx = plate.left()+10;
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
            QColor fc = cp->speaking() ? th.accent : th.online;
            p.setBrush(fc);
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

    QString sub = state == CallManager::Incoming ? tr("Incoming call")
                : state == CallManager::Outgoing ? tr("Calling…")
                : state == CallManager::Connecting ? tr("Connecting…")
                : m_call->statusDetail().isEmpty() ? tr("Waiting for others to join")
                                                   : m_call->statusDetail();
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
        if (m_controlsVisible) paintControlBar(p, th);
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
        QColor f = hv ? th.danger.lighter(112) : th.danger;
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
    // Aggregate state → the one signal colour.
    bool reconnecting = false, degraded = false;
    for (auto *cp : m_call->participants()) {
        if (cp->isSelf()) continue;
        if (cp->connState()==CallParticipant::Reconnecting
            || cp->connState()==CallParticipant::Failed) reconnecting = true;
        if (cp->connState()==CallParticipant::Connecting) degraded = true;
    }
    const auto st = m_call->state();
    QColor dot = reconnecting ? th.danger
               : (degraded || st==CallManager::Connecting || st==CallManager::Outgoing)
                 ? th.amber : th.accent;
    QString word = reconnecting ? tr("RECONNECTING")
                 : st==CallManager::Outgoing ? tr("CALLING")
                 : st==CallManager::Connecting ? tr("CONNECTING")
                 : tr("LIVE");
    QString dur = st==CallManager::Active ? "  "+fmtDuration(m_call->callDuration()) : QString();
    QString text = word + dur;

    QFont f = monoFont(11); p.setFont(f);
    QFontMetrics fm(f);
    QRectF pill(16, 16, fm.horizontalAdvance(text)+38, 26);
    QColor bg = th.bgSecondary; bg.setAlphaF(0.9);
    p.setBrush(bg); p.setPen(QPen(th.divider, 1));
    p.drawRoundedRect(pill, 13, 13);

    qreal pulse = reducedMotion() ? 1.0 : (0.55 + 0.45*qSin(m_glowPhase));
    QColor d = dot; d.setAlphaF(pulse);
    p.setBrush(d); p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(pill.left()+13, pill.center().y()-4, 8, 8));
    p.setPen(th.textPrimary);
    p.drawText(pill.adjusted(30, 0, -8, 0), Qt::AlignVCenter|Qt::AlignLeft, text);

    m_statusPillBottom = pill.bottom();
}

// Small secondary chip under the status pill: live proof of the negotiated
// send codec + whether the encoder is hardware-accelerated. Quiet by
// design — it's diagnostic reassurance, not a primary control.
void CallStage::paintCodecPill(QPainter &p, const PainterTheme &th)
{
    const QString enc = m_call->activeVideoEncoder();
    if (enc.isEmpty()) return;

    const bool hw = m_call->activeVideoEncoderIsHw();
    const QString codec = enc.section(QStringLiteral(" · "), 0, 0);
    const QString text  = codec + (hw ? QStringLiteral(" · HW")
                                       : QStringLiteral(" · SW"));
    const QColor sig = hw ? th.success : th.amber;

    QFont f = monoFont(10); p.setFont(f);
    QFontMetrics fm(f);
    QRectF pill(16, m_statusPillBottom + 8,
                fm.horizontalAdvance(text) + 34, 22);
    QColor bg = th.bgSecondary; bg.setAlphaF(0.9);
    QColor border = sig; border.setAlphaF(0.55);
    p.setBrush(bg); p.setPen(QPen(border, 1));
    p.drawRoundedRect(pill, 11, 11);

    p.setBrush(sig); p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(pill.left() + 11, pill.center().y() - 3, 6, 6));
    p.setPen(th.textSecondary);
    p.drawText(pill.adjusted(26, 0, -8, 0),
               Qt::AlignVCenter | Qt::AlignLeft, text);

    // RX-resolution chip, immediately right of the codec pill: the decoded
    // incoming resolution (active simulcast layer / BW awareness). Quiet,
    // same row, only when we're actually receiving a decoded frame.
    qreal cursor = pill.right();
    const QString rx = m_call->activeRxResolution();
    if (!rx.isEmpty()) {
        const QString rxText = QStringLiteral("RX ") + rx;
        QRectF rxPill(cursor + 8, pill.top(),
                      fm.horizontalAdvance(rxText) + 26, pill.height());
        QColor rxBorder = th.divider; rxBorder.setAlphaF(0.7);
        p.setBrush(bg); p.setPen(QPen(rxBorder, 1));
        p.drawRoundedRect(rxPill, 11, 11);
        p.setBrush(th.textSecondary); p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(rxPill.left() + 11, rxPill.center().y() - 3, 6, 6));
        p.setPen(th.textSecondary);
        p.drawText(rxPill.adjusted(24, 0, -8, 0),
                   Qt::AlignVCenter | Qt::AlignLeft, rxText);
        cursor = rxPill.right();
    }

    // #8 Quality chip — clickable; cycles Auto -> L -> M -> H -> Auto.
    // Active in all builds as of 0.38.0 (simulcast publishes in stable too,
    // so the chip's selectStream actually has layers to switch between).
    // The dot colour signals the active mode (textSecondary for Auto,
    // accent for any forced layer) so the override is glanceable.
    static const char *const kLabels[] = { "LOW", "MED", "HIGH" };
    const QString qText = (m_qualityOverride < 0)
        ? QStringLiteral("AUTO")
        : QString::fromLatin1(kLabels[m_qualityOverride]);
    QRectF qPill(cursor + 8, pill.top(),
                 fm.horizontalAdvance(qText) + 26, pill.height());
    const bool forced = (m_qualityOverride >= 0);
    QColor qDot     = forced ? th.accent : th.textSecondary;
    QColor qBorder  = qDot;  qBorder.setAlphaF(forced ? 0.65 : 0.55);
    p.setBrush(bg); p.setPen(QPen(qBorder, 1));
    p.drawRoundedRect(qPill, 11, 11);
    p.setBrush(qDot); p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(qPill.left() + 11, qPill.center().y() - 3, 6, 6));
    p.setPen(forced ? th.textPrimary : th.textSecondary);
    p.drawText(qPill.adjusted(24, 0, -8, 0),
               Qt::AlignVCenter | Qt::AlignLeft, qText);
    m_qualityPillRect = qPill;
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
    qreal pillW = fm.horizontalAdvance(text) + 40;
    QRectF pill((width() - pillW) / 2.0, 14, pillW, 28);

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
    if (hov != m_hoverBtn) {
        m_hoverBtn = hov;
        setCursor(hov.isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
    }
    pokeControls();
    update();
}

void CallStage::mousePressEvent(QMouseEvent *e)
{
    pokeControls();
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
        }
        return;
    }

    // #8 Quality chip — cycle Auto -> L -> M -> H -> Auto on left-click.
    if (e->button() == Qt::LeftButton
        && !m_qualityPillRect.isNull()
        && m_qualityPillRect.contains(e->position())) {
        m_qualityOverride = (m_qualityOverride + 2) % 4 - 1;  // -1,0,1,2 cycle
        updateStreamQualities();
        update();
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
    // click a rail tile → pin / unpin to stage
    for (const Tile &t : m_tiles) {
        if (t.p && !t.isStage && t.rect.contains(e->position())) {
            m_pinned = (m_pinned == t.p) ? nullptr : t.p;
            relayout(); update();
            return;
        }
    }
}

void CallStage::mouseDoubleClickEvent(QMouseEvent *e)
{
    // A double-click on the control bar (or the draggable self-PiP) must
    // NOT toggle fullscreen. Rapidly clicking a control — e.g. the camera
    // switch — otherwise registers every other click as a double-click and
    // bounces the window in and out of fullscreen. Only the bare video
    // surface goes fullscreen on double-click.
    const bool onControl = m_controlsVisible && !hitButton(e->position()).isEmpty();
    const bool onPip = !m_pipRect.isNull() && m_pipRect.contains(e->position());
    if (onControl || onPip) {
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
