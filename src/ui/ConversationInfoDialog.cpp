#include "ConversationInfoDialog.h"

#include "core/ApiClient.h"
#include "core/NcUser.h"
#include "core/ShiftStatusService.h"
#include "painter/PainterTheme.h"

#include <QCursor>
#include <QEvent>
#include <QPalette>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QLabel>
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
#include <QVBoxLayout>
#include <QTimer>
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

    // #25 — group picture (groups only; 1:1 avatars are the other user's). A
    // placeholder disc with the conversation's initial; moderators get a
    // "Change picture" button that uploads to the Talk room-avatar API.
    if (roomType != 1) {
        auto *avRow = new QHBoxLayout();
        m_avatar = new QLabel(this);
        m_avatar->setFixedSize(56, 56);
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
            "QLabel { background:%1; color:%2; border-radius:28px;"
            "         font-size:24px; font-weight:600; }")
            .arg(avatarBg.name(QColor::HexRgb), th.inkOn(avatarBg).name(QColor::HexRgb)));
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
        outer->addLayout(avRow);
        outer->addSpacing(18);
        fetchRoomAvatar();   // #25 — show the CURRENT picture, not just the letter
    }

    // Name
    auto *nameEyebrow = new QLabel(tr("NAME"), this);
    nameEyebrow->setObjectName("eyebrow");
    outer->addWidget(nameEyebrow);
    m_nameEdit = new QLineEdit(currentName, this);
    m_nameEdit->setObjectName("name");
    m_nameEdit->setReadOnly(!m_amOwnerOrMod || roomType == 1);
    outer->addWidget(m_nameEdit);
    outer->addSpacing(16);

    // Description
    auto *descEyebrow = new QLabel(tr("DESCRIPTION"), this);
    descEyebrow->setObjectName("eyebrow");
    outer->addWidget(descEyebrow);
    m_descEdit = new QLineEdit(currentDescription, this);
    m_descEdit->setPlaceholderText(tr("Add a description\u2026"));
    m_descEdit->setReadOnly(!m_amOwnerOrMod);
    outer->addWidget(m_descEdit);
    outer->addSpacing(20);

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
    outer->addLayout(memRow);
    outer->addSpacing(6);

    m_memberList = new QListWidget(this);
    m_memberList->setSelectionMode(QAbstractItemView::NoSelection);
    m_memberList->setContextMenuPolicy(Qt::CustomContextMenu);
    outer->addWidget(m_memberList, 1);

    // Inline add-people panel (hidden until "+ Add people" is clicked)
    m_addPanel = new QWidget(this);
    auto *addLay = new QVBoxLayout(m_addPanel);
    addLay->setContentsMargins(0, 8, 0, 0);
    addLay->setSpacing(6);
    m_addSearch = new QLineEdit(m_addPanel);
    m_addSearch->setPlaceholderText(tr("\U0001F50D  Search people to add\u2026"));
    addLay->addWidget(m_addSearch);
    m_addResults = new QListWidget(m_addPanel);
    m_addResults->setFixedHeight(150);
    m_addResults->setSelectionMode(QAbstractItemView::NoSelection);
    addLay->addWidget(m_addResults);
    outer->addWidget(m_addPanel);
    m_addPanel->hide();

    m_addDebounce = new QTimer(this);
    m_addDebounce->setSingleShot(true);
    m_addDebounce->setInterval(250);

    // ── Bots section ──────────────────────────────────────
    // Everyone in the room can SEE which bots are enabled (transparent ID of
    // automated participants). Only moderators get the +Add button — the API
    // would reject a non-moderator's enable call anyway, so showing the
    // button is a permissions-leak / dead-button trap.
    outer->addSpacing(16);
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
    outer->addLayout(botRow);
    outer->addSpacing(6);

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
    outer->addWidget(m_botsContainer);

    connect(m_addBotBtn, &QPushButton::clicked,
            this, &ConversationInfoDialog::onAddBotClicked);

    // ── Shared files section ──────────────────────────────
    // Scans the most recent chat page for file shares. Bounded to one API
    // page so it stays cheap; click a row to open the file in the browser.
    outer->addSpacing(16);
    m_filesHeader = new QLabel(tr("SHARED FILES"), this);
    m_filesHeader->setObjectName("eyebrow");
    outer->addWidget(m_filesHeader);
    outer->addSpacing(6);

    m_filesList = new QListWidget(this);
    m_filesList->setSelectionMode(QAbstractItemView::NoSelection);
    m_filesList->setFixedHeight(150);
    m_filesList->setCursor(Qt::PointingHandCursor);
    outer->addWidget(m_filesList);
    connect(m_filesList, &QListWidget::itemClicked, this,
            [](QListWidgetItem *item) {
        const QString link = item->data(Qt::UserRole).toString();
        if (!link.isEmpty())
            QDesktopServices::openUrl(QUrl(link));
    });

    outer->addSpacing(8);
    // Dial-in details. Created hidden and filled by setSipInfo(); a room
    // without SIP never shows the row, which is every room on a server with no
    // SIP bridge configured.
    m_sipLabel = new QLabel(QString(), this);
    m_sipLabel->setWordWrap(true);
    m_sipLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_sipLabel->setVisible(false);
    outer->addWidget(m_sipLabel);

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
        || e->type() == QEvent::ThemeChange)
        applyChrome();
}

void ConversationInfoDialog::applyAvatarPixmap(const QImage &img)
{
    // Center-crop to a square and scale to the 56px disc, so the preview is
    // never stretched (the old setScaledContents path squished wide photos) and
    // matches EXACTLY what setRoomAvatar uploads (center-cropped square).
    if (!m_avatar || img.isNull()) return;
    const int side = qMin(img.width(), img.height());
    const QImage sq = img.copy((img.width() - side) / 2, (img.height() - side) / 2, side, side)
                         .scaled(56, 56, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    m_avatar->setText(QString());
    m_avatar->setStyleSheet(QStringLiteral("QLabel { border-radius:28px; }"));
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
            if (!ok) { m_status->setText(tr("Couldn't load members.")); return; }
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
                    word = (st == talq::ShiftState::OnBreak) ? tr("On break") : tr("Off shift");
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

    m_status->clear();
}

bool ConversationInfoDialog::eventFilter(QObject *watched, QEvent *event)
{
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
    const bool opening = !m_addPanel->isVisible();
    m_addPanel->setVisible(opening);
    m_addBtn->setText(opening ? tr("Close add panel") : tr("+ Add people"));
    if (opening) {
        m_addSearch->clear();
        m_addResults->clear();
        m_addSearch->setFocus();
    }
}

void ConversationInfoDialog::onAddSearchChanged()
{
    m_addDebounce->start();
}

void ConversationInfoDialog::runAddSearch()
{
    const QString q = m_addSearch->text().trimmed();
    m_addResults->clear();
    if (q.size() < 2) return;
    m_api->searchNcUsers(q, this,
        [this](bool ok, const QVector<NcUser> &users) {
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
        });
}

void ConversationInfoDialog::onAddResultClicked(QListWidgetItem *item)
{
    if (!item) return;
    const QString userId = item->data(kActorIdRole).toString();
    if (userId.isEmpty()) return;
    const QString source = item->data(kUserSourceRole).toString();

    m_status->setText(tr("Adding %1\u2026").arg(userId));
    m_api->addRoomParticipant(m_token, userId, this,
        [this, userId](bool ok, const QString &err) {
            if (!ok) {
                m_status->setProperty("role", "danger");
                m_status->setText(tr("Couldn't add %1: %2")
                                      .arg(userId, err.isEmpty() ? tr("refused") : err));
                return;
            }
            m_status->setProperty("role", "secondary");
            m_status->setText(tr("Added %1.").arg(userId));
            refreshParticipants();
            emit roomChanged();
        }, source);
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
