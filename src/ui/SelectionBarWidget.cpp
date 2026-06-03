#include "SelectionBarWidget.h"

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
        layout->addWidget(btn);
        return btn;
    };

    m_forwardBtn = makeBtn(QStringLiteral("\u2197\uFE0F Forward"), nullptr);
    m_copyBtn = makeBtn(QStringLiteral("\U0001F4CB Copy"), nullptr);
    m_deleteBtn = makeBtn(QStringLiteral("\U0001F5D1\uFE0F Delete"), "danger");
    m_cancelBtn = makeBtn(QStringLiteral("\u2715 Cancel"), "ghost");

    connect(m_forwardBtn, &QPushButton::clicked, this, &SelectionBarWidget::forwardClicked);
    connect(m_copyBtn, &QPushButton::clicked, this, &SelectionBarWidget::copyClicked);
    connect(m_deleteBtn, &QPushButton::clicked, this, &SelectionBarWidget::deleteClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &SelectionBarWidget::cancelClicked);

    setCount(0);
}

void SelectionBarWidget::setCount(int count)
{
    m_countLabel->setText(tr("%n message(s) selected", nullptr, count));
}

void SelectionBarWidget::setDeleteVisible(bool visible)
{
    m_deleteBtn->setVisible(visible);
}
