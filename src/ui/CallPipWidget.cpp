#include "CallPipWidget.h"
#include "core/VideoFrameProvider.h"
#include "painter/VectorIcons.h"

#include <QGuiApplication>
#include <QScreen>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>

namespace {
QString pipInitials(const QString &name)
{
    const QStringList parts = name.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return QStringLiteral("?");
    QString s = parts.first().left(1).toUpper();
    if (parts.size() > 1) s += parts.last().left(1).toUpper();
    return s;
}
}

CallPipWidget::CallPipWidget(CallManager *call, QWidget *parent)
    : QWidget(parent), m_call(call)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::PointingHandCursor);

    connect(m_call, &CallManager::participantAdded, this, [this](CallParticipant *) { rebindProvider(); update(); });
    connect(m_call, &CallManager::participantRemoved, this, [this](const QString &) { rebindProvider(); update(); });
    connect(m_call, &CallManager::participantsChanged, this, [this] { update(); });
    connect(m_call, &CallManager::muteChanged, this, [this] { update(); });
    connect(m_call, &CallManager::callInfoChanged, this, [this] { update(); });

    rebindProvider();
}

void CallPipWidget::setTheme(PainterTheme::Theme t)
{
    if (m_themeId == t) return;
    m_themeId = t;
    update();
}

CallParticipant *CallPipWidget::bestParticipant() const
{
    const auto parts = m_call->participants();
    for (CallParticipant *p : parts)
        if (p && !p->isSelf()) return p;
    return m_call->selfParticipant();
}

void CallPipWidget::rebindProvider()
{
    for (const auto &c : m_conns) QObject::disconnect(c);
    m_conns.clear();
    m_thumb = QImage();

    CallParticipant *p = bestParticipant();
    m_shown = p;
    if (!p) return;

    m_conns << connect(p, &CallParticipant::changed, this, [this] { update(); });
    m_conns << connect(p, &CallParticipant::videoProvidersChanged, this, [this] { rebindProvider(); update(); });
    if (auto *cam = p->camera())
        m_conns << connect(cam, &VideoFrameProvider::imageReady, this, &CallPipWidget::onFrame);
}

void CallPipWidget::onFrame(const QImage &img)
{
    m_thumb = img;
    update();
}

QImage CallPipWidget::avatarDisc(const QString &id, const QString &name, int size, const PainterTheme &th) const
{
    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter g(&img);
    g.setRenderHint(QPainter::Antialiasing);
    g.setBrush(PainterTheme::authorColor(id)); g.setPen(Qt::NoPen);
    g.drawEllipse(QRectF(0.5, 0.5, size - 1.0, size - 1.0));
    QFont f = th.nameFont(); f.setPixelSize(int(size * 0.36)); f.setWeight(QFont::DemiBold);
    g.setFont(f); g.setPen(th.controlInk);
    g.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, pipInitials(name));
    return img;
}

void CallPipWidget::paintEvent(QPaintEvent *)
{
    const PainterTheme th(m_themeId);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF rc = rect();
    p.fillRect(rc, th.bgSurface);

    if (!m_thumb.isNull()) {
        // "cover" fit: scale to fill, center-crop via clip.
        const QImage scaled = m_thumb.scaled(rc.size().toSize(),
            Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const QPointF topLeft((rc.width() - scaled.width()) / 2.0,
                               (rc.height() - scaled.height()) / 2.0);
        p.save();
        p.setClipRect(rc);
        p.drawImage(topLeft, scaled);
        p.restore();
    } else {
        const QString name = m_shown ? m_shown->displayName() : QString();
        const QString id = m_shown ? m_shown->sessionId() : QStringLiteral("self");
        const int d = qMin(int(rc.height() * 0.5), 84);
        const QImage disc = avatarDisc(id, name, d, th);
        QRectF ar(0, 0, d, d);
        ar.moveCenter(rc.center() - QPointF(0, 8));
        p.drawImage(ar, disc);
    }

    // Bottom control strip: mute-state chip (left) + hang-up button (right).
    // These are the ONLY two interactive/informational bits the dock shows —
    // everything else (mic/cam toggles, share, telemetry…) lives in the full
    // CallStage you get back to via a double-click.
    const qreal barH = 34;
    const QRectF bar(0, rc.height() - barH, rc.width(), barH);
    p.fillRect(bar, QColor(0, 0, 0, 140));

    const bool muted = m_call->isMuted();
    // Stored as a member (not a local) so mousePress/Release can hit-test it:
    // clicking the chip toggles mute, same as the mic button on the full stage.
    m_micRect = QRectF(10, bar.top() + (barH - 22) / 2.0, 22, 22);
    const QColor micBg = muted ? QColor(0xE2, 0x3B, 0x33, 220) : QColor(255, 255, 255, 30);
    p.setBrush(micBg); p.setPen(Qt::NoPen);
    p.drawEllipse(m_micRect);
    VectorIcons::draw(p, QStringLiteral("mic"), m_micRect.adjusted(4, 4, -4, -4),
                       Qt::white, muted, micBg);

    m_hangupRect = QRectF(rc.width() - 10 - 28, bar.top() + (barH - 28) / 2.0, 28, 28);
    const QColor hangupRed(0xE2, 0x3B, 0x33);
    p.setBrush(hangupRed); p.setPen(Qt::NoPen);
    p.drawEllipse(m_hangupRect);
    VectorIcons::draw(p, QStringLiteral("end"), m_hangupRect.adjusted(6, 6, -6, -6),
                       Qt::white, false, hangupRed);

    // Explicit "expand back to full call" control, just left of hang-up.
    // Restore was previously only a double-click, which wasn't discoverable.
    m_expandRect = QRectF(m_hangupRect.left() - 8 - 28, bar.top() + (barH - 28) / 2.0, 28, 28);
    const QColor expandBg(255, 255, 255, 30);
    p.setBrush(expandBg); p.setPen(Qt::NoPen);
    p.drawEllipse(m_expandRect);
    VectorIcons::draw(p, QStringLiteral("full"), m_expandRect.adjusted(7, 7, -7, -7),
                       Qt::white, false, expandBg);

    // "Minimize the dock entirely" control, just left of the expand button:
    // takes the dock off-screen (call stays live), returns when TalQ is reopened.
    m_minimizeRect = QRectF(m_expandRect.left() - 8 - 28, bar.top() + (barH - 28) / 2.0, 28, 28);
    const QColor minBg(255, 255, 255, 30);
    p.setBrush(minBg); p.setPen(Qt::NoPen);
    p.drawEllipse(m_minimizeRect);
    // Simple minimize glyph: a short horizontal bar (drawn directly, no icon dep).
    p.setPen(QPen(Qt::white, 2, Qt::SolidLine, Qt::RoundCap));
    const QPointF mc = m_minimizeRect.center();
    p.drawLine(QPointF(mc.x() - 6, mc.y() + 3), QPointF(mc.x() + 6, mc.y() + 3));

    // Faint border so the frameless dock reads as a distinct floating chip
    // against whatever desktop content sits behind it.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(th.divider, 1));
    p.drawRect(rc.adjusted(0.5, 0.5, -0.5, -0.5));
}

void CallPipWidget::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
    m_pressOnHangup = m_hangupRect.contains(e->position());
    m_pressOnExpand = m_expandRect.contains(e->position());
    m_pressOnMinimize = m_minimizeRect.contains(e->position());
    m_pressOnMic = m_micRect.contains(e->position());
    if (!m_pressOnHangup && !m_pressOnExpand && !m_pressOnMinimize && !m_pressOnMic) {
        m_dragging = true;
        m_dragMoved = false;
        m_dragStartGlobalPos = e->globalPosition().toPoint();
        m_dragStartWinPos = window()->pos();
    }
}

void CallPipWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_dragging) return;
    const QPoint delta = e->globalPosition().toPoint() - m_dragStartGlobalPos;
    if (!m_dragMoved && delta.manhattanLength() > 4) m_dragMoved = true;
    if (m_dragMoved) window()->move(m_dragStartWinPos + delta);
}

void CallPipWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) { QWidget::mouseReleaseEvent(e); return; }
    const bool wasDraggedAndMoved = m_dragging && m_dragMoved;
    const bool clickedHangup = m_pressOnHangup && !m_dragMoved && m_hangupRect.contains(e->position());
    const bool clickedExpand = m_pressOnExpand && !m_dragMoved && m_expandRect.contains(e->position());
    const bool clickedMinimize = m_pressOnMinimize && !m_dragMoved && m_minimizeRect.contains(e->position());
    const bool clickedMic = m_pressOnMic && !m_dragMoved && m_micRect.contains(e->position());
    m_dragging = false;
    m_dragMoved = false;
    m_pressOnHangup = false;
    m_pressOnExpand = false;
    m_pressOnMinimize = false;
    m_pressOnMic = false;

    if (clickedHangup) { m_call->hangUp(); return; }
    if (clickedExpand) { emit restoreRequested(); return; }
    if (clickedMinimize) { emit minimizeRequested(); return; }
    if (clickedMic) { m_call->toggleMute(); return; }
    // "Drop" the dragged dock: snap it home to the nearest corner instead of
    // leaving it wherever the mouse happened to release.
    if (wasDraggedAndMoved) snapToNearestCorner();
}

void CallPipWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (m_hangupRect.contains(e->position())) return;    // never restore via the hang-up button
    if (m_minimizeRect.contains(e->position())) return;  // nor via the minimize button
    if (m_micRect.contains(e->position())) return;       // nor via the mute chip
    emit restoreRequested();
}

void CallPipWidget::snapToNearestCorner()
{
    QWidget *win = window();
    QScreen *sc = win->screen();
    if (!sc) sc = QGuiApplication::primaryScreen();
    if (!sc) return;

    const QRect avail = sc->availableGeometry();
    const int m = 24;
    const QSize sz = win->size();
    const QPoint c = win->pos() + QPoint(sz.width() / 2, sz.height() / 2);
    const bool left = (c.x() - avail.left()) < (avail.right() - c.x());
    const bool top  = (c.y() - avail.top())  < (avail.bottom() - c.y());
    const int x = left ? avail.left() + m : avail.right() - sz.width() - m;
    const int y = top  ? avail.top()  + m : avail.bottom() - sz.height() - m;
    win->move(x, y);
}
