#include "CallStage.h"
#include "core/VideoFrameProvider.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTimer>
#include <QSettings>
#include <QSet>
#include <QRegularExpression>
#include <QFontMetrics>
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
    connect(m_call, &CallManager::callStatsChanged, this, [this]{ if (m_telemetryOpen) update(); });
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
        if (auto *scr = p->screen())
            m_conns << connect(scr, &VideoFrameProvider::imageReady, this,
                [this, p](const QImage &img){ onFrame(p, true, img); });
    }
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
}

void CallStage::onFrame(CallParticipant *p, bool screen, const QImage &img)
{
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
        if (p->isSelf()) continue;                    // self = PiP overlay
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
    // self-PiP rect (only in stage modes where self is not a rail tile)
    bool selfIsTile = false;
    for (const Tile &t : m_tiles) if (t.p && t.p->isSelf()) selfIsTile = true;
    if (!selfIsTile && m_call->selfParticipant()) {
        qreal w = qBound(140.0, width()*0.18, 240.0), h = w*9.0/16.0, m = 18;
        qreal x = (m_pipCorner % 2 == 0) ? m : width()-w-m;
        qreal y = (m_pipCorner < 2)      ? m : height()-h-m-72; // clear control bar
        m_pipRect = QRectF(x, y, w, h);
    } else {
        m_pipRect = QRectF();
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
    QRectF plate(rc.left()+10, rc.bottom()-30, qMin<qreal>(tw+50, rc.width()-20), 22);
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
    p.drawText(QRectF(tx, plate.top(), plate.right()-tx-6, plate.height()),
               Qt::AlignVCenter|Qt::AlignLeft, fm.elidedText(label, Qt::ElideRight,
               int(plate.right()-tx-6)));

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
        qreal bw = 132, bh = 46, gap = 24, cy = height()/2.0 + 84;
        QRectF acc(width()/2.0-bw-gap/2, cy, bw, bh);
        QRectF dec(width()/2.0+gap/2, cy, bw, bh);
        p.setBrush(th.accent); p.setPen(Qt::NoPen);
        p.drawRoundedRect(acc, 10, 10);
        p.setPen(th.controlInk); p.setFont(th.nameFont());
        p.drawText(acc, Qt::AlignCenter, tr("Accept"));
        p.setBrush(Qt::NoBrush); p.setPen(QPen(th.danger, 1.3));
        p.drawRoundedRect(dec, 10, 10);
        p.setPen(th.danger);
        p.drawText(dec, Qt::AlignCenter, tr("Decline"));
        m_buttons.push_back({QStringLiteral("accept"), acc, {}, false, false});
        m_buttons.push_back({QStringLiteral("decline"), dec, {}, false, true});
    } else {
        paintStatusPill(p, th);
        if (m_controlsVisible) paintControlBar(p, th);
    }
}

void CallStage::buildButtons()
{
    m_buttons.clear();
    if (m_call->state() == CallManager::Incoming) return;
    struct Def { const char *id; const char *glyph; };
    const QVector<Def> defs = {
        {"mic",   "\U0001F3A4"}, {"cam", "\U0001F3A5"}, {"share", "\U0001F5A5"},
        {"telemetry", "☷"}, {"roster", "\U0001F465"}, {"full", "⛶"},
        {"end",   "✕"},
    };
    qreal bs = 46, gap = 12;
    qreal total = defs.size()*bs + (defs.size()-1)*gap + 16;
    qreal x = (width()-total)/2.0, y = height()-bs-22;
    for (const Def &d : defs) {
        Btn b; b.id = d.id; b.glyph = QString::fromUtf8(d.glyph);
        b.rect = QRectF(x, y, bs, bs);
        if (b.id == "end") { b.danger = true; b.rect.translate(8, 0); }
        if (b.id == "mic")   b.on = !m_call->isMuted();
        if (b.id == "cam")   b.on = m_call->isCameraOn();
        if (b.id == "share") b.on = m_call->isScreenSharing();
        if (b.id == "telemetry") b.on = m_telemetryOpen;
        if (b.id == "roster") b.on = m_rosterOpen;
        m_buttons << b;
        x += bs + gap;
    }
}

void CallStage::paintControlBar(QPainter &p, const PainterTheme &th)
{
    if (m_buttons.isEmpty()) return;
    QRectF bar = m_buttons.first().rect.united(m_buttons.last().rect)
                     .adjusted(-16, -12, 16, 12);
    QColor barBg = th.bgSecondary; barBg.setAlphaF(0.92);
    p.setBrush(barBg); p.setPen(QPen(th.divider, 1));
    p.drawRoundedRect(bar, 26, 26);

    for (const Btn &b : m_buttons) {
        QColor fill, ink;
        if (b.danger)      { fill = th.danger;  ink = th.controlInk; }
        else if (b.on && (b.id=="share"||b.id=="telemetry"||b.id=="roster"))
                           { fill = th.accent;  ink = th.controlInk; }
        else if (!b.on && (b.id=="mic"||b.id=="cam"))
                           { fill = th.bgSurface; ink = th.danger; }   // off = danger ink
        else               { fill = th.bgSurface; ink = th.textPrimary; }
        p.setBrush(fill); p.setPen(Qt::NoPen);
        p.drawEllipse(b.rect);
        QFont gf = th.nameFont(); gf.setPixelSize(18); p.setFont(gf);
        p.setPen(ink);
        p.drawText(b.rect, Qt::AlignCenter, b.glyph);
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
}

void CallStage::paintTelemetry(QPainter &p, const PainterTheme &th)
{
    qreal w = qBound(280.0, width()*0.30, 380.0);
    QRectF panel(width()-w, 0, w, height());
    QColor bg = th.bgSecondary; bg.setAlphaF(0.97);
    p.setBrush(bg); p.setPen(Qt::NoPen);
    p.fillRect(panel, bg);
    p.setPen(QPen(th.divider, 1));
    p.drawLine(panel.topLeft(), panel.bottomLeft());

    qreal x = panel.left()+18, y = 22;
    p.setPen(th.textSecondary); p.setFont(monoFont(11));
    p.drawText(QPointF(x, y), QStringLiteral("● FLIGHT LOG · TELEMETRY"));
    y += 28;
    p.setFont(monoFont(11));
    auto line = [&](const QString &k, const QString &v, const QColor &vc){
        p.setPen(th.textTime);  p.drawText(QPointF(x, y), k);
        p.setPen(vc);           p.drawText(QPointF(x+136, y), v);
        y += 20;
    };
    line("CODEC",   m_call->activeVideoCodec().isEmpty() ? "—" : m_call->activeVideoCodec(), th.textPrimary);
    QString dec = m_call->activeVideoDecoder();
    line("DECODER", dec.isEmpty() ? "—" : dec, dec=="Software" ? th.danger : th.success);
    line("PEER",    m_call->remotePeerClient().isEmpty() ? "—" : m_call->remotePeerClient(), th.textPrimary);
    y += 8;
    p.setPen(th.textSecondary); p.drawText(QPointF(x, y), QStringLiteral("SUBSYSTEMS")); y += 22;
    for (auto *cp : m_call->participants()) {
        if (cp->isSelf()) continue;
        QColor c = cp->connState()==CallParticipant::Connected ? th.success
                 : cp->connState()==CallParticipant::Failed ? th.danger : th.amber;
        QString stx = cp->connState()==CallParticipant::Connected ? "OK"
                    : cp->connState()==CallParticipant::Failed ? "FAIL" : "…";
        QString nm = cp->displayName().left(16);
        p.setPen(th.textTime); p.drawText(QPointF(x, y), nm);
        p.setPen(c); p.drawText(QPointF(x+200, y), stx);
        y += 20;
    }
    y += 10;
    p.setPen(th.textSecondary); p.drawText(QPointF(x, y), QStringLiteral("STATS")); y += 20;
    p.setPen(th.textTime);
    const QString stats = m_call->callStats();
    for (const QString &ln : stats.split('\n', Qt::SkipEmptyParts)) {
        p.drawText(QRectF(x, y-12, w-36, 18), Qt::TextWordWrap, ln);
        y += 20;
        if (y > height()-20) break;
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
    pokeControls();
    update();
}

void CallStage::mousePressEvent(QMouseEvent *e)
{
    pokeControls();
    const QString id = hitButton(e->position());
    if (!id.isEmpty()) {
        if (id=="mic")        m_call->toggleMute();
        else if (id=="cam")   m_call->toggleCamera();
        else if (id=="share") emit /*window picks target*/ requestToggleShare();
        else if (id=="telemetry") { m_telemetryOpen=!m_telemetryOpen; update(); }
        else if (id=="roster")    { m_rosterOpen=!m_rosterOpen; update(); }
        else if (id=="full")  emit requestToggleFullscreen();
        else if (id=="end")   m_call->hangUp();
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

void CallStage::mouseDoubleClickEvent(QMouseEvent *)
{
    emit requestToggleFullscreen();
}

void CallStage::leaveEvent(QEvent *)
{
    // Don't strand the user with a hidden bar + blank cursor when the
    // pointer leaves while idle; restore both so re-entry is never blind.
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
