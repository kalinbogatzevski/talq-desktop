#include "EmojiPickerWidget.h"

#include "core/EmojiData.h"

#include <QAbstractButton>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QEvent>
#include <QPalette>

class EmojiCell : public QAbstractButton
{
    Q_OBJECT
public:
    EmojiCell(const EmojiData::EmojiEntry *e, QWidget *parent)
        : QAbstractButton(parent), m_entry(e)
    {
        setFixedSize(40, 40);
        setCursor(Qt::PointingHandCursor);
        setToolTip(e->shortcodes.isEmpty() ? e->name : e->shortcodes.first());
    }
    const EmojiData::EmojiEntry *entry() const { return m_entry; }

signals:
    void variantChosen(const QString &codepoints);

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        if (underMouse()) {
            // Theme-driven hover wash: a low-alpha accent tint reads as a soft
            // hover state on every theme (the old hardcoded white vanished on
            // the light Paper surface). Accent = QPalette::Highlight here.
            QColor wash = palette().color(QPalette::Highlight);
            wash.setAlpha(36);
            p.fillRect(rect(), wash);
        }
        QPixmap pm = EmojiData::pixmapFor(m_entry->codepoints, 32);
        if (!pm.isNull())
            p.drawPixmap((width()-32)/2, (height()-32)/2, pm);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_pressTimer.restart();
        QAbstractButton::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_entry->skinToneVariants.isEmpty() && m_pressTimer.elapsed() > 300) {
            showVariants(event->globalPosition().toPoint());
            return;   // swallow the click — user is browsing variants
        }
        QAbstractButton::mouseReleaseEvent(event);
    }
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (!m_entry->skinToneVariants.isEmpty())
            showVariants(event->globalPos());
    }

private:
    void showVariants(const QPoint &globalPos)
    {
        auto *popup = new QWidget(this, Qt::Popup);
        popup->setAttribute(Qt::WA_DeleteOnClose);
        popup->setStyleSheet(QStringLiteral(
            "background: %1; border: 1px solid %2; border-radius: 6px;"
            " padding: 4px;")
            .arg(palette().color(QPalette::AlternateBase).name(),
                 palette().color(QPalette::Mid).name()));
        auto *lay = new QHBoxLayout(popup);
        lay->setContentsMargins(6, 6, 6, 6);
        lay->setSpacing(4);

        auto addBtn = [this, lay, popup](const QString &cp) {
            auto *b = new QPushButton(popup);
            b->setFixedSize(34, 34);
            b->setFlat(true);
            b->setCursor(Qt::PointingHandCursor);
            QPixmap pm = EmojiData::pixmapFor(cp, 28);
            if (!pm.isNull()) b->setIcon(QIcon(pm));
            b->setIconSize(QSize(28, 28));
            connect(b, &QPushButton::clicked, this, [this, cp, popup]() {
                emit variantChosen(cp);
                popup->close();
            });
            lay->addWidget(b);
        };
        addBtn(m_entry->codepoints);           // base (no tone)
        for (const QString &v : m_entry->skinToneVariants) addBtn(v);

        popup->adjustSize();
        popup->move(globalPos - QPoint(popup->width()/2, popup->height() + 8));
        popup->show();
    }

    const EmojiData::EmojiEntry *m_entry;
    QElapsedTimer m_pressTimer;
};

EmojiPickerWidget::EmojiPickerWidget(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(400, 360);
    applyChrome();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search emoji…"));
    // Search field inherits the app-wide AppStyle QLineEdit (theme-driven).
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_filter = t;
        rebuild();
    });
    root->addWidget(m_search);

    // Category tab row
    auto *tabRow = new QWidget(this);
    tabRow->setStyleSheet("background: transparent;");
    auto *tabLayout = new QHBoxLayout(tabRow);
    tabLayout->setContentsMargins(0, 0, 0, 0);
    tabLayout->setSpacing(2);

    struct Tab { const char *emoji; EmojiData::Category cat; };
    const Tab tabs[] = {
        {"\xF0\x9F\x98\x80", EmojiData::Category::Smileys},    // 😀
        {"\xF0\x9F\x91\x8B", EmojiData::Category::People},     // 👋
        {"\xF0\x9F\x90\xB6", EmojiData::Category::Animals},    // 🐶
        {"\xF0\x9F\x8D\x8E", EmojiData::Category::Food},       // 🍎
        {"\xE2\x9A\xBD",     EmojiData::Category::Activities}, // ⚽
        {"\xE2\x9C\x88",     EmojiData::Category::Travel},     // ✈
        {"\xF0\x9F\x92\xA1", EmojiData::Category::Objects},    // 💡
        {"\xE2\x9D\xA4",     EmojiData::Category::Symbols},    // ❤
        {"\xF0\x9F\x8F\xB3", EmojiData::Category::Flags},      // 🏳
    };
    for (const auto &t : tabs) {
        auto *btn = new QPushButton(QString::fromUtf8(t.emoji), tabRow);
        btn->setFlat(true);
        btn->setFixedSize(34, 30);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setProperty("variant", "ghost");
        btn->setStyleSheet(QStringLiteral("QPushButton { font-size: 18px; }"));
        auto cat = t.cat;
        connect(btn, &QPushButton::clicked, this, [this, cat]() { scrollToCategory(cat); });
        tabLayout->addWidget(btn);
    }
    tabLayout->addStretch();
    root->addWidget(tabRow);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setStyleSheet("QScrollArea { background: transparent; } QScrollBar:vertical { width: 6px; }");

    m_gridHost = new QWidget;
    m_gridHost->setStyleSheet("background: transparent;");
    m_gridLayout = new QVBoxLayout(m_gridHost);
    m_gridLayout->setContentsMargins(0, 0, 0, 0);
    m_gridLayout->setSpacing(4);
    m_scroll->setWidget(m_gridHost);
    root->addWidget(m_scroll, 1);

    rebuild();
}

void EmojiPickerWidget::applyChrome()
{
    // Frameless popover card — theme-driven via the app palette.
    setStyleSheet(QStringLiteral(
        "EmojiPickerWidget { background: %1; border: 1px solid %2;"
        " border-radius: 10px; }")
        .arg(palette().color(QPalette::Window).name(),
             palette().color(QPalette::Mid).name()));
}

void EmojiPickerWidget::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    // ApplicationPaletteChange only — PaletteChange recurses via setStyleSheet.
    if (e->type() == QEvent::ApplicationPaletteChange
        || e->type() == QEvent::ThemeChange) {
        applyChrome();
        rebuild();
    }
}

void EmojiPickerWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) { emit cancelled(); close(); return; }
    QWidget::keyPressEvent(event);
}

void EmojiPickerWidget::onCellClicked(const EmojiData::EmojiEntry *e)
{
    EmojiData::pushRecent(e);
    emit emojiSelected(e->codepoints);
}

void EmojiPickerWidget::rebuild()
{
    // Clear existing
    while (auto *child = m_gridLayout->takeAt(0)) {
        if (auto *w = child->widget()) w->deleteLater();
        delete child;
    }
    m_sectionAnchors.clear();

    auto addSection = [this](const QString &title,
                             const QVector<const EmojiData::EmojiEntry*> &items,
                             int categoryKey) {
        if (items.isEmpty()) return;
        auto *hdr = new QLabel(title, m_gridHost);
        hdr->setProperty("role", "secondary");
        hdr->setStyleSheet(QStringLiteral(
            "font-size: 11px; font-weight: 600; padding: 6px 2px;"));
        m_gridLayout->addWidget(hdr);
        if (categoryKey >= 0)
            m_sectionAnchors.insert(categoryKey, hdr);
        auto *rowHost = new QWidget(m_gridHost);
        auto *grid = new QGridLayout(rowHost);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(2);
        const int cols = 9;
        for (int i = 0; i < items.size(); ++i) {
            auto *cell = new EmojiCell(items[i], rowHost);
            connect(cell, &QAbstractButton::clicked, this, [this, cell]() { onCellClicked(cell->entry()); });
            connect(cell, &EmojiCell::variantChosen, this, [this](const QString &cp) {
                // Emit directly; skip pushRecent for variants (recent tracks base entries only).
                emit emojiSelected(cp);
            });
            grid->addWidget(cell, i / cols, i % cols);
        }
        m_gridLayout->addWidget(rowHost);
    };

    if (m_filter.trimmed().isEmpty()) {
        addSection(tr("Recently Used"), EmojiData::recent(), -1);
        using C = EmojiData::Category;
        addSection(tr("Smileys & Emotion"), EmojiData::inCategory(C::Smileys),    int(C::Smileys));
        addSection(tr("People & Body"),     EmojiData::inCategory(C::People),     int(C::People));
        addSection(tr("Animals & Nature"),  EmojiData::inCategory(C::Animals),    int(C::Animals));
        addSection(tr("Food & Drink"),      EmojiData::inCategory(C::Food),       int(C::Food));
        addSection(tr("Activities"),        EmojiData::inCategory(C::Activities), int(C::Activities));
        addSection(tr("Travel & Places"),   EmojiData::inCategory(C::Travel),     int(C::Travel));
        addSection(tr("Objects"),           EmojiData::inCategory(C::Objects),    int(C::Objects));
        addSection(tr("Symbols"),           EmojiData::inCategory(C::Symbols),    int(C::Symbols));
        addSection(tr("Flags"),             EmojiData::inCategory(C::Flags),      int(C::Flags));
    } else {
        const auto results = EmojiData::search(m_filter);
        if (results.isEmpty()) {
            // A gibberish search used to leave a totally blank scroll area —
            // indistinguishable from the picker being broken. Centred muted
            // label, same "role=secondary" idiom as the section headers above.
            // Leading stretch (paired with the trailing one added below)
            // centers it vertically in the viewport, matching the brief's
            // "centred" and ThreadsPainter::paintEmptyState's vertical
            // mid-point placement rather than pinning it to the top.
            m_gridLayout->addStretch();
            auto *empty = new QLabel(tr("No emoji found"), m_gridHost);
            empty->setAlignment(Qt::AlignCenter);
            empty->setProperty("role", "secondary");
            empty->setStyleSheet(QStringLiteral("padding: 24px;"));
            m_gridLayout->addWidget(empty);
        } else {
            addSection(tr("Results"), results, -1);
        }
    }
    m_gridLayout->addStretch();
}

void EmojiPickerWidget::scrollToCategory(EmojiData::Category c)
{
    auto it = m_sectionAnchors.constFind(int(c));
    if (it == m_sectionAnchors.constEnd()) return;
    m_scroll->ensureWidgetVisible(it.value(), 0, 0);
}

#include "EmojiPickerWidget.moc"
