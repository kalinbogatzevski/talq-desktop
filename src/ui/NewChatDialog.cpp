#include "NewChatDialog.h"

#include "core/ApiClient.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace {

constexpr int kUserIdRole     = Qt::UserRole + 1;
constexpr int kUserNameRole   = Qt::UserRole + 2;
constexpr int kUserSourceRole = Qt::UserRole + 3;
constexpr int kPickedRole     = Qt::UserRole + 4;

QString glyphForSource(const QString &source)
{
    if (source == QStringLiteral("groups"))  return QStringLiteral("\U0001F465");
    if (source == QStringLiteral("circles")) return QStringLiteral("\U0001F4AB");
    return QStringLiteral("\u25CF");   // a filled bullet for individuals — looks like an avatar dot
}

QPushButton *makeTabButton(const QString &text, QWidget *parent)
{
    auto *b = new QPushButton(text, parent);
    b->setFlat(true);
    b->setCheckable(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedHeight(38);
    b->setStyleSheet(
        "QPushButton { background: transparent; color: #8a8680; border: none;"
        "  border-bottom: 2px solid transparent;"
        "  padding: 0 18px; font-size: 12px; letter-spacing: 2px;"
        "  text-transform: uppercase; font-weight: 600; }"
        "QPushButton:hover   { color: #b9b4ac; }"
        "QPushButton:checked { color: #e4e0da; border-bottom-color: #14b8a6; }"
    );
    return b;
}

QPushButton *makeChip(const QString &label, const QString &id, QWidget *parent)
{
    auto *b = new QPushButton(parent);
    b->setText(label + QStringLiteral("   \u00D7"));
    b->setProperty("chipId", id);
    b->setCursor(Qt::PointingHandCursor);
    b->setFlat(true);
    b->setStyleSheet(
        "QPushButton { background: #26312f; color: #d4ccc2; border: 1px solid #34423f;"
        "  border-radius: 13px; padding: 4px 10px; font-size: 12px; }"
        "QPushButton:hover { background: #2f3b38; border-color: #14b8a6; color: #e4e0da; }"
    );
    return b;
}

} // namespace

// ────────────────────────────────────────────────────────────────────────
// NewChatDialog
// ────────────────────────────────────────────────────────────────────────

NewChatDialog::NewChatDialog(ApiClient *api, QWidget *parent)
    : QDialog(parent), m_api(api)
{
    setWindowTitle(tr("New conversation"));
    setModal(true);
    resize(560, 640);
    setStyleSheet(
        "QDialog { background: #141210; color: #f4efe6; }"
        "QLabel  { color: #f4efe6; }"
        "QLabel#eyebrow { color: #7a726a; font-size: 10px; letter-spacing: 2px;"
        "  text-transform: uppercase; font-weight: 700; }"
        "QLabel#headline { color: #f4efe6; font-size: 22px; font-weight: 600;"
        "  letter-spacing: -0.2px; }"
        "QLineEdit { background: transparent; border: none;"
        "  border-bottom: 1px solid #2a241f; padding: 8px 0; font-size: 14px;"
        "  color: #f4efe6; selection-background-color: #14b8a6;"
        "  selection-color: #0e1817; }"
        "QLineEdit:focus { border-bottom-color: #14b8a6; }"
        "QLineEdit#groupname { font-size: 20px; font-weight: 600; padding: 10px 0; }"
        "QListWidget { background: #1a1613; border: 1px solid #2a241f;"
        "  border-radius: 12px; color: #f4efe6; padding: 4px; outline: none; }"
        "QListWidget::item { padding: 0; border-radius: 8px; color: #f4efe6; }"
        "QListWidget::item:hover    { background: #241f1a; }"
        "QListWidget::item:selected { background: #241f1a; }"
        "QRadioButton { color: #b9b4ac; font-size: 13px; font-weight: 500;"
        "  padding: 4px 8px; }"
        "QRadioButton:checked { color: #f4efe6; }"
        "QScrollArea, QScrollArea > QWidget, QScrollArea > QWidget > QWidget {"
        "  background: transparent; border: none; }"
        "QPushButton#cancel  { background: transparent; color: #a8a096; border: none;"
        "  padding: 10px 18px; font-size: 12px; letter-spacing: 0.5px;"
        "  text-transform: uppercase; font-weight: 600; }"
        "QPushButton#cancel:hover { color: #f4efe6; }"
        "QPushButton#primary { background: #14b8a6; color: #0e1817; border: none;"
        "  border-radius: 22px; padding: 10px 28px; font-size: 12px;"
        "  letter-spacing: 0.5px; text-transform: uppercase; font-weight: 700; }"
        "QPushButton#primary:hover    { background: #2dd4bf; }"
        "QPushButton#primary:pressed  { background: #0d9488; }"
        "QPushButton#primary:disabled { background: #1c2b2a; color: #546361; }"
    );

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(28, 24, 28, 20);
    outer->setSpacing(0);

    // ── Headline ──
    auto *title = new QLabel(tr("New conversation"), this);
    title->setObjectName("headline");
    outer->addWidget(title);
    outer->addSpacing(18);

    // ── Tab row ──
    auto *tabRow = new QHBoxLayout();
    tabRow->setContentsMargins(0, 0, 0, 0);
    tabRow->setSpacing(0);
    m_directTab = makeTabButton(tr("Direct"), this);
    m_groupTab  = makeTabButton(tr("Group"),  this);
    m_directTab->setChecked(true);
    tabRow->addWidget(m_directTab);
    tabRow->addWidget(m_groupTab);
    tabRow->addStretch();
    outer->addLayout(tabRow);

    auto *tabRule = new QFrame(this);
    tabRule->setFrameShape(QFrame::HLine);
    tabRule->setStyleSheet("background: #2a2a26; border: none; max-height: 1px;");
    tabRule->setFixedHeight(1);
    outer->addWidget(tabRule);
    outer->addSpacing(18);

    // ── Group name (hidden in Direct mode) ──
    m_groupNameBlock = new QWidget(this);
    auto *gnLay = new QVBoxLayout(m_groupNameBlock);
    gnLay->setContentsMargins(0, 0, 0, 0);
    gnLay->setSpacing(2);
    auto *gnEyebrow = new QLabel(tr("NAME YOUR GROUP"), m_groupNameBlock);
    gnEyebrow->setObjectName("eyebrow");
    gnLay->addWidget(gnEyebrow);
    m_groupNameEdit = new QLineEdit(m_groupNameBlock);
    m_groupNameEdit->setObjectName("groupname");
    m_groupNameEdit->setPlaceholderText(tr("Untitled group"));
    gnLay->addWidget(m_groupNameEdit);
    outer->addWidget(m_groupNameBlock);
    outer->addSpacing(22);
    m_groupNameBlock->hide();

    // ── Members chip row (hidden in Direct mode) ──
    m_chipsBlock = new QWidget(this);
    auto *chipLay = new QVBoxLayout(m_chipsBlock);
    chipLay->setContentsMargins(0, 0, 0, 0);
    chipLay->setSpacing(6);
    m_chipsEyebrow = new QLabel(tr("MEMBERS"), m_chipsBlock);
    m_chipsEyebrow->setObjectName("eyebrow");
    chipLay->addWidget(m_chipsEyebrow);

    m_chipsScroll = new QScrollArea(m_chipsBlock);
    m_chipsScroll->setWidgetResizable(true);
    m_chipsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_chipsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_chipsScroll->setFixedHeight(44);

    m_chipsHost = new QWidget(m_chipsScroll);
    m_chipsLayout = new QHBoxLayout(m_chipsHost);
    m_chipsLayout->setContentsMargins(0, 6, 0, 6);
    m_chipsLayout->setSpacing(6);
    m_chipsLayout->addStretch();
    m_chipsScroll->setWidget(m_chipsHost);
    chipLay->addWidget(m_chipsScroll);

    outer->addWidget(m_chipsBlock);
    outer->addSpacing(18);
    m_chipsBlock->hide();

    // ── Search ──
    auto *addEyebrow = new QLabel(tr("ADD PEOPLE"), this);
    addEyebrow->setObjectName("eyebrow");
    outer->addWidget(addEyebrow);
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Type a name or username\u2026"));
    outer->addWidget(m_searchEdit);
    outer->addSpacing(12);

    m_results = new QListWidget(this);
    m_results->setSelectionMode(QAbstractItemView::NoSelection);
    m_results->setFocusPolicy(Qt::NoFocus);
    m_results->setSpacing(0);
    m_results->setUniformItemSizes(false);
    outer->addWidget(m_results, 1);

    outer->addSpacing(10);
    m_status = new QLabel(QString(), this);
    m_status->setStyleSheet("color: #6f6a62; font-size: 12px;");
    outer->addWidget(m_status);
    outer->addSpacing(6);

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    m_cancelBtn->setObjectName("cancel");
    m_cancelBtn->setCursor(Qt::PointingHandCursor);
    m_createBtn = new QPushButton(tr("Create room"), this);
    m_createBtn->setObjectName("primary");
    m_createBtn->setEnabled(false);
    m_createBtn->setDefault(true);
    m_createBtn->setCursor(Qt::PointingHandCursor);
    btnRow->addWidget(m_cancelBtn);
    btnRow->addSpacing(6);
    btnRow->addWidget(m_createBtn);
    outer->addLayout(btnRow);

    // ── Debounce ──
    m_searchDebounce = new QTimer(this);
    m_searchDebounce->setSingleShot(true);
    m_searchDebounce->setInterval(250);

    // ── Wiring ──
    connect(m_directTab, &QPushButton::clicked, this, [this]() { setMode(/*group*/false); });
    connect(m_groupTab,  &QPushButton::clicked, this, [this]() { setMode(/*group*/true);  });
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this]() { m_searchDebounce->start(); });
    connect(m_searchDebounce, &QTimer::timeout, this, &NewChatDialog::runSearch);
    connect(m_results, &QListWidget::itemClicked, this, &NewChatDialog::onResultClicked);
    connect(m_createBtn, &QPushButton::clicked, this, &NewChatDialog::onCreateClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_groupNameEdit, &QLineEdit::textChanged, this, [this]() { refreshCreateEnabled(); });

    m_searchEdit->setFocus();
    refreshCreateEnabled();
    m_status->setText(tr("Start typing to search for people."));
}

void NewChatDialog::setMode(bool group)
{
    m_directTab->setChecked(!group);
    m_groupTab->setChecked(group);
    m_directTab->update();
    m_groupTab->update();
    m_groupNameBlock->setVisible(group);
    m_chipsBlock->setVisible(group);
    m_selected.clear();
    rebuildChips();
    if (!m_searchEdit->text().trimmed().isEmpty()) runSearch();
    refreshCreateEnabled();
}

void NewChatDialog::runSearch()
{
    const QString q = m_searchEdit->text().trimmed();
    m_results->clear();
    if (q.size() < 2) {
        m_status->setText(tr("Type at least 2 characters."));
        return;
    }
    m_status->setText(tr("Searching\u2026"));
    const bool group = m_groupTab->isChecked();
    m_api->searchNcUsers(q, this,
        [this, group](bool ok, const QVector<NcUser> &users) {
            if (!ok) {
                m_status->setStyleSheet("color: #ff6b6b; font-size: 12px;");
                m_status->setText(tr("Search failed — the server refused the request."));
                return;
            }
            m_status->setStyleSheet("color: #6f6a62; font-size: 12px;");
            int shown = 0;
            for (const NcUser &u : users) {
                if (!group && u.source != QStringLiteral("users")) continue;
                addResultRow(u);
                ++shown;
            }
            m_status->setText(shown == 0
                ? tr("No results.")
                : tr("%n people shown — tap to %1",
                     nullptr, shown).arg(group ? tr("add/remove") : tr("start chat")));
        });
}

void NewChatDialog::addResultRow(const NcUser &u)
{
    auto *item = new QListWidgetItem(m_results);
    item->setSizeHint(QSize(0, 56));
    item->setData(kUserIdRole,     u.id);
    item->setData(kUserNameRole,   u.displayName);
    item->setData(kUserSourceRole, u.source);
    item->setData(kPickedRole,     isPicked(u.id));

    auto *row = new QWidget(m_results);
    row->setObjectName("row");
    row->setStyleSheet(
        "QWidget#row { background: transparent; }"
        "QLabel#glyph   { color: #14b8a6; font-size: 22px; }"
        "QLabel#glyph[picked=\"1\"]   { color: #0e1817; background: #14b8a6;"
        "  border-radius: 16px; min-width: 32px; min-height: 32px; max-width: 32px; max-height: 32px;"
        "  qproperty-alignment: AlignCenter; font-size: 14px; font-weight: 700; }"
        "QLabel#glyph[picked=\"0\"]   { color: #14b8a6; border: 1.5px solid #14b8a6;"
        "  border-radius: 16px; min-width: 30px; min-height: 30px; max-width: 30px; max-height: 30px;"
        "  qproperty-alignment: AlignCenter; font-size: 14px; }"
        "QLabel#name    { color: #f4f0ea; font-size: 14px; font-weight: 500; }"
        "QLabel#meta    { color: #7f7a72; font-size: 11px;"
        "  letter-spacing: 1px; text-transform: uppercase; }"
        "QLabel#status  { color: #14b8a6; font-size: 11px; font-weight: 600;"
        "  letter-spacing: 1px; text-transform: uppercase; }"
    );
    auto *rowLay = new QHBoxLayout(row);
    rowLay->setContentsMargins(10, 8, 14, 8);
    rowLay->setSpacing(12);

    auto *glyph = new QLabel(row);
    glyph->setObjectName("glyph");
    glyph->setProperty("picked", isPicked(u.id) ? "1" : "0");
    glyph->setText(isPicked(u.id) ? QStringLiteral("\u2713") : glyphForSource(u.source));
    rowLay->addWidget(glyph);

    auto *textCol = new QVBoxLayout();
    textCol->setContentsMargins(0, 0, 0, 0);
    textCol->setSpacing(2);
    auto *name = new QLabel(u.displayName.isEmpty() ? u.id : u.displayName, row);
    name->setObjectName("name");
    auto *meta = new QLabel(
        QStringLiteral("%1 · %2")
            .arg(u.source == QStringLiteral("users")
                     ? tr("person")
                     : u.source == QStringLiteral("groups")
                           ? tr("group")
                           : tr("circle"),
                 u.id), row);
    meta->setObjectName("meta");
    textCol->addWidget(name);
    textCol->addWidget(meta);
    rowLay->addLayout(textCol, 1);

    auto *st = new QLabel(isPicked(u.id) ? tr("Added") : QString(), row);
    st->setObjectName("status");
    rowLay->addWidget(st);

    m_results->setItemWidget(item, row);
}

void NewChatDialog::onResultClicked(QListWidgetItem *item)
{
    if (!item) return;
    NcUser u;
    u.id          = item->data(kUserIdRole).toString();
    u.displayName = item->data(kUserNameRole).toString();
    u.source      = item->data(kUserSourceRole).toString();
    if (u.id.isEmpty()) return;

    if (!m_groupTab->isChecked()) {
        m_selected.clear();
        m_selected.push_back(u);
        refreshCreateEnabled();
        // Also accept immediately — direct mode is one-click.
        if (m_createBtn->isEnabled()) onCreateClicked();
        return;
    }

    auto it = std::find_if(m_selected.begin(), m_selected.end(),
                           [&](const NcUser &x) { return x.id == u.id; });
    if (it != m_selected.end()) m_selected.erase(it);
    else m_selected.push_back(u);

    rebuildChips();
    // Rebuild the clicked row to reflect new state.
    replaceResultRow(item, u);
    refreshCreateEnabled();
}

void NewChatDialog::replaceResultRow(QListWidgetItem *item, const NcUser &u)
{
    // Delete old widget and replace.
    QWidget *old = m_results->itemWidget(item);
    if (old) old->deleteLater();
    // Easiest to rebuild via the same path.
    item->setData(kPickedRole, isPicked(u.id));
    const int row = m_results->row(item);
    m_results->takeItem(row);                  // remove and re-add via addResultRow
    // Rebuild at the same index
    auto *fresh = new QListWidgetItem();
    fresh->setSizeHint(QSize(0, 56));
    fresh->setData(kUserIdRole,     u.id);
    fresh->setData(kUserNameRole,   u.displayName);
    fresh->setData(kUserSourceRole, u.source);
    fresh->setData(kPickedRole,     isPicked(u.id));
    m_results->insertItem(row, fresh);

    // Build widget inline (duplicates addResultRow's construction for a single item).
    auto *w = new QWidget(m_results);
    w->setObjectName("row");
    w->setStyleSheet(
        "QWidget#row { background: transparent; }"
        "QLabel#glyph[picked=\"1\"] { color: #0e1817; background: #14b8a6;"
        "  border-radius: 16px; min-width: 32px; min-height: 32px; max-width: 32px; max-height: 32px;"
        "  qproperty-alignment: AlignCenter; font-size: 14px; font-weight: 700; }"
        "QLabel#glyph[picked=\"0\"] { color: #14b8a6; border: 1.5px solid #14b8a6;"
        "  border-radius: 16px; min-width: 30px; min-height: 30px; max-width: 30px; max-height: 30px;"
        "  qproperty-alignment: AlignCenter; font-size: 14px; }"
        "QLabel#name    { color: #f4f0ea; font-size: 14px; font-weight: 500; }"
        "QLabel#meta    { color: #7f7a72; font-size: 11px;"
        "  letter-spacing: 1px; text-transform: uppercase; }"
        "QLabel#status  { color: #14b8a6; font-size: 11px; font-weight: 600;"
        "  letter-spacing: 1px; text-transform: uppercase; }"
    );
    auto *lay = new QHBoxLayout(w);
    lay->setContentsMargins(10, 8, 14, 8);
    lay->setSpacing(12);
    auto *g = new QLabel(w);
    g->setObjectName("glyph");
    g->setProperty("picked", isPicked(u.id) ? "1" : "0");
    g->setText(isPicked(u.id) ? QStringLiteral("\u2713") : glyphForSource(u.source));
    lay->addWidget(g);
    auto *col = new QVBoxLayout();
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(2);
    auto *n = new QLabel(u.displayName.isEmpty() ? u.id : u.displayName, w);
    n->setObjectName("name");
    auto *m = new QLabel(
        QStringLiteral("%1 · %2")
            .arg(u.source == QStringLiteral("users") ? tr("person")
                 : u.source == QStringLiteral("groups") ? tr("group")
                 : tr("circle"), u.id), w);
    m->setObjectName("meta");
    col->addWidget(n);
    col->addWidget(m);
    lay->addLayout(col, 1);
    auto *st = new QLabel(isPicked(u.id) ? tr("Added") : QString(), w);
    st->setObjectName("status");
    lay->addWidget(st);
    m_results->setItemWidget(fresh, w);
}

void NewChatDialog::rebuildChips()
{
    // Remove all chip widgets, keep the trailing stretch.
    while (m_chipsLayout->count() > 1) {
        QLayoutItem *it = m_chipsLayout->takeAt(0);
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    for (const NcUser &u : m_selected) {
        const QString label = u.displayName.isEmpty() ? u.id : u.displayName;
        auto *chip = makeChip(label, u.id, m_chipsHost);
        connect(chip, &QPushButton::clicked, this, [this, chip]() {
            const QString id = chip->property("chipId").toString();
            auto it = std::find_if(m_selected.begin(), m_selected.end(),
                                   [&](const NcUser &x) { return x.id == id; });
            if (it != m_selected.end()) {
                m_selected.erase(it);
                rebuildChips();
                refreshCreateEnabled();
                for (int i = 0; i < m_results->count(); ++i) {
                    if (m_results->item(i)->data(kUserIdRole).toString() == id) {
                        NcUser u;
                        u.id          = m_results->item(i)->data(kUserIdRole).toString();
                        u.displayName = m_results->item(i)->data(kUserNameRole).toString();
                        u.source      = m_results->item(i)->data(kUserSourceRole).toString();
                        replaceResultRow(m_results->item(i), u);
                        break;
                    }
                }
            }
        });
        m_chipsLayout->insertWidget(m_chipsLayout->count() - 1, chip);
    }
    m_chipsEyebrow->setText(m_selected.isEmpty()
        ? tr("MEMBERS — NONE YET")
        : tr("MEMBERS · %1").arg(m_selected.size()));
}

bool NewChatDialog::isPicked(const QString &id) const
{
    return std::any_of(m_selected.cbegin(), m_selected.cend(),
                       [&](const NcUser &x) { return x.id == id; });
}

void NewChatDialog::refreshCreateEnabled()
{
    const bool group = m_groupTab->isChecked();
    const bool canCreate =
        group ? (!m_selected.isEmpty() && !m_groupNameEdit->text().trimmed().isEmpty())
              : (m_selected.size() == 1);
    m_createBtn->setEnabled(canCreate);
    m_createBtn->setText(group ? tr("Create group") : tr("Start chat"));
}

void NewChatDialog::onCreateClicked()
{
    if (m_selected.isEmpty()) return;
    m_createBtn->setEnabled(false);
    m_cancelBtn->setEnabled(false);
    m_status->setStyleSheet("color: #6f6a62; font-size: 12px;");
    m_status->setText(tr("Creating room\u2026"));

    if (!m_groupTab->isChecked()) {
        const QString invite = m_selected.first().id;
        m_api->createRoom(1, QString(), invite, this,
            [this](bool ok, const QString &token, const QString &error) {
                if (!ok) {
                    m_status->setStyleSheet("color: #ff6b6b; font-size: 12px;");
                    m_status->setText(error.isEmpty() ? tr("Server refused.") : error);
                    m_createBtn->setEnabled(true);
                    m_cancelBtn->setEnabled(true);
                    return;
                }
                m_createdToken = token;
                accept();
            });
        return;
    }

    const QString name = m_groupNameEdit->text().trimmed();
    m_api->createRoom(2, name, QString(), this,
        [this](bool ok, const QString &token, const QString &error) {
            if (!ok) {
                m_status->setStyleSheet("color: #ff6b6b; font-size: 12px;");
                m_status->setText(error.isEmpty() ? tr("Server refused.") : error);
                m_createBtn->setEnabled(true);
                m_cancelBtn->setEnabled(true);
                return;
            }
            m_createdToken = token;
            auto *remaining = new int(m_selected.size());
            auto *errors    = new QStringList();
            for (const NcUser &u : m_selected) {
                m_api->addRoomParticipant(token, u.id, this,
                    [this, remaining, errors, u](bool addOk, const QString &err) {
                        if (!addOk) errors->append(
                            QStringLiteral("%1 (%2)")
                                .arg(u.displayName.isEmpty() ? u.id : u.displayName,
                                     err.isEmpty() ? tr("refused") : err));
                        if (--(*remaining) == 0) {
                            if (!errors->isEmpty()) {
                                m_status->setStyleSheet("color: #ff6b6b; font-size: 12px;");
                                m_status->setText(tr("Room created, some invites failed: %1")
                                                      .arg(errors->join(QStringLiteral("; "))));
                            }
                            delete remaining;
                            delete errors;
                            accept();
                        }
                    });
            }
        });
}
