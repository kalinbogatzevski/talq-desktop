#pragma once

#include <QDialog>
#include <QVector>
#include "core/NcUser.h"

class ApiClient;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QTimer;
class QWidget;
class QHBoxLayout;

/**
 * Create a new Nextcloud Talk room.
 *
 * Direct tab — one click on a person starts a 1-on-1.
 * Group tab  — name the group; added people show as removable chips
 *              above the search. Click a result to add (row gets a
 *              ✓), click a chip to remove.
 */
class NewChatDialog : public QDialog
{
    Q_OBJECT
public:
    explicit NewChatDialog(ApiClient *api, QWidget *parent = nullptr);

    QString createdToken() const { return m_createdToken; }

protected:
    void changeEvent(QEvent *e) override;   // re-theme on palette change

private slots:
    void onResultClicked(QListWidgetItem *item);
    void onCreateClicked();

private:
    void applyChrome();   // dialog typography/inputs, palette-driven
    void setMode(bool group);
    void runSearch();
    void rebuildChips();
    void refreshCreateEnabled();
    void addResultRow(const NcUser &u);
    void replaceResultRow(QListWidgetItem *item, const NcUser &u);
    bool isPicked(const QString &id) const;

    ApiClient    *m_api = nullptr;
    QPushButton  *m_directTab = nullptr;
    QPushButton  *m_groupTab  = nullptr;

    QWidget      *m_groupNameBlock = nullptr;
    QLineEdit    *m_groupNameEdit  = nullptr;

    QWidget      *m_chipsBlock   = nullptr;
    QLabel       *m_chipsEyebrow = nullptr;
    QScrollArea  *m_chipsScroll  = nullptr;
    QWidget      *m_chipsHost    = nullptr;
    QHBoxLayout  *m_chipsLayout  = nullptr;

    QLineEdit    *m_searchEdit = nullptr;
    QListWidget  *m_results    = nullptr;
    QLabel       *m_status     = nullptr;
    QPushButton  *m_createBtn  = nullptr;
    QPushButton  *m_cancelBtn  = nullptr;
    QTimer       *m_searchDebounce = nullptr;

    QVector<NcUser> m_selected;
    QString         m_createdToken;
};
