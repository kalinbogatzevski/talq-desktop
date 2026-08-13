#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>

#include "painter/PainterTheme.h"

class SelectionBarWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SelectionBarWidget(QWidget *parent = nullptr);

    void setCount(int count);
    void setDeleteVisible(bool visible);
    // Re-tint the four VectorIcons icons to the current theme. Called once
    // from MainWindow::restyleChrome() at construction and again on every
    // theme switch (same as m_uploadBar/m_searchField/m_filterMenu there) --
    // the icons are baked QIcons, not QSS-driven, so they need an explicit
    // repaint hook the plain-text buttons this widget used to have never did.
    void retheme(const PainterTheme &t);

signals:
    void forwardClicked();
    void copyClicked();
    void deleteClicked();
    void cancelClicked();

private:
    QLabel *m_countLabel = nullptr;
    QPushButton *m_forwardBtn = nullptr;
    QPushButton *m_copyBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    QPushButton *m_cancelBtn = nullptr;
};
