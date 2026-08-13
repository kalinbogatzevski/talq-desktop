#pragma once

#include <QWidget>
#include <QVector>
#include <QHash>
#include <QSet>
#include <QImage>
#include "PainterTheme.h"

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
    bool isFavorite = false;
    qint64 lastActivity = 0;
    int notificationLevel = 0;  // 0=default, 1=always, 2=mention, 3=never

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
                      FilterDirect = 3, FilterGroups = 4 };

    int sortMode() const { return m_sortMode; }
    void setSortMode(int mode);
    int filterMode() const { return m_filterMode; }
    void setFilterMode(int mode);

signals:
    void selectedIndexChanged();

    void conversationClicked(const QString &token, const QString &displayName,
                             const QString &participantUserId, int conversationType,
                             const QString &userStatus,
                             const QString &statusMessage,
                             const QString &statusIcon);

    void contextMenuRequested(int modelIndex, int notificationLevel, qreal globalX, qreal globalY);

    void homeRequested();

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
    void paintAvatar(QPainter *p, const ConversationLayout &cl, const QRectF &rect);
    void paintUnreadBadge(QPainter *p, int count, bool mention, const QRectF &badgeArea);
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
    qreal m_scrollY = 0;
    int m_hoveredRow = -1;  // visible row index

    PainterTheme m_theme;

    // All conversation layouts (model-order)
    QVector<ConversationLayout> m_layouts;

    // Visible layouts after filtering (indices into m_layouts)
    QVector<int> m_visibleIndices;

    // ── Image caches ──
    QHash<QString, QImage> m_avatarCache;   // cacheKey -> circular avatar
    QSet<QString> m_avatarPending;          // in-flight avatar requests
    QHash<QString, qint64> m_avatarFetchedMs; // cacheKey -> last fetch epoch-ms (TTL)
};
