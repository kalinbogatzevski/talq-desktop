#include "SidebarPainter.h"
#include "models/ConversationListModel.h"
#include "core/ApiClient.h"
#include "core/AvatarFetchPolicy.h"
#include "core/SignalingClient.h"
#include "core/ShiftStatusService.h"
#include "core/ConversationKind.h"
#include "EmojiTextRenderer.h"
#include <QApplication>
#include <QCursor>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPointer>
#include <QTimer>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QHelpEvent>
#include <QToolTip>
#include <QNetworkReply>
#include <QFontMetrics>
#include <QtMath>
#include <QDateTime>
#include <algorithm>

namespace {

// A dragged conversation travels as THIS type and no other. Not text/plain and
// above all not text/uri-list: ChatPainter's file-drop handler accepts any URL
// mime, so a URL-shaped payload could be dropped onto the open chat and
// uploaded. Foreign drops (files, text) are ignored by the sidebar for the same
// reason in reverse -- it has no use for them.
constexpr char kConversationMime[] = "application/x-talq-conversation-token";

// While a drag hovers this close to the top or bottom edge the list scrolls
// toward that edge, faster the nearer the edge (talq::dragAutoScrollStep).
constexpr int kDragScrollZone = 36;        // px
constexpr int kDragScrollMaxStep = 14;     // px per tick at the very edge
constexpr int kDragScrollTickMs = 30;

} // namespace

// ═══════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════

SidebarPainter::SidebarPainter(QWidget *parent)
    : QWidget(parent)
    , m_theme(m_darkMode, 1.0)
{
    setAttribute(Qt::WA_Hover);
    setMouseTracking(true);
    // Only for dragging a conversation onto a folder; dragEnterEvent refuses
    // everything else (and everything at all unless folders are in force).
    setAcceptDrops(true);
    m_dragScrollTimer = new QTimer(this);
    m_dragScrollTimer->setInterval(kDragScrollTickMs);
    connect(m_dragScrollTimer, &QTimer::timeout, this, &SidebarPainter::onDragScrollTick);
}

qint64 SidebarPainter::avatarCacheBytes() const
{
    qint64 total = 0;
    for (auto it = m_avatarCache.cbegin(); it != m_avatarCache.cend(); ++it)
        total += it.value().sizeInBytes();
    return total;
}

// ═══════════════════════════════════════════════════════
// Properties
// ═══════════════════════════════════════════════════════

void SidebarPainter::setModel(ConversationListModel *mdl)
{
    if (mdl == m_model) return;

    if (m_model) {
        disconnect(m_model, nullptr, this, nullptr);
    }

    m_model = mdl;

    if (m_model) {
        connect(m_model, &QAbstractItemModel::dataChanged, this, &SidebarPainter::onDataChanged);
        connect(m_model, &QAbstractItemModel::modelReset, this, &SidebarPainter::onModelReset);
        connect(m_model, &QAbstractItemModel::rowsInserted, this, &SidebarPainter::onRowsInserted);
        connect(m_model, &QAbstractItemModel::rowsRemoved, this, &SidebarPainter::onRowsRemoved);
    }

    rebuildLayouts();
}

void SidebarPainter::setApi(ApiClient *api)
{
    if (api == m_api) return;
    m_api = api;
}

void SidebarPainter::setShiftStatus(ShiftStatusService *shiftStatus)
{
    if (shiftStatus == m_shiftStatus) return;
    m_shiftStatus = shiftStatus;
    // Repaint when a batch lands, so markers appear without waiting for the
    // next model refresh.
    if (m_shiftStatus) {
        connect(m_shiftStatus, &ShiftStatusService::statusesChanged,
                this, qOverload<>(&QWidget::update));
    }
}

void SidebarPainter::setSignaling(SignalingClient *signaling)
{
    if (signaling == m_signaling) return;
    m_signaling = signaling;
    // Repaint when peer client info arrives so the TalQ badge appears
    // without waiting for the next model update.
    if (m_signaling) {
        connect(m_signaling, &SignalingClient::peerClientInfoChanged,
                this, [this]() { update(); });
    }
}

void SidebarPainter::setDarkMode(bool dark)
{
    if (m_darkMode == dark) return;
    m_darkMode = dark;
    m_theme = PainterTheme(m_darkMode, 1.0);
    update();
}

void SidebarPainter::setTheme(PainterTheme::Theme t)
{
    if (m_themeId == t) return;
    m_themeId = t;
    m_theme = PainterTheme(t, 1.0);
    update();
}

void SidebarPainter::setSelectedIndex(int idx)
{
    if (m_selectedIndex == idx) return;
    m_selectedIndex = idx;
    m_selectedToken = (idx >= 0 && idx < m_layouts.size()) ? m_layouts[idx].token : QString();
    update();
    emit selectedIndexChanged();
}

void SidebarPainter::setSqueezed(bool sq)
{
    if (m_squeezed == sq) return;
    m_squeezed = sq;
    m_scrollY = 0;
    // The icon rail has no room for a folder header (and the build drops them
    // there), but nothing else rebuilds the rows on a squeeze: without this the
    // rail would keep painting the open list's headers until the next model
    // refresh, up to 30 s. Folders only -- the tag path has always waited for
    // the next signal and is left byte-for-byte as it was.
    if (m_groupMode == GroupFolders && !m_folders.isEmpty())
        rebuildLayouts();
    update();
}

void SidebarPainter::setFilterText(const QString &text)
{
    if (m_filterText == text) return;
    m_filterText = text;
    rebuildLayouts();
}

void SidebarPainter::setSortMode(int mode)
{
    if (m_sortMode == mode) return;
    m_sortMode = mode;
    rebuildLayouts();
}

void SidebarPainter::setFilterMode(int mode)
{
    if (m_filterMode == mode) return;
    m_filterMode = mode;
    rebuildLayouts();
}

void SidebarPainter::setTagFilter(const QString &tagId)
{
    if (m_tagFilterId == tagId) return;
    m_tagFilterId = tagId;
    rebuildLayouts();
}

QString SidebarPainter::tagFilterName() const
{
    return m_tagFilterName;
}

void SidebarPainter::setTagFilterName(const QString &name)
{
    // Display-only: the empty-state message names the tag the user picked
    // rather than showing its numeric id, which would mean nothing to them.
    if (m_tagFilterName == name) return;
    m_tagFilterName = name;
    update();
}

// ═══════════════════════════════════════════════════════
// Model signal handlers
// ═══════════════════════════════════════════════════════

void SidebarPainter::onDataChanged(const QModelIndex &, const QModelIndex &)
{
    rebuildLayouts();
}

void SidebarPainter::onModelReset()
{
    rebuildLayouts();
}

void SidebarPainter::onRowsInserted(const QModelIndex &, int, int)
{
    rebuildLayouts();
}

void SidebarPainter::onRowsRemoved(const QModelIndex &, int, int)
{
    rebuildLayouts();
}

// ═══════════════════════════════════════════════════════
// Layout
// ═══════════════════════════════════════════════════════

void SidebarPainter::rebuildLayouts()
{
    // Preserve selection across rebuilds: save the selected token
    QString prevSelectedToken = m_selectedToken;
    if (prevSelectedToken.isEmpty() && m_selectedIndex >= 0 && m_selectedIndex < m_layouts.size())
        prevSelectedToken = m_layouts[m_selectedIndex].token;

    m_layouts.clear();
    m_rows.clear();
    m_matching.clear();
    m_listEmpty = true;
    // Conversation indices that survive the text/mode/tag filters, in display
    // order. buildVisibleRows() turns this into m_rows — flat, or split into
    // tag or folder sections when grouping is on.
    QVector<int> matching;

    if (!m_model) {
        update();
        return;
    }

    int count = m_model->rowCount();
    m_layouts.reserve(count);

    for (int i = 0; i < count; ++i) {
        QModelIndex idx = m_model->index(i);
        ConversationLayout cl;
        cl.token = m_model->data(idx, ConversationListModel::TokenRole).toString();
        cl.displayName = m_model->data(idx, ConversationListModel::DisplayNameRole).toString();
        cl.lastMessage = m_model->data(idx, ConversationListModel::LastMessageRole).toString();
        cl.lastAuthor = m_model->data(idx, ConversationListModel::LastAuthorRole).toString();
        cl.userStatus = m_model->data(idx, ConversationListModel::UserStatusRole).toString();
        cl.userStatusMessage = m_model->data(idx, ConversationListModel::UserStatusMessageRole).toString();
        cl.userStatusIcon    = m_model->data(idx, ConversationListModel::UserStatusIconRole).toString();
        cl.participantUserId = m_model->data(idx, ConversationListModel::ActorIdRole).toString();
        cl.conversationType = m_model->data(idx, ConversationListModel::TypeRole).toInt();
        cl.unreadCount = m_model->data(idx, ConversationListModel::UnreadCountRole).toInt();
        cl.unreadMention = m_model->data(idx, ConversationListModel::UnreadMentionRole).toBool();
        cl.unreadMentionDirect = m_model->data(idx, ConversationListModel::UnreadMentionDirectRole).toBool();
        cl.isArchived = m_model->data(idx, ConversationListModel::ArchivedRole).toBool();
        cl.isFavorite = m_model->data(idx, ConversationListModel::FavoriteRole).toBool();
        cl.lastActivity = m_model->data(idx, ConversationListModel::LastActivityRole).toLongLong();
        cl.notificationLevel = m_model->data(idx, ConversationListModel::NotificationLevelRole).toInt();
        cl.tagIds = m_model->data(idx, ConversationListModel::TagIdsRole).toStringList();

        cl.timeString = PainterTheme::formatRelativeTime(cl.lastActivity);
        cl.previewText = PainterTheme::formatPreviewText(cl.lastAuthor, cl.lastMessage);

        m_layouts.append(cl);
    }

    // Build visible indices (text filter + filter mode)
    for (int i = 0; i < m_layouts.size(); ++i) {
        const ConversationLayout &cl = m_layouts[i];

        if (!m_filterText.isEmpty() &&
            !cl.displayName.contains(m_filterText, Qt::CaseInsensitive))
            continue;

        // Archived rooms are hidden from EVERY mode except the archived one.
        // Filtering them out here rather than per-case is deliberate: an
        // archived room must not reappear just because it is unread, a
        // favourite, or a group. That is the whole point of archiving it.
        if (cl.isArchived != (m_filterMode == FilterArchived))
            continue;

        bool modeOk = true;
        switch (m_filterMode) {
        case FilterUnread:    modeOk = cl.unreadCount > 0;          break;
        case FilterFavorites: modeOk = cl.isFavorite;               break;
        case FilterDirect:    modeOk = cl.conversationType == 1;    break;  // OneToOne
        case FilterGroups:    modeOk = cl.conversationType == 2
                                    || cl.conversationType == 3;    break;  // Group / Public
        case FilterArchived:  modeOk = true;                        break;  // already filtered above
        case FilterAll:
        default:              modeOk = true;                        break;
        }
        if (!modeOk)
            continue;

        // Talk 24 tag filter, applied on top of the built-in mode filter so
        // the two compose (e.g. "unread" AND tagged "Work"). The two "!"-
        // prefixed pseudo-tags mirror the server's special tag types:
        // favourites, and "Other" = carries no tags at all.
        if (!m_tagFilterId.isEmpty()) {
            bool tagOk;
            if (m_tagFilterId == QLatin1String("!favorites"))
                tagOk = cl.isFavorite;
            else if (m_tagFilterId == QLatin1String("!other"))
                tagOk = cl.tagIds.isEmpty();
            else
                tagOk = cl.tagIds.contains(m_tagFilterId);
            if (!tagOk)
                continue;
        }

        matching.append(i);
    }

    // Sort visible indices. Favorites are always grouped first (except when
    // already filtering to favorites), then the chosen sort key applies.
    const QVector<ConversationLayout> &L = m_layouts;
    // When grouping by tag is on, the Favourites SECTION does the favourites-
    // first job; keeping the flat favourites-float as well would pull
    // favourites to the top of every other section too. Folders have no such
    // section, so with folders the float stays on: each folder is a stable
    // slice of this sorted list, which puts a favourite at the top of ITS
    // folder (and keeps it floating in the flat list when there are no folders).
    const bool groupFavs = (m_filterMode != FilterFavorites) && m_groupMode != GroupTags;
    const int sortMode = m_sortMode;
    std::stable_sort(matching.begin(), matching.end(),
                     [&L, groupFavs, sortMode](int a, int b) {
        const ConversationLayout &x = L[a];
        const ConversationLayout &y = L[b];
        if (groupFavs && x.isFavorite != y.isFavorite)
            return x.isFavorite;            // favorites first
        switch (sortMode) {
        case SortName:
            return x.displayName.localeAwareCompare(y.displayName) < 0;
        case SortUnread: {
            const bool xu = x.unreadCount > 0;
            const bool yu = y.unreadCount > 0;
            if (xu != yu) return xu;        // unread first
            if (xu && x.unreadCount != y.unreadCount)
                return x.unreadCount > y.unreadCount;
            return x.lastActivity > y.lastActivity;
        }
        case SortRecent:
        default:
            return x.lastActivity > y.lastActivity;
        }
    });

    buildVisibleRows(matching);
    m_matching = matching;

    // Restore selection: find the new index for the previously selected token
    if (!prevSelectedToken.isEmpty()) {
        m_selectedIndex = -1;
        for (int i = 0; i < m_layouts.size(); ++i) {
            if (m_layouts[i].token == prevSelectedToken) {
                m_selectedIndex = i;
                break;
            }
        }
        m_selectedToken = prevSelectedToken;
    }

    clampScroll();
    // m_hoveredRow is an index into the rows just rebuilt. In folder mode the
    // rows change under a still pointer (a header toggled, a chat filed, the
    // 30 s refresh), so re-derive it rather than leave the highlight on
    // whatever now has that index.
    if (m_groupMode == GroupFolders)
        refreshHover();
    update();
}

void SidebarPainter::buildVisibleRows(const QVector<int> &matching)
{
    m_rows.clear();
    m_tagSectionCounts.clear();
    m_folderRollups.clear();
    m_otherRollup = talq::FolderRollup();
    const int rowH = m_squeezed ? RowHeightSqueezed : RowHeight;

    // Folders have their own branch, tested FIRST: the tag branch below is
    // skipped on a server with no tags (pre-Talk-24) or with tag grouping off,
    // and folders are local, so they must work there too.
    if (foldersBranchActive()) {
        buildFolderRows(matching, rowH);
        return;
    }

    // Grouping is off, no tags, or the icon rail (56 px — no room for a header
    // label): emit the flat list. This is byte-for-byte the 0.64 layout, and
    // sidebar-row-layout-test pins that the geometry still matches the old
    // `visibleIdx * rowH` arithmetic exactly. (Folder mode with no folder
    // lands here too: groupByTag() is false, so it is the flat list.)
    if (!groupByTag() || m_tags.isEmpty() || m_squeezed) {
        m_rows.reserve(matching.size());
        for (int idx : matching) {
            talq::SidebarRow r;
            r.layoutIndex = idx;
            m_rows.push_back(r);
        }
        talq::assignRowGeometry(m_rows, rowH, HeaderHeight);
        m_listEmpty = m_rows.empty();
        return;
    }

    // Grouped. A conversation appears under EVERY section it belongs to, which
    // is how a favourite that is also tagged "Work" shows in both — matching
    // how favourites have always behaved here (pinned above, still listed).
    // Sections with no conversations are omitted entirely rather than rendered
    // empty; "Other" in particular is empty exactly when everything is tagged.
    for (int t = 0; t < m_tags.size(); ++t) {
        const SidebarTag &tag = m_tags[t];

        QVector<int> members;
        for (int idx : matching) {
            const ConversationLayout &cl = m_layouts[idx];
            bool in;
            if (tag.id == QLatin1String("!favorites"))
                in = cl.isFavorite;
            else if (tag.id == QLatin1String("!other"))
                in = cl.tagIds.isEmpty();
            else
                in = cl.tagIds.contains(tag.id);
            if (in)
                members.append(idx);
        }
        if (members.isEmpty())
            continue;
        m_tagSectionCounts.insert(tag.id, members.size());

        talq::SidebarRow h;
        h.layoutIndex = -1;
        h.tagIndex = t;
        m_rows.push_back(h);

        // A collapsed section keeps its header (that is how you re-open it)
        // but contributes no conversation rows.
        if (tag.collapsed)
            continue;
        for (int idx : members) {
            talq::SidebarRow r;
            r.layoutIndex = idx;
            r.tagIndex = t;
            m_rows.push_back(r);
        }
    }

    talq::assignRowGeometry(m_rows, rowH, HeaderHeight);
    m_listEmpty = m_rows.empty();
}

bool SidebarPainter::folderDisplayedClosed(int folderIndex) const
{
    if (!m_filterText.isEmpty())
        return false;       // a search shows every folder open (D5)
    if (folderIndex == talq::kFolderOther)
        return m_otherCollapsed;
    return folderIndex >= 0 && folderIndex < m_folders.size()
        && m_folders[folderIndex].collapsed;
}

void SidebarPainter::buildFolderRows(const QVector<int> &matching, int rowH)
{
    // Nothing matched: the "No conversations ..." panel is what paints, and it
    // covers the whole list. The rows must be empty too, or empty-folder headers
    // would sit under it unseen yet still answer a click (toggle and persist a
    // folder nobody can see), a right-click (a Rename / Delete menu for it) and
    // the wheel. m_rows was cleared by buildVisibleRows(). The price: a folder
    // made while nothing matches stays unseen until a conversation does.
    if (matching.isEmpty()) {
        m_listEmpty = true;
        return;
    }

    const bool searching = !m_filterText.isEmpty();
    // "A filter is active" for the empty-folder rule is exactly what
    // rebuildLayouts() filters on besides the always-on archived rule: a text
    // search, a filter mode other than All, a tag filter.
    const bool filterActive = searching || m_filterMode != FilterAll
                              || !m_tagFilterId.isEmpty();

    // Partition the already filtered AND sorted list, stably: each folder (and
    // Other) gets its slice in the order `matching` already has, so favourites
    // float inside their folder and every sort mode applies per folder. One
    // O(1) hash lookup per conversation; nothing here touches disk.
    const int nFolders = m_folders.size();
    QVector<QVector<int>> members(nFolders);
    QVector<int> other;
    m_folderRollups.resize(nFolders);
    for (int idx : matching) {
        const ConversationLayout &cl = m_layouts[idx];
        const int f = m_tokenToFolderIndex.value(cl.token, -1);
        const bool inFolder = f >= 0 && f < nFolders;
        (inFolder ? members[f] : other).append(idx);
        // The roll-up counts the members that survive the filters (the rows an
        // open folder would show): muted rooms included -- a muted row paints
        // its badge too -- and archived rooms excluded, because `matching`
        // already left them out.
        talq::addToRollup(inFolder ? m_folderRollups[f] : m_otherRollup,
                          cl.unreadCount, cl.unreadMention, cl.unreadMentionDirect);
    }

    // The folder index is stamped on the header AND on every row under it, so
    // a drop on either resolves to the same folder. tagIndex stays -1: nothing
    // that reads it may mistake this for a tag section.
    const auto emitSection = [this](int section, const QVector<int> &mem, bool closed) {
        talq::SidebarRow h;
        h.layoutIndex = -1;
        h.folderIndex = section;
        m_rows.push_back(h);
        if (closed)
            return;         // a closed folder keeps its header and shows no rows
        for (int idx : mem) {
            talq::SidebarRow r;
            r.layoutIndex = idx;
            r.folderIndex = section;
            m_rows.push_back(r);
        }
    };

    m_rows.reserve(matching.size() + nFolders + 1);
    for (int f = 0; f < nFolders; ++f) {
        // An empty folder is shown (it is a drop target, and it is how you see
        // a folder you just made) -- unless something is filtering, when a
        // folder with no matching member is omitted, as an empty tag section is.
        // (Not when nothing matches at all: see the early return above.)
        if (members[f].isEmpty() && filterActive)
            continue;
        emitSection(f, members[f], folderDisplayedClosed(f));
    }
    // Other: only when it has a member, whatever the filters say.
    if (!other.isEmpty())
        emitSection(talq::kFolderOther, other, folderDisplayedClosed(talq::kFolderOther));

    talq::assignRowGeometry(m_rows, rowH, HeaderHeight);
    // Something matched, so it is in a folder or in Other and has a section.
    m_listEmpty = m_rows.empty();
}

void SidebarPainter::setTags(const QVector<SidebarTag> &tags)
{
    m_tags = tags;
    if (groupByTag())
        rebuildLayouts();
}

void SidebarPainter::setGroupMode(GroupMode mode)
{
    if (m_groupMode == mode) return;
    const GroupMode previous = m_groupMode;
    m_groupMode = mode;
    rebuildLayouts();
    // Folder headers appeared or went, so the hovered index from the previous
    // mode points at some other row. None <-> Tags is left exactly as it was.
    if (previous == GroupFolders || mode == GroupFolders)
        refreshHover();
}

void SidebarPainter::setGroupByTag(bool on)
{
    // The old bool said "is TAG grouping on?"; keep that meaning exactly, so a
    // caller that says "off" while folders are the mode changes nothing.
    if (groupByTag() == on) return;
    setGroupMode(on ? GroupTags : GroupNone);
}

void SidebarPainter::setFolders(const QVector<talq::Folder> &folders, bool otherCollapsed,
                                const QHash<QString, QString> &tokenToFolderId)
{
    // MainWindow pushes the whole state after EVERY store change, including the
    // echo of a toggle this widget just reported. Skipping an identical push
    // spares a second full rebuildLayouts() (it re-reads every role of every
    // room) and keeps the hover highlight from being recomputed twice.
    bool same = otherCollapsed == m_otherCollapsed
             && folders.size() == m_folders.size()
             && tokenToFolderId == m_tokenToFolderId;
    for (int i = 0; same && i < folders.size(); ++i) {
        same = folders[i].id == m_folders[i].id
            && folders[i].name == m_folders[i].name
            && folders[i].collapsed == m_folders[i].collapsed;
    }
    if (same)
        return;

    m_folders = folders;
    m_otherCollapsed = otherCollapsed;
    m_tokenToFolderId = tokenToFolderId;

    // Resolve folder ids to indices ONCE here, so the row build is a single
    // token lookup per conversation. A duplicate id keeps its first index; an
    // assignment to an id that is not in the list is simply left out, which
    // makes that conversation unfiled (Other) -- never an error.
    QHash<QString, int> indexById;
    indexById.reserve(m_folders.size());
    for (int i = 0; i < m_folders.size(); ++i) {
        if (!indexById.contains(m_folders[i].id))
            indexById.insert(m_folders[i].id, i);
    }
    m_tokenToFolderIndex.clear();
    m_tokenToFolderIndex.reserve(m_tokenToFolderId.size());
    for (auto it = m_tokenToFolderId.cbegin(); it != m_tokenToFolderId.cend(); ++it) {
        const int idx = indexById.value(it.value(), -1);
        if (idx >= 0)
            m_tokenToFolderIndex.insert(it.key(), idx);
    }

    // Only folder grouping reads any of this; in the other modes there is
    // nothing to redraw.
    if (m_groupMode == GroupFolders)
        rebuildLayouts();
}

int SidebarPainter::sectionOfToken(const QString &token) const
{
    return m_tokenToFolderIndex.value(token, talq::kFolderOther);
}

QString SidebarPainter::folderIdOfSection(int section) const
{
    if (section >= 0 && section < m_folders.size())
        return m_folders[section].id;
    return QString();       // Other (or out of range): the "no folder" id
}

bool SidebarPainter::sectionIsValid(int section) const
{
    return section == talq::kFolderOther || (section >= 0 && section < m_folders.size());
}

int SidebarPainter::layoutIndexOfToken(const QString &token) const
{
    if (token.isEmpty())
        return -1;
    for (int i = 0; i < m_layouts.size(); ++i) {
        if (m_layouts[i].token == token)
            return i;
    }
    return -1;
}

void SidebarPainter::setSectionCollapsed(int section, bool collapsed)
{
    // The id is copied BEFORE the emit: a slot may answer by pushing a fresh
    // folder list into setFolders(), which replaces m_folders and would leave a
    // reference into it dangling while the slot still reads its argument.
    QString id;
    if (section == talq::kFolderOther) {
        if (m_otherCollapsed == collapsed) return;
        m_otherCollapsed = collapsed;
    } else if (section >= 0 && section < m_folders.size()) {
        if (m_folders[section].collapsed == collapsed) return;
        m_folders[section].collapsed = collapsed;
        id = m_folders[section].id;
    } else {
        return;
    }
    emit folderToggled(id, collapsed);   // NEVER tagSectionToggled: that PUTs to the server
    rebuildLayouts();
}

void SidebarPainter::refreshHover()
{
    // While a drag runs the pointer belongs to the drag, and a hover fill on top
    // of the drop highlight would only muddy it (the 30 s refresh can rebuild
    // the rows in the middle of a drag).
    if (m_dragInProgress || !underMouse()) {
        m_hoveredRow = -1;
        return;
    }
    m_hoveredRow = rowAtY(mapFromGlobal(QCursor::pos()).y());
}

void SidebarPainter::revealToken(const QString &token)
{
    if (!foldersBranchActive())
        return;     // flat or tag-grouped: nothing to open, and no scroll-into-view was ever asked for
    const int layoutIdx = layoutIndexOfToken(token);
    // Unknown token, or one the filters hide: opening a folder would show nothing.
    if (layoutIdx < 0 || !m_matching.contains(layoutIdx))
        return;

    // Selecting a chat that sits in a closed folder must not leave the
    // selection invisible. (While a search is on the folder is already shown
    // open, so folderDisplayedClosed() is false and nothing is toggled.)
    const int section = sectionOfToken(token);
    if (folderDisplayedClosed(section))
        setSectionCollapsed(section, false);

    // Re-find the row by layout index: setSectionCollapsed() rebuilt m_rows.
    for (const talq::SidebarRow &r : m_rows) {
        if (r.layoutIndex != layoutIdx)
            continue;
        m_scrollY = talq::scrollToReveal(m_scrollY, r.y, r.y + r.height, height());
        clampScroll();
        update();
        return;
    }
}

void SidebarPainter::clampScroll()
{
    qreal contentH = talq::totalHeight(m_rows);
    qreal maxScroll = qMax(0.0, contentH - height());
    m_scrollY = qBound(0.0, m_scrollY, maxScroll);
}

int SidebarPainter::rowAtY(qreal viewportY) const
{
    // No longer `canvasY / rowH`: with section headers the rows are two
    // different heights, and the division would silently return a wrong row.
    return talq::rowAtY(m_rows, viewportY + m_scrollY);
}

// ═══════════════════════════════════════════════════════
// Geometry
// ═══════════════════════════════════════════════════════

void SidebarPainter::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    clampScroll();
    update();
}

bool SidebarPainter::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(e);
        int row = rowAtY(he->pos().y());
        if (row >= 0 && row < static_cast<int>(m_rows.size())
            && !m_rows[row].isHeader()) {
            const ConversationLayout &cl = m_layouts[m_rows[row].layoutIndex];
            if (cl.nameElided && cl.nameRect.contains(he->pos())) {
                QToolTip::showText(he->globalPos(), cl.displayName, this);
                return true;
            }
        }
        QToolTip::hideText();
        return true;
    }
    if (e->type() == QEvent::HoverMove) {
        auto *he = static_cast<QHoverEvent *>(e);
        // Replicate hoverMoveEvent
        int row = rowAtY(he->position().y());
        if (row != m_hoveredRow) {
            m_hoveredRow = row;
            update();
        }
        return true;
    }
    if (e->type() == QEvent::HoverLeave) {
        if (m_hoveredRow != -1) {
            m_hoveredRow = -1;
            update();
        }
        return true;
    }
    return QWidget::event(e);
}

// ═══════════════════════════════════════════════════════
// Input handling
// ═══════════════════════════════════════════════════════

void SidebarPainter::wheelEvent(QWheelEvent *event)
{
    qreal delta = event->angleDelta().y();
    qreal scroll = -delta / 120.0 * 40.0;
    m_scrollY += scroll;
    clampScroll();
    update();
    event->accept();
}

void SidebarPainter::clearPressState()
{
    m_pressRow = -1;
    m_pressButton = Qt::NoButton;
    m_pressToken.clear();
}

void SidebarPainter::mousePressEvent(QMouseEvent *event)
{
    // Remember where and on what the button went down. The release still does
    // the hit-test that decides a click, but a drag has to know its origin: by
    // the time the pointer has moved far enough to start one it is over some
    // other row. The token (not the row index) is kept, because a model
    // refresh can re-lay the rows between press and move.
    m_swallowRelease = false;   // a stale flag must never eat a later click
    m_lastReleaseWasFolderHeader = false;
    m_pressPos = event->position().toPoint();
    m_pressRow = rowAtY(event->position().y());
    m_pressButton = event->button();
    m_pressToken.clear();
    if (event->button() == Qt::LeftButton && m_pressRow >= 0
        && m_pressRow < static_cast<int>(m_rows.size())
        && !m_rows[m_pressRow].isHeader())
        m_pressToken = m_layouts[m_rows[m_pressRow].layoutIndex].token;
    event->accept(); // claim the event so release fires
}

void SidebarPainter::mouseMoveEvent(QMouseEvent *event)
{
    // Mouse tracking is on, so this also sees plain hovering (no button); that
    // is handled by the HoverMove branch of event() and must still reach the
    // base class exactly as before.
    if (!(event->buttons() & Qt::LeftButton) || m_pressButton != Qt::LeftButton
        || m_pressToken.isEmpty() || !dragPossible()) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    if ((event->position().toPoint() - m_pressPos).manhattanLength()
        < QApplication::startDragDistance()) {
        event->accept();
        return;
    }
    event->accept();
    // By value: startDrag() clears the press state (and so m_pressToken) before
    // it is done with the token.
    const QString token = m_pressToken;
    startDrag(token);
}

void SidebarPainter::startDrag(const QString &token)
{
    // exec() runs its own event loop and no release is delivered back to this
    // widget afterwards, so the press state must be cleared BEFORE it, and the
    // "swallow the next release" flag raised after it (some platforms do send
    // one, and it must not open a chat).
    clearPressState();
    m_hoveredRow = -1;      // hover events stop for the whole drag
    update();

    auto *mime = new QMimeData;
    mime->setData(QLatin1String(kConversationMime), token.toUtf8());
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);

    QPointer<SidebarPainter> self(this);
    m_dragInProgress = true;
    drag->exec(Qt::MoveAction);
    if (!self)
        return;
    m_dragInProgress = false;
    endDragFeedback();
    m_swallowRelease = true;
    refreshHover();
    update();
}

void SidebarPainter::mouseDoubleClickEvent(QMouseEvent *event)
{
    // QWidget's default turns a double-click into a second mousePressEvent, and
    // that press's release would run the click branch again: a folder header
    // would open and shut in one double-click. Swallow it. (The tag header has
    // the same quirk and is deliberately left alone; this only covers folders.)
    // Decided by what the FIRST click did, not by where this one landed: that
    // click may have closed a folder and scrolled a different row under the
    // pointer.
    if (m_lastReleaseWasFolderHeader) {
        m_lastReleaseWasFolderHeader = false;
        clearPressState();
        m_swallowRelease = true;
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void SidebarPainter::mouseReleaseEvent(QMouseEvent *event)
{
    // The press state is consumed by its release, whatever the release does.
    const int pressRow = m_pressRow;
    const bool swallow = m_swallowRelease;
    m_swallowRelease = false;
    m_lastReleaseWasFolderHeader = false;
    clearPressState();
    if (swallow) {          // the tail of a drag, or the 2nd click of a double-click
        event->accept();
        return;
    }

    int row = rowAtY(event->position().y());
    if (row < 0 || row >= static_cast<int>(m_rows.size())) {
        event->accept();
        return;
    }

    // A folder header. Checked BEFORE the tag branch below, which assumes every
    // header is a tag: it would index m_tags with tagIndex (-1 here) and
    // silently do nothing, and the tag toggle's server PUT must never see a
    // folder. A click needs the press AND the release on this same header --
    // toggling is a layout change, so a press that wandered off must not fire.
    if (m_rows[row].isFolderHeader()) {
        m_lastReleaseWasFolderHeader = true;     // see mouseDoubleClickEvent
        if (row == pressRow) {
            const int section = m_rows[row].folderIndex;
            if (event->button() == Qt::LeftButton) {
                // While a search is on every folder is displayed open, so a
                // click would flip state the user cannot see: ignore it (D5).
                if (m_filterText.isEmpty()) {
                    const bool closed = section == talq::kFolderOther
                        ? m_otherCollapsed
                        : (section >= 0 && section < m_folders.size() && m_folders[section].collapsed);
                    setSectionCollapsed(section, !closed);
                }
            } else if (event->button() == Qt::RightButton && section >= 0
                       && section < m_folders.size()) {
                // Real folders only; Other has no menu. Copied out before the
                // emit for the same reason as in setSectionCollapsed().
                const QString id = m_folders[section].id;
                const QPointF global = mapToGlobal(event->position());
                emit folderContextMenuRequested(id, global.x(), global.y());
            }
        }
        event->accept();
        return;
    }

    // A tag section header: left-click toggles the section. The new state is
    // applied optimistically AND reported so MainWindow can persist it
    // server-side — collapsed state is per-user server state, not local UI
    // state, so it must survive a restart and follow the user to other
    // devices. Right-click on a header does nothing (there is no per-section
    // context menu), rather than opening a conversation's menu.
    if (m_rows[row].isHeader()) {
        if (event->button() == Qt::LeftButton) {
            const int t = m_rows[row].tagIndex;
            if (t >= 0 && t < m_tags.size()) {
                m_tags[t].collapsed = !m_tags[t].collapsed;
                emit tagSectionToggled(m_tags[t].id, m_tags[t].collapsed);
                rebuildLayouts();
            }
        }
        event->accept();
        return;
    }

    int modelIdx = m_rows[row].layoutIndex;
    const auto &cl = m_layouts[modelIdx];

    if (event->button() == Qt::RightButton) {
        // Map to global for context menu
        QPointF global = mapToGlobal(event->position());
        emit contextMenuRequested(modelIdx, cl.notificationLevel, global.x(), global.y());
    } else if (event->button() == Qt::LeftButton) {
        m_selectedIndex = modelIdx;
        m_selectedToken = cl.token;
        emit selectedIndexChanged();
        emit conversationClicked(cl.token, cl.displayName, cl.participantUserId,
                                 cl.conversationType, cl.userStatus,
                                 cl.userStatusMessage, cl.userStatusIcon);
        update();
    }

    event->accept();
}

// hoverMoveEvent/hoverLeaveEvent handled in event() override

// ═══════════════════════════════════════════════════════
// Drag and drop (conversation row -> folder)
// ═══════════════════════════════════════════════════════

int SidebarPainter::updateDropTarget(const QPoint &pos, const QString &token)
{
    // A folder header, or any row inside that folder, is that folder; the Other
    // header or a row in Other is "take it out of its folder".
    int section = talq::folderSectionOfRow(m_rows, rowAtY(pos.y()));
    if (!sectionIsValid(section))
        section = -1;
    // The highlight promises "a drop here files it here", so it is shown only
    // for a section that would change something. The RETURN value is a different
    // question -- "is the pointer over a place a drop can land?" -- and is what
    // dragMoveEvent answers the platform with: a drop on the chat's own folder
    // is accepted and then ignored by dropEvent, because an answer tied to the
    // highlight can go stale (the autoscroll tick below moves another folder
    // under a still pointer and no dragMove arrives to correct it).
    const int wash = talq::dropChangesFolder(section, sectionOfToken(token)) ? section : -1;
    if (wash != m_dropSection) {
        m_dropSection = wash;
        update();
    }
    return section;
}

void SidebarPainter::updateAutoScroll()
{
    const double step = talq::dragAutoScrollStep(m_dragPos.y(), height(),
                                                 kDragScrollZone, kDragScrollMaxStep);
    if (step != 0 && !m_dragScrollTimer->isActive())
        m_dragScrollTimer->start();
    else if (step == 0)
        m_dragScrollTimer->stop();
}

void SidebarPainter::onDragScrollTick()
{
    const double step = talq::dragAutoScrollStep(m_dragPos.y(), height(),
                                                 kDragScrollZone, kDragScrollMaxStep);
    if (step == 0) {
        m_dragScrollTimer->stop();
        return;
    }
    m_scrollY += step;
    clampScroll();
    // The content moved under a pointer that did not, and no dragMove arrives
    // for a still pointer: re-resolve the target ourselves.
    updateDropTarget(m_dragPos, m_dragToken);
    update();
}

void SidebarPainter::endDragFeedback()
{
    m_dragScrollTimer->stop();
    m_dragToken.clear();
    if (m_dropSection != -1) {
        m_dropSection = -1;
        update();
    }
}

void SidebarPainter::dragEnterEvent(QDragEnterEvent *event)
{
    // Our own mime type only, and only while folders are in force. Anything else
    // (a file, text, a URL) is refused here rather than half-handled.
    if (!dragPossible() || !event->mimeData()->hasFormat(QLatin1String(kConversationMime))) {
        event->ignore();
        return;
    }
    const QString token = QString::fromUtf8(
        event->mimeData()->data(QLatin1String(kConversationMime)));
    if (layoutIndexOfToken(token) < 0) {    // a room this list does not know
        event->ignore();
        return;
    }
    m_dragToken = token;
    m_dragPos = event->position().toPoint();
    // Accepted for the widget as a whole; dragMoveEvent accepts or refuses per
    // position (a place a drop can land, or not). The folder the chat is already
    // in is accepted too and only goes without the highlight.
    event->acceptProposedAction();
    updateDropTarget(m_dragPos, m_dragToken);
    updateAutoScroll();
}

void SidebarPainter::dragMoveEvent(QDragMoveEvent *event)
{
    if (m_dragToken.isEmpty()) {
        event->ignore();
        return;
    }
    m_dragPos = event->position().toPoint();
    // Over any section, the chat's own folder included: the platform keeps the
    // LAST answer given here, and on Windows a release after an "ignore" is
    // delivered as a drag-leave, not a drop. See updateDropTarget().
    if (updateDropTarget(m_dragPos, m_dragToken) != -1)
        event->acceptProposedAction();
    else
        event->ignore();
    updateAutoScroll();
}

void SidebarPainter::dragLeaveEvent(QDragLeaveEvent *event)
{
    endDragFeedback();
    event->accept();
}

void SidebarPainter::dropEvent(QDropEvent *event)
{
    const QString token = event->mimeData()->hasFormat(QLatin1String(kConversationMime))
        ? QString::fromUtf8(event->mimeData()->data(QLatin1String(kConversationMime)))
        : QString();
    const QPoint pos = event->position().toPoint();
    endDragFeedback();

    // Re-validate everything at the moment of the drop: the 30 s refresh may
    // have re-laid the rows, or dropped the room, since the drag began.
    const int section = talq::folderSectionOfRow(m_rows, rowAtY(pos.y()));
    if (!dragPossible() || layoutIndexOfToken(token) < 0 || !sectionIsValid(section)
        || !talq::dropChangesFolder(section, sectionOfToken(token))) {
        event->ignore();
        return;
    }
    const QString folderId = folderIdOfSection(section);   // empty = unfile
    event->acceptProposedAction();
    emit conversationDropped(token, folderId);
}

// ═══════════════════════════════════════════════════════
// PAINT
// ═══════════════════════════════════════════════════════

void SidebarPainter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    p.fillRect(QRectF(0, 0, width(), height()), m_theme.bgSidebar);

    // m_listEmpty is m_rows.empty() in every mode (folder mode leaves the rows
    // empty too when nothing matched), so the panel below never covers a header
    // or a row that a click could still reach.
    if (m_listEmpty && m_squeezed) {
        // Squeezed is a 56px icon-only rail (MainWindow.cpp:1861-1864) with no
        // room for a sentence — rows never show text there either. Stay a
        // bare panel rather than clip a message into an unreadable fragment.
        return;
    }

    if (m_listEmpty) {
        // Name the reason the panel is bare — a text search finding nothing
        // reads differently from a filter (Unread/Favorites/Direct/Groups)
        // hiding everything, which reads differently from a genuinely empty
        // conversation list. Idiom copied from ThreadsPainter::paintEmptyState.
        QString msg;
        if (!m_filterText.isEmpty()) {
            msg = tr("No conversations match \"%1\"").arg(m_filterText);
        } else if (!m_tagFilterId.isEmpty()) {
            // A tag filter hiding everything reads differently again — and
            // naming the tag is the difference between "this is broken" and
            // "nothing is tagged that way yet".
            msg = m_tagFilterName.isEmpty()
                      ? tr("Nothing tagged here yet")
                      : tr("Nothing tagged “%1” yet").arg(m_tagFilterName);
        } else {
            switch (m_filterMode) {
            case FilterUnread:    msg = tr("No unread conversations"); break;
            case FilterFavorites: msg = tr("No favorites yet"); break;
            case FilterDirect:    msg = tr("No direct messages"); break;
            case FilterGroups:    msg = tr("No groups"); break;
            case FilterAll:
            default:              msg = tr("No conversations yet"); break;
            }
        }
        p.setPen(m_theme.textSecondary);
        QFont f = m_theme.bodyFont();
        p.setFont(f);
        // msg can embed m_filterText, which is unbounded user input (the
        // other branches are all fixed strings) — elide it against the
        // available width so a long search doesn't clip mid-glyph.
        qreal maxTextWidth = width() - 2 * PainterTheme::spacingLarge;
        QString elided = QFontMetrics(f).elidedText(msg, Qt::ElideRight,
                                                      static_cast<int>(maxTextWidth));
        p.drawText(QRectF(0, 0, width(), height()), Qt::AlignCenter, elided);
        return;
    }

    qreal vpTop = m_scrollY;
    qreal vpBottom = m_scrollY + height();

    for (int vi = 0; vi < static_cast<int>(m_rows.size()); ++vi) {
        const talq::SidebarRow &row = m_rows[vi];
        qreal rowTop = row.y;
        qreal rowBottom = rowTop + row.height;

        if (rowBottom < vpTop || rowTop > vpBottom)
            continue;

        if (row.isHeader()) {
            if (row.isFolderHeader())
                paintFolderHeader(&p, row, vi);
            else
                paintSectionHeader(&p, row);
            continue;
        }

        // A drag is over this row's folder: wash the whole section so the
        // target reads even when its header has scrolled out of view. Painted
        // first, so a selected or hovered row's own fill still wins.
        if (m_dropSection != -1 && row.folderIndex == m_dropSection)
            p.fillRect(QRectF(0, rowTop - m_scrollY, width(), row.height), m_theme.bgHover);

        const auto &cl = m_layouts[row.layoutIndex];
        if (m_squeezed)
            paintRowSqueezed(&p, cl, vi);
        else
            paintRowNormal(&p, cl, vi);
    }

    paintScrollbar(&p);
}

// ═══════════════════════════════════════════════════════
// Normal row painting (expanded sidebar)
// ═══════════════════════════════════════════════════════

void SidebarPainter::paintRowNormal(QPainter *p, const ConversationLayout &cl, int visibleIdx)
{
    int rowH = RowHeight;
    qreal rowTop = m_rows[visibleIdx].y - m_scrollY;
    qreal w = width();
    int modelIdx = m_rows[visibleIdx].layoutIndex;

    // ── Background (selection / hover) ──
    bool selected = (modelIdx == m_selectedIndex);
    bool hovered = (visibleIdx == m_hoveredRow);

    if (selected) {
        p->fillRect(QRectF(0, rowTop, w, rowH), m_theme.bgSelected);
    } else if (hovered) {
        p->fillRect(QRectF(0, rowTop, w, rowH), m_theme.bgHover);
    }

    // ── Selection indicator bar (+ soft accent glow flavour) ──
    if (selected) {
        qreal barH = rowH * 0.5;
        qreal barY = rowTop + (rowH - barH) / 2.0;
        QLinearGradient gg(0, 0, 28, 0);
        QColor g0 = m_theme.glow; g0.setAlpha(46);
        QColor g1 = m_theme.glow; g1.setAlpha(0);
        gg.setColorAt(0, g0);
        gg.setColorAt(1, g1);
        p->setPen(Qt::NoPen);
        p->fillRect(QRectF(0, rowTop, 28, rowH), gg);
        p->setBrush(m_theme.accent);
        p->drawRoundedRect(QRectF(0, barY, SelectionBarWidth, barH), 2, 2);
    }

    // ── Layout constants ──
    qreal padLeft = PainterTheme::spacingLarge;
    qreal padRight = PainterTheme::spacingNormal;
    qreal padTop = PainterTheme::spacingSmall;
    qreal avatarY = rowTop + (rowH - AvatarSize) / 2.0;
    qreal textLeft = padLeft + AvatarSize + PainterTheme::spacingNormal;
    qreal textRight = w - padRight;

    // ── Avatar ──
    QRectF avatarRect(padLeft, avatarY, AvatarSize, AvatarSize);
    // ── Shift status ──
    // Every known state is marked, on-shift included. An earlier draft marked
    // only the exceptions, on the theory that badging the normal case is
    // noise -- but that made "on shift" and "we have no idea" look identical
    // in the list, which is the one question this feature exists to answer.
    //
    // It cannot be a corner badge shaped like the presence dot: the dot owns
    // bottom-right, the TalQ "Q" pill owns top-right, and `online` and
    // `success` are literally the same green in every theme. So this takes
    // the one free corner, bottom-LEFT, and is a rounded SQUARE -- position
    // and shape carry the distinction, not hue.
    //
    // Green here alongside a green presence dot is deliberate rather than
    // accidental: two greens means "at work AND reachable", which is exactly
    // the state you are scanning the list for.
    talq::ShiftState shiftState = talq::ShiftState::Unknown;
    if (m_shiftStatus && cl.conversationType == 1 && !cl.participantUserId.isEmpty())
        shiftState = m_shiftStatus->stateFor(cl.participantUserId);

    // Off-shift also dims the avatar. The marker says which state it is; the
    // dimming is what makes a row recede when you are scanning for someone to
    // ask right now.
    const bool dimForOffShift = (shiftState == talq::ShiftState::OffShift);
    if (dimForOffShift)
        p->setOpacity(0.55);
    paintAvatar(p, cl, avatarRect);
    if (dimForOffShift)
        p->setOpacity(1.0);

    if (talq::shiftStateIsDrawable(shiftState)) {
        QColor shiftColor;
        switch (shiftState) {
        case talq::ShiftState::OnShift:  shiftColor = m_theme.online;    break;
        case talq::ShiftState::OnBreak:  shiftColor = m_theme.amber;     break;
        case talq::ShiftState::OffShift: shiftColor = m_theme.textMuted; break;
        case talq::ShiftState::Unknown:  break;
        }
        const qreal s = StatusDotSize;
        const qreal x = avatarRect.left() - 1;
        const qreal y = avatarRect.bottom() - s + 1;
        // Ring it in the sidebar background, same as the presence dot, so it
        // reads against any avatar image behind it.
        p->setPen(Qt::NoPen);
        p->setBrush(m_theme.bgSidebar);
        p->drawRoundedRect(QRectF(x - 1.5, y - 1.5, s + 3, s + 3), 3.5, 3.5);
        p->setBrush(shiftColor);
        p->drawRoundedRect(QRectF(x, y, s, s), 2.5, 2.5);
    }

    // ── Status dot (1:1 conversations only, colored by presence) ──
    if (cl.conversationType == 1) {
        QColor dotColor;
        if (cl.userStatus == QStringLiteral("online")) dotColor = m_theme.online;
        else if (cl.userStatus == QStringLiteral("away")) dotColor = m_theme.amber;       // warm secondary
        else if (cl.userStatus == QStringLiteral("dnd")) dotColor = m_theme.danger;      // warm clay, not fire-engine
        // "busy" (Nextcloud 30+: a calendar meeting or a call) is a do-not-
        // interrupt state too. With no dot it read exactly like Offline.
        else if (cl.userStatus == QStringLiteral("busy")) dotColor = m_theme.danger;
        if (dotColor.isValid()) {
            qreal dotSize = StatusDotSize;
            qreal dotX = avatarRect.right() - dotSize + 1;
            qreal dotY = avatarRect.bottom() - dotSize + 1;
            // Border ring
            p->setPen(Qt::NoPen);
            p->setBrush(m_theme.bgSidebar);
            p->drawEllipse(QRectF(dotX - 1.5, dotY - 1.5, dotSize + 3, dotSize + 3));
            // Colored dot
            p->setBrush(dotColor);
            p->drawEllipse(QRectF(dotX, dotY, dotSize, dotSize));
        }
    }

    // ── Top row: [fav dot] Name ............ Time ──
    QFont nameFont;
    nameFont.setPixelSize(m_theme.fontSizeNormal);
    if (cl.unreadCount > 0)
        nameFont.setWeight(QFont::DemiBold);
    p->setFont(nameFont);

    QFont timeFont;
    timeFont.setPixelSize(m_theme.fontSizeTiny);
    QFontMetrics timeFM(timeFont);
    qreal timeW = timeFM.horizontalAdvance(cl.timeString);

    qreal nameY = rowTop + padTop;
    qreal nameLeft = textLeft;
    qreal nameRight = textRight - timeW - PainterTheme::spacingSmall;

    // Favorite dot — affinity, not a "needs you" signal. One-Signal Rule:
    // it must NOT use the teal accent (which competes with unread/active).
    if (cl.isFavorite) {
        p->setPen(Qt::NoPen);
        p->setBrush(m_theme.amber);   // warm "starred"
        qreal dotY2 = nameY + (m_theme.fontSizeNormal - FavDotSize) / 2.0 + 3;
        p->drawEllipse(QRectF(nameLeft, dotY2, FavDotSize, FavDotSize));
        nameLeft += FavDotSize + PainterTheme::spacingSmall;
    }

    // Display name (elided)
    QFontMetrics nameFM(nameFont);
    QString elidedName = nameFM.elidedText(cl.displayName, Qt::ElideRight,
                                            static_cast<int>(nameRight - nameLeft));
    const QRectF nameDrawRect(nameLeft, nameY, nameRight - nameLeft, m_theme.fontSizeNormal + 6);
    cl.nameRect = nameDrawRect;
    cl.nameElided = (elidedName != cl.displayName);
    p->setPen(m_theme.textPrimary);
    p->setFont(nameFont);
    p->drawText(nameDrawRect, Qt::AlignLeft | Qt::AlignVCenter, elidedName);

    // Timestamp
    // accentText, not accent: this is an 11px timestamp, and the fill-tuned
    // accent scores 2.85:1 on the selected row in the light theme.
    QColor timeColor = cl.unreadCount > 0 ? m_theme.accentText : m_theme.textTime;
    p->setPen(timeColor);
    p->setFont(timeFont);
    p->drawText(QRectF(textRight - timeW, nameY, timeW, m_theme.fontSizeNormal + 6),
                Qt::AlignRight | Qt::AlignVCenter, cl.timeString);

    // ── Bottom row: Preview ............ [badge] [muted] ──
    qreal bottomY = nameY + m_theme.fontSizeNormal + 6 + 3;

    // Defined once so the measured and drawn strings stay identical.
    const QString mutedLabel = tr("Muted");

    // Compute right-side width for badge + muted label
    qreal rightStuffW = 0;
    if (cl.unreadCount > 0)
        rightStuffW = unreadBadgeWidth(cl.unreadCount) + PainterTheme::spacingSmall;
    // Single-sourced: measured once here (both to reserve layout width and,
    // below, to size the actual draw rect) so a longer translation can never
    // clip against a width computed from a different font/string pairing.
    QFont mutedFont;
    mutedFont.setPixelSize(m_theme.fontSizeTiny);
    qreal mutedLabelW = 0;
    if (cl.notificationLevel == 3) {
        QFontMetrics mfm(mutedFont);
        mutedLabelW = mfm.horizontalAdvance(mutedLabel);
        rightStuffW += mutedLabelW + PainterTheme::spacingSmall;
    }

    // Preview text — textSecondary gives readable contrast against the warm
    // sidebar bg; textMuted is reserved for disabled-looking states.
    QFont previewFont;
    previewFont.setPixelSize(m_theme.fontSizeSmall);
    qreal previewRight = textRight - rightStuffW;
    p->setPen(m_theme.textSecondary);
    p->setFont(previewFont);
    EmojiTextRenderer::drawElided(p,
        QRectF(textLeft, bottomY, previewRight - textLeft, m_theme.fontSizeSmall + 4),
        cl.previewText,
        Qt::ElideRight);

    // Unread badge
    if (cl.unreadCount > 0) {
        QRectF badgeArea(previewRight, bottomY - 1, textRight - previewRight, BadgeHeight);
        paintUnreadBadge(p, cl.unreadCount, cl.unreadMention, cl.unreadMentionDirect, badgeArea);
    }

    // Muted label — full-alpha textMuted (the dedicated de-emphasized token,
    // not a 60%-tinted textSecondary — was ~2.5:1 on Paper, effectively
    // invisible), drawn into the width measured above rather than a
    // hardcoded 30px rect that clipped longer translations.
    if (cl.notificationLevel == 3) {
        p->setPen(m_theme.textMuted);
        p->setFont(mutedFont);
        p->drawText(QRectF(textRight - mutedLabelW, bottomY, mutedLabelW, m_theme.fontSizeSmall + 4),
                    Qt::AlignRight | Qt::AlignVCenter, mutedLabel);
    }
}

// ═══════════════════════════════════════════════════════
// Squeezed row painting (avatar-only sidebar)
// ═══════════════════════════════════════════════════════

void SidebarPainter::paintRowSqueezed(QPainter *p, const ConversationLayout &cl, int visibleIdx)
{
    int rowH = RowHeightSqueezed;
    qreal rowTop = m_rows[visibleIdx].y - m_scrollY;
    qreal w = width();
    int modelIdx = m_rows[visibleIdx].layoutIndex;

    // Selection highlight
    bool selected = (modelIdx == m_selectedIndex);
    bool hovered = (visibleIdx == m_hoveredRow);
    if (selected) {
        p->fillRect(QRectF(0, rowTop, w, rowH), m_theme.bgSelected);
    } else if (hovered) {
        p->fillRect(QRectF(0, rowTop, w, rowH), m_theme.bgHover);
    }

    // Avatar centered
    qreal avatarX = (w - AvatarSizeSqueezed) / 2.0;
    qreal avatarY = rowTop + (rowH - AvatarSizeSqueezed) / 2.0;
    QRectF avatarRect(avatarX, avatarY, AvatarSizeSqueezed, AvatarSizeSqueezed);
    paintAvatar(p, cl, avatarRect);

    // Unread badge overlay (top-right of avatar)
    if (cl.unreadCount > 0) {
        QString countStr = cl.unreadCount > 99
            ? QStringLiteral("99+") : QString::number(cl.unreadCount);
        QFont badgeFont;
        badgeFont.setPixelSize(BadgeFontSize);
        badgeFont.setWeight(QFont::DemiBold);
        const qreal badgeW = unreadBadgeWidth(cl.unreadCount);
        qreal badgeX = avatarRect.right() - badgeW / 2.0;
        qreal badgeY = avatarRect.top() - BadgeHeight / 2.0 + 2;

        // Three levels rather than two. `unreadMention` is set by an @all
        // just as much as by a direct @you, so before 0.65.3 a room-wide
        // announcement raised the same alarm-red badge as someone actually
        // asking you something -- which trains people to ignore the red one.
        //   accent = plain unread   amber = @all   danger = @you
        // Servers without `direct-mention-flag` never set the direct bit, so
        // every mention stays amber there rather than being wrongly promoted.
        QColor bgColor = cl.unreadMentionDirect ? m_theme.danger
                       : cl.unreadMention       ? m_theme.amber
                                                : m_theme.accent;
        p->setPen(Qt::NoPen);
        p->setBrush(bgColor);
        p->drawRoundedRect(QRectF(badgeX, badgeY, badgeW, BadgeHeight),
                           BadgeHeight / 2.0, BadgeHeight / 2.0);

        // Score the ink against the ACTUAL fill: this badge is accent for a
        // plain unread but danger for a mention, and controlInk is calibrated
        // for accent alone. Same idiom already used for author-colour fills.
        p->setPen(m_theme.inkOn(bgColor));
        p->setFont(badgeFont);
        p->drawText(QRectF(badgeX, badgeY, badgeW, BadgeHeight),
                    Qt::AlignCenter, countStr);
    }
}

// ═══════════════════════════════════════════════════════
// Avatar painting
// ═══════════════════════════════════════════════════════

void SidebarPainter::paintAvatar(QPainter *p, const ConversationLayout &cl, const QRectF &rect)
{
    // Note to self (type 6) — special bookmark icon
    if (cl.conversationType == 6) {
        const QColor fill = PainterTheme::authorColor(QStringLiteral("note-to-self"));  // in-palette, not gray
        p->setPen(Qt::NoPen);
        p->setBrush(fill);
        p->drawEllipse(rect);
        QFont iconFont;
        iconFont.setPixelSize(static_cast<int>(rect.width() * 0.5));
        p->setPen(m_theme.inkOn(fill));   // No-Gray: ink scored against the actual fill
        p->setFont(iconFont);
        p->drawText(rect, Qt::AlignCenter, QStringLiteral("\U0001F516"));  // 🔖
        return;
    }

    int size = static_cast<int>(rect.width());
    QImage img = fetchAvatar(cl.participantUserId, cl.token, cl.conversationType, size);

    if (!img.isNull()) {
        p->drawImage(rect, img);
    } else {
        // Fallback: colored disc with initial -- a rounded square for a group,
        // so the placeholder reads the same as the picture that replaces it.
        QColor bgColor = PainterTheme::authorColor(
            cl.participantUserId.isEmpty() ? cl.displayName : cl.participantUserId);
        p->setPen(Qt::NoPen);
        p->setBrush(bgColor);
        if (talq::isGroupConversation(cl.conversationType))
            p->drawRoundedRect(rect, size / 4, size / 4);
        else
            p->drawEllipse(rect);

        // Initial letter
        QChar initial = cl.displayName.isEmpty() ? QChar('?') : cl.displayName.at(0).toUpper();
        QFont initFont;
        initFont.setPixelSize(size / 2);
        initFont.setWeight(QFont::DemiBold);
        p->setPen(m_theme.inkOn(bgColor));   // No-Gray: ink scored against the actual fill
        p->setFont(initFont);
        p->drawText(rect, Qt::AlignCenter, QString(initial));
    }

    // TalQ marker: small "Q" pill on the top-right of the avatar when
    // this conversation's primary participant is known to be using TalQ.
    // Only meaningful for 1-on-1 chats (conversationType == 1).
    if (m_signaling && cl.conversationType == 1 && !cl.participantUserId.isEmpty()) {
        const QString info = m_signaling->peerClientInfo(cl.participantUserId);
        if (info.startsWith("TalQ")) {
            const qreal badgeSize = qMax(qreal(12), rect.width() * 0.35);
            // Top-right corner: the presence status dot occupies bottom-right.
            QRectF badge(rect.right() - badgeSize, rect.top(),
                         badgeSize, badgeSize);
            // Identity marker, not a "needs you" signal — must NOT use the
            // teal One-Signal accent nor the old foreign cobalt. In-palette
            // violet keeps it distinct and warm.
            p->setPen(QPen(m_theme.bgSidebar, qMax(1.0, badgeSize * 0.08)));
            p->setBrush(PainterTheme::topicColor(4));   // in-palette violet, per-theme
            p->drawEllipse(badge);

            QFont qFont;
            qFont.setPixelSize(int(badgeSize * 0.7));
            qFont.setWeight(QFont::Bold);
            // Same contrast bug slice D fixed for authorColor fills, via a
            // different palette: topicColor(4) is #9b7cd4, the SAME hex as
            // s_authorPalette[4], and controlInk measures ~3.31:1 against it
            // on Paper -- an AA fail. Score against the actual fill instead.
            p->setPen(m_theme.inkOn(PainterTheme::topicColor(4)));
            p->setFont(qFont);
            p->drawText(badge, Qt::AlignCenter, QStringLiteral("Q"));
        }
    }
}

// ═══════════════════════════════════════════════════════
// Unread badge
// ═══════════════════════════════════════════════════════

// The one place a badge width is worked out: both row paths, paintUnreadBadge()
// and the folder header call it, so a header can reserve exactly what
// paintUnreadBadge() will draw before it elides the folder name.
qreal SidebarPainter::unreadBadgeWidth(int count)
{
    const QString countStr = count > 99 ? QStringLiteral("99+") : QString::number(count);
    QFont badgeFont;
    badgeFont.setPixelSize(BadgeFontSize);
    badgeFont.setWeight(QFont::DemiBold);
    const qreal textW = QFontMetrics(badgeFont).horizontalAdvance(countStr);
    return qMax(BadgeHeight * 1.0, textW + 10.0);
}

void SidebarPainter::paintUnreadBadge(QPainter *p, int count, bool mention, bool directMention,
                                      const QRectF &badgeArea)
{
    QString countStr = count > 99 ? QStringLiteral("99+") : QString::number(count);
    QFont badgeFont;
    badgeFont.setPixelSize(BadgeFontSize);
    badgeFont.setWeight(QFont::DemiBold);
    qreal badgeW = unreadBadgeWidth(count);

    // Right-align within badgeArea
    qreal badgeX = badgeArea.right() - badgeW;
    qreal badgeY = badgeArea.top() + (badgeArea.height() - BadgeHeight) / 2.0;

    // Three levels, matching the full-row path: direct @you = clay (danger),
    // @all = amber, plain unread = teal. Before 0.65.3 an @all raised the same
    // alarm colour as a direct mention, which is how a red badge stops meaning
    // anything. Servers without `direct-mention-flag` never set the direct
    // bit, so mentions stay amber there rather than being wrongly promoted.
    QColor bgColor = directMention ? m_theme.danger
                   : mention       ? m_theme.amber
                                   : m_theme.unreadBadge;
    p->setPen(Qt::NoPen);
    p->setBrush(bgColor);
    p->drawRoundedRect(QRectF(badgeX, badgeY, badgeW, BadgeHeight),
                       BadgeHeight / 2.0, BadgeHeight / 2.0);

    p->setPen(m_theme.inkOn(bgColor));   // accent OR danger fill -- score it
    p->setFont(badgeFont);
    p->drawText(QRectF(badgeX, badgeY, badgeW, BadgeHeight), Qt::AlignCenter, countStr);
}

// ═══════════════════════════════════════════════════════
// Scrollbar
// ═══════════════════════════════════════════════════════

void SidebarPainter::paintSectionHeader(QPainter *p, const talq::SidebarRow &row)
{
    if (row.tagIndex < 0 || row.tagIndex >= m_tags.size())
        return;
    const SidebarTag &tag = m_tags[row.tagIndex];

    const qreal top = row.y - m_scrollY;
    const qreal w = width();
    const int padX = PainterTheme::spacingLarge;

    // No fill: the header reads as a label over the sidebar, not as a
    // selectable row. Painting a band here would make it look clickable in the
    // same way a conversation is, and the two do very different things.
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);

    // Disclosure triangle, rotated by state. Drawn rather than glyphed so it
    // matches at any DPI and needs no font that carries the character.
    const qreal cx = padX + 4;
    const qreal cy = top + row.height / 2.0;
    QPainterPath tri;
    if (tag.collapsed) {
        tri.moveTo(cx - 2.5, cy - 4);
        tri.lineTo(cx + 3.5, cy);
        tri.lineTo(cx - 2.5, cy + 4);
    } else {
        tri.moveTo(cx - 4, cy - 2.5);
        tri.lineTo(cx + 4, cy - 2.5);
        tri.lineTo(cx, cy + 3.5);
    }
    tri.closeSubpath();
    p->fillPath(tri, m_theme.textSecondary);

    // Eyebrow treatment, matching the app's section-label convention.
    QFont f = m_theme.bodyFont();
    f.setPointSizeF(f.pointSizeF() * 0.82);
    f.setWeight(QFont::DemiBold);
    f.setCapitalization(QFont::AllUppercase);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    p->setFont(f);
    p->setPen(m_theme.textSecondary);

    const qreal textX = cx + 12;
    const QFontMetrics fm(f);
    // Reserve room for the count so a long tag name elides against the count
    // rather than painting through it.
    const QString count = QString::number(m_tagSectionCounts.value(tag.id, 0));
    const qreal countW = fm.horizontalAdvance(count) + 8;
    const qreal availW = w - textX - padX - countW;
    const QString name = fm.elidedText(tag.name, Qt::ElideRight,
                                       static_cast<int>(qMax(0.0, availW)));
    p->drawText(QRectF(textX, top, availW, row.height),
                Qt::AlignVCenter | Qt::AlignLeft, name);

    // Count on the right. Its value is the section's TOTAL, so a collapsed
    // section still says how much is hidden under it.
    p->drawText(QRectF(w - padX - countW, top, countW, row.height),
                Qt::AlignVCenter | Qt::AlignRight, count);

    p->restore();
}

void SidebarPainter::paintFolderHeader(QPainter *p, const talq::SidebarRow &row, int visibleIdx)
{
    const int fi = row.folderIndex;
    const bool isOther = (fi == talq::kFolderOther);
    // Never paint a header whose folder is gone (the rows are rebuilt together
    // with the folder list, so this is a guard, not an expected path).
    if (!isOther && (fi < 0 || fi >= m_folders.size()))
        return;

    const qreal top = row.y - m_scrollY;
    const qreal w = width();
    const int padX = PainterTheme::spacingLarge;
    const bool closed = folderDisplayedClosed(fi);
    const talq::FolderRollup ru = isOther ? m_otherRollup : m_folderRollups.value(fi);

    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);

    // Unlike a tag header, a folder header IS clickable and a drop target, so it
    // gets a fill: the drop target wins over hover. Existing tokens only.
    const QRectF band(0, top, w, row.height);
    if (m_dropSection == fi) {
        p->fillRect(band, m_theme.bgSelected);
        // accentText, not accent: it is the token the conformance sweep scores
        // against bgSelected, so the outline is guaranteed to read there.
        p->setPen(QPen(m_theme.accentText, 1.5));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(band.adjusted(4, 1.5, -4, -1.5),
                           PainterTheme::radiusSmall, PainterTheme::radiusSmall);
    } else if (visibleIdx == m_hoveredRow) {
        p->fillRect(band, m_theme.bgHover);
    }

    // Disclosure triangle: a painted path, not a glyph (same recipe as the tag
    // header). Points right when closed, down when open.
    const qreal cx = padX + 4;
    const qreal cy = top + row.height / 2.0;
    QPainterPath tri;
    if (closed) {
        tri.moveTo(cx - 2.5, cy - 4);
        tri.lineTo(cx + 3.5, cy);
        tri.lineTo(cx - 2.5, cy + 4);
    } else {
        tri.moveTo(cx - 4, cy - 2.5);
        tri.lineTo(cx + 4, cy - 2.5);
        tri.lineTo(cx, cy + 3.5);
    }
    tri.closeSubpath();
    p->fillPath(tri, m_theme.textSecondary);

    // Eyebrow label. The tag header shrinks bodyFont() with setPointSizeF(
    // pointSizeF() * 0.82), but bodyFont() is PIXEL-sized and pointSizeF() is -1
    // for such a font, so Qt rejects it ("Point size <= 0") and that label is
    // never shrunk. Shrink in pixels here, where it takes effect.
    QFont f = m_theme.bodyFont();
    f.setPixelSize(qMax(9, qRound(m_theme.fontSizeNormal * 0.82)));
    f.setWeight(QFont::DemiBold);
    f.setCapitalization(QFont::AllUppercase);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    p->setFont(f);
    p->setPen(m_theme.textSecondary);

    const qreal textX = cx + 12;
    const QFontMetrics fm(f);
    const QString count = QString::number(ru.count);
    const qreal countW = fm.horizontalAdvance(count) + 8;
    // A CLOSED folder with anything unread shows the sum so nothing hides in it.
    // The badge and the count are placed from the right edge first and the name
    // elided against what is left (talq::folderHeaderSpans), never the reverse.
    const bool showBadge = closed && talq::rollupShowsBadge(ru);
    const qreal badgeW = showBadge ? unreadBadgeWidth(ru.unread) : 0;
    const talq::FolderHeaderSpans sp = talq::folderHeaderSpans(
        w, textX, padX, countW, badgeW, PainterTheme::spacingSmall);

    const QString name = fm.elidedText(isOther ? tr("Other") : m_folders[fi].name,
                                       Qt::ElideRight, static_cast<int>(sp.nameW));
    p->drawText(QRectF(textX, top, sp.nameW, row.height),
                Qt::AlignVCenter | Qt::AlignLeft, name);
    p->drawText(QRectF(sp.countX, top, countW, row.height),
                Qt::AlignVCenter | Qt::AlignRight, count);

    if (showBadge) {
        // Three-level colour (direct @you / @all / plain) comes from the very
        // same paintUnreadBadge the rows use; it right-aligns in this area.
        paintUnreadBadge(p, ru.unread, ru.mention, ru.directMention,
                         QRectF(sp.badgeX, top, badgeW, row.height));
    }

    p->restore();
}

void SidebarPainter::paintScrollbar(QPainter *p)
{
    qreal contentH = talq::totalHeight(m_rows);
    if (contentH <= height())
        return; // no scrollbar needed

    qreal viewH = height();
    qreal thumbH = qMax(20.0, (viewH / contentH) * viewH);
    qreal maxScroll = contentH - viewH;
    qreal thumbY = (m_scrollY / maxScroll) * (viewH - thumbH);

    qreal scrollbarW = 4;
    qreal scrollbarX = width() - scrollbarW - 1;

    p->setPen(Qt::NoPen);
    QColor thumbColor = m_theme.textMuted;
    thumbColor.setAlphaF(0.3f);
    p->setBrush(thumbColor);
    p->drawRoundedRect(QRectF(scrollbarX, thumbY, scrollbarW, thumbH), 2, 2);
}

// ═══════════════════════════════════════════════════════
// Avatar image loading
// ═══════════════════════════════════════════════════════

QString SidebarPainter::avatarCacheKey(const QString &userId, const QString &token, int convType) const
{
    // For 1:1 chats, use the userId. For groups, use the token (room avatar).
    if (convType == 1 && !userId.isEmpty())
        return QStringLiteral("user:") + userId;
    return QStringLiteral("room:") + token;
}

// Avatar cache freshness. No HTTP cache exists, so the server always returns the
// current avatar; re-fetch once the cached copy is due. The rules (the TTLs, the
// download's timeout, when an "in flight" marker stops being believed, what a failed
// refresh may do to a picture we hold) live in core/AvatarFetchPolicy.h, shared with
// the header and the chat bubbles so the three cannot drift apart.
QImage SidebarPainter::fetchAvatar(const QString &userId, const QString &token, int convType, int size)
{
    QString key = avatarCacheKey(userId, token, convType);
    auto it = m_avatarCache.find(key);
    if (it != m_avatarCache.end()) {
        const QImage &cached = it.value();
        // Re-fetch in the background once the cached entry is stale so a
        // changed avatar appears without a restart; a failed (null) entry
        // retries on the shorter error backoff instead of staying blank.
        if (talq::avatarfetch::due(cached.isNull(), m_avatarFetchedMs.value(key, 0),
                                   QDateTime::currentMSecsSinceEpoch()))
            requestAvatar(userId, token, convType, size);
        // Return cached, but re-scale if size differs
        if (cached.isNull()) return cached;
        if (cached.width() == size) return cached;
        return cached.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    requestAvatar(userId, token, convType, size);
    return QImage();
}

void SidebarPainter::requestAvatar(const QString &userId, const QString &token, int convType, int size)
{
    QString key = avatarCacheKey(userId, token, convType);
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    // A download in flight is not doubled -- but the marker is believed for a minute
    // only. One that never finishes used to block every later refresh of this picture
    // until TalQ was restarted.
    const auto pending = m_avatarPending.constFind(key);
    if (!talq::avatarfetch::mayStart(pending != m_avatarPending.constEnd(),
                                     pending != m_avatarPending.constEnd() ? pending.value() : 0,
                                     started))
        return;
    if (!m_api)
        return;

    m_avatarPending.insert(key, started);

    // Build URL: user avatar for 1:1, room avatar for groups
    QString url;
    if (convType == 1 && !userId.isEmpty()) {
        url = QStringLiteral("/index.php/avatar/") + userId + QStringLiteral("/64");
    } else {
        // Cache-buster: a hardcoded ?v=1 never changes, so any HTTP cache in the
        // path (reverse proxy / Nextcloud) keeps serving the OLD room avatar even
        // after it's changed. Stamp a fresh value so each fetch bypasses caches and
        // returns the CURRENT avatar (paired with invalidateAvatars() on roomChanged).
        url = QStringLiteral("/ocs/v2.php/apps/spreed/api/v1/room/") + token
              + QStringLiteral("/avatar?v=")
              + QString::number(QDateTime::currentSecsSinceEpoch());
    }

    // With a transfer timeout: a stalled download errors out instead of hanging.
    QNetworkReply *reply = m_api->getAbsoluteUrl(url, talq::avatarfetch::kTransferTimeoutMs);

    connect(reply, &QNetworkReply::finished, this, [this, reply, key, size, started, convType]() {
        reply->deleteLater();
        // Only the download that owns the marker releases it: one that outlived the
        // give-up window and finishes late must not release a NEWER download's marker.
        if (m_avatarPending.value(key, -1) == started)
            m_avatarPending.remove(key);
        const qint64 done = QDateTime::currentMSecsSinceEpoch();

        // A failed refresh (a timeout is a normal outcome now, so is an offline blip)
        // never blanks a picture we already hold; it is just asked for again soon.
        // With nothing to keep, a null entry marks the failure; the TTL/backoff in
        // fetchAvatar drives the retry either way.
        const auto failed = [this, key, done]() {
            const auto held = m_avatarCache.constFind(key);
            const bool hadImage = held != m_avatarCache.constEnd() && !held.value().isNull();
            m_avatarFetchedMs[key] = talq::avatarfetch::failedStampMs(hadImage, done);
            if (!hadImage)
                m_avatarCache[key] = QImage();
        };

        if (reply->error() != QNetworkReply::NoError) {
            failed();
            return;
        }

        QByteArray data = reply->readAll();
        QImage img;
        if (!img.loadFromData(data)) {
            failed();
            return;
        }

        m_avatarFetchedMs[key] = done;
        // The cache holds PRE-MASKED pixels, so the shape is chosen here, from
        // the conversation type captured when the request was made. size/4 is
        // proportional on purpose: fetchAvatar() rescales this image when the
        // rail squeezes (44 -> 40), and a fixed radius would not scale with it.
        m_avatarCache[key] = talq::isGroupConversation(convType)
            ? PainterTheme::cropToRoundedSquare(img, size, size / 4)
            : PainterTheme::cropToCircle(img, size);
        qInfo().nospace() << "SidebarPainter: avatar fetched key=" << key
                          << " bytes=" << data.size();
        update(); // repaint with loaded avatar
    });
}
