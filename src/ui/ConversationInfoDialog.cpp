#include "ConversationInfoDialog.h"

#include "core/ApiClient.h"
#include "core/NcUser.h"

#include <QCursor>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QTimer>

namespace {
constexpr int kAttendeeIdRole    = Qt::UserRole + 1;
constexpr int kActorIdRole       = Qt::UserRole + 2;
constexpr int kParticipantTypeRole = Qt::UserRole + 3;

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
}

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
    setStyleSheet(
        "QDialog { background: #141210; color: #f4efe6; }"
        "QLabel  { color: #f4efe6; }"
        "QLabel#eyebrow { color: #7a726a; font-size: 10px; letter-spacing: 2px;"
        "  text-transform: uppercase; font-weight: 700; }"
        "QLabel#headline { color: #f4efe6; font-size: 22px; font-weight: 600;"
        "  letter-spacing: -0.2px; }"
        "QLineEdit { background: transparent; border: none;"
        "  border-bottom: 1px solid #2a241f; padding: 8px 0; color: #f4efe6;"
        "  font-size: 14px; selection-background-color: #14b8a6;"
        "  selection-color: #0e1817; }"
        "QLineEdit:focus { border-bottom-color: #14b8a6; }"
        "QLineEdit#name  { font-size: 20px; font-weight: 600; }"
        "QListWidget { background: #1a1613; border: 1px solid #2a241f;"
        "  border-radius: 12px; color: #f4efe6; padding: 4px; outline: none; }"
        "QListWidget::item { padding: 10px 12px; border-radius: 8px; color: #f4efe6; }"
        "QListWidget::item:hover    { background: #241f1a; }"
        "QListWidget::item:selected { background: #241f1a; }"
        "QPushButton { background: #241f1a; color: #f4efe6; border: none;"
        "  border-radius: 8px; padding: 8px 16px; font-size: 13px;"
        "  font-weight: 500; }"
        "QPushButton:hover   { background: #2e271f; }"
        "QPushButton:pressed { background: #2a241f; }"
        "QPushButton#primary { background: #14b8a6; color: #0e1817;"
        "  font-weight: 700; letter-spacing: 0.5px; padding: 8px 18px;"
        "  border-radius: 8px; font-size: 12px; text-transform: uppercase; }"
        "QPushButton#primary:hover   { background: #2dd4bf; }"
        "QPushButton#primary:pressed { background: #0d9488; }"
        "QPushButton#danger  { background: transparent; color: #e8866b;"
        "  border: 1px solid #4a2a22;"
        "  letter-spacing: 0.5px; text-transform: uppercase;"
        "  font-weight: 600; font-size: 12px; padding: 8px 14px; }"
        "QPushButton#danger:hover    { background: rgba(232,134,107,0.08);"
        "  border-color: #e8866b; }"
        "QPushButton#ghost   { background: transparent; color: #a8a096;"
        "  border: none;"
        "  letter-spacing: 0.5px; text-transform: uppercase;"
        "  font-weight: 600; font-size: 12px; padding: 8px 14px; }"
        "QPushButton#ghost:hover     { color: #f4efe6; }"
    );

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(28, 24, 28, 20);
    outer->setSpacing(0);

    auto *title = new QLabel(tr("Conversation info"), this);
    title->setObjectName("headline");
    outer->addWidget(title);
    outer->addSpacing(20);

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
    m_addBtn->setObjectName("primary");
    m_addBtn->setCursor(Qt::PointingHandCursor);
    m_addBtn->setVisible(m_amOwnerOrMod && roomType != 1);
    memRow->addWidget(m_addBtn);
    outer->addLayout(memRow);
    outer->addSpacing(6);

    m_memberList = new QListWidget(this);
    m_memberList->setSelectionMode(QAbstractItemView::NoSelection);
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

    outer->addSpacing(8);
    m_status = new QLabel(QString(), this);
    m_status->setStyleSheet("color: #6f6a62; font-size: 12px;");
    outer->addWidget(m_status);
    outer->addSpacing(8);

    // Footer buttons
    auto *footer = new QHBoxLayout();
    m_leaveBtn = new QPushButton(tr("Leave"), this);
    m_leaveBtn->setObjectName("danger");
    m_leaveBtn->setCursor(Qt::PointingHandCursor);
    footer->addWidget(m_leaveBtn);

    m_deleteBtn = new QPushButton(tr("Delete"), this);
    m_deleteBtn->setObjectName("danger");
    m_deleteBtn->setCursor(Qt::PointingHandCursor);
    m_deleteBtn->setVisible(m_myType == RoomParticipant::Owner);
    footer->addWidget(m_deleteBtn);

    footer->addStretch();
    m_closeBtn = new QPushButton(tr("Done"), this);
    m_closeBtn->setObjectName("ghost");
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
    connect(m_memberList, &QListWidget::itemClicked,
            this, &ConversationInfoDialog::onRemoveMember);
    connect(m_leaveBtn,  &QPushButton::clicked, this, &ConversationInfoDialog::onLeaveClicked);
    connect(m_deleteBtn, &QPushButton::clicked, this, &ConversationInfoDialog::onDeleteClicked);
    connect(m_closeBtn,  &QPushButton::clicked, this, &QDialog::accept);

    refreshParticipants();
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
            m_status->setStyleSheet(ok ? "color: #6f6a62; font-size: 12px;"
                                       : "color: #ff6b6b; font-size: 12px;");
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
            m_status->setStyleSheet(ok ? "color: #6f6a62; font-size: 12px;"
                                       : "color: #ff6b6b; font-size: 12px;");
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
        const QString label = QStringLiteral("\U0001F464  %1  \u00B7  %2  \u00B7  %3%4")
                                  .arg(name, id, role, canRemove);
        auto *item = new QListWidgetItem(label, m_memberList);
        item->setData(kAttendeeIdRole, p.attendeeId);
        item->setData(kActorIdRole, p.userId);
        item->setData(kParticipantTypeRole, p.participantType);
    }
    m_status->clear();
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
    menu.setStyleSheet(
        "QMenu { background: #1c1c1a; border: 1px solid #2a2a26; color: #e4e0da;"
        "  padding: 6px; border-radius: 6px; }"
        "QMenu::item { padding: 6px 16px; border-radius: 4px; }"
        "QMenu::item:selected { background: #2a2a26; }"
        "QMenu::item:disabled { color: #545048; }"
    );

    const bool canManage = m_amOwnerOrMod && !isMe
                           && type != RoomParticipant::Owner;

    QAction *promote = nullptr;
    QAction *demote  = nullptr;
    QAction *remove  = nullptr;

    if (canManage) {
        if (type == RoomParticipant::Moderator
            || type == RoomParticipant::GuestModerator) {
            demote  = menu.addAction(QStringLiteral("\u2B07\uFE0F  ") + tr("Demote to member"));
        } else if (type == RoomParticipant::User
                   || type == RoomParticipant::UserSelfJoined
                   || type == RoomParticipant::Guest) {
            promote = menu.addAction(QStringLiteral("\u2B06\uFE0F  ") + tr("Promote to moderator"));
        }
        menu.addSeparator();
        remove  = menu.addAction(QStringLiteral("\u2716\uFE0F  ") + tr("Remove from conversation"));
    } else {
        // Non-actionable — show a placeholder so the menu isn't empty.
        QAction *info = menu.addAction(isMe ? tr("This is you") : tr("No actions available"));
        info->setEnabled(false);
    }

    QAction *picked = menu.exec(QCursor::pos());
    if (!picked) return;

    auto done = [this](bool ok, const QString &err) {
        if (!ok) {
            m_status->setStyleSheet("color: #ff6b6b; font-size: 12px;");
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
                // Only individual users can be added via the participants
                // endpoint — groups/circles resolve to their members.
                if (u.source != QStringLiteral("users")) continue;
                const QString label = QStringLiteral("\U0001F464  %1  \u00B7  %2")
                                          .arg(u.displayName.isEmpty() ? u.id : u.displayName,
                                               u.id);
                auto *item = new QListWidgetItem(label, m_addResults);
                item->setData(kActorIdRole, u.id);
            }
            if (m_addResults->count() == 0) m_addResults->addItem(tr("No matches."));
        });
}

void ConversationInfoDialog::onAddResultClicked(QListWidgetItem *item)
{
    if (!item) return;
    const QString userId = item->data(kActorIdRole).toString();
    if (userId.isEmpty()) return;

    m_status->setText(tr("Adding %1\u2026").arg(userId));
    m_api->addRoomParticipant(m_token, userId, this,
        [this, userId](bool ok, const QString &err) {
            if (!ok) {
                m_status->setStyleSheet("color: #ff6b6b; font-size: 12px;");
                m_status->setText(tr("Couldn't add %1: %2")
                                      .arg(userId, err.isEmpty() ? tr("refused") : err));
                return;
            }
            m_status->setStyleSheet("color: #6f6a62; font-size: 12px;");
            m_status->setText(tr("Added %1.").arg(userId));
            refreshParticipants();
            emit roomChanged();
        });
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
                m_status->setStyleSheet("color: #ff6b6b; font-size: 12px;");
                m_status->setText(err.isEmpty() ? tr("Leave refused.") : err);
                return;
            }
            emit roomDeleted();
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
                m_status->setStyleSheet("color: #ff6b6b; font-size: 12px;");
                m_status->setText(err.isEmpty() ? tr("Delete refused.") : err);
                return;
            }
            emit roomDeleted();
            accept();
        });
}
