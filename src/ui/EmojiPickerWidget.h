#pragma once

#include <QWidget>
#include <QVector>
#include <QHash>

class QLineEdit;
class QScrollArea;
class QVBoxLayout;

namespace EmojiData { struct EmojiEntry; enum class Category; }

class EmojiCell;

class EmojiPickerWidget : public QWidget
{
    Q_OBJECT
public:
    explicit EmojiPickerWidget(QWidget *parent = nullptr);

signals:
    void emojiSelected(const QString &codepoints);
    void cancelled();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *e) override;   // re-theme on palette change

private:
    void applyChrome();   // widget bg, palette-driven
    void rebuild();
    void onCellClicked(const EmojiData::EmojiEntry *e);
    void scrollToCategory(EmojiData::Category c);

    QLineEdit *m_search = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_gridHost = nullptr;
    QVBoxLayout *m_gridLayout = nullptr;
    QString m_filter;
    QHash<int, QWidget*> m_sectionAnchors;  // keyed by int(Category)
};
