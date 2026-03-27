#pragma once

#include <QQuickPaintedItem>
#include <QVector>
#include "MessageLayout.h"
#include "PainterTheme.h"

class MessageListModel;

/**
 * QQuickPaintedItem that renders the entire chat message list
 * using QPainter. Replaces QML ListView + MessageBubble delegates.
 *
 * Phases:
 *  1. Skeleton — compiles, shows background
 *  2. Layout engine — computes message positions
 *  3. Basic painting — renders text, bubbles, avatars, timestamps
 */
class ChatPainter : public QQuickPaintedItem
{
    Q_OBJECT

    Q_PROPERTY(QObject* model READ modelObject WRITE setModelObject NOTIFY modelChanged)
    Q_PROPERTY(QString myUserId READ myUserId WRITE setMyUserId NOTIFY myUserIdChanged)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    Q_PROPERTY(qreal fontScale READ fontScale WRITE setFontScale NOTIFY fontScaleChanged)
    Q_PROPERTY(bool atBottom READ atBottom NOTIFY atBottomChanged)
    Q_PROPERTY(qreal scrollY READ scrollY WRITE setScrollY NOTIFY scrollYChanged)
    Q_PROPERTY(qreal contentHeight READ contentHeight NOTIFY contentHeightChanged)
    Q_PROPERTY(qreal visibleHeight READ visibleHeight NOTIFY visibleHeightChanged)
    Q_PROPERTY(int hoveredIndex READ hoveredIndex NOTIFY hoveredIndexChanged)

public:
    explicit ChatPainter(QQuickItem *parent = nullptr);

    void paint(QPainter *painter) override;

    // ── Property accessors ──
    QObject *modelObject() const;
    void setModelObject(QObject *obj);

    QString myUserId() const { return m_myUserId; }
    void setMyUserId(const QString &id);

    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool dark);

    qreal fontScale() const { return m_fontScale; }
    void setFontScale(qreal scale);

    bool atBottom() const;
    qreal scrollY() const { return m_scrollY; }
    void setScrollY(qreal y);

    qreal contentHeight() const { return m_contentHeight; }
    qreal visibleHeight() const { return height(); }

    int hoveredIndex() const { return m_hoveredIndex; }

    Q_INVOKABLE void scrollToBottom();

signals:
    void modelChanged();
    void myUserIdChanged();
    void darkModeChanged();
    void fontScaleChanged();
    void atBottomChanged();
    void scrollYChanged();
    void contentHeightChanged();
    void visibleHeightChanged();
    void hoveredIndexChanged();

protected:
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void geometryChange(const QRectF &newGeom, const QRectF &oldGeom) override;

private slots:
    void onRowsInserted(const QModelIndex &parent, int first, int last);
    void onRowsRemoved(const QModelIndex &parent, int first, int last);
    void onDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight);
    void onModelReset();

private:
    void rebuildAllLayouts();
    void clampScroll();
    int layoutIndexAtY(qreal viewportY) const;

    // ── Painting helpers ──
    void paintDateSep(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintSystemMessage(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintOwnMessage(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintOtherMessage(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintReplyQuote(QPainter *p, const MessageLayout &ml, qreal offsetY);

    // ── State ──
    MessageListModel *m_model = nullptr;
    QString m_myUserId;
    bool m_darkMode = true;
    qreal m_fontScale = 1.0;
    qreal m_scrollY = 0;
    qreal m_contentHeight = 0;
    int m_hoveredIndex = -1;

    // Mouse drag state
    bool m_dragging = false;
    qreal m_dragStartY = 0;
    qreal m_dragStartScroll = 0;

    PainterTheme m_theme;

    // Layouts: index 0 = oldest message (top of view)
    // Model is newest-first, so m_layouts[i] corresponds to model row (rowCount - 1 - i)
    QVector<MessageLayout> m_layouts;
};
