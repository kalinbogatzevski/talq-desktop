#pragma once

#include <QDialog>
#include <QVector>
#include "core/RoomParticipant.h"

class ApiClient;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

/**
 * Manage an existing Nextcloud Talk room: rename, edit description,
 * add/remove members, leave, or delete.
 *
 * Emits `roomChanged` when changes were made so the caller can refresh the
 * sidebar, and `roomDeleted` when the room was removed or the current user
 * left.
 */
class ConversationInfoDialog : public QDialog
{
    Q_OBJECT
public:
    ConversationInfoDialog(ApiClient *api,
                           const QString &token,
                           const QString &currentName,
                           const QString &currentDescription,
                           int roomType,
                           int myParticipantType,
                           QWidget *parent = nullptr);

signals:
    void roomChanged();   // name/description/members changed
    void roomDeleted();   // room deleted OR current user left

private slots:
    void saveName();
    void saveDescription();
    void onAddMembersToggled();
    void onAddSearchChanged();
    void onAddResultClicked(QListWidgetItem *item);
    void onRemoveMember(QListWidgetItem *item);
    void onLeaveClicked();
    void onDeleteClicked();

private:
    void refreshParticipants();
    void populateParticipants(const QVector<RoomParticipant> &items);
    void runAddSearch();

    ApiClient   *m_api = nullptr;
    QString      m_token;
    int          m_roomType = 0;
    int          m_myType   = 0;
    bool         m_amOwnerOrMod = false;

    QLineEdit   *m_nameEdit = nullptr;
    QLineEdit   *m_descEdit = nullptr;
    QLabel      *m_memberCount = nullptr;
    QListWidget *m_memberList = nullptr;
    QPushButton *m_addBtn = nullptr;

    QWidget     *m_addPanel = nullptr;
    QLineEdit   *m_addSearch = nullptr;
    QListWidget *m_addResults = nullptr;
    class QTimer *m_addDebounce = nullptr;

    QPushButton *m_leaveBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
    QLabel      *m_status = nullptr;
};
