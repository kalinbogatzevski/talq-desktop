#include "SelectionBarWidget.h"
#include "painter/VectorIcons.h"

#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

namespace {
// Bake a VectorIcons glyph into a QIcon tinted with the given colour. The
// four selection-bar buttons used to carry a colour-emoji glyph in their
// text (up-right arrow, clipboard, wastebasket, all with a VS16 emoji
// presentation selector) -- Segoe UI Emoji renders those from its own fixed
// COLR/CPAL palette and ignores the QPainter pen/QSS colour entirely, so
// they never tinted with the theme the way every other icon in the app
// does. VectorIcons draws a plain stroked/filled path instead, which the
// caller tints explicitly, same idiom as TalqIconButton and CallStage.
QIcon vectorIcon(const QString &id, const QColor &color, int px)
{
    const qreal dpr = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
    QPixmap pm(QSize(px, px) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    VectorIcons::draw(p, id, QRectF(0, 0, px, px), color);
    p.end();
    return QIcon(pm);
}
}

SelectionBarWidget::SelectionBarWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 8, 16, 8);
    layout->setSpacing(6);

    // Surface + controls inherit the app-wide AppStyle sheet (theme-driven).
    setObjectName(QStringLiteral("selectionBar"));

    m_countLabel = new QLabel(this);
    m_countLabel->setProperty("role", "title");
    layout->addWidget(m_countLabel);
    layout->addStretch();

    auto makeBtn = [this, layout](const QString &text, const char *variant) {
        auto *btn = new QPushButton(text, this);
        btn->setCursor(Qt::PointingHandCursor);
        if (variant) btn->setProperty("variant", variant);
        btn->setIconSize(QSize(16, 16));
        layout->addWidget(btn);
        return btn;
    };

    // Forward/Copy are standalone secondary actions (exactly the
    // "Cancel / Done / Refresh / Edit" case AppStyle.cpp's own comment names
    // for variant="default") but were shipping with no variant at all, so
    // they fell through to the base QPushButton rule -- transparent, no
    // border -- and read as plain text until hovered.
    // Wrapped for translation in 0.65.3 (the deferred "Slice C" item): these
    // are four visible buttons, about as user-facing as a string gets.
    m_forwardBtn = makeBtn(tr("Forward"), "default");
    m_copyBtn = makeBtn(tr("Copy"), "default");
    m_deleteBtn = makeBtn(tr("Delete"), "danger");
    m_cancelBtn = makeBtn(tr("Cancel"), "ghost");

    connect(m_forwardBtn, &QPushButton::clicked, this, &SelectionBarWidget::forwardClicked);
    connect(m_copyBtn, &QPushButton::clicked, this, &SelectionBarWidget::copyClicked);
    connect(m_deleteBtn, &QPushButton::clicked, this, &SelectionBarWidget::deleteClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &SelectionBarWidget::cancelClicked);

    setCount(0);
}

void SelectionBarWidget::retheme(const PainterTheme &t)
{
    // Ink per button matches the colour its OWN variant's QSS text already
    // renders in (AppStyle.cpp), so the icon and label read as one colour:
    // default -> ink (textPrimary), danger -> danger, ghost -> ink2 (textSecondary).
    constexpr int kIconPx = 16;
    if (m_forwardBtn) m_forwardBtn->setIcon(vectorIcon(QStringLiteral("forward"), t.textPrimary, kIconPx));
    if (m_copyBtn)    m_copyBtn->setIcon(vectorIcon(QStringLiteral("copy"), t.textPrimary, kIconPx));
    if (m_deleteBtn)  m_deleteBtn->setIcon(vectorIcon(QStringLiteral("trash"), t.danger, kIconPx));
    if (m_cancelBtn)  m_cancelBtn->setIcon(vectorIcon(QStringLiteral("close"), t.textSecondary, kIconPx));
}

void SelectionBarWidget::setCount(int count)
{
    m_countLabel->setText(tr("%n message(s) selected", nullptr, count));
}

void SelectionBarWidget::setDeleteVisible(bool visible)
{
    m_deleteBtn->setVisible(visible);
}
