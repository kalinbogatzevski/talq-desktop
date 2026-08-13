#include "ConversationPickerDialog.h"
#include "models/ConversationListModel.h"

ConversationPickerDialog::ConversationPickerDialog(ConversationListModel *model,
                                                     const QString &excludeToken,
                                                     QWidget *parent)
    : QDialog(parent)
    , m_model(model)
    , m_excludeToken(excludeToken)
{
    setWindowTitle(tr("Forward to..."));
    setFixedSize(380, 480);
    // Inherits the themed app stylesheet (AppStyle) — no hardcoded colours, so
    // it tracks all four themes, including the light (Paper) theme.

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    m_searchField = new QLineEdit(this);
    m_searchField->setPlaceholderText(tr("Search conversations..."));
    layout->addWidget(m_searchField);

    m_list = new QListWidget(this);
    layout->addWidget(m_list);

    populateList();

    connect(m_searchField, &QLineEdit::textChanged, this, &ConversationPickerDialog::populateList);
    auto selectItem = [this](QListWidgetItem *item) {
        m_selectedToken = item->data(Qt::UserRole).toString();
        m_selectedName = item->text();
        accept();
    };
    connect(m_list, &QListWidget::itemClicked, this, selectItem);
    connect(m_list, &QListWidget::itemActivated, this, selectItem);
}

void ConversationPickerDialog::populateList(const QString &filter)
{
    m_list->clear();
    int count = m_model->rowCount();
    for (int i = 0; i < count; ++i) {
        QModelIndex idx = m_model->index(i, 0);
        QString token = idx.data(ConversationListModel::TokenRole).toString();
        if (token == m_excludeToken)
            continue;
        QString name = idx.data(ConversationListModel::DisplayNameRole).toString();
        if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive))
            continue;
        auto *item = new QListWidgetItem(name, m_list);
        item->setData(Qt::UserRole, token);
    }
}
