#pragma once

#include <QWidget>
#include <QVector>
#include <QHash>
#include <QSet>
#include <QImage>
#include <vector>
#include "PainterTheme.h"
#include "SidebarRowLayout.h"

class ConversationListModel;
class ApiClient;
class SignalingClient;
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

    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool dark);
    void setTheme(PainterTheme::Theme t);

    int selectedIndex() const { return m_selectedIndex; }
    void setSelectedIndex(int idx);

    bool squeezed() const { return m_squeezed; }
    void setSqueezed(bool sq);

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
    bool groupByTag() const { return m_groupByTag; }
    void setGroupByTag(bool on);

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

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
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
    void paintSectionHeader(QPainter *p, const talq::SidebarRow &row);
    // Builds m_rows from m_layouts: flat when grouping is off (identical to
    // 0.64 behaviour), grouped into tag sections when it is on.
    void buildVisibleRows(const QVector<int> &matching);
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
    // The visible list. Holds conversation rows AND tag section headers, each
    // carrying its own y/height — the sidebar is no longer uniform-row-height,
    // so nothing may compute a position as `index * rowH` any more. Geometry
    // and hit testing live in talq::SidebarRowLayout, which pins that a
    // header-free list still reproduces the old arithmetic exactly.
    std::vector<talq::SidebarRow> m_rows;
    QVector<SidebarTag> m_tags;   // ordered; empty unless the server has tags
    bool m_groupByTag = false;
    // Conversations per section, keyed by tag id. Held separately from m_rows
    // because a COLLAPSED section contributes no rows yet must still show how
    // many conversations are hidden under it.
    QHash<QString, int> m_tagSectionCounts;

    // ── Image caches ──
    QHash<QString, QImage> m_avatarCache;   // cacheKey -> circular avatar
    QSet<QString> m_avatarPending;          // in-flight avatar requests
    QHash<QString, qint64> m_avatarFetchedMs; // cacheKey -> last fetch epoch-ms (TTL)
};
