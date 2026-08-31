#pragma once

#include <QWidget>
#include <QVector>
#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QSet>
#include <QFont>
#include "MessageLayout.h"
#include "PainterTheme.h"
#include "painter/ReactionLayout.h"

/**
 * Character-level text selection state, spanning one or more messages.
 * Anchor = where the mouse was pressed. Active = where the mouse is now.
 */
struct TextSelection {
    int anchorLayoutIdx = -1;
    int anchorCursorPos = 0;
    int activeLayoutIdx = -1;
    int activeCursorPos = 0;
    bool active = false;

    bool hasSelection() const {
        return active && !(anchorLayoutIdx == activeLayoutIdx
                           && anchorCursorPos == activeCursorPos);
    }
    void clear() {
        anchorLayoutIdx = activeLayoutIdx = -1;
        anchorCursorPos = activeCursorPos = 0;
        active = false;
    }

    struct Range { int startIdx, startPos, endIdx, endPos; };
    Range normalized() const {
        if (anchorLayoutIdx < activeLayoutIdx
            || (anchorLayoutIdx == activeLayoutIdx && anchorCursorPos <= activeCursorPos))
            return {anchorLayoutIdx, anchorCursorPos, activeLayoutIdx, activeCursorPos};
        return {activeLayoutIdx, activeCursorPos, anchorLayoutIdx, anchorCursorPos};
    }
};

class MessageListModel;
class SignalingClient;
class QNetworkReply;

/**
 * QWidget that renders the entire chat message list using QPainter.
 */
class ChatPainter : public QWidget
{
    Q_OBJECT

public:
    // 0.65.3 - whether to show READ status on own messages. Talk's guidance is
    // that the read indicator is reciprocal: a user who has turned off
    // "share my read status" server-side (config.chat.read-privacy = 1) should
    // not be shown other people's either. TalQ both broadcast and displayed it
    // unconditionally before it could read the setting. When false, own
    // messages still show DELIVERED, so the user keeps the confirmation that
    // the message left the client -- only the read distinction is withheld.
    void setShowReadStatus(bool show) { if (m_showReadStatus == show) return;
                                        m_showReadStatus = show; update(); }

    explicit ChatPainter(QWidget *parent = nullptr);

    // ── Property accessors ──
    void setModel(MessageListModel *model);
    MessageListModel *model() const { return m_model; }
    void setSignaling(SignalingClient *signaling);

    QString myUserId() const { return m_myUserId; }
    void setMyUserId(const QString &id);

    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool dark);
    void setTheme(PainterTheme::Theme t);

    qreal fontScale() const { return m_fontScale; }
    void setFontScale(qreal scale);

    bool atBottom() const;
    qreal scrollY() const { return m_scrollY; }
    void setScrollY(qreal y);

    qreal contentHeight() const { return m_contentHeight; }
    qreal visibleHeight() const { return height(); }

    int hoveredIndex() const { return m_hoveredIndex; }

    void scrollToBottom();
    // bug 1 — pin the view to the bottom across the open-time load/reset storm.
    // Set on conversation open; every rebuildAllLayouts then re-lands at the
    // true bottom (immune to the transient m_scrollY churn from the multiple
    // cache-load/refreshLatest resets), so messages that arrive ~1s after open
    // via the poller are visible without a manual re-open. Auto-cleared by
    // setScrollY the moment a scroll lands off-bottom (i.e. a genuine user
    // scroll-up), so it never traps the user away from history.
    void pinToBottom();
    QString hitTestAt(qreal x, qreal y);
    QVariantMap messageAt(qreal x, qreal y);
    void setHoveredPos(qreal x, qreal y);
    QImage cachedPreview(int fileId) const { return m_previewCache.value(fileId); }

    // ── Cache stats (for DebugMonitor) ──
    int avatarCacheCount() const { return m_avatarCache.size(); }
    qint64 avatarCacheBytes() const;
    // Mark every cached avatar stale so the next paint re-fetches it in the
    // background (the old image stays on screen until the new one arrives).
    // Called when the window regains focus so an avatar changed elsewhere
    // shows up without a restart.
    void invalidateAvatars() { m_avatarFetchedMs.clear(); update(); }
    int previewCacheCount() const { return m_previewCache.size(); }
    qint64 previewCacheBytes() const;
    int layoutCacheCount() const { return m_layoutCache.size(); }
    qint64 layoutCacheBytes() const;

    // ── Selection ──
    bool selectionMode() const { return m_selectionMode; }
    QSet<int> selectedIds() const { return m_selectedIds; }
    void enterSelectionMode(int firstMessageId);
    void exitSelectionMode();
    void toggleMessageSelection(int messageId);
    bool allSelectedOwn() const;
    QVector<QVariantMap> selectedMessages() const;

public slots:
    void setUnreadBoundary(int id);
    void dismissUnreadSeparator();
    void scrollToMessage(int messageId);

signals:
    void atBottomChanged();
    void scrollYChanged();
    void contentHeightChanged();
    void visibleHeightChanged();
    void hoveredIndexChanged();
    void linkActivated(const QString &url);
    void fileClicked(int fileId, const QString &mime, const QString &fileName);
    // A poll card was clicked; the window opens the voting dialog.
    // The quote block on a reply was clicked: jump to the message it quotes.
    void quotedMessageClicked(int parentMessageId);
    void pollClicked(int pollId);
    void reactionClicked(int messageId, const QString &emoji);
    void replyRequested(int messageId, const QString &author, const QString &text);
    void reactRequested(int messageId, const QPoint &globalPos);
    // A message author's avatar was clicked; the window opens the person card.
    // The anchor is the avatar's global rect, so the card can point at the
    // thing that was clicked rather than at the cursor.
    void avatarClicked(const QString &actorId, const QString &actorName,
                       const QRect &anchorGlobal);
    void contextMenuRequested(const QVariantMap &msgData, const QPoint &globalPos);
    void fileDropped(const QString &filePath);
    void selectionModeChanged(bool active);
    void selectionChanged(int count);
    void moreHistoryRequested();
    // Fired when the user has effectively "seen" the unread region — either
    // the dismiss timer expired with the divider above the viewport, or the
    // user interacted with the composer / sent a message. Listeners should
    // advance the server-side read marker so the divider doesn't reappear
    // on the next conversation switch.
    void unreadSeparatorDismissed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool event(QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void onRowsInserted(const QModelIndex &parent, int first, int last);
    void onRowsRemoved(const QModelIndex &parent, int first, int last);
    void onDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                       const QList<int> &roles);
    void onModelReset();

private:
    bool m_showReadStatus = true;

    void rebuildAllLayouts();
    void clampScroll();
    int layoutIndexAtY(qreal viewportY) const;
    QString hitTestLink(const MessageLayout &ml, const QPointF &localPos) const;
    QString hitTestReaction(const MessageLayout &ml, const QPointF &localPos) const;
    int hitTestBodyCursor(const MessageLayout &ml, const QPointF &canvasPos) const;

    // ── Reaction pill geometry (single source for paintReactions AND
    // hitTestReaction — see ReactionLayout.h for why this must not fork) ──
    talq::ReactionLayoutParams reactionLayoutParams(const QRectF &barRect) const;
    struct ReactionFonts { QFont emoji, count; };
    ReactionFonts reactionFonts() const;
    bool isOnBodyText(const QPointF &canvasPos, int layoutIdx) const;
    void copySelectedText();
    QVariantMap variantMapFromLayout(const MessageLayout &ml) const;

    // ── Painting helpers ──
    void paintDateSep(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintUnreadSep(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintSystemMessage(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintOwnMessage(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintOtherMessage(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintReplyQuote(QPainter *p, const MessageLayout &ml, qreal offsetY);
    // Poll card. Non-interactive by design - the dialog does the voting.
    void paintPollCard(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintFileAttachment(QPainter *p, const MessageLayout &ml, qreal offsetY);
    // Timestamp floated over the bottom-right of an edge-to-edge image bubble,
    // on a dark scrim so it stays legible over any photo.
    void paintImageTimeOverlay(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintReactions(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void paintHoverBar(QPainter *p, const MessageLayout &ml, qreal offsetY);
    void drawEmoji(QPainter *p, const QString &codepoints, const QRectF &rect);
    void paintMessageEmojis(QPainter *p, const MessageLayout &ml, qreal offsetY);

    // ── Hover bar geometry ──
    QRectF hoverBarReactRect(const MessageLayout &ml) const;
    QRectF hoverBarReplyRect(const MessageLayout &ml) const;

    // ── Image loading ──
    QImage fetchAvatar(const QString &userId);
    QImage fetchFilePreview(int fileId);
    void requestAvatar(const QString &userId);
    // isRetry=true is ONLY for requestFilePreview's own backoff-timer
    // continuation (see the QTimer::singleShot call in the .cpp). It bypasses
    // BOTH the m_previewPending "already in flight" guard and the initial
    // model/api null-check, so that the retry can re-enter without tripping
    // the guard meant for external callers. A new call site passing true
    // would silently reintroduce the duplicate-request race this parameter
    // exists to close — external callers (fetchFilePreview) must always use
    // the default isRetry=false.
    void requestFilePreview(int fileId, bool isRetry = false);
    void evictPreviewCache();

    // ── State ──
    MessageListModel *m_model = nullptr;
    SignalingClient *m_signaling = nullptr;
    QString m_myUserId;
    int m_unreadBoundary = 0;
    bool m_unreadSepDismissed = false;
    class QTimer *m_unreadSepDismissTimer = nullptr;   // single-shot 2s

    int m_highlightMessageId = 0;
    class QTimer *m_highlightTimer = nullptr;

    bool m_darkMode = true;
    PainterTheme::Theme m_themeId = PainterTheme::Theme::Vivid;
    qreal m_fontScale = 1.0;
    qreal m_scrollY = 0;
    bool  m_forcePinBottom = false;   // bug 1 — keep view at bottom across open-time reset storm
    qreal m_contentHeight = 0;
    int m_hoveredIndex = -1;

    // Mouse drag state
    bool m_dragging = false;
    bool m_dragMoved = false;
    bool m_draggingScrollbar = false;
    qreal m_scrollbarThumbOffset = 0;   // Y offset from thumb top at press
    QString m_pressHit;  // hit test result saved at press time  // true if mouse moved >4px during drag
    QPointF m_pressCanvasPos;  // press position in canvas coordinates
    qreal m_dragStartY = 0;
    qreal m_dragStartScroll = 0;

    PainterTheme m_theme;

    // Cached background (bgPrimary + ambient glow) so the per-theme radial
    // gradient is rasterised once per size/theme change, not every paint.
    QPixmap m_ambientCache;

    // Layouts: index 0 = oldest message (top of view)
    // Model is newest-first, so m_layouts[i] corresponds to model row (rowCount - 1 - i)
    QVector<MessageLayout> m_layouts;

    // Per-message layout cache — messageId -> (cacheKey, layout). On rebuild,
    // a row whose key matches the cached entry is reused (with y-translation)
    // instead of re-running QTextDocument::setHtml + grapheme scan. Cleared
    // when width/theme/font changes invalidate every entry.
    QHash<int, QPair<QString, MessageLayout>> m_layoutCache;

    // Resize debounce — avoid full rebuild on every pixel of a window drag.
    class QTimer *m_resizeDebounceTimer = nullptr;
    qreal m_lastWidth = 0;

    // ── Chat-history load state ──
    // Tracks MessageListModel::isLoading() only to pick the empty-body text
    // (blank while loading vs "No messages yet" when done). The ANIMATED
    // loading line itself now lives in the header (HeaderPainter), wired from
    // the same loadingChanged signal in MainWindow — so there is no spinner in
    // the chat body any more.
    void updateLoadingState();
    bool m_modelLoading = false;

    // ── Image caches ──
    QHash<QString, QImage> m_avatarCache;   // userId -> circular avatar
    QSet<QString> m_avatarPending;          // in-flight avatar requests
    QHash<QString, qint64> m_avatarFetchedMs; // userId -> last fetch epoch-ms (TTL)
    QHash<int, QImage> m_previewCache;      // fileId -> preview image
    QHash<int, qreal> m_previewAspect;     // fileId -> height/width ratio (0 = unknown)
    QSet<int> m_previewPending;            // in-flight preview requests
    QHash<int, int> m_previewAttempts;     // fileId -> retry count (NC generates previews async)

    // ── Selection state ──
    bool m_selectionMode = false;
    QSet<int> m_selectedIds;

    // ── Text selection state (character-level, Telegram-style) ──
    TextSelection m_textSelection;
    bool m_textAnchorSet = false;  // true if mousePress landed on body text
    bool m_wordSelectMode = false; // true after double-click (extend by whole words)
    int m_wordAnchorStart = 0;     // original double-clicked word start
    int m_wordAnchorEnd = 0;       // original double-clicked word end
};
