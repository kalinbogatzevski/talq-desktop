#include "ui/AppStyle.h"

#include <QApplication>
#include <QEvent>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStyle>
#include <QWidget>

namespace {

QString hx(const QColor &c) { return c.name(QColor::HexRgb); }

// Linear blend a→b; used to derive state tints from theme tokens so we
// never hardcode a colour (everything is a function of PainterTheme).
QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF()   + (b.redF()   - a.redF())   * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF()  + (b.blueF()  - a.blueF())  * t);
}

// One re-polish filter for the whole app: a setProperty("variant"/"role")
// on an already-shown widget would otherwise not re-run the QSS selector.
class RepolishFilter : public QObject
{
public:
    using QObject::QObject;
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::DynamicPropertyChange) {
            auto *w = qobject_cast<QWidget *>(o);
            auto *pe = static_cast<QDynamicPropertyChangeEvent *>(e);
            const QByteArray n = pe->propertyName();
            if (w && (n == "variant" || n == "role")) {
                w->style()->unpolish(w);
                w->style()->polish(w);
                w->update();
            }
        }
        return QObject::eventFilter(o, e);
    }
};

} // namespace

namespace AppStyle {

QString sheet(const PainterTheme &t)
{
    // Derived state tints — calm, warm, flat-but-stateful. No raw hex.
    const QColor hoverWash  = mix(t.bgSecondary, t.accent, 0.16);
    const QColor accentHi   = t.accent.lighter(115);
    const QColor accentDim  = mix(t.bgSecondary, t.accent, 0.30);
    const QColor dangerHi   = t.danger.lighter(112);
    const QColor dangerWash = mix(t.bgSecondary, t.danger, 0.16);
    const QColor handleCol  = mix(t.bgSecondary, t.textMuted, 0.55);
    const QColor inputBg    = mix(t.bgPrimary, t.bgSecondary, 0.6);

    QHash<QString, QString> v {
        { "bgPrimary",   hx(t.bgPrimary)     },
        { "bgSecondary", hx(t.bgSecondary)   },
        { "bgSurface",   hx(t.bgSurface)     },
        { "bgSelected",  hx(t.bgSelected)    },
        { "bgHover",     hx(t.bgHover)       },
        { "ink",         hx(t.textPrimary)   },
        { "ink2",        hx(t.textSecondary) },
        { "inkMuted",    hx(t.textMuted)     },
        { "accent",      hx(t.accent)        },
        { "accentHi",    hx(accentHi)        },
        { "accentDim",   hx(accentDim)       },
        { "controlInk",  hx(t.controlInk)    },
        { "danger",      hx(t.danger)        },
        { "dangerHi",    hx(dangerHi)        },
        { "dangerWash",  hx(dangerWash)      },
        { "divider",     hx(t.divider)       },
        { "hoverWash",   hx(hoverWash)       },
        { "inputBg",     hx(inputBg)         },
        { "handle",      hx(handleCol)       },
        { "rN", QString::number(PainterTheme::radiusNormal)  },
        { "rS", QString::number(PainterTheme::radiusSmall)   },
        { "rC", QString::number(PainterTheme::radiusControl) },
        { "rCard", QString::number(PainterTheme::radiusCard) },
    };

    // @{token} template — generic chrome only (never bare QWidget: QPainter
    // surfaces like CallStage/SidebarPainter paint their own ground).
    QString s = QStringLiteral(R"QSS(
QDialog, QMessageBox { background:@{bgPrimary}; }

QLabel { background:transparent; color:@{ink}; }
QLabel[role="secondary"] { color:@{ink2}; }
QLabel[role="muted"]     { color:@{inkMuted}; }
QLabel[role="title"]     { color:@{ink}; font-size:15px; font-weight:600; }
QLabel[role="danger"]    { color:@{danger}; }
QLabel[role="success"]   { color:@{accent}; }

/* Settings surface: group eyebrow + two-column setting rows. Theme-driven
 * (tokens only); the calm-surface / confident-control idiom. */
QLabel[role="eyebrow"]     { color:@{inkMuted}; font-size:11px; font-weight:600; }
QLabel[role="settingName"] { color:@{ink};  font-size:13px; font-weight:600; }
QLabel[role="settingDesc"] { color:@{ink2}; font-size:11px; }
QWidget#settingsHeader     { background:@{bgPrimary}; }

/* Named component surfaces (single-source, theme-driven — not drift). */
QWidget#selectionBar { background:@{bgSecondary}; }

/* Calm callout / hint box (full tint + border — never a side-stripe). */
QFrame[role="hint"] {
    background:@{hoverWash}; border:1px solid @{divider};
    border-radius:@{rN}px;
}
QFrame[role="card"] {
    background:@{bgSurface}; border:1px solid @{divider};
    border-radius:@{rCard}px;
}

QToolTip {
    background:@{bgSecondary}; color:@{ink};
    border:1px solid @{divider}; border-radius:@{rS}px;
    padding:5px 8px;
}

/* ── Buttons ──
 * Base is deliberately NON-INVASIVE: colour + radius + a hover wash only.
 * No border, no background, no padding — so the app's many fixed-size
 * icon-glyph buttons (sidebar/header) are never clipped or boxed. The
 * rich filled "pill" look is OPT-IN via the variant property, applied to
 * real text call-to-actions (never to tiny icon buttons). */
QPushButton {
    background:transparent; color:@{ink};
    border:none; border-radius:@{rC}px;
}
QPushButton:hover    { background:@{hoverWash}; }
QPushButton:pressed  { background:@{bgSelected}; }
QPushButton:disabled { color:@{inkMuted}; }
QPushButton:focus    { outline:none; }

QPushButton[variant="primary"] {
    background:@{accent}; color:@{controlInk}; font-weight:600;
    padding:8px 18px;
}
QPushButton[variant="primary"]:hover  { background:@{accentHi}; }
QPushButton[variant="primary"]:pressed{ background:@{accentDim}; }
QPushButton[variant="primary"]:disabled{ background:@{accentDim};
                                          color:@{inkMuted}; }

QPushButton[variant="danger"] {
    background:transparent; color:@{danger};
    border:1px solid @{danger}; padding:8px 14px;
}
QPushButton[variant="danger"]:hover  { background:@{dangerWash};
                                       color:@{dangerHi};
                                       border-color:@{dangerHi}; }
QPushButton[variant="danger"]:pressed{ background:@{dangerWash}; }

/* Default — the quiet workhorse (DESIGN.md). A neutral, tactile button for
 * standalone secondary actions (Cancel / Done / Refresh / Edit): a filled
 * well tone + hairline border so it ALWAYS reads as a button at rest, never
 * as plain text. Matches the message-box button metric for one consistent
 * neutral-button look app-wide. */
QPushButton[variant="default"] {
    background:@{inputBg}; color:@{ink};
    border:1px solid @{divider}; border-radius:@{rC}px;
    padding:8px 16px;
}
QPushButton[variant="default"]:hover    { background:@{hoverWash}; border-color:@{accent}; }
QPushButton[variant="default"]:pressed  { background:@{bgSelected}; }
QPushButton[variant="default"]:disabled { color:@{inkMuted}; background:@{bgSecondary}; }

/* Ghost is the quiet tertiary affordance — INLINE only (breadcrumbs, chips),
 * never a standalone footer button (those use "default" so they read as
 * buttons at rest). Flat-but-tactile: fill only on hover. */
QPushButton[variant="ghost"] {
    background:transparent; color:@{ink2}; border:none; padding:7px 14px;
}
QPushButton[variant="ghost"]:hover  { color:@{ink}; background:@{hoverWash}; }
QPushButton[variant="ghost"]:pressed{ background:@{bgSelected}; }

/* ── Message boxes ──
 * Every QMessageBox button gets the real button look app-wide, so no box is
 * ever styled by hand again. Non-default buttons read as a quiet outlined
 * control on the input-well tone; the default/accept button is the accent
 * fill. Scoped to QMessageBox descendants so it can never touch the app's
 * icon-glyph buttons. Themed from existing tokens → re-tints live on theme
 * change with the rest of the sheet. */
QMessageBox QPushButton {
    background:@{inputBg}; color:@{ink};
    border:1px solid @{divider}; border-radius:@{rC}px;
    padding:8px 16px; min-width:84px; font-weight:500;
}
QMessageBox QPushButton:hover    { background:@{hoverWash}; border-color:@{accent}; }
QMessageBox QPushButton:pressed  { background:@{bgSelected}; }
QMessageBox QPushButton:disabled { color:@{inkMuted}; background:@{bgSecondary}; }
QMessageBox QPushButton:default {
    background:@{accent}; color:@{controlInk};
    border:1px solid @{accent}; font-weight:600;
}
QMessageBox QPushButton:default:hover   { background:@{accentHi}; border-color:@{accentHi}; }
QMessageBox QPushButton:default:pressed { background:@{accentDim}; }

/* Icon-style tool buttons — quiet until touched, never resized. */
QToolButton {
    background:transparent; color:@{ink};
    border:none; border-radius:@{rS}px;
}
QToolButton:hover   { background:@{hoverWash}; }
QToolButton:pressed { background:@{bgSelected}; }
QToolButton:checked { background:@{accentDim}; color:@{ink}; }
QToolButton::menu-indicator { image:none; }

/* ── Text inputs ── */
QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox,
QDateTimeEdit, QDateEdit, QTimeEdit, QAbstractSpinBox {
    background:@{inputBg}; color:@{ink};
    border:1px solid @{divider}; border-radius:@{rC}px;
    padding:6px 9px; selection-background-color:@{accent};
    selection-color:@{controlInk};
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus,
QSpinBox:focus, QDoubleSpinBox:focus, QDateTimeEdit:focus,
QDateEdit:focus, QTimeEdit:focus, QAbstractSpinBox:focus {
    border-color:@{accent}; background:@{bgHover};
}
QLineEdit:disabled, QPlainTextEdit:disabled, QTextEdit:disabled {
    color:@{inkMuted};
}

/* ── Combo box ── */
QComboBox {
    background:@{inputBg}; color:@{ink};
    border:1px solid @{divider}; border-radius:@{rS}px; padding:5px 9px;
}
QComboBox:hover { border-color:@{accent}; }
QComboBox:on    { border-color:@{accent}; }
QComboBox::drop-down { border:none; width:18px; }
QComboBox QAbstractItemView {
    background:@{bgSurface}; color:@{ink};
    border:1px solid @{divider}; border-radius:@{rS}px;
    selection-background-color:@{bgHover}; selection-color:@{ink};
    outline:none; padding:4px;
}

/* ── Checkable controls ── */
QCheckBox, QRadioButton { background:transparent; color:@{ink}; spacing:8px; }
QCheckBox::indicator, QRadioButton::indicator { width:16px; height:16px; }
/* Unchecked indicator sits on the SURFACE step of the ladder, not on
 * the input-well tone — on tabs whose ground is close to inputBg
 * (Settings → Updates) the well-tinted indicator blends into the
 * background. bgSurface always reads as raised-one-step (Ladder Rule). */
QCheckBox::indicator {
    border:1px solid @{divider}; border-radius:4px; background:@{bgSurface};
}
QRadioButton::indicator {
    border:1px solid @{divider}; border-radius:8px; background:@{bgSurface};
}
QCheckBox::indicator:hover, QRadioButton::indicator:hover {
    border-color:@{accent};
}
QCheckBox::indicator:checked, QRadioButton::indicator:checked {
    background:@{accent}; border-color:@{accent};
}

/* ── Lists / trees / tables ── */
QListWidget, QListView, QTreeView, QTableView, QAbstractItemView {
    background:@{bgSurface}; color:@{ink};
    border:1px solid @{divider}; border-radius:@{rN}px;
    outline:none; alternate-background-color:@{bgSurface};
}
QListWidget::item, QListView::item, QTreeView::item {
    padding:6px 8px; border-radius:@{rS}px;
}
QListWidget::item:hover, QListView::item:hover,
QTreeView::item:hover { background:@{bgHover}; }
QListWidget::item:selected, QListView::item:selected,
QTreeView::item:selected { background:@{bgSelected}; color:@{ink}; }

/* ── Menus ──
 * font-size:13px is the blessed "chrome-dense" size (DESIGN.md Typography) --
 * folded in here from two formerly-bespoke per-instance QMenu overrides in
 * MainWindow.cpp (message context menu, sidebar filter menu) so every
 * right-click menu in the app now reads alike instead of three different
 * ways. item:checked exists because the filter/sort menu's checked entries
 * relied on it; without it here, deleting that menu's own override would
 * have silently dropped the checked-state highlight. */
QMenu {
    background:@{bgSurface}; color:@{ink};
    border:1px solid @{divider}; border-radius:@{rN}px; padding:6px;
}
QMenu::item { padding:7px 28px 7px 22px; border-radius:@{rS}px; font-size:13px; }
QMenu::item:selected { background:@{bgHover}; color:@{ink}; }
QMenu::item:disabled { color:@{inkMuted}; }
QMenu::item:checked { color:@{accent}; font-weight:600; }
QMenu::separator { height:1px; background:@{divider}; margin:6px 8px; }
QMenuBar { background:transparent; color:@{ink}; }
QMenuBar::item:selected { background:@{bgHover}; border-radius:@{rS}px; }

/* ── Scrollbars: calm, thin, warm ── */
QScrollBar:vertical { background:transparent; width:11px; margin:2px; }
QScrollBar::handle:vertical {
    background:@{handle}; border-radius:4px; min-height:28px;
}
QScrollBar::handle:vertical:hover { background:@{inkMuted}; }
QScrollBar:horizontal { background:transparent; height:11px; margin:2px; }
QScrollBar::handle:horizontal {
    background:@{handle}; border-radius:4px; min-width:28px;
}
QScrollBar::handle:horizontal:hover { background:@{inkMuted}; }
QScrollBar::add-line, QScrollBar::sub-line { width:0; height:0; }
QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }

/* ── Misc chrome ── */
QProgressBar {
    background:@{bgSecondary}; color:@{ink};
    border:1px solid @{divider}; border-radius:@{rS}px;
    text-align:center; height:16px;
}
QProgressBar::chunk { background:@{accent}; border-radius:4px; }

QGroupBox {
    border:1px solid @{divider}; border-radius:@{rN}px;
    margin-top:14px; padding-top:6px;
}
QGroupBox::title {
    subcontrol-origin:margin; left:12px; padding:0 4px;
    color:@{ink2}; font-weight:600;
}

QTabWidget::pane { border:1px solid @{divider}; border-radius:@{rN}px; }
QTabBar::tab {
    background:transparent; color:@{ink2};
    padding:7px 16px; border:none;
}
QTabBar::tab:selected { color:@{ink}; border-bottom:2px solid @{accent}; }
QTabBar::tab:hover    { color:@{ink}; }

QFrame[frameShape="4"], QFrame[frameShape="5"] { color:@{divider}; }
)QSS");

    for (auto it = v.constBegin(); it != v.constEnd(); ++it)
        s.replace(QStringLiteral("@{") + it.key() + QLatin1Char('}'),
                  it.value());
    return s;
}

void installRepolishFilter()
{
    static RepolishFilter *filter = nullptr;
    if (filter || !qApp) return;
    filter = new RepolishFilter(qApp);
    qApp->installEventFilter(filter);
}

} // namespace AppStyle
