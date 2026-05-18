#include "ui/TalqIconButton.h"
#include "painter/VectorIcons.h"

#include <QPainter>
#include <QPaintEvent>
#include <QPalette>

namespace {
QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF()   + (b.redF()   - a.redF())   * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF()  + (b.blueF()  - a.blueF())  * t);
}
}

TalqIconButton::TalqIconButton(const QString &iconId, QWidget *parent)
    : QAbstractButton(parent), m_iconId(iconId)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover, true);
}

void TalqIconButton::setIconId(const QString &id)
{
    if (m_iconId == id) return;
    m_iconId = id;
    update();
}

void TalqIconButton::setShowSlashWhenOff(bool on)
{
    m_slashWhenOff = on;
    update();
}

void TalqIconButton::setDanger(bool on)
{
    m_danger = on;
    update();
}

QSize TalqIconButton::sizeHint() const { return {38, 38}; }

void TalqIconButton::enterEvent(QEnterEvent *) { m_hover = true;  update(); }
void TalqIconButton::leaveEvent(QEvent *)      { m_hover = false; update(); }

void TalqIconButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QPalette pal = palette();
    const QColor surface = pal.color(QPalette::Window);
    const QColor accent  = pal.color(QPalette::Highlight);
    const QColor onAccent = pal.color(QPalette::HighlightedText);
    const QColor ink     = pal.color(QPalette::WindowText);
    const QColor dim     = pal.color(QPalette::PlaceholderText);

    const bool toggle   = isCheckable();
    const bool on       = isChecked();
    const bool offState = toggle && m_slashWhenOff && !on;

    QColor chip(Qt::transparent);
    QColor glyph = ink;
    QColor slashBack = surface;
    bool   slash = false;

    if (m_danger) {
        // Solid danger chip (hang-up idiom); ink stays high-contrast.
        const QColor danger = mix(surface, QColor(0xd0, 0x55, 0x44), 0.85);
        chip  = m_hover ? danger.lighter(112) : danger;
        glyph = onAccent;
    } else if (toggle && on) {
        chip  = mix(surface, accent, 0.22);
        glyph = accent.lighter(118);
        if (m_hover) chip = chip.lighter(114);
    } else if (offState) {
        chip  = mix(surface, ink, 0.06);
        glyph = dim;
        slash = true;
        slashBack = chip;
        if (m_hover) chip = chip.lighter(114);
    } else if (m_hover) {
        chip = mix(surface, accent, 0.14);
    }
    if (isDown() && !m_danger)
        chip = chip.alpha() ? chip.darker(108) : mix(surface, accent, 0.20);

    const QRectF r = rect().adjusted(2.5, 2.5, -2.5, -2.5);
    if (chip.alpha() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(chip);
        p.drawRoundedRect(r, 9, 9);
    }

    const qreal isz = qMin(r.width(), r.height()) * 0.52;
    QRectF ib(0, 0, isz, isz);
    ib.moveCenter(r.center());
    VectorIcons::draw(p, m_iconId, ib, glyph, slash, slashBack);
}
