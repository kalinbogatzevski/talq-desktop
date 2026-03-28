#pragma once

#include <QQuickPaintedItem>
#include <QVector>
#include <QHash>
#include <QSet>
#include <QImage>
#include "PainterTheme.h"

class ConversationListModel;
class ApiClient;
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
};

/**
 * QQuickPaintedItem that renders the conversation sidebar using QPainter.
 * Replaces QML ListView + ConversationItem delegates.
 *
 * Features:
 *  - Scrollable list with hover/selection highlighting
 *  - Avatar (circular), display name, last message preview, timestamp
 *  - Unread badge, mute indicator, favorite dot, online status
 *  - Squeeze mode (avatar-only, 56px wide)
 *  - Search filtering
 *  - Right-click context menu (mute/unmute via signal)
 */
class SidebarPainter : public QQuickPaintedItem
{
    Q_OBJECT

    Q_PROPERTY(QObject* model READ modelObject WRITE setModelObject NOTIFY modelChanged)
    Q_PROPERTY(QObject* api READ apiObject WRITE setApiObject NOTIFY apiChanged)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex WRITE setSelectedIndex NOTIFY selectedIndexChanged)
    Q_PROPERTY(bool squeezed READ squeezed WRITE setSqueezed NOTIFY squeezedChanged)
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)

public:
    explicit SidebarPainter(QQuickItem *parent = nullptr);

    void paint(QPainter *painter) override;

    // ── Property accessors ──
    QObject *modelObject() const;
    void setModelObject(QObject *obj);

    QObject *apiObject() const;
    void setApiObject(QObject *obj);

    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool dark);

    int selectedIndex() const { return m_selectedIndex; }
    void setSelectedIndex(int idx);

    bool squeezed() const { return m_squeezed; }
    void setSqueezed(bool sq);

    QString filterText() const { return m_filterText; }
    void setFilterText(const QString &text);

signals:
    void modelChanged();
    void apiChanged();
    void darkModeChanged();
    void selectedIndexChanged();
    void squeezedChanged();
    void filterTextChanged();

    /**
     * Emitted when the user clicks a conversation row.
     * Parameters match ConversationList.qml's conversationSelected signal.
     */
    void conversationClicked(const QString &token, const QString &displayName,
                             const QString &participantUserId, int conversationType,
                             const QString &userStatus);

    /**
     * Emitted on right-click for context menu (mute/unmute).
     * QML handles the actual Menu popup.
     */
    void contextMenuRequested(int modelIndex, int notificationLevel, qreal globalX, qreal globalY);

protected:
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void geometryChange(const QRectF &newGeom, const QRectF &oldGeom) override;

private slots:
    void onDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight);
    void onModelReset();
    void onRowsInserted(const QModelIndex &parent, int first, int last);
    void onRowsRemoved(const QModelIndex &parent, int first, int last);

private:
    // ── Layout ──
    static constexpr int RowHeight = 76;       // matches Theme.conversationHeight
    static constexpr int RowHeightSqueezed = 52;
    static constexpr int AvatarSize = 44;      // matches Theme.avatarSize
    static constexpr int AvatarSizeSqueezed = 40;
    static constexpr int BadgeHeight = 18;
    static constexpr int BadgeFontSize = 10;
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
    bool m_darkMode = true;
    int m_selectedIndex = -1;
    bool m_squeezed = false;
    QString m_filterText;
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
};
