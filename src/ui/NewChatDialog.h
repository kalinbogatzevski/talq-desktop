#pragma once

#include <QDialog>
#include <QVector>
#include "core/NcUser.h"

class ApiClient;
class QButtonGroup;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QRadioButton;
class QTimer;
class QWidget;

/**
 * Dialog for creating a new Nextcloud Talk room. Two modes: Direct (pick a
 * single user → one-to-one room) and Group (name + multi-select users →
 * private group).
 *
 * On accept, the created room's token is available via createdToken();
 * caller opens that conversation.
 */
class NewChatDialog : public QDialog
{
    Q_OBJECT
public:
    explicit NewChatDialog(ApiClient *api, QWidget *parent = nullptr);

    QString createdToken() const { return m_createdToken; }

private slots:
    void onModeChanged();
    void onSearchTextChanged();
    void onResultDoubleClicked(QListWidgetItem *item);
    void onCreateClicked();

private:
    void runSearch();
    void refreshSelectedView();
    void setStatus(const QString &text, bool isError = false);

    ApiClient    *m_api = nullptr;
    QRadioButton *m_directRadio = nullptr;
    QRadioButton *m_groupRadio = nullptr;
    QWidget      *m_groupNameRow = nullptr;
    QLineEdit    *m_groupNameEdit = nullptr;
    QLineEdit    *m_searchEdit = nullptr;
    QListWidget  *m_results = nullptr;
    QLabel       *m_selectedLabel = nullptr;
    QPushButton  *m_createBtn = nullptr;
    QPushButton  *m_cancelBtn = nullptr;
    QLabel       *m_status = nullptr;
    QTimer       *m_searchDebounce = nullptr;

    // Selected users (Direct: exactly one; Group: any number).
    QVector<NcUser> m_selected;

    QString       m_createdToken;
};
