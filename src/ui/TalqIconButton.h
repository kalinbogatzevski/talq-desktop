#pragma once

#include <QAbstractButton>
#include <QString>

// ── TalqIconButton ──────────────────────────────────────────────────────
//
// The call bar's chip button, generalised for use anywhere. Paints a
// VectorIcons glyph on a rounded chip whose tint reads state the same way
// the call control bar does:
//
//   • action (not checkable): quiet; accent wash on hover
//   • toggle  (checkable)    : checked → accent chip; unchecked → neutral
//   • off-state toggles (showSlashWhenOff) : unchecked → dimmed + slash
//
// Colours come from the widget palette (set app-wide from PainterTheme), so
// it tracks all four themes with zero per-call-site styling. Tooltip text
// is shown via the standard QToolTip (AppStyle themes it).

class TalqIconButton : public QAbstractButton
{
    Q_OBJECT
public:
    explicit TalqIconButton(const QString &iconId, QWidget *parent = nullptr);

    void setIconId(const QString &id);
    // mic/cam idiom: when checkable and unchecked, dim + diagonal slash.
    void setShowSlashWhenOff(bool on);
    // Destructive action (e.g. hang-up): solid danger-tinted chip.
    void setDanger(bool on);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    QString m_iconId;
    bool    m_slashWhenOff = false;
    bool    m_danger = false;
    bool    m_hover = false;
};
