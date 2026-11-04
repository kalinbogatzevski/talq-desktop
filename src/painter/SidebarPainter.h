#pragma once

#include <QWidget>
#include <QVector>
#include <QHash>
#include <QSet>
#include <QImage>
#include <vector>
#include "PainterTheme.h"
#include "SidebarRowLayout.h"
#include "core/FolderLogic.h"

class QTimer;
class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;
class ConversationListModel;
class ApiClient;
class SignalingClient;
class ShiftStatusService;
class QNetworkReply;

/**
 * Pre-computed layout for a single conversation row.
 */
struct ConversationLayout {
    // Data from model
    QString token;
    QString displayName;
    QString lastMessage;
    QString lastAuthor;
    QString userStatus;      // "online", "away", "dnd", "offline"
    QString userStatusMessage;
    QString userStatusIcon;
    QString participantUserId;
    int conversationType = 0;
    int unreadCount = 0;
    bool unreadMention = false;
    // Direct @you, not @all. Drives badge severity.
    bool unreadMentionDirect = false;
    bool isArchived = false;
    bool isFavorite = false;
    qint64 lastActivity = 0;
    int notificationLevel = 0;  // 0=default, 1=always, 2=mention, 3=never
    QStringList tagIds;         // Talk 24 conversation tags (empty pre-24)

    // Computed
    QString timeString;
    QString previewText;       // "author: message" truncated
    qreal y = 0;               // top position in content coordinates

    // Name-label geometry as last painted (paintRowNormal only -- squeezed
    // rows show no name text). mutable: this struct is handed to the paint
    // routines as a const ref, but caching where the (possibly elided) name
    // landed is a pure paint-time byproduct, read back by event()'s
    // QEvent::ToolTip handler to show the full name on hover -- same idiom
    // ChatPainter uses for its timestamp tooltip, minus that class's already
    // more elaborate per-message layout cache.
    mutable QRectF nameRect;
    mutable bool nameElided = false;
};

/**
 * QWidget that renders the conversation sidebar using QPainter.
 */
class SidebarPainter : public QWidget
{
    Q_OBJECT

public:
    explicit SidebarPainter(QWidget *parent = nullptr);

    // ── Property accessors ──
    void setModel(ConversationListModel *model);
    void setApi(ApiClient *api);
    void setSignaling(SignalingClient *signaling);
    // Optional. Absent (or unpaired) means no shift marker is drawn -- rows
    // render exactly as they do today.
    void setShiftStatus(ShiftStatusService *shiftStatus);

    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool dark);
    void setTheme(PainterTheme::Theme t);

    int selectedIndex() const { return m_selectedIndex; }
    void setSelectedIndex(int idx);

    bool squeezed() const { return m_squeezed; }
    void setSqueezed(bool sq);

    // True while a row is being dragged (QDrag::exec() is running). The folder
    // sync apply gate reads it: a remote reorder or delete must not land mid-drag,
    // because the drop highlight is an index into the folder list.
    bool isDragInProgress() const { return m_dragInProgress; }

    QImage cachedAvatar(const QString &key) const { return m_avatarCache.value(key); }
    // Mark every cached row avatar stale so the next paint re-fetches it (the
    // current image stays visible until the new one loads). Called on window
    // focus so a changed group/user avatar shows up without a restart.
    void invalidateAvatars() { m_avatarFetchedMs.clear(); update(); }

    // Cache stats (for DebugMonitor)
    int avatarCacheCount() const { return m_avatarCache.size(); }
    qint64 avatarCacheBytes() const;

    QString filterText() const { return m_filterText; }
    void setFilterText(const QString &text);

    // ── Sort / filter modes (F1) ──
    enum SortMode  { SortRecent = 0, SortUnread = 1, SortName = 2 };
    enum FilterMode { FilterAll = 0, FilterUnread = 1, FilterFavorites = 2,
                      FilterDirect = 3, FilterGroups = 4,
                      // Archived is the ONLY mode that shows archived rooms;
                      // every other mode hides them, which is what archiving is.
                      FilterArchived = 5 };

    int sortMode() const { return m_sortMode; }
    void setSortMode(int mode);
    int filterMode() const { return m_filterMode; }
    void setFilterMode(int mode);

    // ── Talk 24 tag filter ──
    // Orthogonal to FilterMode on purpose: a tag is a user-defined label, not
    // one of the five built-in buckets, and squeezing tags into the FilterMode
    // enum would mean renumbering a value already persisted in QSettings.
    // Empty string = no tag filter. The special ids "!favorites" and "!other"
    // select the server's two fixed pseudo-tags (favourited conversations, and
    // conversations carrying no tags at all) — real tag ids are numeric, so a
    // "!"-prefix can never collide with one.
    QString tagFilter() const { return m_tagFilterId; }
    void setTagFilter(const QString &tagId);
    // Human-readable name of the active tag filter, for the empty state only.
    QString tagFilterName() const;
    void setTagFilterName(const QString &name);

    // ── Talk 24 tag grouping (section headers) ──
    // One tag as the sidebar needs it: already ordered by the caller, and
    // already localised (built-in Favourites/Other names are rendered by
    // MainWindow from the tag TYPE, because the server freezes their names at
    // whatever locale first created them).
    struct SidebarTag {
        QString id;         // real tag id, or "!favorites" / "!other"
        QString name;
        bool collapsed = false;
    };
    void setTags(const QVector<SidebarTag> &tags);

    // ── Grouping (what owns the section headers) ──
    // One owner at a time: tag sections and folder sections are different
    // things with different rules (a conversation sits under EVERY tag it has
    // but in exactly ONE folder), so both at once would mean two meanings for
    // one header row. Explicit mode, not "folders win when any exist": deleting
    // the last folder must not silently bring tag headers back.
    enum GroupMode { GroupNone = 0, GroupTags = 1, GroupFolders = 2 };
    GroupMode groupMode() const { return m_groupMode; }
    void setGroupMode(GroupMode mode);
    // Pre-folders entry points, kept so existing callers compile. They speak
    // only about TAG grouping: setGroupByTag(false) while folders are the mode
    // is a no-op (it was already "off"), exactly as the old bool behaved.
    bool groupByTag() const { return m_groupMode == GroupTags; }
    void setGroupByTag(bool on);

    // ── Folders ──
    // Pushed in by MainWindow after every change to its FolderStore. Held in
    // memory only: rebuildLayouts() runs on every single-row unread update, so
    // nothing here may touch QSettings or disk. `tokenToFolderId` is the
    // store's whole assignment map; an entry naming a folder that is not in
    // `folders` counts as unfiled (the Other section). The painter never
    // changes this state itself except for the open/closed flag of a header
    // the user just clicked, which it reports through folderToggled().
    void setFolders(const QVector<talq::Folder> &folders, bool otherCollapsed,
                    const QHash<QString, QString> &tokenToFolderId);
    // Make the row for `token` visible: open its folder if that folder is
    // closed (reported through folderToggled(), so the store persists it), then
    // scroll so the row is fully on screen. A no-op when the token is unknown,
    // is filtered out, or the list is not grouped by folders.
    void revealToken(const QString &token);

signals:
    void selectedIndexChanged();

    void conversationClicked(const QString &token, const QString &displayName,
                             const QString &participantUserId, int conversationType,
                             const QString &userStatus,
                             const QString &statusMessage,
                             const QString &statusIcon);

    void contextMenuRequested(int modelIndex, int notificationLevel, qreal globalX, qreal globalY);

    void homeRequested();

    // A tag section header was clicked. Collapsed state is SERVER-persisted
    // (PUT /tags/{id}/collapsed), so the painter reports the intent and lets
    // MainWindow own the round-trip rather than reaching for ApiClient itself.
    void tagSectionToggled(const QString &tagId, bool collapsed);

    // A FOLDER header was clicked (or revealToken opened one). Deliberately a
    // different signal from tagSectionToggled: that one makes MainWindow PUT
    // /tags/{id}/collapsed to the Talk server, and a folder is local to this
    // PC. `folderId` is EMPTY for the built-in Other section.
    void folderToggled(const QString &folderId, bool collapsed);
    // Right-click on a real folder header (not on Other, which has no menu).
    void folderContextMenuRequested(const QString &folderId, qreal globalX, qreal globalY);
    // A dragged conversation row was dropped on a folder (a header, or any row
    // inside it) or on Other. EMPTY folderId = take it out of its folder. Never
    // emitted for a drop on the folder it is already in. The painter changes
    // no state on a drop; MainWindow files it and pushes the result back.
    void conversationDropped(const QString &token, const QString &folderId);

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool event(QEvent *event) override;

private slots:
    void onDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight);
    void onModelReset();
    void onRowsInserted(const QModelIndex &parent, int first, int last);
    void onRowsRemoved(const QModelIndex &parent, int first, int last);

private:
    // ── Layout ──
    static constexpr int RowHeight = 62;       // compact, Telegram-style
    static constexpr int RowHeightSqueezed = 52;
    // Tag section header. Deliberately less than half a conversation row: a
    // header is a label, and at full row height a few sections would push the
    // conversations themselves off the screen.
    static constexpr int HeaderHeight = 28;
    static constexpr int AvatarSize = 44;      // matches Theme.avatarSize
    static constexpr int AvatarSizeSqueezed = 40;
    static constexpr int BadgeHeight = PainterTheme::badgeHeight;
    static constexpr int BadgeFontSize = PainterTheme::badgeFontSize;
    static constexpr int FavDotSize = 6;
    static constexpr int StatusDotSize = 10;
    static constexpr int SelectionBarWidth = 3;

    void rebuildLayouts();
    void clampScroll();
    int rowAtY(qreal viewportY) const;  // returns visible row index, or -1

    // ── Painting helpers ──
    void paintRowNormal(QPainter *p, const ConversationLayout &cl, int visibleIdx);
    void paintRowSqueezed(QPainter *p, const ConversationLayout &cl, int visibleIdx);
    void paintSectionHeader(QPainter *p, const talq::SidebarRow &row);   // TAG header
    void paintFolderHeader(QPainter *p, const talq::SidebarRow &row, int visibleIdx);
    // Builds m_rows from m_layouts: flat when grouping is off (identical to
    // 0.64 behaviour), grouped into tag sections when tag grouping is on, or
    // into folder sections (buildFolderRows) when folder grouping is on.
    void buildVisibleRows(const QVector<int> &matching);
    void buildFolderRows(const QVector<int> &matching, int rowH);
    // Folder grouping is actually in force: the mode, at least one folder, and
    // not the 56 px rail (no room for a header). Single-sourced because the
    // row build, the drag, and revealToken must all agree on it.
    bool foldersBranchActive() const
    {
        return m_groupMode == GroupFolders && !m_folders.isEmpty() && !m_squeezed;
    }
    // A text search shows every folder open (stored state untouched), so a hit
    // inside a closed folder is never hidden; this is the state as DISPLAYED.
    bool folderDisplayedClosed(int folderIndex) const;
    // Section a conversation is in now: its folder's index, or kFolderOther
    // when it is unfiled or filed under a folder that no longer exists.
    int sectionOfToken(const QString &token) const;
    QString folderIdOfSection(int section) const;   // empty for kFolderOther / out of range
    bool sectionIsValid(int section) const;         // kFolderOther, or a real folder index
    int layoutIndexOfToken(const QString &token) const;   // -1 when unknown
    // Open/closed flip of one folder section (kFolderOther = the Other section).
    // Mutates the in-memory flag, reports it through folderToggled(), rebuilds.
    void setSectionCollapsed(int section, bool collapsed);
    // m_hoveredRow is a VISIBLE-row index, so any rebuild that changes the row
    // list leaves it pointing at a different row; re-derive it from the cursor.
    void refreshHover();
    bool dragPossible() const { return foldersBranchActive(); }
    void clearPressState();
    void startDrag(const QString &token);
    // Resolve the folder section under a drag at `pos` and update the drop
    // highlight, which is set only when a drop there would change the chat's
    // folder. Returns the section under the pointer whether or not it would
    // (-1 = no section there): the answer dragMoveEvent gives the platform.
    int updateDropTarget(const QPoint &pos, const QString &token);
    void updateAutoScroll();
    void endDragFeedback();
    void onDragScrollTick();
    static qreal unreadBadgeWidth(int count);
    void paintAvatar(QPainter *p, const ConversationLayout &cl, const QRectF &rect);
    void paintUnreadBadge(QPainter *p, int count, bool mention, bool directMention,
                          const QRectF &badgeArea);
    void paintScrollbar(QPainter *p);

    // ── Avatar loading ──
    QImage fetchAvatar(const QString &userId, const QString &token, int convType, int size);
    void requestAvatar(const QString &userId, const QString &token, int convType, int size);
    QString avatarCacheKey(const QString &userId, const QString &token, int convType) const;

    // ── State ──
    ConversationListModel *m_model = nullptr;
    ApiClient *m_api = nullptr;
    SignalingClient *m_signaling = nullptr;
    ShiftStatusService *m_shiftStatus = nullptr;
    bool m_darkMode = true;
    PainterTheme::Theme m_themeId = PainterTheme::Theme::Vivid;
    int m_selectedIndex = -1;
    QString m_selectedToken;
    bool m_squeezed = false;
    QString m_filterText;
    int m_sortMode = SortRecent;
    int m_filterMode = FilterAll;
    QString m_tagFilterId;   // Talk 24; empty = no tag filter
    QString m_tagFilterName; // display name of the above, for the empty state
    qreal m_scrollY = 0;
    int m_hoveredRow = -1;  // visible row index

    PainterTheme m_theme;

    // All conversation layouts (model-order)
    QVector<ConversationLayout> m_layouts;

    // Visible layouts after filtering (indices into m_layouts)
    // The visible list. Holds conversation rows AND section headers (tag
    // headers, or folder headers -- never both kinds at once, the group mode
    // decides), each
    // carrying its own y/height — the sidebar is no longer uniform-row-height,
    // so nothing may compute a position as `index * rowH` any more. Geometry
    // and hit testing live in talq::SidebarRowLayout, which pins that a
    // header-free list still reproduces the old arithmetic exactly.
    std::vector<talq::SidebarRow> m_rows;
    QVector<SidebarTag> m_tags;   // ordered; empty unless the server has tags
    GroupMode m_groupMode = GroupNone;
    // Conversations per section, keyed by tag id. Held separately from m_rows
    // because a COLLAPSED section contributes no rows yet must still show how
    // many conversations are hidden under it.
    QHash<QString, int> m_tagSectionCounts;

    // ── Folders (in-memory copy of the store; see setFolders) ──
    QVector<talq::Folder> m_folders;                // display order
    bool m_otherCollapsed = false;
    QHash<QString, QString> m_tokenToFolderId;      // as pushed in
    // Derived once in setFolders(): token -> index into m_folders, only for
    // assignments whose folder exists. The row build does ONE O(1) lookup per
    // conversation here -- a folder id is never resolved per row per rebuild,
    // and the folder lookup is skipped entirely in every other mode.
    QHash<QString, int> m_tokenToFolderIndex;
    // Per-header unread roll-up, computed in buildFolderRows() from the same
    // members the open folder would show. Indexed by folder index; Other has its
    // own. Cleared at the top of buildVisibleRows().
    QVector<talq::FolderRollup> m_folderRollups;
    talq::FolderRollup m_otherRollup;
    // Indices into m_layouts that survived the filters, in display order (what
    // buildVisibleRows was given). revealToken() asks it whether a row WOULD be
    // visible before it opens a folder for it.
    QVector<int> m_matching;
    // True when no conversation matched, which is exactly when m_rows is empty
    // (folder mode too: it emits no empty-folder headers under the "No
    // conversations ..." panel, where they could be clicked but never seen).
    bool m_listEmpty = true;

    // ── Press / drag state ──
    // Everything used to happen in mouseReleaseEvent from a fresh hit-test; a
    // drag needs to know where and on what the button went DOWN.
    QPoint m_pressPos;
    int m_pressRow = -1;                // visible-row index at press
    Qt::MouseButton m_pressButton = Qt::NoButton;
    QString m_pressToken;               // token of the conversation row pressed (left button only)
    // The next release belongs to a drag that just ended, or to the second click
    // of a double-click on a folder header: it must not open a chat or toggle.
    bool m_swallowRelease = false;
    // The last release landed on a FOLDER header. The second click of a
    // double-click is decided by this, not by hit-testing where it landed: the
    // first click may have closed a folder near the bottom of a scrolled list,
    // clampScroll() then slides a different row under the still pointer, and a
    // hit-test would send the second click to a chat (or another folder).
    bool m_lastReleaseWasFolderHeader = false;
    // QDrag::exec() is running (started from this widget). Hover is meaningless
    // for the whole drag, and a model refresh in the middle must not paint one.
    bool m_dragInProgress = false;
    // Drop highlight: the folder section under the drag (a folder index or
    // kFolderOther), or -1 for none.
    int m_dropSection = -1;
    QPoint m_dragPos;                   // last drag position, widget coordinates
    QString m_dragToken;                // token being dragged over this widget
    QTimer *m_dragScrollTimer = nullptr;

    // ── Image caches ──
    // cacheKey -> pre-masked avatar: a circle, or a rounded square for a group
    // conversation (the shape is chosen when the download finishes).
    QHash<QString, QImage> m_avatarCache;
    // cacheKey -> when the download in flight for it began. The marker is only believed
    // for talq::avatarfetch::kPendingGiveUpMs: a download that never finishes must not
    // freeze its picture for the life of the process.
    QHash<QString, qint64> m_avatarPending;
    QHash<QString, qint64> m_avatarFetchedMs; // cacheKey -> last fetch epoch-ms (TTL)
};
