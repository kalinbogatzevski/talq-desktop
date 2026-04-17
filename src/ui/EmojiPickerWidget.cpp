#include "EmojiPickerWidget.h"

#include "core/EmojiData.h"

#include <QAbstractButton>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

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
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        if (underMouse()) {
            p.fillRect(rect(), QColor(255,255,255,20));
        }
        QPixmap pm = EmojiData::pixmapFor(m_entry->codepoints, 32);
        if (!pm.isNull())
            p.drawPixmap((width()-32)/2, (height()-32)/2, pm);
    }
private:
    const EmojiData::EmojiEntry *m_entry;
};

EmojiPickerWidget::EmojiPickerWidget(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(400, 360);
    setStyleSheet("EmojiPickerWidget { background: #1e1e2a; border-radius: 10px; }");

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search emoji…"));
    m_search->setStyleSheet("QLineEdit { background: #2a2a3a; color: #eee; padding: 6px 10px; border: none; border-radius: 6px; }");
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
        btn->setStyleSheet("QPushButton { background: transparent; font-size: 18px; color: #eee; } QPushButton:hover { background: rgba(255,255,255,0.08); border-radius: 6px; }");
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

void EmojiPickerWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) { emit emojiSelected(QString()); close(); return; }
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
        hdr->setStyleSheet("color: #888; font-size: 11px; font-weight: 600; padding: 6px 2px;");
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
        addSection(tr("Results"), EmojiData::search(m_filter), -1);
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
