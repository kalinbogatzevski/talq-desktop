#include "ui/SearchFieldClear.h"

#include "painter/VectorIcons.h"

#include <QAction>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QIcon>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QToolButton>

namespace talq::searchclear {
namespace {

constexpr char kActionName[] = "searchFieldClear";
// QLineEdit draws a side action at the style's PM_LineEditIconSize, 16 px.
constexpr int kIconPx = 16;

QPixmap glyph(const QColor &ink, qreal dpr)
{
    QPixmap pm(QSize(kIconPx, kIconPx) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    VectorIcons::draw(p, QStringLiteral("close"), QRectF(0, 0, kIconPx, kIconPx), ink);
    p.end();
    return pm;
}

// Baked at 1x and 2x, plus QGuiApplication::devicePixelRatio() when that is neither. That call
// returns the HIGHEST ratio among the attached screens, so the extra pixmap only covers a lone
// odd ratio (a single 1.5x or 3x screen). A 1.5x screen next to a 2x one gets no 1.5x pixmap:
// QIcon draws the nearest one (2x) scaled down there.
QIcon makeIcon(const PainterTheme &theme)
{
    const Colors c = colorsFor(theme);
    QList<qreal> ratios = { 1.0, 2.0 };
    if (qGuiApp && !ratios.contains(qGuiApp->devicePixelRatio()))
        ratios << qGuiApp->devicePixelRatio();
    QIcon icon;
    for (qreal dpr : ratios) {
        icon.addPixmap(glyph(c.glyph, dpr), QIcon::Normal);
        icon.addPixmap(glyph(c.glyphPressed, dpr), QIcon::Active);   // QLineEdit: "pressed"
    }
    return icon;
}

QAction *clearAction(QLineEdit *field)
{
    return field ? field->findChild<QAction *>(QLatin1String(kActionName),
                                               Qt::FindDirectChildrenOnly)
                 : nullptr;
}

// Esc with text in the field clears it; Esc in an empty field is not ours. ShortcutOverride is
// claimed too, so a window-wide Esc shortcut does not take the key from a field with text.
class EscClears : public QObject
{
public:
    explicit EscClears(QLineEdit *field) : QObject(field), m_field(field) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != m_field
            || (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride))
            return false;
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() != Qt::Key_Escape || key->modifiers() != Qt::NoModifier
            || m_field->text().isEmpty())
            return false;
        if (event->type() == QEvent::KeyPress)
            m_field->clear();
        event->accept();
        return true;
    }

private:
    QLineEdit *m_field;
};

} // namespace

void attach(QLineEdit *field, const PainterTheme &theme)
{
    if (!field || clearAction(field))
        return;
    auto *clear = new QAction(field);
    clear->setObjectName(QLatin1String(kActionName));
    clear->setIcon(makeIcon(theme));
    clear->setToolTip(QCoreApplication::translate("SearchFieldClear", "Clear search (Esc)"));
    field->addAction(clear, QLineEdit::TrailingPosition);
    // AFTER addAction: the field's button for the action follows a CHANGE of the action's
    // visibility. An action added already hidden left its button showing on an empty field.
    clear->setVisible(!field->text().isEmpty());
    QObject::connect(field, &QLineEdit::textChanged, clear,
                     [clear](const QString &text) { clear->setVisible(!text.isEmpty()); });
    QObject::connect(clear, &QAction::triggered, field, [field]() {
        field->clear();
        field->setFocus(Qt::MouseFocusReason);
    });
    // The button QLineEdit made for the action: a pointing hand says it can be clicked (the
    // field's own I-beam would say "type here").
    for (QObject *o : clear->associatedObjects())
        if (auto *button = qobject_cast<QToolButton *>(o))
            button->setCursor(Qt::PointingHandCursor);
    field->installEventFilter(new EscClears(field));
}

void retint(QLineEdit *field, const PainterTheme &theme)
{
    if (QAction *clear = clearAction(field))
        clear->setIcon(makeIcon(theme));
}

QString sidebarPillStyleSheet(const PainterTheme &t)
{
    auto hx = [](const QColor &c) { return c.name(QColor::HexRgb); };
    return QString(
        "QLineEdit{background:%1;border:1px solid %2;border-radius:16px;"
        "padding:4px 14px;font-size:13px;color:%3;}"
        "QLineEdit:focus{border-color:%4;background:%5;}")
        .arg(hx(t.bgSecondary), hx(t.divider), hx(t.textPrimary),
             hx(t.accent), hx(t.bgHover));
}

} // namespace talq::searchclear
