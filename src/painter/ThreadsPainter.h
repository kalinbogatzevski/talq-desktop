#pragma once

#include <QQuickPaintedItem>
#include <QVector>
#include "PainterTheme.h"

class ThreadListModel;

/**
 * Pre-computed layout for a single thread row.
 */
struct ThreadLayout {
    int threadId = 0;
    QString title;
    QString lastMessage;
    QString lastAuthor;
    qint64 lastActivity = 0;
    int replyCount = 0;
    int iconColor = 0;
    int unreadCount = 0;
    bool isAllMessages = false;

    // Computed
    QString timeString;
    QString previewText;   // "author: message"
    QColor threadColor;
    qreal y = 0;
};

/**
 * QQuickPaintedItem that renders the thread/topics list using QPainter.
 * Replaces QML ThreadListView + ThreadItem delegates.
 *
 * Features:
 *  - Scrollable list with hover/selection highlighting
 *  - Colored dot + title + last message preview + timestamp
 *  - Unread badge with topic color
 *  - Selection bar with topic color tint
 *  - "New Topic" button at bottom
 *  - Empty state
 *  - Header with back button and group name
 */
class ThreadsPainter : public QQuickPaintedItem
{
    Q_OBJECT

    Q_PROPERTY(QObject* model READ modelObject WRITE setModelObject NOTIFY modelChanged)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    Q_PROPERTY(int selectedThreadId READ selectedThreadId WRITE setSelectedThreadId NOTIFY selectedThreadIdChanged)
    Q_PROPERTY(QString groupName READ groupName WRITE setGroupName NOTIFY groupNameChanged)
    Q_PROPERTY(bool creating READ creating WRITE setCreating NOTIFY creatingChanged)

public:
    explicit ThreadsPainter(QQuickItem *parent = nullptr);

    void paint(QPainter *painter) override;

    // Property accessors
    QObject *modelObject() const;
    void setModelObject(QObject *obj);

    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool dark);

    int selectedThreadId() const { return m_selectedThreadId; }
    void setSelectedThreadId(int id);

    QString groupName() const { return m_groupName; }
    void setGroupName(const QString &name);

    bool creating() const { return m_creating; }
    void setCreating(bool c);

signals:
    void modelChanged();
    void darkModeChanged();
    void selectedThreadIdChanged();
    void groupNameChanged();
    void creatingChanged();

    void threadClicked(int threadId, const QString &title);
    void backClicked();
    void newTopicClicked();

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
    // Layout constants
    static constexpr int HeaderHeight = 54;
    static constexpr int RowHeight = 64;
    static constexpr int DotSize = 8;
    static constexpr int BadgeHeight = 18;
    static constexpr int BadgeFontSize = 10;
    static constexpr int SelectionBarWidth = 3;
    static constexpr int NewTopicHeight = 44;

    void rebuildLayouts();
    void clampScroll();
    int rowAtY(qreal viewportY) const;  // returns row index, or -1
    qreal listTop() const { return HeaderHeight; }
    qreal listBottom() const { return height() - NewTopicHeight; }
    bool isInNewTopicButton(qreal y) const;
    bool isInBackButton(qreal x, qreal y) const;

    // Painting helpers
    void paintHeader(QPainter *p);
    void paintRow(QPainter *p, const ThreadLayout &tl, int visibleIdx);
    void paintUnreadBadge(QPainter *p, int count, const QColor &color, const QRectF &badgeArea);
    void paintNewTopicButton(QPainter *p);
    void paintEmptyState(QPainter *p);
    void paintScrollbar(QPainter *p);

    // Topic color from index
    static QColor topicColor(int index);

    // State
    ThreadListModel *m_model = nullptr;
    bool m_darkMode = true;
    int m_selectedThreadId = -1;
    QString m_groupName;
    bool m_creating = false;
    qreal m_scrollY = 0;
    int m_hoveredRow = -1;     // -1 = none, -2 = new topic button, -3 = back button
    bool m_modelLoading = false;

    PainterTheme m_theme;

    QVector<ThreadLayout> m_layouts;
};
