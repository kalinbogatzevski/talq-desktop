#include "ConversationPickerDialog.h"
#include "models/ConversationListModel.h"

ConversationPickerDialog::ConversationPickerDialog(ConversationListModel *model,
                                                     const QString &excludeToken,
                                                     QWidget *parent)
    : QDialog(parent)
    , m_model(model)
    , m_excludeToken(excludeToken)
{
    setWindowTitle("Forward to...");
    setFixedSize(380, 480);
    setStyleSheet(
        "QDialog { background: #1e1e2e; }"
        "QLineEdit { background: #2a2a3e; color: #e0e0e0; border: 1px solid #363c48;"
        "  border-radius: 8px; padding: 8px 12px; font-size: 13px; }"
        "QLineEdit:focus { border-color: #2ec4b6; }"
        "QListWidget { background: transparent; border: none; outline: none; }"
        "QListWidget::item { color: #e0e0e0; padding: 10px 12px; border-radius: 8px;"
        "  font-size: 13px; }"
        "QListWidget::item:hover { background: rgba(255,255,255,0.06); }"
        "QListWidget::item:selected { background: rgba(46,196,182,0.15); }"
    );

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    m_searchField = new QLineEdit(this);
    m_searchField->setPlaceholderText("Search conversations...");
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
