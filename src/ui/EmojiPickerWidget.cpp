#include "EmojiPickerWidget.h"

#include "core/EmojiData.h"

#include <QAbstractButton>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
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

    auto addSection = [this](const QString &title, const QVector<const EmojiData::EmojiEntry*> &items) {
        if (items.isEmpty()) return;
        auto *hdr = new QLabel(title, m_gridHost);
        hdr->setStyleSheet("color: #888; font-size: 11px; font-weight: 600; padding: 6px 2px;");
        m_gridLayout->addWidget(hdr);
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
        addSection(tr("Recently Used"), EmojiData::recent());
        using C = EmojiData::Category;
        addSection(tr("Smileys & Emotion"), EmojiData::inCategory(C::Smileys));
        addSection(tr("People & Body"),     EmojiData::inCategory(C::People));
        addSection(tr("Animals & Nature"),  EmojiData::inCategory(C::Animals));
        addSection(tr("Food & Drink"),      EmojiData::inCategory(C::Food));
        addSection(tr("Activities"),        EmojiData::inCategory(C::Activities));
        addSection(tr("Travel & Places"),   EmojiData::inCategory(C::Travel));
        addSection(tr("Objects"),           EmojiData::inCategory(C::Objects));
        addSection(tr("Symbols"),           EmojiData::inCategory(C::Symbols));
        addSection(tr("Flags"),             EmojiData::inCategory(C::Flags));
    } else {
        addSection(tr("Results"), EmojiData::search(m_filter));
    }
    m_gridLayout->addStretch();
}

#include "EmojiPickerWidget.moc"
