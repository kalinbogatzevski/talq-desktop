#pragma once

#include <QQuickPaintedItem>
#include <QVector>
#include <QHash>
#include <QImage>
#include <QSet>
#include "MessageLayout.h"
#include "PainterTheme.h"

class MessageListModel;
class QNetworkReply;

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
    Q_INVOKABLE QString hitTestAt(qreal x, qreal y);
    Q_INVOKABLE QVariantMap messageAt(qreal x, qreal y);  // returns message data for context menu

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
    void linkActivated(const QString &url);
    void fileClicked(int fileId, const QString &fileName);
    void reactionClicked(int messageId, const QString &emoji);

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
    QString hitTestLink(const MessageLayout &ml, const QPointF &localPos) const;
    QString hitTestReaction(const MessageLayout &ml, const QPointF &localPos) const;

    // ── Painting helpers ──
    void paintDateSep(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintSystemMessage(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintOwnMessage(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintOtherMessage(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintReplyQuote(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintFileAttachment(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintReactions(QPainter *p, const MessageLayout &ml, qreal offsetY);

    // ── Image loading ──
    QImage fetchAvatar(const QString &userId);
    QImage fetchFilePreview(int fileId);
    void requestAvatar(const QString &userId);
    void requestFilePreview(int fileId);

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
    bool m_dragMoved = false;  // true if mouse moved >4px during drag
    QPointF m_pressCanvasPos;  // press position in canvas coordinates
    qreal m_dragStartY = 0;
    qreal m_dragStartScroll = 0;

    PainterTheme m_theme;

    // Layouts: index 0 = oldest message (top of view)
    // Model is newest-first, so m_layouts[i] corresponds to model row (rowCount - 1 - i)
    QVector<MessageLayout> m_layouts;

    // ── Image caches ──
    QHash<QString, QImage> m_avatarCache;   // userId -> circular avatar
    QSet<QString> m_avatarPending;          // in-flight avatar requests
    QHash<int, QImage> m_previewCache;      // fileId -> preview image
    QSet<int> m_previewPending;             // in-flight preview requests
};
