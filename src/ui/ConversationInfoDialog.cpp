#include "ConversationInfoDialog.h"

#include "core/ApiClient.h"
#include "core/AvatarPaths.h"
#include "core/ConversationKind.h"
#include "core/NcUser.h"
#include "core/Talk25Rooms.h"
#include "core/ShiftStatusService.h"
#include "painter/PainterTheme.h"
#include "ui/ImageViewerDialog.h"
#include "ui/ScreenFit.h"
#include "ui/SearchFieldClear.h"

#include <QCheckBox>
#include <QCursor>
#include <QEvent>
#include <QPalette>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QLabel>
#include <QLocale>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QImage>
#include <QNetworkReply>
#include <QDateTime>
#include <QPushButton>
#include <QProgressBar>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>
#include <QTimer>
#include <QWindow>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrlQuery>
#include <QUrl>
#include <QDesktopServices>
#include <QPointer>
#include <QSet>
#include <QSettings>

namespace {
constexpr int kAttendeeIdRole    = Qt::UserRole + 1;
constexpr int kActorIdRole       = Qt::UserRole + 2;
constexpr int kParticipantTypeRole = Qt::UserRole + 3;
// The bare display name. item->text() is the whole composed row -- name, id,
// role and shift suffix -- and is not a name anyone should be handed.
// +5 rather than +4: the search list below already claims +4, and although the
// two roles live on different widgets, reusing the number is how they stop
// living on different widgets one refactor from now.
constexpr int kDisplayNameRole   = Qt::UserRole + 5;
// Which kind of thing the search hit is - "users" / "groups" / "circles".
// Carried on the item so the add call can tell the server what it is being
// given; without it every id was sent as though it were an account.
constexpr int kUserSourceRole     = Qt::UserRole + 4;

// Resolve the active theme (single source of truth = PainterTheme), so the
// bot chrome below tints from the user's theme accent rather than a hardcoded
// dark-theme literal. Reads the same QSettings key MainWindow/SettingsDialog
// persist the picker to.
PainterTheme activeTheme()
{
    QSettings s("TalQ", "TalQ");
    s.beginGroup("Theme");
    const QString id = s.value("theme",
        PainterTheme::themeId(PainterTheme::Theme::Vivid)).toString();
    s.endGroup();
    return PainterTheme(PainterTheme::themeFromId(id, PainterTheme::Theme::Vivid));
}

// The picture preview next to the name is a fixed 56 px label. A group chat
// (talq::isGroupConversation: Group and Public) is a ROUNDED SQUARE whose
// radius is a quarter of the side, the house idiom shared with the sidebar and
// the chat header; every other room type keeps the disc it always had.
constexpr int kAvatarPx = 56;
constexpr int avatarRadiusPx(int roomType)
{
    return talq::isGroupConversation(roomType) ? kAvatarPx / 4 : kAvatarPx / 2;
}

QPixmap makeBotIcon(int sizePx)
{
    const PainterTheme th = activeTheme();
    QPixmap pm(sizePx, sizePx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(th.accent);
    // Inset 0.5px so the antialiased edge is not clipped at the pixmap's
    // right/bottom boundary (drawing the full 0..sizePx bounds cuts it).
    p.drawEllipse(QRectF(0.5, 0.5, sizePx - 1.0, sizePx - 1.0));
    QFont f;
    f.setPixelSize(int(sizePx * 0.55));
    f.setWeight(QFont::Black);
    p.setFont(f);
    p.setPen(th.controlInk);
    p.drawText(QRect(0, 0, sizePx, sizePx), Qt::AlignCenter, QStringLiteral("B"));
    return pm;
}

// Self-contained primary-button QSS built from theme tokens (accent fill +
// control-ink text, hover/pressed shift within the accent). Set directly on
// the button so it wins over the selector-less `background` that leaks in from
// m_botsContainer / the inherited dialog QSS and can't reach the global
// AppStyle "primary" variant from here.
QString primaryButtonStyle(const QString &extra = QString())
{
    const PainterTheme th = activeTheme();
    auto n = [](const QColor &c){ return c.name(QColor::HexRgb); };
    return QStringLiteral(
        // font-weight converged onto AppStyle's variant="primary" (600, not
        // 700) -- radius/padding/font-size/hover already matched exactly.
        "QPushButton { background: %1; color: %2;"
        "  font-weight: 600; font-size: 12px; border: none;"
        "  border-radius: 8px; padding: 8px 18px; %5 }"
        "QPushButton:hover   { background: %3; }"
        "QPushButton:pressed { background: %4; }")
        .arg(n(th.accent), n(th.inkOn(th.accent)),
             n(th.accent.lighter(115)), n(th.accent.darker(115)), extra);
}

QString roleLabel(int pt)
{
    using R = RoomParticipant;
    switch (pt) {
    case R::Owner:          return QObject::tr("Owner");
    case R::Moderator:      return QObject::tr("Moderator");
    case R::GuestModerator: return QObject::tr("Guest mod");
    case R::Guest:          return QObject::tr("Guest");
    case R::UserSelfJoined: return QObject::tr("Joined");
    default:                return QObject::tr("Member");
    }
}

} // namespace

ConversationInfoDialog::ConversationInfoDialog(ApiClient *api,
                                               const QString &token,
                                               const QString &currentName,
                                               const QString &currentDescription,
                                               int roomType,
                                               int myParticipantType,
                                               QWidget *parent)
    : QDialog(parent), m_api(api), m_token(token),
      m_roomType(roomType), m_myType(myParticipantType)
{
    m_amOwnerOrMod = (myParticipantType == RoomParticipant::Owner
                      || myParticipantType == RoomParticipant::Moderator);
    setWindowTitle(tr("Conversation info"));
    setModal(true);
    resize(520, 620);
    applyChrome();

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(28, 24, 28, 20);
    outer->setSpacing(0);

    auto *title = new QLabel(tr("Conversation info"), this);
    title->setObjectName("headline");
    outer->addWidget(title);
    outer->addSpacing(20);

    // Everything between the title and the footer. It scrolls only when the screen is too short
    // for it (a 720 px work area, a room with many bots), so the title and the footer -- the
    // status line and Leave / Clear history / Delete / Done -- are always in view. Until this the
    // whole column was the dialog's layout, which made its MINIMUM height the sum of everything:
    // two fixed 150 px lists plus ~47 px per bot, 877 px for a room with 3 bots on a 852 px work
    // area, and "+ Add people" added 200 px more. No frame and no fill: where it fits it looks
    // exactly as before.
    m_body = new QScrollArea(this);
    m_body->setFrameShape(QFrame::NoFrame);
    m_body->setWidgetResizable(true);
    m_bodyContent = new QWidget(m_body);
    auto *body = new QVBoxLayout(m_bodyContent);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);

    // #25 — group picture (groups only; 1:1 avatars are the other user's). A
    // placeholder disc with the conversation's initial; moderators get a
    // "Change picture" button that uploads to the Talk room-avatar API.
    if (roomType != 1) {
        auto *avRow = new QHBoxLayout();
        m_avatar = new QLabel(this);
        m_avatar->setFixedSize(kAvatarPx, kAvatarPx);
        m_avatar->setAlignment(Qt::AlignCenter);
        m_avatar->setScaledContents(true);
        const QString initial = currentName.trimmed().isEmpty()
            ? QStringLiteral("#") : currentName.trimmed().left(1).toUpper();
        m_avatar->setText(initial);
        // Palette-derived placeholder (no-gray convention): authorColor keyed
        // on the conversation name for the fill, ink scored per-fill via
        // inkOn() — controlInk alone fails AA against several author-palette
        // hues on Paper. Same pairing SidebarPainter's avatar-fallback uses.
        const PainterTheme th = activeTheme();
        const QColor avatarBg = PainterTheme::authorColor(currentName);
        m_avatar->setStyleSheet(QStringLiteral(
            "QLabel { background:%1; color:%2; border-radius:%3px;"
            "         font-size:24px; font-weight:600; }")
            .arg(avatarBg.name(QColor::HexRgb), th.inkOn(avatarBg).name(QColor::HexRgb))
            .arg(avatarRadiusPx(roomType)));
        // Clickable: the picture opens large (eventFilter). A label has no click signal,
        // so the pointing hand and the tip are what say it can be clicked.
        m_avatar->setCursor(Qt::PointingHandCursor);
        m_avatar->setToolTip(tr("View picture"));
        m_avatar->installEventFilter(this);
        avRow->addWidget(m_avatar);
        avRow->addSpacing(14);
        if (m_amOwnerOrMod) {
            auto *avCol = new QVBoxLayout();
            avCol->setSpacing(5);
            m_changeAvatarBtn = new QPushButton(tr("Change picture…"), this);
            m_changeAvatarBtn->setProperty("variant", "ghost");
            m_changeAvatarBtn->setCursor(Qt::PointingHandCursor);
            connect(m_changeAvatarBtn, &QPushButton::clicked,
                    this, &ConversationInfoDialog::onChangeAvatar);
            avCol->addWidget(m_changeAvatarBtn);
            // Prominent feedback right next to the avatar (not the easy-to-miss
            // shared status at the very bottom). Hidden until a change is attempted.
            m_avatarStatus = new QLabel(QString(), this);
            m_avatarStatus->setWordWrap(true);
            // Colour-free weight rule, set ONCE here (not in setAvatarStatus,
            // which only ever touches the "role" property for colour). This
            // keeps the label visually distinct from the plainer m_status
            // below — the "Prominent feedback" this comment promises — while
            // still letting AppStyle.cpp's role selectors own the colour.
            // Verified empirically that a widget-local stylesheet survives
            // AppStyle's RepolishFilter (unpolish+polish on a role change,
            // including a live role SWAP): see task-10-report.md fix-round-1.
            m_avatarStatus->setStyleSheet(QStringLiteral("QLabel { font-weight: 600; }"));
            m_avatarStatus->setVisible(false);
            avCol->addWidget(m_avatarStatus);
            // Slim indeterminate "busy" bar, shown only while the upload is in flight.
            m_avatarProgress = new QProgressBar(this);
            m_avatarProgress->setRange(0, 0);          // indeterminate
            m_avatarProgress->setTextVisible(false);
            m_avatarProgress->setFixedHeight(4);
            m_avatarProgress->setVisible(false);
            avCol->addWidget(m_avatarProgress);
            avRow->addLayout(avCol);
        }
        avRow->addStretch();
        body->addLayout(avRow);
        body->addSpacing(18);
        fetchRoomAvatar();   // #25 — show the CURRENT picture, not just the letter
    }

    // Name
    auto *nameEyebrow = new QLabel(tr("NAME"), this);
    nameEyebrow->setObjectName("eyebrow");
    body->addWidget(nameEyebrow);
    m_nameEdit = new QLineEdit(currentName, this);
    m_nameEdit->setObjectName("name");
    m_nameEdit->setReadOnly(!m_amOwnerOrMod || roomType == 1);
    body->addWidget(m_nameEdit);
    body->addSpacing(16);

    // Description
    auto *descEyebrow = new QLabel(tr("DESCRIPTION"), this);
    descEyebrow->setObjectName("eyebrow");
    body->addWidget(descEyebrow);
    m_descEdit = new QLineEdit(currentDescription, this);
    m_descEdit->setPlaceholderText(tr("Add a description\u2026"));
    m_descEdit->setReadOnly(!m_amOwnerOrMod);
    body->addWidget(m_descEdit);
    body->addSpacing(20);

    // Talk 25 room kind. Built hidden and filled by setTalk25State(); an
    // ordinary room never shows it. The gap below is the box's own margin, so a
    // hidden box leaves none.
    m_kindBox = new QWidget(this);
    auto *kindLay = new QVBoxLayout(m_kindBox);
    kindLay->setContentsMargins(0, 0, 0, 20);
    kindLay->setSpacing(8);
    m_kindLabel = new QLabel(m_kindBox);
    m_kindLabel->setWordWrap(true);
    m_kindLabel->setProperty("role", "secondary");
    kindLay->addWidget(m_kindLabel);

    m_preserveCheck = new QCheckBox(tr("Preserve this conversation"), m_kindBox);
    m_preserveCheck->setCursor(Qt::PointingHandCursor);
    kindLay->addWidget(m_preserveCheck);
    m_preserveHint = new QLabel(m_kindBox);
    m_preserveHint->setWordWrap(true);
    m_preserveHint->setProperty("role", "secondary");
    kindLay->addWidget(m_preserveHint);

    m_keepRow = new QWidget(m_kindBox);
    auto *keepLay = new QVBoxLayout(m_keepRow);
    keepLay->setContentsMargins(0, 0, 0, 0);
    keepLay->setSpacing(8);
    m_keepLabel = new QLabel(m_keepRow);
    m_keepLabel->setWordWrap(true);
    keepLay->addWidget(m_keepLabel);
    m_keepBtn = new QPushButton(tr("Keep this conversation"), m_keepRow);
    m_keepBtn->setProperty("variant", "primary");
    m_keepBtn->setCursor(Qt::PointingHandCursor);
    keepLay->addWidget(m_keepBtn, 0, Qt::AlignLeft);
    kindLay->addWidget(m_keepRow);

    m_kindBox->setVisible(false);
    body->addWidget(m_kindBox);

    // Members header + Add button
    auto *memRow = new QHBoxLayout();
    m_memberCount = new QLabel(tr("MEMBERS"), this);
    m_memberCount->setObjectName("eyebrow");
    memRow->addWidget(m_memberCount);
    memRow->addStretch();
    m_addBtn = new QPushButton(tr("+ Add people"), this);
    m_addBtn->setProperty("variant", "primary");
    m_addBtn->setCursor(Qt::PointingHandCursor);
    m_addBtn->setVisible(m_amOwnerOrMod && roomType != 1);
    memRow->addWidget(m_addBtn);
    body->addLayout(memRow);
    body->addSpacing(6);

    // Under the MEMBERS header: the room overview (members, bots, shared files, dial-in), or
    // the add-people panel in its place. The panel used to open BELOW the member list, adding
    // its ~200 px to the dialog's height. As a page it gets the overview's whole height
    // instead: on a 1440 x 852 work area with three bots the results list is ~380 px (a dozen
    // rows), where taking only the member list's place would have left it ~80 px.
    m_pages = new QStackedWidget(m_bodyContent);
    m_overview = new QWidget(m_pages);
    auto *ov = new QVBoxLayout(m_overview);
    ov->setContentsMargins(0, 0, 0, 0);
    ov->setSpacing(0);

    m_memberList = new QListWidget(this);
    m_memberList->setSelectionMode(QAbstractItemView::NoSelection);
    m_memberList->setContextMenuPolicy(Qt::CustomContextMenu);
    // Stretch 2 against the shared files' 1: when the room is short, the members keep the
    // larger share. (A box layout shrinks an item WITH stretch to its minimum before it takes
    // anything from one without, so a files list without stretch would win every pixel.)
    ov->addWidget(m_memberList, 2);

    // Inline add-people panel (a page of m_pages, shown by "+ Add people")
    m_addPanel = new QWidget(m_pages);
    auto *addLay = new QVBoxLayout(m_addPanel);
    addLay->setContentsMargins(0, 8, 0, 0);
    addLay->setSpacing(6);
    m_addSearch = new QLineEdit(m_addPanel);
    m_addSearch->setPlaceholderText(tr("\U0001F50D  Search people to add\u2026"));
    addLay->addWidget(m_addSearch);
    // The same clear "x" and Esc as the chat-list search. The first Esc in a box with text
    // clears it; an Esc in an empty box still closes the dialog, as it always did.
    talq::searchclear::attach(m_addSearch, activeTheme());
    m_addResults = new QListWidget(m_addPanel);
    m_addResults->setSelectionMode(QAbstractItemView::NoSelection);
    addLay->addWidget(m_addResults, 1);   // takes the page's height, no fixed 150 px

    m_addDebounce = new QTimer(this);
    m_addDebounce->setSingleShot(true);
    m_addDebounce->setInterval(250);

    // ── Bots section ──────────────────────────────────────
    // Everyone in the room can SEE which bots are enabled (transparent ID of
    // automated participants). Only moderators get the +Add button — the API
    // would reject a non-moderator's enable call anyway, so showing the
    // button is a permissions-leak / dead-button trap.
    ov->addSpacing(16);
    auto *botRow = new QHBoxLayout();
    m_botsHeader = new QLabel(tr("BOTS"), this);
    m_botsHeader->setObjectName("eyebrow");
    botRow->addWidget(m_botsHeader);
    botRow->addStretch();
    m_addBotBtn = new QPushButton(tr("+ Add bot"), this);
    m_addBotBtn->setProperty("variant", "primary");
    m_addBotBtn->setCursor(Qt::PointingHandCursor);
    m_addBotBtn->setVisible(m_amOwnerOrMod);
    botRow->addWidget(m_addBotBtn);
    ov->addLayout(botRow);
    ov->addSpacing(6);

    m_botsContainer = new QWidget(this);
    m_botsContainer->setObjectName(QStringLiteral("botsCard"));
    m_botsContainer->setStyleSheet(QStringLiteral(
        "QWidget#botsCard { background: %1; border: 1px solid %2;"
        " border-radius: %3px; }")
        .arg(palette().color(QPalette::Base).name(),
             palette().color(QPalette::Mid).name())
        .arg(PainterTheme::radiusCard));
    m_botsLayout = new QVBoxLayout(m_botsContainer);
    m_botsLayout->setContentsMargins(4, 4, 4, 4);
    m_botsLayout->setSpacing(2);
    ov->addWidget(m_botsContainer);

    connect(m_addBotBtn, &QPushButton::clicked,
            this, &ConversationInfoDialog::onAddBotClicked);

    // ── Shared files section ──────────────────────────────
    // Scans the most recent chat page for file shares. Bounded to one API
    // page so it stays cheap; click a row to open the file in the browser.
    ov->addSpacing(16);
    m_filesHeader = new QLabel(tr("SHARED FILES"), this);
    m_filesHeader->setObjectName("eyebrow");
    ov->addWidget(m_filesHeader);
    ov->addSpacing(6);

    // Up to its old 150 px when there is room, down to about two rows when there is not.
    m_filesList = new QListWidget(this);
    m_filesList->setSelectionMode(QAbstractItemView::NoSelection);
    m_filesList->setMinimumHeight(64);
    m_filesList->setMaximumHeight(150);
    m_filesList->setCursor(Qt::PointingHandCursor);
    ov->addWidget(m_filesList, 1);
    connect(m_filesList, &QListWidget::itemClicked, this,
            [](QListWidgetItem *item) {
        const QString link = item->data(Qt::UserRole).toString();
        if (!link.isEmpty())
            QDesktopServices::openUrl(QUrl(link));
    });

    // Dial-in details. Created hidden and filled by setSipInfo(); a room
    // without SIP never shows the row, which is every room on a server with no
    // SIP bridge configured. The gap above it is its own margin, so a hidden
    // row leaves none.
    m_sipLabel = new QLabel(QString(), this);
    m_sipLabel->setWordWrap(true);
    m_sipLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_sipLabel->setContentsMargins(0, 8, 0, 0);
    m_sipLabel->setVisible(false);
    ov->addWidget(m_sipLabel);

    m_pages->addWidget(m_overview);
    m_pages->addWidget(m_addPanel);
    body->addWidget(m_pages, 1);
    showPage(m_overview);

    m_body->setWidget(m_bodyContent);
    // setWidget() turns the content's autoFillBackground on; the dialog's own
    // ground must show through, as it did before there was a scroll area.
    m_bodyContent->setAutoFillBackground(false);
    m_body->viewport()->setAutoFillBackground(false);
    outer->addWidget(m_body, 1);

    outer->addSpacing(8);
    m_status = new QLabel(QString(), this);
    m_status->setProperty("role", "secondary");
    outer->addWidget(m_status);
    outer->addSpacing(8);

    // Footer buttons
    auto *footer = new QHBoxLayout();
    footer->setContentsMargins(0, 8, 0, 0);
    footer->setSpacing(8);
    m_leaveBtn = new QPushButton(tr("Leave"), this);
    m_leaveBtn->setProperty("variant", "danger");
    m_leaveBtn->setCursor(Qt::PointingHandCursor);
    footer->addWidget(m_leaveBtn);

    m_clearHistoryBtn = new QPushButton(tr("Clear history"), this);
    m_clearHistoryBtn->setProperty("variant", "danger");
    m_clearHistoryBtn->setCursor(Qt::PointingHandCursor);
    // Moderators/owners (any room) + both parties of a 1:1 may clear history.
    m_clearHistoryBtn->setVisible(m_amOwnerOrMod || m_roomType == 1);
    footer->addWidget(m_clearHistoryBtn);

    m_deleteBtn = new QPushButton(tr("Delete"), this);
    m_deleteBtn->setProperty("variant", "danger");
    m_deleteBtn->setCursor(Qt::PointingHandCursor);
    m_deleteBtn->setVisible(m_myType == RoomParticipant::Owner);
    footer->addWidget(m_deleteBtn);

    footer->addStretch();
    m_closeBtn = new QPushButton(tr("Done"), this);
    m_closeBtn->setProperty("variant", "primary");
    m_closeBtn->setCursor(Qt::PointingHandCursor);
    m_closeBtn->setDefault(true);
    footer->addWidget(m_closeBtn);
    outer->addLayout(footer);

    // Wiring
    connect(m_nameEdit, &QLineEdit::editingFinished, this, &ConversationInfoDialog::saveName);
    connect(m_descEdit, &QLineEdit::editingFinished, this, &ConversationInfoDialog::saveDescription);
    connect(m_addBtn,   &QPushButton::clicked, this, &ConversationInfoDialog::onAddMembersToggled);
    connect(m_addSearch, &QLineEdit::textChanged, this, &ConversationInfoDialog::onAddSearchChanged);
    connect(m_addDebounce, &QTimer::timeout, this, &ConversationInfoDialog::runAddSearch);
    connect(m_addResults, &QListWidget::itemClicked,
            this, &ConversationInfoDialog::onAddResultClicked);
    // Right-click a member row for the promote/demote/remove menu — left-clicks
    // don't do anything so tapping names doesn't keep popping menus.
    // Left-click opens the person card. Removal lives on the context menu, so
    // the two gestures must not compete -- and QListWidget::itemClicked fires
    // for EVERY button, so using it would open the card and the remove menu
    // together on one right-click. Filtering the viewport's release event is
    // the way to be sure which button was actually pressed.
    m_memberList->viewport()->installEventFilter(this);

    connect(m_memberList, &QListWidget::customContextMenuRequested,
            this, [this](const QPoint &pos) {
        if (auto *item = m_memberList->itemAt(pos)) onRemoveMember(item);
    });
    connect(m_leaveBtn,  &QPushButton::clicked, this, &ConversationInfoDialog::onLeaveClicked);
    connect(m_clearHistoryBtn, &QPushButton::clicked, this, &ConversationInfoDialog::onClearHistoryClicked);
    connect(m_deleteBtn, &QPushButton::clicked, this, &ConversationInfoDialog::onDeleteClicked);
    connect(m_closeBtn,  &QPushButton::clicked, this, &QDialog::accept);
    connect(m_preserveCheck, &QCheckBox::clicked, this, &ConversationInfoDialog::onPreserveToggled);
    connect(m_keepBtn, &QPushButton::clicked, this, &ConversationInfoDialog::onKeepClicked);

    refreshParticipants();
    refreshBots();
    refreshSharedFiles();
}

void ConversationInfoDialog::applyChrome()
{
    // Typographic identity + underline inputs, palette-driven. QDialog bg,
    // QLabel, QListWidget, QMenu and the primary/danger/ghost buttons all
    // come from the app-wide AppStyle sheet (single source of truth).
    const QPalette p = palette();
    auto n = [&](QPalette::ColorRole r){ return p.color(r).name(); };
    setStyleSheet(QString(
        // Converged onto AppStyle's role="eyebrow" values (11px/600/inkMuted)
        // -- %1 already resolves to PlaceholderText=theme.textMuted, the same
        // token as AppStyle's inkMuted, so only size/weight/tracking changed.
        "QLabel#eyebrow { color: %1; font-size: 11px; font-weight: 600; }"
        "QLabel#headline { color: %2; font-size: 22px; font-weight: 600;"
        "  letter-spacing: -0.2px; }"
        "QLineEdit { background: transparent; border: none;"
        "  border-bottom: 1px solid %3; padding: 8px 0; color: %2;"
        "  font-size: 14px; selection-background-color: %4;"
        "  selection-color: %5; }"
        "QLineEdit:focus { border-bottom-color: %4; }"
        "QLineEdit#name  { font-size: 20px; font-weight: 600; }")
        .arg(n(QPalette::PlaceholderText), n(QPalette::WindowText),
             n(QPalette::Mid), n(QPalette::Highlight),
             n(QPalette::HighlightedText)));
}

void ConversationInfoDialog::changeEvent(QEvent *e)
{
    QDialog::changeEvent(e);
    // ApplicationPaletteChange only — PaletteChange recurses via setStyleSheet.
    if (e->type() == QEvent::ApplicationPaletteChange
        || e->type() == QEvent::ThemeChange) {
        applyChrome();
        talq::searchclear::retint(m_addSearch, activeTheme());
    }
}

void ConversationInfoDialog::showEvent(QShowEvent *e)
{
    // QDialog::showEvent centres the dialog over its parent with whatever size it has at that
    // moment, so the size is chosen first. A spontaneous show (restored from minimised) changes
    // nothing.
    const bool first = !m_placed && !e->spontaneous();
    if (first)
        sizeForScreen();
    QDialog::showEvent(e);
    if (first) {
        m_placed = true;
        keepOnScreen();   // the centring only estimates the frame; this uses the real one
        // Queued: on Windows the screen change comes in the middle of the move (and of a DPI
        // change, whose own resize must land first); the slot reads the screen it is on then.
        if (QWindow *w = windowHandle())
            connect(w, &QWindow::screenChanged, this, [this]() { shrinkToScreen(); },
                    Qt::QueuedConnection);
    }
}

void ConversationInfoDialog::shrinkToScreen()
{
    // Opened at 859 px on a 1080p screen and dragged to a laptop with an 852 px work area, the
    // frame was ~898 px: the footer could not be put on screen wherever the dialog was dropped.
    // fitClientSize() with the current size as the wish can only shrink it, and leaves it alone
    // when it already fits, so a move to a taller screen changes nothing.
    QScreen *scr = screen();
    if (!scr)
        return;
    const QSize fitted = talq::screenfit::fitClientSize(size(), minimumSize(),
                                                        scr->availableGeometry(),
                                                        frameMarginsForFit());
    if (fitted != size())
        resize(fitted);
}

QScreen *ConversationInfoDialog::screenToFit() const
{
    // Where QDialog centres the dialog: on its parent window's screen. Before the first show
    // the dialog has no position of its own to ask.
    if (QWidget *p = parentWidget())
        if (QScreen *s = p->window()->screen())
            return s;
    return screen();
}

QMargins ConversationInfoDialog::frameMarginsForFit() const
{
    // The title bar and borders (~31 px on top at 200 % on Windows). The dialog's own once its
    // native window exists, which it does by showEvent; failing that, the parent window's,
    // which is on screen with the same kind of frame.
    if (const QWindow *w = windowHandle()) {
        const QMargins m = w->frameMargins();
        if (!m.isNull())
            return m;
    }
    if (QWidget *p = parentWidget())
        if (const QWindow *pw = p->window()->windowHandle())
            return pw->frameMargins();
    return QMargins();
}

void ConversationInfoDialog::sizeForScreen()
{
    QScreen *scr = screenToFit();
    if (!scr || !layout())
        return;
    layout()->activate();
    // What the body holds, unscrolled. The outer layout counts the body at the scroll area's own
    // size hint, which Qt caps at a few hundred pixels, so the content's hint is swapped in.
    const int wanted = layout()->sizeHint().height() - m_body->sizeHint().height()
                       + m_bodyContent->sizeHint().height();
    resize(talq::screenfit::fitClientSize(QSize(width(), wanted), minimumSize(),
                                          scr->availableGeometry(), frameMarginsForFit()));
}

void ConversationInfoDialog::keepOnScreen()
{
    QScreen *scr = screenToFit();
    if (!scr)
        return;
    const QRect frame = frameGeometry();
    const QPoint to = talq::screenfit::keepFrameOnScreen(frame, scr->availableGeometry());
    if (to != frame.topLeft())
        move(to);
}

void ConversationInfoDialog::showPage(QWidget *page)
{
    for (int i = 0; i < m_pages->count(); ++i) {
        QWidget *w = m_pages->widget(i);
        w->setSizePolicy(w == page ? QSizePolicy::Preferred : QSizePolicy::Ignored,
                         w == page ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    }
    m_pages->setCurrentWidget(page);
}

void ConversationInfoDialog::applyAvatarPixmap(const QImage &img)
{
    // Center-crop to a square and scale to the 56px disc, so the preview is
    // never stretched (the old setScaledContents path squished wide photos) and
    // matches EXACTLY what setRoomAvatar uploads (center-cropped square).
    if (!m_avatar || img.isNull()) return;
    if (talq::isGroupConversation(m_roomType)) {
        // A group's picture is masked HERE, in the pixels. It is NOT verified
        // that Qt clips a QLabel's pixmap to a stylesheet border-radius (the
        // radius is only known to round the label's own background), so the
        // stylesheet below is kept matching but never relied on.
        // cropToRoundedSquare does the same centre-crop and cover-scale as the
        // lines below. Rendered at 56 px with no devicePixelRatio, like the
        // rest of this dialog.
        const int radius = avatarRadiusPx(m_roomType);
        m_avatar->setText(QString());
        m_avatar->setStyleSheet(QStringLiteral("QLabel { border-radius:%1px; }").arg(radius));
        m_avatar->setPixmap(QPixmap::fromImage(
            PainterTheme::cropToRoundedSquare(img, kAvatarPx, radius)));
        return;
    }
    const int side = qMin(img.width(), img.height());
    const QImage sq = img.copy((img.width() - side) / 2, (img.height() - side) / 2, side, side)
                         .scaled(kAvatarPx, kAvatarPx, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    m_avatar->setText(QString());
    m_avatar->setStyleSheet(QStringLiteral("QLabel { border-radius:%1px; }")
                                .arg(avatarRadiusPx(m_roomType)));
    m_avatar->setPixmap(QPixmap::fromImage(sq));
}

void ConversationInfoDialog::fetchRoomAvatar()
{
    // The dialog used to only draw the letter placeholder, so re-opening it never
    // showed the actual group picture. Fetch the current room avatar and display it.
    if (!m_api || m_token.isEmpty()) return;
    QNetworkReply *reply = m_api->getAbsoluteUrl(
        QStringLiteral("/ocs/v2.php/apps/spreed/api/v1/room/") + m_token
        + QStringLiteral("/avatar?v=") + QString::number(QDateTime::currentSecsSinceEpoch()));
    if (!reply) return;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        QImage img;
        if (img.loadFromData(reply->readAll()))
            applyAvatarPixmap(img);
    });
}

void ConversationInfoDialog::setAvatarStatus(const QString &text, bool error)
{
    if (!m_avatarStatus) return;
    m_avatarStatus->setText(text);
    // Shared danger/success roles (AppStyle.cpp), not a one-off hardcoded
    // red/green — same tokens every other status label in this dialog uses
    // (m_status below), so failure/success read consistently across themes.
    // The app-wide RepolishFilter re-runs the QSS selector on this dynamic
    // property change even though the label is already shown.
    m_avatarStatus->setProperty("role", error ? "danger" : "success");
    m_avatarStatus->setVisible(!text.isEmpty());
}

void ConversationInfoDialog::onChangeAvatar()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose a group picture"), QString(),
        tr("Images (*.png *.jpg *.jpeg)"));
    if (path.isEmpty()) return;
    setAvatarStatus(tr("Uploading picture…"), false);
    if (m_avatarProgress) m_avatarProgress->setVisible(true);
    if (m_changeAvatarBtn) m_changeAvatarBtn->setEnabled(false);
    m_api->setRoomAvatar(m_token, path, this, [this, path](bool ok, const QString &err) {
        if (m_avatarProgress) m_avatarProgress->setVisible(false);
        if (m_changeAvatarBtn) m_changeAvatarBtn->setEnabled(true);
        if (!ok) {
            setAvatarStatus(err.isEmpty() ? tr("Couldn't update the picture.")
                                          : tr("Couldn't update: %1").arg(err), true);
            return;
        }
        setAvatarStatus(tr("Picture updated ✓"), false);
        // Show the new picture immediately, center-cropped EXACTLY like the upload
        // (no stretch) so the preview matches the result in the header/list.
        applyAvatarPixmap(QImage(path));
        emit roomChanged();   // parent refreshes its avatar cache for this room
    });
}

void ConversationInfoDialog::saveName()
{
    const QString n = m_nameEdit->text().trimmed();
    if (n.isEmpty() || !m_amOwnerOrMod) return;
    m_status->setText(tr("Saving name\u2026"));
    m_api->setRoomName(m_token, n, this,
        [this](bool ok, const QString &err) {
            m_status->setText(ok ? tr("Name saved.")
                                 : (err.isEmpty() ? tr("Couldn't rename.") : err));
            m_status->setProperty("role", ok ? "secondary" : "danger");
            if (ok) emit roomChanged();
        });
}

void ConversationInfoDialog::saveDescription()
{
    if (!m_amOwnerOrMod) return;
    // Guard against wiping the server-side description when the user just
    // tabs past the field without typing — editingFinished fires on every
    // focus loss, and the caller hasn't been passing the current description
    // so a clean open-and-close would otherwise PUT an empty string.
    if (!m_descEdit->isModified()) return;
    const QString d = m_descEdit->text();
    m_status->setText(tr("Saving description\u2026"));
    m_api->setRoomDescription(m_token, d, this,
        [this](bool ok, const QString &err) {
            m_status->setText(ok ? tr("Description saved.")
                                 : (err.isEmpty() ? tr("Couldn't save description.") : err));
            m_status->setProperty("role", ok ? "secondary" : "danger");
            if (ok) emit roomChanged();
        });
}

void ConversationInfoDialog::refreshParticipants()
{
    m_memberList->clear();
    m_status->setText(tr("Loading members\u2026"));
    m_api->fetchRoomParticipants(m_token, this,
        [this](bool ok, const QVector<RoomParticipant> &items) {
            if (!ok) {
                // A channel keeps its member list from everyone but moderators
                // (the server answers 403), which is not a failure to load.
                if (talq::isChannelRoom(m_talk25.attributes) && !m_amOwnerOrMod) {
                    m_memberCount->setText(tr("MEMBERS"));
                    m_status->setText(tr("Only moderators can see who is in a channel."));
                } else {
                    m_status->setText(tr("Couldn't load members."));
                }
                return;
            }
            populateParticipants(items);
        });
}

void ConversationInfoDialog::populateParticipants(const QVector<RoomParticipant> &items)
{
    m_memberCount->setText(tr("MEMBERS · %1").arg(items.size()));
    for (const RoomParticipant &p : items) {
        const bool isMe = (p.userId == m_api->user() && !p.userId.isEmpty());
        const QString name = p.displayName.isEmpty() ? p.userId : p.displayName;
        const QString id   = p.userId.isEmpty() ? tr("(guest)") : p.userId;
        const QString role = roleLabel(p.participantType);
        const QString canRemove = (m_amOwnerOrMod && !isMe) ? QStringLiteral("   \u00D7") : QString();
        // Shift state as TEXT rather than a coloured dot. This list has no
        // delegate -- every row is one flat string -- and a word is both
        // cheaper here and more accessible than a colour that anyone
        // red-green colourblind would have to guess at. Same restraint as the
        // other surfaces: on-shift is the expected case and says nothing, so
        // only the exceptions add a segment.
        QString shiftSuffix;
        bool dimRow = false;
        if (m_shiftStatus && !p.userId.isEmpty()) {
            const talq::ShiftState st = m_shiftStatus->stateFor(p.userId);
            if (talq::shiftMarksExceptionInList(st)) {
                QString word = m_shiftStatus->labelFor(p.userId);
                if (word.isEmpty())
                    word = (st == talq::ShiftState::OnBreak) ? tr("On break") : tr("Outside hours");
                shiftSuffix = QStringLiteral("  \u00B7  ") + word;
                dimRow = (st == talq::ShiftState::OffShift);
            }
        }

        const QString label = QStringLiteral("\U0001F464  %1  \u00B7  %2  \u00B7  %3%4%5")
                                  .arg(name, id, role, shiftSuffix, canRemove);
        auto *item = new QListWidgetItem(label, m_memberList);
        if (dimRow)
            item->setForeground(palette().color(QPalette::Disabled, QPalette::WindowText));
        // A long name/id/role combo elides in the list's fixed width with no
        // way to see the rest -- the other elided text sites in the app all
        // resolve this via a tooltip; QListWidgetItem never gets one unless
        // set explicitly.
        item->setToolTip(label);
        item->setData(kAttendeeIdRole, p.attendeeId);
        item->setData(kDisplayNameRole, name);
        item->setData(kActorIdRole, p.userId);
        item->setData(kParticipantTypeRole, p.participantType);
    }

    // Declare the whole member list to the service. This is the only surface
    // that works for a GROUP room, so without it a group's rows would stay
    // Unknown forever -- the sidebar sweep only ever walks 1:1 conversations.
    // The rows re-render when statusesChanged lands: setShiftStatus() connects
    // it to refreshParticipants(). That connection is what makes this comment
    // true -- it shipped without one in 0.69.3, so the rows were stale.
    if (m_shiftStatus) {
        QStringList uids;
        uids.reserve(items.size());
        for (const RoomParticipant &rp : items)
            if (!rp.userId.isEmpty())
                uids << rp.userId;
        m_shiftStatus->observe(uids);
    }

    // Only its own "Loading members..." line. A confirmation set since -- "Added X.", which
    // onParticipantAdded() writes after starting this reload -- stays on screen.
    if (m_status->text() == tr("Loading members\u2026"))
        m_status->clear();
}

bool ConversationInfoDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (m_avatar && watched == m_avatar && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && m_avatar->rect().contains(me->position().toPoint())) {
            showRoomPicture();
            return true;
        }
    }
    if (m_memberList && watched == m_memberList->viewport()
        && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            if (QListWidgetItem *item = m_memberList->itemAt(me->position().toPoint())) {
                const QString actorId = item->data(kActorIdRole).toString();
                if (!actorId.isEmpty()) {   // a guest has no id to look up
                    const QRect r = m_memberList->visualItemRect(item);
                    emit personCardRequested(
                        actorId, item->data(kDisplayNameRole).toString(),
                        QRect(m_memberList->viewport()->mapToGlobal(r.topLeft()),
                              r.size()));
                }
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void ConversationInfoDialog::showRoomPicture()
{
    if (!m_api)
        return;
    const QString path = talq::avatarpaths::roomPath(m_token, QDateTime::currentSecsSinceEpoch());
    if (path.isEmpty())
        return;                          // not a real token: nothing to ask the server
    if (!m_pictureViewer)
        m_pictureViewer = new ImageViewerDialog(m_api, nullptr, this);   // a child window: see the header
    m_pictureViewer->showPicture(m_nameEdit ? m_nameEdit->text().trimmed() : QString(), path);
    m_pictureViewer->show();
    m_pictureViewer->raise();
    m_pictureViewer->activateWindow();
}

void ConversationInfoDialog::setShiftStatus(ShiftStatusService *shiftStatus)
{
    if (m_shiftStatus == shiftStatus)
        return;
    m_shiftStatus = shiftStatus;
    if (!m_shiftStatus)
        return;
    // Without this the rows show whatever happened to be cached when the
    // dialog opened, and a first-ever lookup never appears at all. The comment
    // in populateParticipants() has claimed otherwise since 0.69.3 -- it was
    // aspirational, and this is what makes it true.
    connect(m_shiftStatus, &ShiftStatusService::statusesChanged,
            this, &ConversationInfoDialog::refreshParticipants,
            Qt::UniqueConnection);
}

void ConversationInfoDialog::onRemoveMember(QListWidgetItem *item)
{
    if (!item) return;
    const QString actorId   = item->data(kActorIdRole).toString();
    const qint64  attendeeId = item->data(kAttendeeIdRole).toLongLong();
    const int     type       = item->data(kParticipantTypeRole).toInt();
    if (attendeeId == 0) return;
    const bool isMe = (actorId == m_api->user() && !actorId.isEmpty());
    const QString displayName =
        item->text().section(QLatin1Char(' '), 2, 2, QString::SectionSkipEmpty);

    QMenu menu(this);
    // QMenu chrome from the app-wide AppStyle sheet (theme-driven).

    const bool canManage = m_amOwnerOrMod && !isMe
                           && type != RoomParticipant::Owner;
    if (!canManage) return;  // no actions possible → don't pop an empty menu

    QAction *promote = nullptr;
    QAction *demote  = nullptr;
    QAction *remove  = nullptr;
    if (type == RoomParticipant::Moderator
        || type == RoomParticipant::GuestModerator) {
        demote = menu.addAction(QStringLiteral("\u2193  ") + tr("Demote to member"));
    } else if (type == RoomParticipant::User
               || type == RoomParticipant::UserSelfJoined
               || type == RoomParticipant::Guest) {
        promote = menu.addAction(QStringLiteral("\u2191  ") + tr("Promote to moderator"));
    }
    menu.addSeparator();
    remove = menu.addAction(QStringLiteral("\u00D7  ") + tr("Remove from conversation"));

    QAction *picked = menu.exec(QCursor::pos());
    if (!picked) return;

    auto done = [this](bool ok, const QString &err) {
        if (!ok) {
            m_status->setProperty("role", "danger");
            m_status->setText(err.isEmpty() ? tr("Server refused.") : err);
            return;
        }
        refreshParticipants();
        emit roomChanged();
    };

    if (picked == promote) {
        m_status->setText(tr("Promoting\u2026"));
        m_api->promoteModerator(m_token, attendeeId, this, done);
    } else if (picked == demote) {
        m_status->setText(tr("Demoting\u2026"));
        m_api->demoteModerator(m_token, attendeeId, this, done);
    } else if (picked == remove) {
        auto c = QMessageBox::question(this, tr("Remove member?"),
            tr("Remove %1 from the conversation?").arg(displayName.isEmpty()
                                                         ? actorId : displayName),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (c == QMessageBox::Yes) {
            m_status->setText(tr("Removing\u2026"));
            m_api->removeRoomParticipant(m_token, attendeeId, this, done);
        }
    }
}

void ConversationInfoDialog::onAddMembersToggled()
{
    const bool opening = m_pages->currentWidget() != m_addPanel;
    showPage(opening ? m_addPanel : m_overview);
    // The panel stands in for the whole overview (members, bots and shared files) while the
    // MEMBERS header stays above it, so the way out is a plain "Back".
    m_addBtn->setText(opening ? tr("Back") : tr("+ Add people"));
    ++m_addSearchSeq;   // a search still in flight belongs to the panel as it was
    if (opening) {
        m_addSearch->clear();
        m_addResults->clear();
        m_addSearch->setFocus();
    }
}

void ConversationInfoDialog::onAddSearchChanged()
{
    // Any reply still in flight is for text that is no longer in the box (this includes the
    // box being cleared after an add). The debounced search below starts a new generation.
    ++m_addSearchSeq;
    // An empty box (the x, Esc, an add) has nothing to search for, and its old rows must not
    // stay clickable for the debounce's 250 ms under a box that no longer asks for them.
    if (m_addSearch->text().isEmpty()) {
        m_addDebounce->stop();
        m_addResults->clear();
        return;
    }
    m_addDebounce->start();
}

void ConversationInfoDialog::runAddSearch()
{
    const QString q = m_addSearch->text().trimmed();
    const int seq = ++m_addSearchSeq;
    m_addResults->clear();
    if (q.size() < 2) return;
    m_api->searchNcUsers(q, this,
        [this, seq, q](bool ok, const QVector<NcUser> &users) {
            showAddSearchResults(seq, q, ok, users);
        });
}

void ConversationInfoDialog::showAddSearchResults(int seq, const QString &query, bool ok,
                                                  const QVector<NcUser> &users)
{
    // runAddSearch() captured only `this` until now, so a reply that came back after the box
    // was cleared or changed filled the list with rows for a query nobody could see, and a
    // click on one added that person. A reply counts only for the newest search, and only
    // while the box still holds what it searched for.
    if (seq != m_addSearchSeq || query != m_addSearch->text().trimmed())
        return;
    if (!ok) { m_addResults->addItem(tr("Search failed.")); return; }
    for (const NcUser &u : users) {
        // Groups and Teams are added by passing their own `source`;
        // the server expands them to their members. This used to skip
        // everything that was not an account, on the mistaken premise
        // that the participants endpoint took users only - so a
        // moderator could never add a department group to an existing
        // room and got no explanation, just no search results.
        // QString, not QChar: QChar is a single UTF-16 code unit and
        // every one of these emoji is above the BMP, so a QChar would
        // silently truncate to a replacement glyph.
        const QString glyph = u.source == QStringLiteral("groups")
                                  ? QStringLiteral("\U0001F465")   // busts = group
                                  : u.source == QStringLiteral("circles")
                                        ? QStringLiteral("\U0001F310")  // globe = Team
                                        : QStringLiteral("\U0001F464"); // bust = person
        const QString label = QStringLiteral("%1  %2  ·  %3")
                                  .arg(glyph)
                                  .arg(u.displayName.isEmpty() ? u.id : u.displayName,
                                       u.id);
        auto *item = new QListWidgetItem(label, m_addResults);
        item->setData(kActorIdRole, u.id);
        item->setData(kUserSourceRole, u.source);
    }
    if (m_addResults->count() == 0) m_addResults->addItem(tr("No matches."));
}

void ConversationInfoDialog::onAddResultClicked(QListWidgetItem *item)
{
    if (!item) return;
    const QString userId = item->data(kActorIdRole).toString();
    if (userId.isEmpty()) return;
    const QString source = item->data(kUserSourceRole).toString();
    const QString queryAtClick = m_addSearch->text();

    m_status->setText(tr("Adding %1\u2026").arg(userId));
    m_api->addRoomParticipant(m_token, userId, this,
        [this, userId, queryAtClick](bool ok, const QString &err) {
            onParticipantAdded(userId, queryAtClick, ok, err);
        }, source);
}

void ConversationInfoDialog::onParticipantAdded(const QString &userId,
                                                const QString &queryAtClick, bool ok,
                                                const QString &err)
{
    if (!ok) {
        // The query and the results stay, so the user can try again from where they were.
        m_status->setProperty("role", "danger");
        m_status->setText(tr("Couldn't add %1: %2")
                              .arg(userId, err.isEmpty() ? tr("refused") : err));
        return;
    }
    // The reload first: it writes "Loading members..." to the status line, which used to land
    // on top of the confirmation below in this same call.
    refreshParticipants();
    m_status->setProperty("role", "secondary");
    m_status->setText(tr("Added %1.").arg(userId));
    // Ready for the next person: an empty box with the cursor in it. Only if the box still
    // holds the search that found this person; if the user has started typing the next name
    // while the add was in flight, that text is theirs.
    if (m_addSearch->text() == queryAtClick) {
        m_addSearch->clear();     // textChanged: a search still in flight is now stale
        m_addDebounce->stop();    // ...and there is nothing to search for
        m_addResults->clear();
    }
    // The cursor goes back to the box only from the panel itself (the result clicked, the box)
    // or from nowhere. Anywhere else -- the user has moved on to the name or the description
    // while the add was in flight -- taking the focus would end that edit, and editingFinished
    // saves a half-typed name or description to the server.
    QWidget *focus = focusWidget();
    if (m_pages->currentWidget() == m_addPanel
        && (!focus || focus == m_addPanel || m_addPanel->isAncestorOf(focus)))
        m_addSearch->setFocus();
    emit roomChanged();
}

void ConversationInfoDialog::onLeaveClicked()
{
    auto reply = QMessageBox::question(this, tr("Leave conversation?"),
        tr("You'll stop receiving messages from this conversation. You can be re-added by a moderator."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;
    m_status->setText(tr("Leaving\u2026"));
    m_api->leaveRoom(m_token, this,
        [this](bool ok, const QString &err) {
            if (!ok) {
                m_status->setProperty("role", "danger");
                m_status->setText(err.isEmpty() ? tr("Leave refused.") : err);
                return;
            }
            emit roomDeleted();
            accept();
        });
}

void ConversationInfoDialog::onClearHistoryClicked()
{
    auto reply = QMessageBox::warning(this, tr("Clear chat history?"),
        tr("This deletes ALL messages in this conversation for EVERYONE, on every "
           "device, and cannot be undone."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;
    m_status->setText(tr("Clearing history…"));
    m_api->clearChatHistory(m_token, this, [this](bool ok, const QString &err) {
        if (!ok) {
            m_status->setProperty("role", "danger");
            m_status->setText(err.isEmpty()
                ? tr("Only moderators can clear the history.") : err);
            return;
        }
        // The open conversation purges itself when the server's "history_cleared"
        // system message arrives via the poller (here and on every other device).
        m_status->setText(tr("History cleared."));
        accept();
    });
}

void ConversationInfoDialog::onDeleteClicked()
{
    auto reply = QMessageBox::warning(this, tr("Delete conversation?"),
        tr("This cannot be undone. All messages and call history for this conversation will be deleted for everyone."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;
    m_status->setText(tr("Deleting\u2026"));
    m_api->deleteRoom(m_token, this,
        [this](bool ok, const QString &err) {
            if (!ok) {
                m_status->setProperty("role", "danger");
                m_status->setText(err.isEmpty() ? tr("Delete refused.") : err);
                return;
            }
            emit roomDeleted();
            accept();
        });
}

// \u2500\u2500\u2500 Bots \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500

void ConversationInfoDialog::refreshBots()
{
    // Clear the bots container.
    while (auto *item = m_botsLayout->takeAt(0)) {
        if (auto *w = item->widget()) w->deleteLater();
        delete item;
    }
    // Bump the sequence so any in-flight callback from a previous refresh
    // (e.g. user clicked Remove twice in quick succession) bails out before
    // appending stale rows on top of our freshly cleared layout.
    const int seq = ++m_botsRefreshSeq;
    m_api->fetchEnabledBots(m_token, this,
        [this, seq](bool ok, const QVector<BotInfo> &bots) {
            if (seq != m_botsRefreshSeq) return;
            if (!ok) {
                auto *l = new QLabel(tr("(couldn't load bots)"), m_botsContainer);
                l->setProperty("role", "secondary");
                m_botsLayout->addWidget(l);
                return;
            }
            if (bots.isEmpty()) {
                auto *l = new QLabel(tr("No bots enabled in this conversation."),
                                     m_botsContainer);
                l->setProperty("role", "secondary");
                m_botsLayout->addWidget(l);
                return;
            }
            for (const BotInfo &b : bots) populateBotRow(b);
        });
}

// ─── Shared files ─────────────────────────────────────────

void ConversationInfoDialog::refreshSharedFiles()
{
    m_filesList->clear();
    auto *loading = new QListWidgetItem(tr("Loading…"), m_filesList);
    loading->setFlags(Qt::NoItemFlags);

    // One recent page is enough — this is a quick "what was shared lately"
    // view, not a full archive crawl.
    QUrlQuery params;
    params.addQueryItem("lookIntoFuture", "0");
    params.addQueryItem("limit", "200");
    // Browsing the shared-files panel must NOT mark the conversation read — the
    // NC server marks read by default when setReadMarker is absent on a /chat GET.
    params.addQueryItem("setReadMarker", "0");

    QPointer<ConversationInfoDialog> guard(this);
    m_api->getArray("apps/spreed/api/v1/chat/" + m_token, params,
        [guard](bool ok, const QJsonArray &data, int) {
        if (!guard) return;
        ConversationInfoDialog *self = guard;
        self->m_filesList->clear();

        if (!ok) {
            auto *it = new QListWidgetItem(tr("(couldn't load shared files)"),
                                           self->m_filesList);
            it->setFlags(Qt::NoItemFlags);
            return;
        }

        auto humanSize = [](qint64 b) -> QString {
            if (b <= 0) return QString();
            static const char *u[] = { "B", "KB", "MB", "GB", "TB" };
            double s = b;
            int i = 0;
            while (s >= 1024.0 && i < 4) { s /= 1024.0; ++i; }
            return (i == 0) ? QString("%1 B").arg(b)
                            : QString::number(s, 'f', s < 10 ? 1 : 0) + ' ' + u[i];
        };
        auto glyph = [](const QString &mime) -> QString {
            if (mime.startsWith("image/")) return QString::fromUtf8("\xF0\x9F\x96\xBC");
            if (mime.startsWith("video/")) return QString::fromUtf8("\xF0\x9F\x8E\xAC");
            if (mime.startsWith("audio/")) return QString::fromUtf8("\xF0\x9F\x8E\xB5");
            if (mime == "application/pdf") return QString::fromUtf8("\xF0\x9F\x93\x95");
            return QString::fromUtf8("\xF0\x9F\x93\x84");
        };

        QSet<QString> seen;
        int shown = 0;
        for (const QJsonValue &mv : data) {
            const QJsonObject mp = mv.toObject()
                                      .value("messageParameters").toObject();
            for (auto it = mp.begin(); it != mp.end(); ++it) {
                const QJsonObject f = it.value().toObject();
                if (f.value("type").toString() != "file") continue;

                const QString id = f.value("id").toString();
                if (!id.isEmpty() && seen.contains(id)) continue;
                if (!id.isEmpty()) seen.insert(id);

                const QString name = f.value("name").toString();
                const QString link = f.value("link").toString();
                const QString mime = f.value("mimetype").toString();
                const qint64  size = f.value("size").toVariant().toLongLong();
                if (name.isEmpty()) continue;

                const QString sz = humanSize(size);
                const QString label = sz.isEmpty()
                    ? QString("%1  %2").arg(glyph(mime), name)
                    : QString("%1  %2   ·   %3").arg(glyph(mime), name, sz);
                auto *row = new QListWidgetItem(label, self->m_filesList);
                if (!link.isEmpty()) {
                    row->setData(Qt::UserRole, link);
                    row->setToolTip(tr("Open in browser"));
                } else {
                    row->setFlags(Qt::NoItemFlags);
                }
                ++shown;
            }
        }

        if (shown == 0) {
            auto *it = new QListWidgetItem(
                tr("No files shared in recent messages."), self->m_filesList);
            it->setFlags(Qt::NoItemFlags);
        }
    });
}

void ConversationInfoDialog::populateBotRow(const BotInfo &bot)
{
    auto *row = new QWidget(m_botsContainer);
    row->setStyleSheet("background: transparent;");
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(8, 6, 8, 6);
    lay->setSpacing(10);

    auto *iconLbl = new QLabel(row);
    iconLbl->setPixmap(makeBotIcon(28));
    iconLbl->setFixedSize(28, 28);
    iconLbl->setStyleSheet("background: transparent;");
    lay->addWidget(iconLbl);

    auto *label = new QLabel(bot.name, row);
    label->setStyleSheet("background: transparent; font-weight: 500;");
    if (!bot.description.isEmpty()) label->setToolTip(bot.description);
    lay->addWidget(label, 1);

    if (bot.state == 3 && !bot.errorMessage.isEmpty()) {
        auto *err = new QLabel(bot.errorMessage, row);
        err->setProperty("role", "danger");
        err->setToolTip(bot.errorMessage);
        lay->addWidget(err);
    } else if (!bot.isEnabled()) {
        auto *note = new QLabel(tr("disabled"), row);
        note->setProperty("role", "secondary");
        lay->addWidget(note);
    }

    if (m_amOwnerOrMod) {
        const int botId = bot.id;
        const QString botName = bot.name;
        const bool errored = (bot.state == 3);
        if (bot.isEnabled() || errored) {
            // Installed & active here \u2192 moderators can disable it.
            auto *removeBtn = new QPushButton(tr("Remove"), row);
            removeBtn->setProperty("variant", "danger");
            removeBtn->setCursor(Qt::PointingHandCursor);
            connect(removeBtn, &QPushButton::clicked, this, [this, botId, botName]() {
                auto reply = QMessageBox::question(
                    this, tr("Remove bot"),
                    tr("Remove %1 from this conversation?").arg(botName),
                    QMessageBox::Yes | QMessageBox::No);
                if (reply != QMessageBox::Yes) return;
                m_status->setProperty("role", "secondary");
                m_status->setText(tr("Removing bot\u2026"));
                m_api->setBotEnabled(m_token, botId, false, this,
                    [this](bool ok, int httpStatus) {
                        if (!ok) {
                            m_status->setProperty("role", "danger");
                            m_status->setText(tr("Couldn't remove (HTTP %1).").arg(httpStatus));
                        } else {
                            m_status->setText(tr("Bot removed."));
                            refreshBots();
                        }
                    });
            });
            lay->addWidget(removeBtn);
        } else {
            // Disabled here \u2192 any moderator can enable it (no admin/CLI
            // needed) provided the bot wasn't installed with --no-setup.
            auto *enableBtn = new QPushButton(tr("Enable"), row);
            // Explicit, self-contained style (theme-tokenized): set directly on
            // the button so it wins over the selector-less `background` on
            // m_botsContainer that would otherwise leak in and make #primary
            // unreadable here.
            enableBtn->setStyleSheet(primaryButtonStyle());
            enableBtn->setCursor(Qt::PointingHandCursor);
            connect(enableBtn, &QPushButton::clicked, this, [this, botId]() {
                m_status->setProperty("role", "secondary");
                m_status->setText(tr("Enabling bot\u2026"));
                m_api->setBotEnabled(m_token, botId, true, this,
                    [this](bool ok, int httpStatus) {
                        if (!ok) {
                            m_status->setProperty("role", "danger");
                            m_status->setText((httpStatus == 400 || httpStatus == 403)
                                ? tr("Server blocked it \u2014 this bot was installed "
                                     "with --no-setup (admin-only via occ).")
                                : tr("Couldn't enable (HTTP %1).").arg(httpStatus));
                        } else {
                            m_status->setText(tr("Bot enabled."));
                            refreshBots();
                        }
                    });
            });
            lay->addWidget(enableBtn);
        }
    }

    m_botsLayout->addWidget(row);
}

void ConversationInfoDialog::onAddBotClicked()
{
    // Build a small modal: admin gets the full server list with Enable
    // buttons; non-admin moderators get a "Bot ID" input (admin still
    // needs to share that ID out-of-band \u2014 the /bot/admin endpoint is
    // admin-only).
    auto *dlg = new QDialog(this);
    dlg->setWindowTitle(tr("Add bot"));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->resize(420, 360);
    dlg->setStyleSheet(this->styleSheet());

    auto *lay = new QVBoxLayout(dlg);
    lay->setContentsMargins(20, 16, 20, 16);

    auto *info = new QLabel(tr("Select a server-installed bot to enable in this "
                               "conversation. If the list is empty, you may not "
                               "have admin permission \u2014 enter a bot ID below."),
                            dlg);
    info->setWordWrap(true);
    info->setProperty("role", "secondary");
    lay->addWidget(info);

    auto *list = new QListWidget(dlg);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    lay->addWidget(list, 1);

    auto *idRow = new QHBoxLayout;
    auto *idLabel = new QLabel(tr("Bot ID:"), dlg);
    auto *idEdit = new QLineEdit(dlg);
    idEdit->setPlaceholderText(tr("e.g. 3"));
    auto *idBtn = new QPushButton(tr("Enable"), dlg);
    idBtn->setProperty("variant", "primary");
    idBtn->setCursor(Qt::PointingHandCursor);
    idRow->addWidget(idLabel);
    idRow->addWidget(idEdit, 1);
    idRow->addWidget(idBtn);
    lay->addLayout(idRow);

    auto *status = new QLabel(QString(), dlg);
    status->setProperty("role", "secondary");
    lay->addWidget(status);

    // Populate the list with server bots (admin only)
    list->addItem(tr("Loading\u2026"));
    m_api->fetchAllBots(dlg, [this, list, status, dlg](bool ok, const QVector<BotInfo> &bots) {
        list->clear();
        if (!ok) {
            auto *it = new QListWidgetItem(tr("(error loading bot list)"));
            it->setFlags(Qt::NoItemFlags);
            list->addItem(it);
            return;
        }
        if (bots.isEmpty()) {
            auto *it = new QListWidgetItem(tr("(no bots visible \u2014 admin only; "
                                              "use the Bot ID input below)"));
            it->setFlags(Qt::NoItemFlags);
            list->addItem(it);
            return;
        }
        for (const BotInfo &b : bots) {
            auto *row = new QWidget;
            auto *rl = new QHBoxLayout(row);
            rl->setContentsMargins(8, 6, 8, 6);
            rl->setSpacing(10);
            auto *icon2 = new QLabel(row);
            icon2->setPixmap(makeBotIcon(28));
            icon2->setFixedSize(28, 28);
            rl->addWidget(icon2);
            auto *txt = new QLabel(b.name + QStringLiteral("  ·  #")
                                  + QString::number(b.id), row);
            txt->setStyleSheet("font-weight: 500;");
            rl->addWidget(txt, 1);
            auto *enableBtn = new QPushButton(tr("Enable"), row);
            // Explicit style (not #primary), theme-tokenized: self-contained so
            // it survives both the inherited dialog QSS and the early sizeHint
            // pass. Extra min-height keeps the styled button from being clipped.
            enableBtn->setStyleSheet(primaryButtonStyle("min-height: 18px;"));
            enableBtn->setCursor(Qt::PointingHandCursor);
            const int botId = b.id;
            connect(enableBtn, &QPushButton::clicked, dlg, [this, botId, status, dlg]() {
                status->setText(tr("Enabling\u2026"));
                m_api->setBotEnabled(m_token, botId, true, dlg,
                    [this, status, dlg](bool ok, int httpStatus) {
                        if (ok) {
                            refreshBots();
                            dlg->accept();
                        } else {
                            status->setProperty("role", "danger");
                            status->setText(tr("Failed (HTTP %1).").arg(httpStatus));
                        }
                    });
            });
            rl->addWidget(enableBtn);
            auto *item = new QListWidgetItem(list);
            // Fixed, generous height: row->sizeHint() here is computed before
            // the inherited stylesheet is polished, so it under-reports and the
            // styled Enable button gets clipped to a thin line.
            const QSize sh = row->sizeHint();
            item->setSizeHint(QSize(qMax(260, sh.width()), 44));
            item->setFlags(Qt::NoItemFlags);
            list->setItemWidget(item, row);
        }
    });

    // Manual ID input
    connect(idBtn, &QPushButton::clicked, dlg, [this, idEdit, status, dlg]() {
        bool ok = false;
        const int botId = idEdit->text().trimmed().toInt(&ok);
        if (!ok || botId <= 0) {
            status->setProperty("role", "danger");
            status->setText(tr("Enter a positive integer."));
            return;
        }
        status->setProperty("role", "secondary");
        status->setText(tr("Enabling\u2026"));
        m_api->setBotEnabled(m_token, botId, true, dlg,
            [this, status, dlg](bool apiOk, int httpStatus) {
                if (apiOk) {
                    refreshBots();
                    dlg->accept();
                } else {
                    status->setProperty("role", "danger");
                    status->setText(tr("Failed (HTTP %1) \u2014 bot may not exist or "
                                       "you lack permission.").arg(httpStatus));
                }
            });
    });

    dlg->open();
}

void ConversationInfoDialog::setSipInfo(int sipEnabled, const QString &pin)
{
    if (!m_sipLabel) return;
    if (sipEnabled == 0) {              // no SIP on this room -- show nothing
        m_sipLabel->setVisible(false);
        return;
    }
    // sipEnabled == 2 means dial-in without a per-user PIN, so a PIN line
    // there would be a lie. Selectable text rather than a button: the useful
    // action is reading the code out to somebody on another phone.
    m_sipLabel->setText(pin.isEmpty() || sipEnabled == 2
        ? tr("You can dial in to this conversation by phone.")
        : tr("You can dial in by phone. Your PIN is %1.").arg(pin));
    m_sipLabel->setVisible(true);
}

// ─── Talk 25 ─────────────────────────────────────────────────────────────

void ConversationInfoDialog::setTalk25State(const Talk25State &state)
{
    m_talk25 = state;
    applyTalk25State();
}

void ConversationInfoDialog::applyTalk25State()
{
    const int attrs = m_talk25.attributes;
    const talq::RoomKind kind = talq::roomKind(attrs);

    // What kind of room this is, in the words of what changes for the reader.
    QString kindText;
    switch (kind) {
    case talq::RoomKind::Channel:
        kindText = tr("This is a channel. Only moderators can post here. Calls are "
                      "turned off, and only moderators see the member list.");
        break;
    case talq::RoomKind::Announcement:
        kindText = tr("This is an announcement. Only moderators can post here. "
                      "Members cannot leave it, calls are turned off, and everyone "
                      "is notified of every message by default.");
        break;
    case talq::RoomKind::Classified:
        kindText = tr("This is a classified conversation. Guests, phone dial-in, "
                      "recordings and bots are turned off, and message text stays "
                      "out of lists and notifications.");
        break;
    case talq::RoomKind::Plain:
        break;
    }
    m_kindLabel->setText(kindText);
    m_kindLabel->setVisible(!kindText.isEmpty());

    // Preserve: the owner's switch. Shown whenever the server offers it, not
    // only once the room is preserved, so there is a way to turn it ON.
    const bool preserved = talq::isPreservedRoom(attrs);
    const bool isOwner = (m_myType == RoomParticipant::Owner);
    const bool showPreserve = m_talk25.canPreserve && isOwner && m_roomType != 1;
    {
        const QSignalBlocker block(m_preserveCheck);
        m_preserveCheck->setChecked(preserved);
    }
    m_preserveCheck->setVisible(showPreserve);
    if (showPreserve) {
        m_preserveHint->setText(
            tr("While it is preserved the conversation cannot be deleted, its chat "
               "history cannot be cleared, and its public link and listing cannot "
               "be changed. Only the owner can turn this on or off."));
        m_preserveHint->setVisible(true);
    } else if (preserved) {
        // Not the owner, but the greyed-out Delete still needs its reason.
        m_preserveHint->setText(
            tr("This conversation is preserved: it cannot be deleted and its chat "
               "history cannot be cleared."));
        m_preserveHint->setVisible(true);
    } else {
        m_preserveHint->setVisible(false);
    }

    // Classified auto-deletion. Say it plainly: this is the one thing in Talk 25
    // that can remove a conversation nobody asked to remove.
    const bool queued = (m_talk25.objectType == QLatin1String("classified"));
    const bool kept = (m_talk25.objectType == QLatin1String("classified_persist"));
    QString keepText;
    if (queued && m_talk25.classifiedDeleteAt > 0) {
        const QString when = QLocale().toString(
            QDateTime::fromSecsSinceEpoch(m_talk25.classifiedDeleteAt).date(),
            QLocale::LongFormat);
        keepText = tr("A call has taken place here, so this conversation is queued for "
                      "automatic deletion: from about %1, unless someone writes in it "
                      "before then. A moderator can keep it for good.").arg(when);
    } else if (queued) {
        keepText = tr("A call has taken place here, so this conversation is queued for "
                      "automatic deletion after a period without activity. A moderator "
                      "can keep it for good.");
    } else if (kept) {
        keepText = tr("A moderator has kept this conversation. It will not be deleted "
                      "automatically.");
    } else if (kind == talq::RoomKind::Classified && m_talk25.classifiedRetentionDays > 0) {
        keepText = tr("Once a call has ended here, this conversation is deleted "
                      "automatically after %n day(s) without activity, unless a "
                      "moderator keeps it.", nullptr, m_talk25.classifiedRetentionDays);
    }
    m_keepLabel->setProperty("role", queued ? "danger" : "secondary");
    m_keepLabel->style()->unpolish(m_keepLabel);
    m_keepLabel->style()->polish(m_keepLabel);
    m_keepLabel->setText(keepText);
    m_keepLabel->setVisible(!keepText.isEmpty());
    m_keepBtn->setVisible(queued && m_talk25.canKeep && m_amOwnerOrMod);
    m_keepRow->setVisible(!keepText.isEmpty());

    m_kindBox->setVisible(!kindText.isEmpty() || showPreserve || preserved || !keepText.isEmpty());

    // Leave: the server has said whether it will accept it.
    switch (talq::leaveBlock(attrs, m_talk25.canLeave)) {
    case talq::LeaveBlock::None:
        m_leaveBtn->setEnabled(true);
        m_leaveBtn->setToolTip(QString());
        break;
    case talq::LeaveBlock::Announcement:
        m_leaveBtn->setEnabled(false);
        m_leaveBtn->setToolTip(tr("An announcement cannot be left. Ask a moderator to "
                                  "remove you."));
        break;
    case talq::LeaveBlock::Refused:
        m_leaveBtn->setEnabled(false);
        m_leaveBtn->setToolTip(tr("This conversation cannot be left."));
        break;
    }

    // Delete and Clear history. Only the PRESERVED case is acted on: the
    // server's bare canDeleteConversation=false also covers rooms where TalQ has
    // always offered Delete (a one-to-one), and removing that is not this change.
    const bool blocked = (talq::deleteBlock(attrs, m_talk25.canDelete)
                          == talq::DeleteBlock::Preserved);
    m_deleteBtn->setEnabled(!blocked);
    m_deleteBtn->setToolTip(blocked
        ? tr("This conversation is preserved. Turn off \"Preserve\" first.") : QString());
    m_clearHistoryBtn->setEnabled(!blocked);
    m_clearHistoryBtn->setToolTip(blocked
        ? tr("The chat history of a preserved conversation cannot be cleared.") : QString());
}

namespace {
// Talk's refusal codes are machine words, never user text (CLAUDE.md, ApiClient).
QString talk25ErrorText(const QString &code)
{
    if (code == QLatin1String("permissions"))
        return QObject::tr("Only the owner can do that.");
    if (code == QLatin1String("preserved"))
        return QObject::tr("This conversation is preserved.");
    if (code == QLatin1String("object-type"))
        return QObject::tr("This conversation is not queued for deletion, so there is "
                           "nothing to keep.");
    return QObject::tr("The server refused that.");
}
} // namespace

void ConversationInfoDialog::onPreserveToggled(bool checked)
{
    // The box has already flipped under the click. Hold it still until the
    // server answers, then settle on what is TRUE rather than what was asked.
    m_preserveCheck->setEnabled(false);
    m_status->setProperty("role", "secondary");
    m_status->setText(checked ? tr("Preserving…") : tr("Turning preserve off…"));
    m_api->setRoomPreserved(m_token, checked, this,
        [this, checked](bool ok, int, const QString &code) {
            m_preserveCheck->setEnabled(true);
            if (!ok) {
                m_status->setProperty("role", "danger");
                m_status->setText(talk25ErrorText(code));
                applyTalk25State();   // back to what the server still says
                return;
            }
            if (checked) m_talk25.attributes |= talq::AttributePreserve;
            else         m_talk25.attributes &= ~talq::AttributePreserve;
            m_status->setText(checked ? tr("Preserved.") : tr("No longer preserved."));
            applyTalk25State();
            emit roomChanged();   // the list re-reads the room
        });
}

void ConversationInfoDialog::onKeepClicked()
{
    m_keepBtn->setEnabled(false);
    m_status->setProperty("role", "secondary");
    m_status->setText(tr("Keeping this conversation…"));
    m_api->keepClassifiedRoom(m_token, this,
        [this](bool ok, const QString &code) {
            m_keepBtn->setEnabled(true);
            if (!ok) {
                m_status->setProperty("role", "danger");
                m_status->setText(talk25ErrorText(code));
                return;
            }
            m_talk25.objectType = QStringLiteral("classified_persist");
            m_status->setText(tr("Kept. It will not be deleted automatically."));
            applyTalk25State();
            emit roomChanged();
        });
}
