#pragma once

#include <QWidget>
#include <QVector>
#include <QHash>
#include <QImage>
#include <QSet>
#include "MessageLayout.h"
#include "PainterTheme.h"

class MessageListModel;
class QNetworkReply;

/**
 * QWidget that renders the entire chat message list using QPainter.
 */
class ChatPainter : public QWidget
{
    Q_OBJECT

public:
    explicit ChatPainter(QWidget *parent = nullptr);

    // ── Property accessors ──
    void setModel(MessageListModel *model);
    MessageListModel *model() const { return m_model; }

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

    void scrollToBottom();
    QString hitTestAt(qreal x, qreal y);
    QVariantMap messageAt(qreal x, qreal y);
    void setHoveredPos(qreal x, qreal y);
    QImage cachedPreview(int fileId) const { return m_previewCache.value(fileId); }

signals:
    void atBottomChanged();
    void scrollYChanged();
    void contentHeightChanged();
    void visibleHeightChanged();
    void hoveredIndexChanged();
    void linkActivated(const QString &url);
    void fileClicked(int fileId, const QString &mime, const QString &fileName);
    void reactionClicked(int messageId, const QString &emoji);
    void replyRequested(int messageId, const QString &author, const QString &text);
    void reactRequested(int messageId);
    void contextMenuRequested(const QVariantMap &msgData, const QPoint &globalPos);
    void fileDropped(const QString &filePath);

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool event(QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

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
    void paintHoverBar(QPainter *p, const MessageLayout &ml, qreal offsetY);

    // ── Hover bar geometry ──
    QRectF hoverBarReactRect(const MessageLayout &ml) const;
    QRectF hoverBarReplyRect(const MessageLayout &ml) const;

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
    QHash<int, qreal> m_previewAspect;     // fileId -> height/width ratio (0 = unknown)
    QSet<int> m_previewPending;            // in-flight preview requests
};
