#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

class ConversationListModel;

class ConversationPickerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ConversationPickerDialog(ConversationListModel *model,
                                       const QString &excludeToken,
                                       QWidget *parent = nullptr);

    QString selectedToken() const { return m_selectedToken; }
    QString selectedName() const { return m_selectedName; }

private:
    void populateList(const QString &filter = {});

    ConversationListModel *m_model;
    QString m_excludeToken;
    QLineEdit *m_searchField = nullptr;
    QListWidget *m_list = nullptr;
    QString m_selectedToken;
    QString m_selectedName;
};
