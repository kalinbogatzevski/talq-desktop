#pragma once

#include <QDialog>
#include <QVector>
#include "core/RoomParticipant.h"
#include "core/BotInfo.h"
#include "core/NcUser.h"

class ApiClient;
class ImageViewerDialog;
class ShiftStatusService;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMargins;
class QPushButton;
class QScreen;
class QScrollArea;
class QStackedWidget;
class QWidget;
class QVBoxLayout;
class QProgressBar;

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
    // tests/conversation_info_dialog_test.cpp: fills the dialog through the
    // real row builders and drives the add-people handlers without a server.
    friend struct ConversationInfoDialogTestPeer;
public:
    // Optional. Absent means rows render exactly as they do today. This is
    // the only surface that can carry shift state for a GROUP room.
    void setShiftStatus(ShiftStatusService *shiftStatus);

    ConversationInfoDialog(ApiClient *api,
                           const QString &token,
                           const QString &currentName,
                           const QString &currentDescription,
                           int roomType,
                           int myParticipantType,
                           QWidget *parent = nullptr);

    // SIP dial-in, if the room has it. `sipEnabled` is the room's state
    // (0 off, 1 on, 2 on without a per-user PIN) and `pin` is THIS user's
    // dial-in code. Called by the owner, which reads both from the
    // conversation list; a room without SIP shows nothing at all.
    void setSipInfo(int sipEnabled, const QString &pin);

signals:
    void roomChanged();   // name/description/members changed
    void roomDeleted();   // room deleted OR current user left
    // A member row was clicked. The owner opens the person card; this dialog
    // deliberately does not own it, so every avatar surface shows the same
    // card wired the same way.
    void personCardRequested(const QString &actorId, const QString &displayName,
                             const QRect &anchorGlobal);

protected:
    void changeEvent(QEvent *e) override;   // re-theme on palette change
    // Picks the dialog's size for the screen it opens on, once, before QDialog centres it.
    void showEvent(QShowEvent *e) override;
    // Watches the member list's viewport so a LEFT release opens the person
    // card and a right one is left entirely to the context menu.
    bool eventFilter(QObject *watched, QEvent *event) override;

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
    // Screen fit. The size is chosen ONCE, at the first show: everything the body holds,
    // unscrolled, but no taller than the work area of the screen the dialog opens on (its parent
    // window's, which is where QDialog centres it) minus the window frame. It never changes on
    // its own after that: members, bots and files that arrive later, and the add panel, fit
    // inside it, and the body scrolls when they cannot. Nothing in the dialog can push it past
    // the bottom of the screen, because its layout minimum is the title, a scroll area and the
    // footer. The one exception is shrinkToScreen().
    QScreen *screenToFit() const;
    QMargins frameMarginsForFit() const;
    void sizeForScreen();
    void keepOnScreen();
    // Moved to another screen (dragged there, or its screen went away): if the dialog is taller
    // or wider than that screen's work area, it shrinks to it. It never grows, and its position
    // stays the user's (a move during a drag would fight the mouse).
    void shrinkToScreen();
    // The add panel takes the place of the members, bots and shared-files sections (one page of
    // m_pages each). The page not shown is size-policy Ignored, so only the shown page's minimum
    // counts and switching never asks the dialog for more room.
    void showPage(QWidget *page);
    bool m_placed = false;
    void refreshParticipants();
    void populateParticipants(const QVector<RoomParticipant> &items);
    void runAddSearch();
    // A search reply for `query`, the search with generation `seq`.
    void showAddSearchResults(int seq, const QString &query, bool ok,
                              const QVector<NcUser> &users);
    // What follows an add request: `queryAtClick` is what the search box held when the result
    // was clicked.
    void onParticipantAdded(const QString &userId, const QString &queryAtClick, bool ok,
                            const QString &err);
    void refreshBots();
    void populateBotRow(const BotInfo &bot);
    void refreshSharedFiles();
    // #25 — prominent picture-change feedback shown right next to the avatar
    // (the shared bottom status label is too easy to miss). error=true styles it red.
    void setAvatarStatus(const QString &text, bool error);
    void applyAvatarPixmap(const QImage &img);   // #25 — center-crop to the disc (no stretch)
    void fetchRoomAvatar();                       // #25 — load the CURRENT room avatar on open
    // A click on the group picture shows it large. This dialog is application-modal, and a
    // top-level window that is not its child would be blocked by that, so the viewer is a
    // child window of the dialog (and goes with it).
    void showRoomPicture();
    ImageViewerDialog *m_pictureViewer = nullptr;

    ApiClient   *m_api = nullptr;
    ShiftStatusService *m_shiftStatus = nullptr;
    QString      m_token;
    int          m_roomType = 0;
    int          m_myType   = 0;
    bool         m_amOwnerOrMod = false;

    QLabel      *m_avatar = nullptr;            // #25 — group picture
    QPushButton *m_changeAvatarBtn = nullptr;   // #25
    QLabel       *m_avatarStatus   = nullptr;   // #25 — prominent status by the avatar
    QProgressBar *m_avatarProgress = nullptr;   // #25 — busy bar during upload
    QLineEdit   *m_nameEdit = nullptr;
    QLineEdit   *m_descEdit = nullptr;
    QLabel      *m_memberCount = nullptr;
    QListWidget *m_memberList = nullptr;
    QPushButton *m_addBtn = nullptr;

    // Everything between the title and the footer scrolls inside m_body (only when the screen is
    // too short for it). m_pages is the region under the MEMBERS header: m_overview (members,
    // bots, shared files, dial-in) or m_addPanel.
    QScrollArea    *m_body = nullptr;
    QWidget        *m_bodyContent = nullptr;
    QStackedWidget *m_pages = nullptr;
    QWidget        *m_overview = nullptr;

    QWidget     *m_addPanel = nullptr;
    QLineEdit   *m_addSearch = nullptr;
    QListWidget *m_addResults = nullptr;
    class QTimer *m_addDebounce = nullptr;
    int          m_addSearchSeq = 0;   // generation of the search the list may show

    QPushButton *m_leaveBtn = nullptr;
    QPushButton *m_clearHistoryBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
    QLabel      *m_sipLabel = nullptr;
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
