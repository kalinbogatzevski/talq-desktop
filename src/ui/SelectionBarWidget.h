#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>

class SelectionBarWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SelectionBarWidget(QWidget *parent = nullptr);

    void setCount(int count);
    void setDeleteVisible(bool visible);

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
