#pragma once

#include <QDialog>
#include <QVector>
#include "core/RoomParticipant.h"
#include "core/BotInfo.h"

class ApiClient;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QWidget;
class QVBoxLayout;

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

protected:
    void changeEvent(QEvent *e) override;   // re-theme on palette change

private slots:
    void saveName();
    void saveDescription();
    void onAddMembersToggled();
    void onAddSearchChanged();
    void onAddResultClicked(QListWidgetItem *item);
    void onRemoveMember(QListWidgetItem *item);
    void onLeaveClicked();
    void onDeleteClicked();
    void onClearHistoryClicked();
    void onAddBotClicked();
    void onChangeAvatar();   // #25 — pick + upload a group picture

private:
    void applyChrome();   // dialog typography/inputs, palette-driven
    void refreshParticipants();
    void populateParticipants(const QVector<RoomParticipant> &items);
    void runAddSearch();
    void refreshBots();
    void populateBotRow(const BotInfo &bot);
    void refreshSharedFiles();

    ApiClient   *m_api = nullptr;
    QString      m_token;
    int          m_roomType = 0;
    int          m_myType   = 0;
    bool         m_amOwnerOrMod = false;

    QLabel      *m_avatar = nullptr;            // #25 — group picture
    QPushButton *m_changeAvatarBtn = nullptr;   // #25
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
    QPushButton *m_clearHistoryBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
    QLabel      *m_status = nullptr;

    // Bots panel (everyone can view; only moderators see the +Add control).
    // Plain QWidget + QVBoxLayout, not a QListWidget — list-widget rows take
    // their width from sizeHint() which gives a too-narrow row when the bot
    // name is short, clipping any extras (a "Remove" button, a state label).
    QWidget     *m_botsContainer = nullptr;
    QVBoxLayout *m_botsLayout = nullptr;
    QPushButton *m_addBotBtn = nullptr;
    QLabel      *m_botsHeader = nullptr;
    int          m_botsRefreshSeq = 0;   // guards async-callback races

    // Shared files: a flat read-only list built from one recent chat page.
    // No thumbnails and no MessageListModel — just filename, size and a
    // click-to-open link, kept deliberately lightweight.
    QLabel      *m_filesHeader = nullptr;
    QListWidget *m_filesList = nullptr;
};
