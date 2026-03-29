#include "SelectionBarWidget.h"

SelectionBarWidget::SelectionBarWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 8, 16, 8);
    layout->setSpacing(6);

    setStyleSheet("background: #252536;");

    m_countLabel = new QLabel(this);
    m_countLabel->setStyleSheet("color: #2ec4b6; font-weight: 600; font-size: 13px; background: transparent;");
    layout->addWidget(m_countLabel);
    layout->addStretch();

    auto makeBtn = [this](const QString &text, const QString &style) {
        auto *btn = new QPushButton(text, this);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(style);
        return btn;
    };

    m_forwardBtn = makeBtn(QStringLiteral("\u2197\uFE0F Forward"),
        "QPushButton { background: #2a2a3e; color: #e0e0e0; border: none; border-radius: 8px; padding: 7px 14px; font-size: 12px; }"
        "QPushButton:hover { background: #353550; }");
    layout->addWidget(m_forwardBtn);

    m_copyBtn = makeBtn(QStringLiteral("\U0001F4CB Copy"),
        "QPushButton { background: #2a2a3e; color: #e0e0e0; border: none; border-radius: 8px; padding: 7px 14px; font-size: 12px; }"
        "QPushButton:hover { background: #353550; }");
    layout->addWidget(m_copyBtn);

    m_deleteBtn = makeBtn(QStringLiteral("\U0001F5D1\uFE0F Delete"),
        "QPushButton { background: rgba(248,81,73,0.15); color: #f85149; border: none; border-radius: 8px; padding: 7px 14px; font-size: 12px; }"
        "QPushButton:hover { background: rgba(248,81,73,0.25); }");
    layout->addWidget(m_deleteBtn);

    m_cancelBtn = makeBtn(QStringLiteral("\u2715 Cancel"),
        "QPushButton { background: #2a2a3e; color: #888; border: none; border-radius: 8px; padding: 7px 14px; font-size: 12px; }"
        "QPushButton:hover { background: #353550; color: #bbb; }");
    layout->addWidget(m_cancelBtn);

    connect(m_forwardBtn, &QPushButton::clicked, this, &SelectionBarWidget::forwardClicked);
    connect(m_copyBtn, &QPushButton::clicked, this, &SelectionBarWidget::copyClicked);
    connect(m_deleteBtn, &QPushButton::clicked, this, &SelectionBarWidget::deleteClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &SelectionBarWidget::cancelClicked);

    setCount(0);
}

void SelectionBarWidget::setCount(int count)
{
    m_countLabel->setText(QString("%1 message%2 selected")
        .arg(count).arg(count == 1 ? "" : "s"));
}

void SelectionBarWidget::setDeleteVisible(bool visible)
{
    m_deleteBtn->setVisible(visible);
}
