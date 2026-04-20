#include "NewChatDialog.h"

#include "core/ApiClient.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kUserIdRole  = Qt::UserRole + 1;
constexpr int kUserNameRole = Qt::UserRole + 2;
}

NewChatDialog::NewChatDialog(ApiClient *api, QWidget *parent)
    : QDialog(parent), m_api(api)
{
    setWindowTitle(tr("New chat"));
    resize(480, 520);
    setStyleSheet(
        "QDialog { background: #1a1a18; color: #e4e0da; }"
        "QLabel, QRadioButton { color: #e4e0da; }"
        "QLineEdit { background: #222220; border: 1px solid #2a2a26;"
        " border-radius: 6px; padding: 6px 10px; font-size: 14px; color: #e4e0da; }"
        "QLineEdit:focus { border-color: #2ec4b6; }"
        "QListWidget { background: #222220; border: 1px solid #2a2a26;"
        " border-radius: 6px; color: #e4e0da; padding: 4px; }"
        "QListWidget::item { padding: 8px 10px; border-radius: 4px; }"
        "QListWidget::item:hover    { background: #2a2a26; }"
        "QListWidget::item:selected { background: #2ec4b6; color: white; }"
        "QPushButton { background: #2a2a26; color: #e4e0da; border: none;"
        " border-radius: 6px; padding: 6px 16px; font-size: 13px; }"
        "QPushButton:hover    { background: #3a3a34; }"
        "QPushButton#primary  { background: #2ec4b6; color: white; }"
        "QPushButton#primary:hover    { background: #3dd4c6; }"
        "QPushButton#primary:disabled { background: #2a2a26; color: #8a8680; }"
    );

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(10);

    auto *modeRow = new QHBoxLayout();
    m_directRadio = new QRadioButton(tr("Direct (1-on-1)"), this);
    m_groupRadio  = new QRadioButton(tr("Group"), this);
    m_directRadio->setChecked(true);
    modeRow->addWidget(m_directRadio);
    modeRow->addWidget(m_groupRadio);
    modeRow->addStretch();
    outer->addLayout(modeRow);

    // Group-name row, shown only in Group mode.
    m_groupNameRow = new QWidget(this);
    auto *groupNameLayout = new QHBoxLayout(m_groupNameRow);
    groupNameLayout->setContentsMargins(0, 0, 0, 0);
    groupNameLayout->setSpacing(8);
    groupNameLayout->addWidget(new QLabel(tr("Group name:"), m_groupNameRow));
    m_groupNameEdit = new QLineEdit(m_groupNameRow);
    m_groupNameEdit->setPlaceholderText(tr("e.g. Project Alpha"));
    groupNameLayout->addWidget(m_groupNameEdit, 1);
    outer->addWidget(m_groupNameRow);
    m_groupNameRow->hide();

    outer->addWidget(new QLabel(tr("Search users:"), this));
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Type a name or username\u2026"));
    outer->addWidget(m_searchEdit);

    m_results = new QListWidget(this);
    m_results->setSelectionMode(QAbstractItemView::SingleSelection);
    outer->addWidget(m_results, 1);

    m_selectedLabel = new QLabel(tr("No one selected."), this);
    m_selectedLabel->setWordWrap(true);
    m_selectedLabel->setStyleSheet("color: #8a8680; font-size: 12px;");
    outer->addWidget(m_selectedLabel);

    m_status = new QLabel(QString(), this);
    m_status->setStyleSheet("color: #8a8680; font-size: 12px;");
    outer->addWidget(m_status);

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    m_createBtn = new QPushButton(tr("Create"), this);
    m_createBtn->setObjectName("primary");
    m_createBtn->setEnabled(false);
    m_createBtn->setDefault(true);
    btnRow->addWidget(m_cancelBtn);
    btnRow->addWidget(m_createBtn);
    outer->addLayout(btnRow);

    m_searchDebounce = new QTimer(this);
    m_searchDebounce->setSingleShot(true);
    m_searchDebounce->setInterval(250);

    connect(m_directRadio, &QRadioButton::toggled, this, &NewChatDialog::onModeChanged);
    connect(m_groupRadio,  &QRadioButton::toggled, this, &NewChatDialog::onModeChanged);
    connect(m_searchEdit,  &QLineEdit::textChanged, this, &NewChatDialog::onSearchTextChanged);
    connect(m_searchDebounce, &QTimer::timeout, this, &NewChatDialog::runSearch);
    connect(m_results, &QListWidget::itemDoubleClicked,
            this, &NewChatDialog::onResultDoubleClicked);
    connect(m_results, &QListWidget::itemSelectionChanged, this, [this]() {
        if (m_directRadio->isChecked()) {
            m_selected.clear();
            if (auto *cur = m_results->currentItem()) {
                NcUser u;
                u.id = cur->data(kUserIdRole).toString();
                u.displayName = cur->data(kUserNameRole).toString();
                if (!u.id.isEmpty()) m_selected.push_back(u);
            }
            refreshSelectedView();
        }
    });
    connect(m_createBtn, &QPushButton::clicked, this, &NewChatDialog::onCreateClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_groupNameEdit, &QLineEdit::textChanged, this, [this]() { refreshSelectedView(); });
}

void NewChatDialog::onModeChanged()
{
    const bool group = m_groupRadio->isChecked();
    m_groupNameRow->setVisible(group);
    m_selected.clear();
    m_results->clearSelection();
    refreshSelectedView();
}

void NewChatDialog::onSearchTextChanged()
{
    m_searchDebounce->start();
}

void NewChatDialog::runSearch()
{
    const QString q = m_searchEdit->text().trimmed();
    m_results->clear();
    if (q.size() < 2) { setStatus(tr("Type at least 2 characters.")); return; }
    setStatus(tr("Searching\u2026"));
    m_api->searchNcUsers(q, this,
        [this](bool ok, const QVector<NcUser> &users) {
            if (!ok) { setStatus(tr("User search failed."), true); return; }
            for (const NcUser &u : users) {
                QString label = u.displayName.isEmpty() ? u.id
                                                        : QStringLiteral("%1  (%2)")
                                                              .arg(u.displayName, u.id);
                auto *item = new QListWidgetItem(label, m_results);
                item->setData(kUserIdRole,  u.id);
                item->setData(kUserNameRole, u.displayName);
            }
            setStatus(users.isEmpty()
                ? tr("No users found.")
                : tr("%n result(s)", nullptr, users.size()));
        });
}

void NewChatDialog::onResultDoubleClicked(QListWidgetItem *item)
{
    if (!item) return;
    NcUser u;
    u.id = item->data(kUserIdRole).toString();
    u.displayName = item->data(kUserNameRole).toString();
    if (u.id.isEmpty()) return;

    if (m_directRadio->isChecked()) {
        m_selected.clear();
        m_selected.push_back(u);
        refreshSelectedView();
        onCreateClicked();
        return;
    }
    // Group mode: toggle inclusion.
    auto it = std::find_if(m_selected.begin(), m_selected.end(),
                           [&](const NcUser &x) { return x.id == u.id; });
    if (it != m_selected.end()) m_selected.erase(it);
    else m_selected.push_back(u);
    refreshSelectedView();
}

void NewChatDialog::refreshSelectedView()
{
    if (m_selected.isEmpty()) {
        m_selectedLabel->setText(m_groupRadio->isChecked()
            ? tr("No one selected \u2014 double-click names to add.")
            : tr("No one selected \u2014 double-click a name to chat."));
    } else {
        QStringList names;
        for (const NcUser &u : m_selected)
            names << (u.displayName.isEmpty() ? u.id : u.displayName);
        m_selectedLabel->setText(tr("Selected: %1").arg(names.join(QStringLiteral(", "))));
    }
    const bool direct = m_directRadio->isChecked();
    const bool canCreate =
        direct ? (m_selected.size() == 1)
               : (!m_selected.isEmpty() && !m_groupNameEdit->text().trimmed().isEmpty());
    m_createBtn->setEnabled(canCreate);
}

void NewChatDialog::setStatus(const QString &text, bool isError)
{
    m_status->setText(text);
    m_status->setStyleSheet(isError
        ? QStringLiteral("color: #ff6b6b; font-size: 12px;")
        : QStringLiteral("color: #8a8680; font-size: 12px;"));
}

void NewChatDialog::onCreateClicked()
{
    if (m_selected.isEmpty()) return;
    m_createBtn->setEnabled(false);
    m_cancelBtn->setEnabled(false);
    setStatus(tr("Creating room\u2026"));

    if (m_directRadio->isChecked()) {
        // One-to-one: server creates (or reuses) the room with that user.
        const QString invite = m_selected.first().id;
        m_api->createRoom(1, QString(), invite, this,
            [this](bool ok, const QString &token, const QString &error) {
                if (!ok) {
                    setStatus(error.isEmpty() ? tr("Server refused.") : error, true);
                    m_createBtn->setEnabled(true);
                    m_cancelBtn->setEnabled(true);
                    return;
                }
                m_createdToken = token;
                accept();
            });
        return;
    }

    // Group: create the room, then invite each selected user one at a time.
    const QString name = m_groupNameEdit->text().trimmed();
    m_api->createRoom(2, name, QString(), this,
        [this](bool ok, const QString &token, const QString &error) {
            if (!ok) {
                setStatus(error.isEmpty() ? tr("Server refused.") : error, true);
                m_createBtn->setEnabled(true);
                m_cancelBtn->setEnabled(true);
                return;
            }
            m_createdToken = token;
            // Walk the selected list and add each participant. We track
            // completion via a shared counter in the capture.
            auto *remaining = new int(m_selected.size());
            auto *errors    = new QStringList();
            for (const NcUser &u : m_selected) {
                m_api->addRoomParticipant(token, u.id, this,
                    [this, remaining, errors, u](bool addOk, const QString &err) {
                        if (!addOk) errors->append(
                            QStringLiteral("%1: %2").arg(u.displayName.isEmpty() ? u.id : u.displayName,
                                                         err.isEmpty() ? tr("refused") : err));
                        if (--(*remaining) == 0) {
                            if (!errors->isEmpty()) {
                                // Room exists; some invites failed. Open it anyway
                                // so the user can retry, but surface the problem.
                                setStatus(tr("Room created, but some invites failed: %1")
                                              .arg(errors->join(QStringLiteral("; "))), true);
                            }
                            delete remaining;
                            delete errors;
                            accept();
                        }
                    });
            }
        });
}
