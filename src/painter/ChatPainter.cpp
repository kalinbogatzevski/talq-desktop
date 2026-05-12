#include "ChatPainter.h"
#include "LayoutEngine.h"
#include "models/MessageListModel.h"
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QFile>
#include <QTextStream>
#include <QTextDocument>
#include <QAbstractTextDocumentLayout>
#include <QRegularExpression>
#include <QNetworkReply>
#include <QTimer>
#include <QtMath>
#include <QApplication>
#include <QClipboard>
#include <QTextCursor>
#include <QMenu>
#include "core/ApiClient.h"
#include "core/EmojiData.h"
#include <QTextBlock>
#include <QTextLayout>
#include <QTextLine>
#include <QFontMetricsF>
#include <QDateTime>
#include <QHelpEvent>
#include <QToolTip>
#include <QElapsedTimer>
#include "core/TalqLog.h"

ChatPainter::ChatPainter(QWidget *parent)
    : QWidget(parent)
    , m_theme(m_darkMode, m_fontScale)
{
    setAttribute(Qt::WA_Hover);
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::ClickFocus);

    m_unreadSepDismissTimer = new QTimer(this);
    m_unreadSepDismissTimer->setSingleShot(true);
    m_unreadSepDismissTimer->setInterval(2000);
    connect(m_unreadSepDismissTimer, &QTimer::timeout, this, [this]() {
        m_unreadSepDismissed = true;
        rebuildAllLayouts();
        emit unreadSeparatorDismissed();
    });

    m_highlightTimer = new QTimer(this);
    m_highlightTimer->setSingleShot(true);
    m_highlightTimer->setInterval(2000);
    connect(m_highlightTimer, &QTimer::timeout, this, [this]() {
        m_highlightMessageId = 0;
        update();
    });

    // Coalesce resize-driven rebuilds — window drags fire resizeEvent every
    // pixel. 50ms idle is below human "delay" threshold but well above the
    // ~16ms drag-event cadence.
    m_resizeDebounceTimer = new QTimer(this);
    m_resizeDebounceTimer->setSingleShot(true);
    m_resizeDebounceTimer->setInterval(50);
    connect(m_resizeDebounceTimer, &QTimer::timeout,
            this, &ChatPainter::rebuildAllLayouts);
}

// ═══════════════════════════════════════════════════════
// Properties
// ═══════════════════════════════════════════════════════

void ChatPainter::setModel(MessageListModel *mdl)
{
    if (mdl == m_model)
        return;

    if (m_selectionMode)
        exitSelectionMode();

    // Disconnect old model
    if (m_model) {
        disconnect(m_model, nullptr, this, nullptr);
    }

    m_model = mdl;

    // Clear file preview caches on model switch (avatars persist across conversations)
    m_previewCache.clear();
    m_previewPending.clear();
    m_previewAspect.clear();
    m_layoutCache.clear();

    // Connect new model
    if (m_model) {
        connect(m_model, &QAbstractItemModel::rowsInserted, this, &ChatPainter::onRowsInserted);
        connect(m_model, &QAbstractItemModel::rowsRemoved, this, &ChatPainter::onRowsRemoved);
        connect(m_model, &QAbstractItemModel::dataChanged, this, &ChatPainter::onDataChanged);
        connect(m_model, &QAbstractItemModel::modelReset, this, &ChatPainter::onModelReset);
        connect(m_model, &MessageListModel::unreadBoundaryChanged, this, [this]() {
            setUnreadBoundary(m_model->unreadBoundary());
        });
        setUnreadBoundary(m_model->unreadBoundary());  // initial pull
    }

    rebuildAllLayouts();
}

void ChatPainter::setMyUserId(const QString &id)
{
    if (m_myUserId == id) return;
    m_myUserId = id;
    m_layoutCache.clear();          // isOwn flips → all rows invalid
    rebuildAllLayouts();
}

void ChatPainter::setUnreadBoundary(int id)
{
    if (m_unreadBoundary == id) return;
    m_unreadBoundary = id;
    m_unreadSepDismissed = false;
    if (m_unreadSepDismissTimer) m_unreadSepDismissTimer->stop();
    m_layoutCache.clear();          // unread-sep row identity changes
    rebuildAllLayouts();
}

void ChatPainter::dismissUnreadSeparator()
{
    if (m_unreadSepDismissed) return;
    m_unreadSepDismissed = true;
    if (m_unreadSepDismissTimer) m_unreadSepDismissTimer->stop();
    m_layoutCache.clear();
    rebuildAllLayouts();
    emit unreadSeparatorDismissed();
}

void ChatPainter::scrollToMessage(int messageId)
{
    for (const auto &ml : m_layouts) {
        if (ml.messageId != messageId) continue;
        qreal target = ml.totalY + ml.totalHeight / 2.0 - height() / 2.0;
        setScrollY(qBound(0.0, target, qMax(0.0, m_contentHeight - qreal(height()))));
        m_highlightMessageId = messageId;
        m_highlightTimer->start();
        update();
        return;
    }
}

void ChatPainter::setDarkMode(bool dark)
{
    if (m_darkMode == dark) return;
    m_darkMode = dark;
    m_theme = PainterTheme(m_darkMode, m_fontScale);
    m_layoutCache.clear();          // colors only affect paint, but bodyDoc fonts are theme-bound
    rebuildAllLayouts();
}

void ChatPainter::setFontScale(qreal scale)
{
    if (qFuzzyCompare(m_fontScale, scale)) return;
    m_fontScale = scale;
    m_theme = PainterTheme(m_darkMode, m_fontScale);
    m_layoutCache.clear();
    rebuildAllLayouts();
}

bool ChatPainter::atBottom() const
{
    return m_contentHeight <= height() || m_scrollY >= m_contentHeight - height() - 5;
}

void ChatPainter::setScrollY(qreal y)
{
    bool wasAtBottom = atBottom();
    m_scrollY = y;
    clampScroll();
    if (wasAtBottom != atBottom())
        emit atBottomChanged();
    emit scrollYChanged();

    // Near the top (oldest visible): request more history.
    // Debounced implicitly because MessageListModel ignores re-entrant loads while m_loading.
    if (m_scrollY < 200.0 && m_contentHeight > height())
        emit moreHistoryRequested();

    // Unread divider auto-clear: if the separator has scrolled above the
    // viewport top, start a 2-second dismissal timer. If the user scrolls
    // back before it fires, cancel.
    if (!m_unreadSepDismissed && m_unreadSepDismissTimer) {
        qreal sepBottom = -1;
        for (const auto &ml : m_layouts) {
            if (ml.showUnreadSep) {
                sepBottom = ml.unreadSepRect.bottom();
                break;
            }
        }
        if (sepBottom > 0) {
            bool aboveViewport = sepBottom < m_scrollY;
            if (aboveViewport && !m_unreadSepDismissTimer->isActive())
                m_unreadSepDismissTimer->start();
            else if (!aboveViewport && m_unreadSepDismissTimer->isActive())
                m_unreadSepDismissTimer->stop();
        }
    }

    update();
}

void ChatPainter::scrollToBottom()
{
    setScrollY(qMax(0.0, m_contentHeight - height()));
}

void ChatPainter::enterSelectionMode(int firstMessageId)
{
    if (m_selectionMode) return;
    m_selectionMode = true;
    m_selectedIds.clear();
    m_selectedIds.insert(firstMessageId);
    m_hoveredIndex = -1;
    emit selectionModeChanged(true);
    emit selectionChanged(1);
    update();
}

void ChatPainter::exitSelectionMode()
{
    if (!m_selectionMode) return;
    m_selectionMode = false;
    m_selectedIds.clear();
    emit selectionModeChanged(false);
    emit selectionChanged(0);
    update();
}

void ChatPainter::toggleMessageSelection(int messageId)
{
    if (m_selectedIds.contains(messageId))
        m_selectedIds.remove(messageId);
    else
        m_selectedIds.insert(messageId);

    if (m_selectedIds.isEmpty()) {
        exitSelectionMode();
        return;
    }

    emit selectionChanged(m_selectedIds.size());
    update();
}

bool ChatPainter::allSelectedOwn() const
{
    for (const auto &ml : m_layouts) {
        if (m_selectedIds.contains(ml.messageId) && !ml.isOwn)
            return false;
    }
    return !m_selectedIds.isEmpty();
}

QVector<QVariantMap> ChatPainter::selectedMessages() const
{
    QVector<QVariantMap> result;
    for (const auto &ml : m_layouts) {
        if (m_selectedIds.contains(ml.messageId))
            result.append(variantMapFromLayout(ml));
    }
    return result;
}

QVariantMap ChatPainter::messageAt(qreal x, qreal y)
{
    qreal canvasY = y + m_scrollY;
    int idx = layoutIndexAtY(canvasY);
    if (idx < 0 || idx >= m_layouts.size()) return {};
    return variantMapFromLayout(m_layouts[idx]);
}

QVariantMap ChatPainter::variantMapFromLayout(const MessageLayout &ml) const
{
    return {
        {"messageId", ml.messageId},
        {"isOwn", ml.isOwn},
        {"actorName", ml.actorName},
        {"messageText", ml.bodyHtml},
        {"timeString", ml.timeString},
        {"hasFile", ml.hasFile},
        {"fileId", ml.fileId},
        {"fileName", ml.fileName},
        {"fileMime", ml.fileMime},
    };
}

QString ChatPainter::hitTestAt(qreal x, qreal y)
{
    // All rects in m_layouts are canvas-absolute (y starts at startY which is cumulative)
    QPointF canvasPos(x, y + m_scrollY);
    int idx = layoutIndexAtY(canvasPos.y());
    if (idx < 0 || idx >= m_layouts.size()) return {};

    const auto &ml = m_layouts[idx];

    // Hover bar buttons — check regardless of hover index (hover may clear during click)
    if (!ml.isSystem && ml.sendStatus != QLatin1String("sending")
        && ml.sendStatus != QLatin1String("failed") && ml.messageId > 0) {
        QRectF replyR = hoverBarReplyRect(ml);
        if (!ml.isOwn) {
            QRectF reactR = hoverBarReactRect(ml);
            if (reactR.contains(canvasPos))
                return QStringLiteral("react:%1").arg(ml.messageId);
        }
        if (replyR.contains(canvasPos))
            return QStringLiteral("reply:%1").arg(ml.messageId);
    }

    // Link
    QString link = hitTestLink(ml, canvasPos);
    if (!link.isEmpty()) return QStringLiteral("link:") + link;

    // File — include MIME for image detection
    if (ml.hasFile && !ml.fileRect.isNull() && ml.fileRect.contains(canvasPos))
        return QStringLiteral("file:%1:%2:%3").arg(ml.fileId).arg(ml.fileMime, ml.fileName);

    // Reaction
    if (!ml.reactions.isEmpty() && !ml.reactBarRect.isNull()) {
        QString emoji = hitTestReaction(ml, canvasPos);
        if (!emoji.isEmpty())
            return QStringLiteral("reaction:%1:%2").arg(ml.messageId).arg(emoji);
    }

    return {};
}

void ChatPainter::setHoveredPos(qreal x, qreal y)
{
    QPointF canvasPos(x, y + m_scrollY);
    int idx = layoutIndexAtY(canvasPos.y());

    // Don't hover system messages or sending/failed messages
    if (idx >= 0 && idx < m_layouts.size()) {
        const auto &ml = m_layouts[idx];
        if (ml.isSystem || ml.sendStatus == QLatin1String("sending")
            || ml.sendStatus == QLatin1String("failed"))
            idx = -1;
    }

    if (idx != m_hoveredIndex) {
        m_hoveredIndex = idx;
        emit hoveredIndexChanged();
        update();
    }
}

// ═══════════════════════════════════════════════════════
// Layout rebuild
// ═══════════════════════════════════════════════════════

// Build a per-message cache key from every input that affects this message's
// computed layout. If two rebuilds produce the same key for a given messageId,
// the cached MessageLayout can be reused (with a y-translation) instead of
// re-running QTextDocument::setHtml + grapheme scan.
static QString makeLayoutCacheKey(QAbstractListModel *model, int modelRow,
                                  qreal width, qreal fontScale, bool darkMode,
                                  const QString &myUserId,
                                  const QString &prevActorId, qint64 prevTimestamp,
                                  bool prevIsSystem, qreal aspect)
{
    auto idx = model->index(modelRow);
    auto get = [&](int role) { return model->data(idx, role); };

    // Note: IsReadRole and SendStatusRole are intentionally absent — they
    // don't affect any rect or text geometry, only paint colors/icons.
    return QString::number(qint64(width * 10)) + '|'
        + QString::number(qint64(fontScale * 1000)) + '|'
        + (darkMode ? '1' : '0') + '|'
        + myUserId + '|'
        + prevActorId + '|'
        + QString::number(prevTimestamp) + '|'
        + (prevIsSystem ? '1' : '0') + '|'
        + QString::number(qint64(aspect * 1000)) + '|'
        + get(MessageListModel::MessageTextRole).toString() + '|'
        + (get(MessageListModel::IsSystemRole).toBool() ? '1' : '0') + '|'
        + (get(MessageListModel::ShowDateSeparatorRole).toBool() ? '1' : '0') + '|'
        + get(MessageListModel::DateStringRole).toString() + '|'
        + get(MessageListModel::ActorNameRole).toString() + '|'
        + get(MessageListModel::ActorIdRole).toString() + '|'
        + get(MessageListModel::TimeStringRole).toString() + '|'
        + QString::number(get(MessageListModel::LastEditTimestampRole).toLongLong()) + '|'
        + get(MessageListModel::ReplyToAuthorRole).toString() + '|'
        + get(MessageListModel::ReplyToTextRole).toString() + '|'
        + get(MessageListModel::ReactionsRole).toString() + '|'
        + (get(MessageListModel::HasFileRole).toBool() ? '1' : '0') + '|'
        + get(MessageListModel::FileNameRole).toString() + '|'
        + get(MessageListModel::FileMimeRole).toString();
}

// Translate every absolute rect inside `ml` by `dy` so a cached layout can
// slide to a new vertical position without re-running computeLayout.
static void translateLayoutY(MessageLayout &ml, qreal dy)
{
    if (qFuzzyIsNull(dy)) return;
    ml.totalY += dy;
    ml.dateSepRect.translate(0, dy);
    ml.dateSepTextRect.translate(0, dy);
    ml.unreadSepRect.translate(0, dy);
    ml.avatarRect.translate(0, dy);
    ml.nameRect.translate(0, dy);
    ml.bodyRect.translate(0, dy);
    ml.bubbleRect.translate(0, dy);
    ml.timeRect.translate(0, dy);
    ml.quoteRect.translate(0, dy);
    ml.fileRect.translate(0, dy);
    ml.reactBarRect.translate(0, dy);
}

void ChatPainter::rebuildAllLayouts()
{
    QElapsedTimer timer;
    if (TalqLog::g_verbose) timer.start();

    // Preserve scroll position when older messages are prepended at the top
    // (model appends at end; layouts reverse model, so older = top).
    qreal preH = m_contentHeight;
    m_layouts.clear();

    if (!m_model || m_model->rowCount() == 0 || width() <= 0) {
        qreal oldH = m_contentHeight;
        m_contentHeight = 0;
        if (!qFuzzyCompare(oldH, m_contentHeight))
            emit contentHeightChanged();
        update();
        return;
    }

    int count = m_model->rowCount();
    m_layouts.reserve(count);

    qreal y = PainterTheme::spacingNormal; // top margin
    QString prevActorId;
    qint64 prevTimestamp = 0;
    bool prevIsSystem = false;

    // Find the first layout position (oldest-first iteration) whose message.id
    // exceeds the unread boundary. If none, or the user has dismissed the
    // divider by scrolling past it, no separator is drawn.
    int firstUnreadLayoutIdx = -1;
    if (!m_unreadSepDismissed) {
        for (int i = 0; i < count; ++i) {
            int modelRow = count - 1 - i;
            int id = m_model->data(m_model->index(modelRow),
                                   MessageListModel::IdRole).toInt();
            if (id > m_unreadBoundary) { firstUnreadLayoutIdx = i; break; }
        }
    }

    QSet<int> liveIds;
    int cacheHits = 0;

    // Model is newest-first. We iterate oldest-first: row (count-1) down to row 0.
    for (int i = 0; i < count; ++i) {
        int modelRow = count - 1 - i;
        auto idx = m_model->index(modelRow);

        int messageId = m_model->data(idx, MessageListModel::IdRole).toInt();
        liveIds.insert(messageId);

        // Look up actual image aspect ratio if cached
        int fileId = m_model->data(idx, MessageListModel::FileIdRole).toInt();
        qreal aspect = m_previewAspect.value(fileId, 0.0);  // 0 = unknown, show compact placeholder
        bool isUnreadSepRow = (i == firstUnreadLayoutIdx);

        MessageLayout ml;
        bool cacheHit = false;

        // Cache lookup. The unread-separator row is not cached: it shifts
        // totalY off the bubble's startY in a way that's awkward to translate
        // and there's at most one such row per rebuild. Also skip caching for
        // not-yet-acked synthetic rows (messageId <= 0) since they have no
        // stable identity.
        QString key;
        if (!isUnreadSepRow && messageId > 0) {
            key = makeLayoutCacheKey(m_model, modelRow, width(), m_fontScale,
                                     m_darkMode, m_myUserId,
                                     prevActorId, prevTimestamp, prevIsSystem, aspect);
            auto it = m_layoutCache.constFind(messageId);
            if (it != m_layoutCache.constEnd() && it->first == key) {
                ml = it->second;
                translateLayoutY(ml, y - ml.totalY);
                cacheHit = true;
                ++cacheHits;
            }
        }

        if (!cacheHit) {
            // Cache miss (or sep row, or synthetic row) — compute fresh.
            qreal sepY = 0;
            if (isUnreadSepRow) {
                sepY = y;
                y += PainterTheme::unreadSepHeight;
            }

            ml = LayoutEngine::computeLayout(
                m_model, modelRow, width(), m_theme, y,
                m_myUserId, prevActorId, prevTimestamp, prevIsSystem, aspect
            );

            if (isUnreadSepRow) {
                ml.showUnreadSep = true;
                ml.unreadSepRect = QRectF(PainterTheme::spacingNormal, sepY,
                                          width() - 2 * PainterTheme::spacingNormal,
                                          qreal(PainterTheme::unreadSepHeight));
                ml.totalY = sepY;
                ml.totalHeight += PainterTheme::unreadSepHeight;
            } else if (messageId > 0) {
                m_layoutCache.insert(messageId, qMakePair(key, ml));
            }
        }

        // Update prev for next iteration
        prevActorId = m_model->data(idx, MessageListModel::ActorIdRole).toString();
        prevTimestamp = m_model->data(idx, MessageListModel::TimestampRole).toLongLong();
        prevIsSystem = m_model->data(idx, MessageListModel::IsSystemRole).toBool();

        y += ml.totalHeight;
        m_layouts.append(ml);
    }

    // Trim cache: drop entries for messages that are no longer in the model.
    if (m_layoutCache.size() > liveIds.size()) {
        for (auto it = m_layoutCache.begin(); it != m_layoutCache.end();) {
            if (!liveIds.contains(it.key()))
                it = m_layoutCache.erase(it);
            else
                ++it;
        }
    }

    y += PainterTheme::spacingLarge; // bottom margin

    bool wasAtBottom = atBottom();
    qreal oldH = m_contentHeight;
    m_contentHeight = y;

    // If content grew at the top (older history loaded) and we weren't at the bottom,
    // shift scrollY by the delta so the user's viewport stays anchored to the same messages.
    if (!wasAtBottom && preH > 0 && m_contentHeight > preH && m_scrollY > 0)
        m_scrollY += (m_contentHeight - preH);

    clampScroll();

    if (!qFuzzyCompare(oldH, m_contentHeight))
        emit contentHeightChanged();

    // Auto-scroll to bottom if we were at bottom
    if (wasAtBottom)
        scrollToBottom();

    update();

    // Clean up selection — remove IDs that are no longer in the model
    if (m_selectionMode) {
        QSet<int> validIds;
        for (const auto &ml : m_layouts)
            validIds.insert(ml.messageId);
        m_selectedIds &= validIds;
        if (m_selectedIds.isEmpty())
            exitSelectionMode();
        else
            emit selectionChanged(m_selectedIds.size());
    }

    if (TalqLog::g_verbose) {
        TLOG(QString("[layout] %1 msgs in %2 ms (%3 cached, %4 fresh)")
                .arg(count).arg(timer.elapsed())
                .arg(cacheHits).arg(count - cacheHits));
    }
}

void ChatPainter::clampScroll()
{
    qreal maxScroll = qMax(0.0, m_contentHeight - height());
    m_scrollY = qBound(0.0, m_scrollY, maxScroll);
}

// ═══════════════════════════════════════════════════════
// Model signal handlers
// ═══════════════════════════════════════════════════════

void ChatPainter::onRowsInserted(const QModelIndex &, int, int)
{
    rebuildAllLayouts();
}

void ChatPainter::onRowsRemoved(const QModelIndex &, int, int)
{
    rebuildAllLayouts();
}

void ChatPainter::onDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                                const QList<int> &roles)
{
    // Read-receipts and send-status are paint-only — they don't affect any
    // rect or text in the layout. During polling these fire constantly; a
    // full rebuild here is what produced the multi-second freezes on long
    // chats. Skip the rebuild, refresh the stale fields on the cached
    // layouts in-place, then repaint.
    if (!roles.isEmpty()) {
        bool onlyPaintRoles = true;
        for (int role : roles) {
            if (role != MessageListModel::IsReadRole
             && role != MessageListModel::SendStatusRole) {
                onlyPaintRoles = false;
                break;
            }
        }
        if (onlyPaintRoles) {
            // Without this, the cached MessageLayout entries keep their
            // baked-in isRead/sendStatus from the original computeLayout
            // call, so the read-tick glyph wouldn't update until a full
            // rebuild (e.g. on chat switch).
            int first = topLeft.isValid() ? topLeft.row() : 0;
            int last  = bottomRight.isValid() ? bottomRight.row()
                                              : m_model->rowCount() - 1;
            QHash<int, QPair<bool, QString>> updates;
            for (int r = first; r <= last; ++r) {
                auto idx = m_model->index(r);
                int id = m_model->data(idx, MessageListModel::IdRole).toInt();
                if (id <= 0) continue;
                updates.insert(id, qMakePair(
                    m_model->data(idx, MessageListModel::IsReadRole).toBool(),
                    m_model->data(idx, MessageListModel::SendStatusRole).toString()));
            }
            for (auto &ml : m_layouts) {
                auto it = updates.constFind(ml.messageId);
                if (it != updates.constEnd()) {
                    ml.isRead = it->first;
                    ml.sendStatus = it->second;
                }
            }
            for (auto it = updates.constBegin(); it != updates.constEnd(); ++it) {
                auto cit = m_layoutCache.find(it.key());
                if (cit != m_layoutCache.end()) {
                    cit->second.isRead = it->first;
                    cit->second.sendStatus = it->second;
                }
            }
            update();
            return;
        }
    }
    rebuildAllLayouts();
}

void ChatPainter::onModelReset()
{
    m_layoutCache.clear();
    rebuildAllLayouts();
}

// ═══════════════════════════════════════════════════════
// Geometry change
// ═══════════════════════════════════════════════════════

void ChatPainter::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (qAbs(qreal(width()) - m_lastWidth) > 0.5) {
        m_lastWidth = width();
        m_layoutCache.clear();      // every layout is width-dependent
    }
    // Defer the rebuild — coalesces a window-drag's worth of resize ticks.
    m_resizeDebounceTimer->start();
    emit visibleHeightChanged();
}

bool ChatPainter::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent*>(e);
        QPointF canvas(he->pos().x(), he->pos().y() + m_scrollY);
        for (const auto &ml : m_layouts) {
            if (ml.isSystem || ml.showDateSep) continue;
            if (!ml.timeRect.contains(canvas)) continue;
            QString full = QDateTime::fromSecsSinceEpoch(ml.timestamp)
                              .toString(QStringLiteral("dddd, d MMMM yyyy 'at' HH:mm:ss"));
            if (ml.lastEditTimestamp > 0) {
                QString edited = QDateTime::fromSecsSinceEpoch(ml.lastEditTimestamp)
                                   .toString(QStringLiteral("dddd, d MMMM yyyy 'at' HH:mm"));
                full += QStringLiteral("\nedited ") + edited;
            }
            QToolTip::showText(he->globalPos(), full, this);
            return true;
        }
        QToolTip::hideText();
        return true;
    }
    // Intercept Ctrl+C when we have a text selection — claim the shortcut
    // so it doesn't go to the composer or other widgets
    if (e->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->matches(QKeySequence::Copy) && m_textSelection.hasSelection()) {
            e->accept();
            return true;
        }
    }
    if (e->type() == QEvent::HoverMove) {
        if (!m_selectionMode) {
            auto *he = static_cast<QHoverEvent *>(e);
            setHoveredPos(he->position().x(), he->position().y());
        }
        return true;
    }
    if (e->type() == QEvent::HoverLeave) {
        setHoveredPos(-1, -1);
        return true;
    }
    return QWidget::event(e);
}

// ═══════════════════════════════════════════════════════
// Input handling
// ═══════════════════════════════════════════════════════

void ChatPainter::wheelEvent(QWheelEvent *event)
{
    qreal delta = event->angleDelta().y();
    // Standard scroll: 120 units = ~3 lines, we use ~40px per 120 units
    qreal scroll = -delta / 120.0 * 40.0;
    setScrollY(m_scrollY + scroll);
    event->accept();
}

void ChatPainter::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton) {
        m_dragging = true;
        m_dragMoved = false;
        m_draggingScrollbar = false;
        m_pressCanvasPos = event->position();
        m_dragStartY = event->position().y();
        m_dragStartScroll = m_scrollY;
        m_pressHit = hitTestAt(event->position().x(), event->position().y());

        // Scrollbar grab: left-button press in the rightmost ~14 px column,
        // when content overflows. This pre-empts drag-to-select / text-anchor.
        if (event->button() == Qt::LeftButton
            && m_contentHeight > height()
            && event->position().x() >= width() - 14) {
            qreal viewH = height();
            qreal thumbH = qMax(20.0, (viewH / m_contentHeight) * viewH);
            qreal maxScroll = m_contentHeight - viewH;
            qreal thumbY = (m_scrollY / maxScroll) * (viewH - thumbH);
            qreal cy = event->position().y();
            // If press lands on the thumb, keep the relative offset so dragging
            // feels natural. If it lands on empty track, jump the thumb center
            // to the cursor.
            if (cy >= thumbY && cy <= thumbY + thumbH) {
                m_scrollbarThumbOffset = cy - thumbY;
            } else {
                m_scrollbarThumbOffset = thumbH / 2.0;
                // Immediate jump for track clicks.
                qreal newThumbY = qBound(0.0, cy - thumbH / 2.0, viewH - thumbH);
                setScrollY((newThumbY / (viewH - thumbH)) * maxScroll);
            }
            m_draggingScrollbar = true;
            event->accept();
            return;
        }

        // Check if press lands on body text (for text selection)
        m_textAnchorSet = false;
        m_wordSelectMode = false;
        if (event->button() == Qt::LeftButton && !m_selectionMode
            && !(event->modifiers() & Qt::ControlModifier)) {
            QPointF canvasPos(event->position().x(), event->position().y() + m_scrollY);
            int idx = layoutIndexAtY(canvasPos.y());
            if (isOnBodyText(canvasPos, idx)) {
                // Don't start text selection if clicking a link
                const auto &ml = m_layouts[idx];
                QString link = hitTestLink(ml, canvasPos);
                if (link.isEmpty()) {
                    int cursorPos = hitTestBodyCursor(ml, canvasPos);
                    if (cursorPos >= 0) {
                        m_textAnchorSet = true;
                        m_textSelection.anchorLayoutIdx = idx;
                        m_textSelection.anchorCursorPos = cursorPos;
                        m_textSelection.activeLayoutIdx = idx;
                        m_textSelection.activeCursorPos = cursorPos;
                        m_textSelection.active = false;  // not yet — wait for drag
                    }
                }
            }
        }

        // Left click clears any existing text selection (unless we just set a new anchor)
        // Right click preserves selection (for future context menu)
        if (event->button() == Qt::LeftButton && !m_textAnchorSet && m_textSelection.hasSelection()) {
            m_textSelection.clear();
            update();
        }

        event->accept();
    }
}

void ChatPainter::mouseMoveEvent(QMouseEvent *event)
{
    // Scrollbar drag: translate cursor Y into a new scroll position.
    if (m_draggingScrollbar && (event->buttons() & Qt::LeftButton)) {
        qreal viewH = height();
        qreal thumbH = qMax(20.0, (viewH / m_contentHeight) * viewH);
        qreal maxScroll = m_contentHeight - viewH;
        qreal newThumbY = qBound(0.0,
            event->position().y() - m_scrollbarThumbOffset,
            viewH - thumbH);
        setScrollY((newThumbY / (viewH - thumbH)) * maxScroll);
        event->accept();
        return;
    }

    if (m_dragging) {
        qreal dx = event->position().x() - m_pressCanvasPos.x();
        qreal dy = event->position().y() - m_dragStartY;
        if (!m_dragMoved && (qAbs(dx) > 4 || qAbs(dy) > 4))
            m_dragMoved = true;

        if (m_dragMoved && event->buttons() & Qt::LeftButton) {
            // Text selection: drag started on body text
            if (m_textAnchorSet) {
                m_textSelection.active = true;
                setFocus(Qt::MouseFocusReason);
                QPointF canvasPos(event->position().x(), event->position().y() + m_scrollY);
                int idx = layoutIndexAtY(canvasPos.y());

                // Clamp to valid range
                if (idx < 0) idx = 0;
                if (idx >= m_layouts.size()) idx = m_layouts.size() - 1;

                if (idx >= 0 && idx < m_layouts.size()) {
                    const auto &ml = m_layouts[idx];
                    if (ml.bodyDoc && !ml.isSystem) {
                        int cursorPos = hitTestBodyCursor(ml, canvasPos);
                        if (cursorPos < 0) {
                            // Mouse is outside body rect — clamp to start or end
                            if (canvasPos.y() < ml.bodyRect.top())
                                cursorPos = 0;
                            else
                                cursorPos = ml.bodyDoc->characterCount() - 1;
                        }

                        // Word-select mode: snap to word boundaries
                        if (m_wordSelectMode && ml.bodyDoc) {
                            QTextCursor wc(ml.bodyDoc.get());
                            wc.setPosition(cursorPos);
                            wc.select(QTextCursor::WordUnderCursor);

                            int anchorIdx = m_textSelection.anchorLayoutIdx;
                            // Determine direction: is active before or after anchor?
                            bool before = (idx < anchorIdx)
                                || (idx == anchorIdx && cursorPos < m_wordAnchorStart);
                            if (before) {
                                // Dragging backward: anchor at word end, active at word start
                                m_textSelection.anchorCursorPos = m_wordAnchorEnd;
                                cursorPos = wc.selectionStart();
                            } else {
                                // Dragging forward: anchor at word start, active at word end
                                m_textSelection.anchorCursorPos = m_wordAnchorStart;
                                cursorPos = wc.selectionEnd();
                            }
                        }

                        m_textSelection.activeLayoutIdx = idx;
                        m_textSelection.activeCursorPos = cursorPos;
                    } else {
                        // System message or no body — clamp to nearest boundary
                        if (idx < m_textSelection.anchorLayoutIdx) {
                            // Dragging upward past a system msg — use the message above it
                            for (int j = idx; j >= 0; --j) {
                                if (!m_layouts[j].isSystem && m_layouts[j].bodyDoc) {
                                    m_textSelection.activeLayoutIdx = j;
                                    m_textSelection.activeCursorPos = 0;
                                    break;
                                }
                            }
                        } else {
                            // Dragging downward past a system msg
                            for (int j = idx; j < m_layouts.size(); ++j) {
                                if (!m_layouts[j].isSystem && m_layouts[j].bodyDoc) {
                                    m_textSelection.activeLayoutIdx = j;
                                    m_textSelection.activeCursorPos = m_layouts[j].bodyDoc->characterCount() - 1;
                                    break;
                                }
                            }
                        }
                    }
                }

                setCursor(Qt::IBeamCursor);
                update();
                event->accept();
                return;
            }

            // Whole-message selection: drag NOT on body text (text selection returned above),
            // or already in selection mode. Dragging across messages enters selection mode
            // even without Ctrl — Ctrl is optional.
            {
                if (!m_selectionMode) {
                    qreal pressCanvasY = m_pressCanvasPos.y() + m_scrollY;
                    int pressIdx = layoutIndexAtY(pressCanvasY);
                    if (pressIdx >= 0 && pressIdx < m_layouts.size()) {
                        const auto &ml = m_layouts[pressIdx];
                        if (!ml.isSystem && ml.messageId > 0)
                            enterSelectionMode(ml.messageId);
                    }
                }

                if (m_selectionMode) {
                    qreal canvasY = event->position().y() + m_scrollY;
                    int idx = layoutIndexAtY(canvasY);
                    if (idx >= 0 && idx < m_layouts.size()) {
                        const auto &ml = m_layouts[idx];
                        if (!ml.isSystem && ml.messageId > 0
                            && !m_selectedIds.contains(ml.messageId)) {
                            m_selectedIds.insert(ml.messageId);
                            emit selectionChanged(m_selectedIds.size());
                            update();
                        }
                    }
                }
            }
        }

        event->accept();
        return;
    }

    // Not dragging — update hover state and cursor
    if (!m_selectionMode) {
        setHoveredPos(event->position().x(), event->position().y());
        QPointF canvasPos(event->position().x(), event->position().y() + m_scrollY);
        int idx = layoutIndexAtY(canvasPos.y());

        // IBeam cursor over body text, pointing hand over links
        QString hit = hitTestAt(event->position().x(), event->position().y());
        if (!hit.isEmpty())
            setCursor(Qt::PointingHandCursor);
        else if (isOnBodyText(canvasPos, idx))
            setCursor(Qt::IBeamCursor);
        else
            setCursor(Qt::ArrowCursor);
    } else {
        setCursor(Qt::ArrowCursor);
    }
}

void ChatPainter::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging) return;
    m_dragging = false;

    // End-of-scrollbar-drag: swallow and return without touching selection state.
    if (m_draggingScrollbar) {
        m_draggingScrollbar = false;
        event->accept();
        return;
    }

    // Text selection: if selection is active (from drag or double-click), freeze and done
    if (m_textAnchorSet && m_textSelection.active) {
        m_textAnchorSet = false;
        event->accept();
        return;
    }
    m_textAnchorSet = false;

    // If we had a text selection and user left-clicked (no drag), clear it
    // Right-click preserves selection (for context menu / copy)
    if (!m_dragMoved && event->button() == Qt::LeftButton && m_textSelection.hasSelection()) {
        m_textSelection.clear();
        update();
    }

    if (!m_dragMoved && event->button() == Qt::LeftButton) {
        // Selection mode: click toggles selection on entire row
        if (m_selectionMode) {
            qreal canvasY = event->position().y() + m_scrollY;
            int idx = layoutIndexAtY(canvasY);
            if (idx >= 0 && idx < m_layouts.size()) {
                const auto &ml = m_layouts[idx];
                if (!ml.isSystem && ml.messageId > 0)
                    toggleMessageSelection(ml.messageId);
            }
        }
        // Ctrl+Click: enter selection mode with this message
        else if (event->modifiers() & Qt::ControlModifier) {
            qreal canvasY = event->position().y() + m_scrollY;
            int idx = layoutIndexAtY(canvasY);
            if (idx >= 0 && idx < m_layouts.size()) {
                const auto &ml = m_layouts[idx];
                if (!ml.isSystem && ml.messageId > 0)
                    enterSelectionMode(ml.messageId);
            }
        }
        // Normal click: existing hit-test logic
        else {
            QString hit = m_pressHit;
            if (hit.startsWith("link:")) {
                QString url = hit.mid(5);
                if (url.startsWith("http://") || url.startsWith("https://"))
                    emit linkActivated(url);
            } else if (hit.startsWith("reaction:")) {
                QStringList rparts = hit.mid(9).split(":");
                if (rparts.size() >= 2)
                    emit reactionClicked(rparts[0].toInt(), rparts.mid(1).join(":"));
            } else if (hit.startsWith("file:")) {
                QStringList parts = hit.mid(5).split(":");
                if (parts.size() >= 3) {
                    int fid = parts[0].toInt();
                    QString mime = parts[1];
                    QString fname = parts.mid(2).join(":");
                    emit fileClicked(fid, mime, fname);
                }
            } else if (hit.startsWith("reply:")) {
                qreal canvasY = event->position().y() + m_scrollY;
                int clickIdx = layoutIndexAtY(canvasY);
                if (clickIdx >= 0 && clickIdx < m_layouts.size()) {
                    const auto &clickMl = m_layouts[clickIdx];
                    emit replyRequested(clickMl.messageId, clickMl.actorName, clickMl.bodyHtml);
                }
            } else if (hit.startsWith("react:")) {
                int rMsgId = hit.mid(6).toInt();
                int rIdx = layoutIndexAtY(event->position().y() + m_scrollY);
                if (rIdx >= 0 && rIdx < m_layouts.size()) {
                    QRectF reactR = hoverBarReactRect(m_layouts[rIdx]);
                    QPoint btnCenter = mapToGlobal(QPoint(
                        qRound(reactR.center().x()),
                        qRound(reactR.center().y() - m_scrollY)));
                    emit reactRequested(rMsgId, btnCenter);
                }
            }
        }
    }

    // Right-click → copy menu for selected text, or normal context menu
    if (!m_dragMoved && event->button() == Qt::RightButton) {
        if (m_textSelection.hasSelection()) {
            QMenu menu(this);
            QAction *copyAction = menu.addAction(tr("Copy"));
            if (menu.exec(mapToGlobal(event->position().toPoint())) == copyAction) {
                copySelectedText();
            }
            m_textSelection.clear();
            update();
        } else if (!m_selectionMode) {
            QVariantMap msg = messageAt(event->position().x(), event->position().y());
            if (msg.value("messageId").toInt() > 0) {
                emit contextMenuRequested(msg, mapToGlobal(event->position().toPoint()));
            }
        }
    }
    event->accept();
}

void ChatPainter::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || m_selectionMode) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    QPointF canvasPos(event->position().x(), event->position().y() + m_scrollY);
    int idx = layoutIndexAtY(canvasPos.y());
    if (idx < 0 || idx >= m_layouts.size()) return;

    const auto &ml = m_layouts[idx];
    if (ml.isSystem || !ml.bodyDoc) return;

    int cursorPos = hitTestBodyCursor(ml, canvasPos);
    if (cursorPos < 0) return;

    QTextCursor cursor(ml.bodyDoc.get());
    cursor.setPosition(cursorPos);
    cursor.select(QTextCursor::WordUnderCursor);

    m_textSelection.anchorLayoutIdx = idx;
    m_textSelection.anchorCursorPos = cursor.selectionStart();
    m_textSelection.activeLayoutIdx = idx;
    m_textSelection.activeCursorPos = cursor.selectionEnd();
    m_textSelection.active = true;
    setFocus(Qt::MouseFocusReason);

    // Save word boundaries for word-by-word extension
    m_wordSelectMode = true;
    m_wordAnchorStart = cursor.selectionStart();
    m_wordAnchorEnd = cursor.selectionEnd();

    // Set up drag state so the user can extend selection by continuing to drag
    m_dragging = true;
    m_dragMoved = false;
    m_textAnchorSet = true;
    m_pressCanvasPos = event->position();
    m_dragStartY = event->position().y();
    m_dragStartScroll = m_scrollY;

    update();
    event->accept();
}

void ChatPainter::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void ChatPainter::dropEvent(QDropEvent *event)
{
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            emit fileDropped(url.toLocalFile());
            break;
        }
    }
    event->acceptProposedAction();
}

void ChatPainter::keyPressEvent(QKeyEvent *event)
{
    if (m_selectionMode && event->key() == Qt::Key_Escape) {
        exitSelectionMode();
        event->accept();
        return;
    }
    // Ctrl+C: copy selected text
    if (event->matches(QKeySequence::Copy) && m_textSelection.hasSelection()) {
        copySelectedText();
        event->accept();
        return;
    }
    // Escape clears text selection
    if (event->key() == Qt::Key_Escape && m_textSelection.hasSelection()) {
        m_textSelection.clear();
        update();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

QString ChatPainter::hitTestLink(const MessageLayout &ml, const QPointF &canvasPos) const
{
    if (!ml.bodyDoc || ml.bodyRect.isNull()) return {};

    // bodyRect is canvas-absolute, canvasPos is canvas-absolute
    QPointF bodyLocal(canvasPos.x() - ml.bodyRect.x(), canvasPos.y() - ml.bodyRect.y());
    if (bodyLocal.x() < 0 || bodyLocal.y() < 0
        || bodyLocal.x() > ml.bodyRect.width() || bodyLocal.y() > ml.bodyRect.height())
        return {};

    return ml.bodyDoc->documentLayout()->anchorAt(bodyLocal);
}

QString ChatPainter::hitTestReaction(const MessageLayout &ml, const QPointF &canvasPos) const
{
    if (!ml.reactBarRect.contains(canvasPos)) return {};

    QStringList tokens = ml.reactions.split(QStringLiteral("  "), Qt::SkipEmptyParts);
    QFontMetrics fm(m_theme.timeFont());
    qreal x = ml.reactBarRect.left();
    qreal y = ml.reactBarRect.top();
    qreal pillH = 22;

    for (const auto &token : tokens) {
        int pillW = fm.horizontalAdvance(token) + 14;
        QRectF pillRect(x, y, pillW, pillH);
        if (pillRect.contains(canvasPos)) {
            int sp = token.indexOf(' ');
            return sp > 0 ? token.left(sp) : token;
        }
        x += pillW + 4;
    }
    return {};
}

int ChatPainter::hitTestBodyCursor(const MessageLayout &ml, const QPointF &canvasPos) const
{
    if (!ml.bodyDoc || ml.bodyRect.isNull()) return -1;
    QPointF bodyLocal(canvasPos.x() - ml.bodyRect.x(), canvasPos.y() - ml.bodyRect.y());
    if (bodyLocal.x() < 0 || bodyLocal.y() < 0
        || bodyLocal.x() > ml.bodyRect.width() || bodyLocal.y() > ml.bodyRect.height())
        return -1;
    return ml.bodyDoc->documentLayout()->hitTest(bodyLocal, Qt::FuzzyHit);
}

bool ChatPainter::isOnBodyText(const QPointF &canvasPos, int layoutIdx) const
{
    if (layoutIdx < 0 || layoutIdx >= m_layouts.size()) return false;
    const auto &ml = m_layouts[layoutIdx];
    if (ml.isSystem || !ml.bodyDoc || ml.bodyRect.isNull()) return false;
    return ml.bodyRect.contains(canvasPos);
}

void ChatPainter::copySelectedText()
{
    if (!m_textSelection.hasSelection()) return;
    auto range = m_textSelection.normalized();

    QStringList parts;
    for (int i = range.startIdx; i <= range.endIdx; ++i) {
        if (i < 0 || i >= m_layouts.size()) continue;
        const auto &ml = m_layouts[i];
        if (!ml.bodyDoc) continue;

        QTextCursor cursor(ml.bodyDoc.get());
        if (i == range.startIdx && i == range.endIdx) {
            cursor.setPosition(range.startPos);
            cursor.setPosition(range.endPos, QTextCursor::KeepAnchor);
        } else if (i == range.startIdx) {
            cursor.setPosition(range.startPos);
            cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        } else if (i == range.endIdx) {
            cursor.setPosition(0);
            cursor.setPosition(range.endPos, QTextCursor::KeepAnchor);
        } else {
            cursor.setPosition(0);
            cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        }

        QString text = cursor.selectedText();
        text.replace(QChar(0x2029), QChar('\n'));  // paragraph separator → newline
        if (!text.isEmpty())
            parts.append(text);
    }

    if (!parts.isEmpty())
        QApplication::clipboard()->setText(parts.join(QStringLiteral("\n")));
}

int ChatPainter::layoutIndexAtY(qreal viewportY) const
{
    // Simple linear search (fine for <500 messages, binary search can come later)
    for (int i = 0; i < m_layouts.size(); ++i) {
        const auto &ml = m_layouts[i];
        if (viewportY >= ml.totalY && viewportY < ml.totalY + ml.totalHeight)
            return i;
    }
    return -1;
}

// ═══════════════════════════════════════════════════════
// PAINT
// ═══════════════════════════════════════════════════════

void ChatPainter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    p.fillRect(QRectF(0, 0, width(), height()), m_theme.bgPrimary);

    if (m_layouts.isEmpty())
        return;

    qreal vpTop = m_scrollY;
    qreal vpBottom = m_scrollY + height();
    qreal offsetY = -m_scrollY;

    // Subtle teal tint behind the unread region, reinforcing the divider.
    qreal unreadY = -1;
    for (const auto &ml : m_layouts) {
        if (ml.showUnreadSep) {
            unreadY = ml.unreadSepRect.top();
            break;
        }
    }
    if (unreadY >= 0) {
        qreal topOnScreen = unreadY - m_scrollY;
        qreal visibleTop = qMax(qreal(0), topOnScreen);
        if (visibleTop < height()) {
            p.fillRect(QRectF(0, visibleTop, width(), height() - visibleTop),
                       QColor(46, 196, 182, 10));  // ~4% opacity teal
        }
    }

    for (int i = 0; i < m_layouts.size(); ++i) {
        const auto &ml = m_layouts[i];

        if (ml.totalY + ml.totalHeight < vpTop || ml.totalY > vpBottom)
            continue;

        // Selection row highlight (painted as background, before message)
        if (m_selectionMode && m_selectedIds.contains(ml.messageId)) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(46, 196, 182, 30));
            p.drawRect(QRectF(0, ml.totalY + offsetY, width(), ml.totalHeight));
        }

        if (ml.showUnreadSep)
            paintUnreadSep(&p, ml, offsetY);

        if (ml.showDateSep)
            paintDateSep(&p, ml, offsetY);

        if (ml.isSystem)
            paintSystemMessage(&p, ml, offsetY);
        else if (ml.isOwn)
            paintOwnMessage(&p, ml, offsetY);
        else
            paintOtherMessage(&p, ml, offsetY);

        // Selection checkbox (painted ON TOP of message content)
        if (m_selectionMode && !ml.isSystem) {
            bool selected = m_selectedIds.contains(ml.messageId);
            qreal ckSize = 24;
            // Far right of the chat row
            qreal ckX = width() - ckSize - 12;
            qreal ckY = ml.totalY + offsetY + (ml.totalHeight - ckSize) / 2.0;

            if (selected) {
                // Filled teal circle with white checkmark
                p.setPen(QPen(Qt::white, 2));
                p.setBrush(m_theme.accent);
                p.drawEllipse(QRectF(ckX, ckY, ckSize, ckSize));
                QPointF c(ckX + ckSize / 2.0, ckY + ckSize / 2.0);
                p.drawLine(QPointF(c.x() - 5, c.y()), QPointF(c.x() - 1, c.y() + 4));
                p.drawLine(QPointF(c.x() - 1, c.y() + 4), QPointF(c.x() + 5, c.y() - 4));
            } else {
                // Dark filled circle with light border
                p.setPen(QPen(QColor(160, 160, 160), 2));
                p.setBrush(QColor(30, 30, 46, 220));
                p.drawEllipse(QRectF(ckX, ckY, ckSize, ckSize));
            }
        }

        if (!m_selectionMode && i == m_hoveredIndex && !ml.isSystem
            && ml.sendStatus != QLatin1String("sending")
            && ml.sendStatus != QLatin1String("failed"))
            paintHoverBar(&p, ml, offsetY);
    }

    // ── Scrollbar thumb ──
    if (m_contentHeight > height()) {
        qreal viewH = height();
        qreal thumbH = qMax(20.0, (viewH / m_contentHeight) * viewH);
        qreal maxScroll = m_contentHeight - viewH;
        qreal thumbY = (m_scrollY / maxScroll) * (viewH - thumbH);

        qreal scrollbarW = 4;
        qreal scrollbarX = width() - scrollbarW - 2;

        p.setPen(Qt::NoPen);
        QColor thumbColor = m_theme.textMuted;
        thumbColor.setAlphaF(0.3f);
        p.setBrush(thumbColor);
        p.drawRoundedRect(QRectF(scrollbarX, thumbY, scrollbarW, thumbH), 2, 2);
    }
}

// ─── Date separator ─────────────────────────────────

void ChatPainter::paintDateSep(QPainter *p, const MessageLayout &ml, qreal offsetY)
{
    QRectF pill = ml.dateSepRect.translated(0, offsetY);

    p->setPen(Qt::NoPen);
    p->setBrush(m_theme.bgSurface);
    p->drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);

    p->setPen(m_theme.textSecondary);
    p->setFont(m_theme.dateSepFont());
    p->drawText(pill, Qt::AlignCenter, ml.dateString);
}

void ChatPainter::paintUnreadSep(QPainter *p, const MessageLayout &ml, qreal offsetY)
{
    if (!ml.showUnreadSep || ml.unreadSepRect.isEmpty()) return;

    QRectF strip = ml.unreadSepRect.translated(0, offsetY);

    QFont pillFont = m_theme.dateSepFont();
    QFontMetrics fm(pillFont);
    QString text = tr("New messages");
    int textW = fm.horizontalAdvance(text);
    int pillW = textW + 20;

    qreal cy = strip.center().y();

    // Teal accent line across the full strip.
    p->setPen(QPen(m_theme.accent, 1));
    p->drawLine(QPointF(strip.left(), cy), QPointF(strip.right(), cy));

    // Centered pill with teal text over the conversation background.
    QRectF pill((strip.width() - pillW) / 2.0 + strip.left(),
                cy - PainterTheme::unreadPillHeight / 2.0,
                pillW, PainterTheme::unreadPillHeight);
    p->setPen(Qt::NoPen);
    p->setBrush(m_theme.bgPrimary);
    p->drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);

    p->setPen(m_theme.accent);
    p->setFont(pillFont);
    p->drawText(pill, Qt::AlignCenter, text);
}

// ─── System message ─────────────────────────────────

void ChatPainter::paintSystemMessage(QPainter *p, const MessageLayout &ml, qreal offsetY)
{
    QRectF r = ml.bodyRect.translated(0, offsetY);
    p->setPen(m_theme.systemMsg);
    p->setFont(m_theme.systemFont());

    // Strip HTML for system messages (they're usually plain)
    QString plain = ml.bodyHtml;
    plain.remove(QRegularExpression("<[^>]*>"));
    p->drawText(r, Qt::AlignHCenter | Qt::AlignVCenter, plain);
}

// ─── Own message ────────────────────────────────────

void ChatPainter::paintOwnMessage(QPainter *p, const MessageLayout &ml, qreal offsetY)
{
    // Avatar (non-grouped)
    if (!ml.isGrouped && !ml.avatarRect.isNull()) {
        QRectF ar = ml.avatarRect.translated(0, offsetY);
        QImage avatarImg = fetchAvatar(m_myUserId);
        if (!avatarImg.isNull()) {
            p->drawImage(ar, avatarImg);
        } else {
            p->setPen(Qt::NoPen);
            p->setBrush(m_theme.accent);
            p->drawEllipse(ar);
            QFont letterFont;
            letterFont.setPixelSize(14);
            letterFont.setWeight(QFont::DemiBold);
            p->setFont(letterFont);
            p->setPen(Qt::white);
            p->drawText(ar, Qt::AlignCenter, QStringLiteral("Me"));
        }
    }

    // Bubble background
    if (!ml.bubbleRect.isNull()) {
        QRectF bubble = ml.bubbleRect.translated(0, offsetY);
        p->setPen(Qt::NoPen);
        p->setBrush(m_theme.bgMessageOwn);
        p->drawRoundedRect(bubble, PainterTheme::radiusNormal, PainterTheme::radiusNormal);
        if (ml.messageId == m_highlightMessageId && m_highlightTimer && m_highlightTimer->isActive()) {
            p->fillRect(bubble, QColor(46, 196, 182, 50));
        }
    }

    // Sending state opacity
    qreal oldOpacity = p->opacity();
    if (ml.sendStatus == QLatin1String("sending"))
        p->setOpacity(0.6);

    // Reply quote
    if (!ml.replyToText.isEmpty())
        paintReplyQuote(p, ml, offsetY);

    // File attachment
    if (ml.hasFile && !ml.fileRect.isNull())
        paintFileAttachment(p, ml, offsetY);

    // Body text
    if (ml.bodyDoc) {
        p->save();
        QRectF bodyR = ml.bodyRect.translated(0, offsetY);
        p->translate(bodyR.topLeft());
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.palette.setColor(QPalette::Text, m_theme.textPrimary);
        ctx.clip = QRectF(0, 0, bodyR.width(), bodyR.height());

        // Text selection highlight
        if (m_textSelection.hasSelection()) {
            auto range = m_textSelection.normalized();
            int layoutIdx = &ml - m_layouts.constData();
            if (layoutIdx >= range.startIdx && layoutIdx <= range.endIdx) {
                QTextCursor cursor(ml.bodyDoc.get());
                if (layoutIdx == range.startIdx && layoutIdx == range.endIdx) {
                    cursor.setPosition(range.startPos);
                    cursor.setPosition(range.endPos, QTextCursor::KeepAnchor);
                } else if (layoutIdx == range.startIdx) {
                    cursor.setPosition(range.startPos);
                    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
                } else if (layoutIdx == range.endIdx) {
                    cursor.setPosition(0);
                    cursor.setPosition(range.endPos, QTextCursor::KeepAnchor);
                } else {
                    cursor.setPosition(0);
                    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
                }
                QAbstractTextDocumentLayout::Selection sel;
                sel.cursor = cursor;
                QTextCharFormat fmt;
                fmt.setBackground(QColor(46, 196, 182, 77));
                sel.format = fmt;
                ctx.selections.append(sel);
            }
        }

        ml.bodyDoc->documentLayout()->draw(p, ctx);
        p->restore();
    }
    paintMessageEmojis(p, ml, offsetY);

    // Reactions
    if (!ml.reactions.isEmpty() && !ml.reactBarRect.isNull())
        paintReactions(p, ml, offsetY);

    // Timestamp + read status (draw once, not twice)
    {
        QRectF tr = ml.timeRect.translated(0, offsetY);
        QColor timeColor = m_darkMode
            ? QColor(255, 255, 255, 115)
            : m_theme.textTime;

        QString timeLabel = ml.lastEditTimestamp > 0
            ? QStringLiteral("(edited) ") + ml.timeString
            : ml.timeString;
        QString timeText = ml.sendStatus == QLatin1String("sending")
            ? QStringLiteral("Sending...") : timeLabel;

        // Determine status icon — ◉ read (green), ○ delivered (muted)
        QString statusChar;
        QColor statusColor = timeColor;
        if (ml.sendStatus == QLatin1String("failed")) {
            statusColor = m_theme.danger;
            timeColor = m_theme.danger;
            statusChar = QStringLiteral("\u26A0");  // ⚠ warning
        } else if (ml.sendStatus != QLatin1String("sending")) {
            statusColor = ml.isRead ? m_theme.accent : timeColor;
            statusChar = ml.isRead ? QStringLiteral("\u25C9") : QStringLiteral("\u25CB");  // ◉ read, ○ delivered
        }

        QFont statusFont = m_theme.timeFont();
        statusFont.setPixelSize(12);
        int sw = statusChar.isEmpty() ? 0 : QFontMetrics(statusFont).horizontalAdvance(statusChar) + 4;

        // Draw time (shifted left to make room for icon)
        QRectF timeArea = tr.adjusted(0, 0, -sw, 0);
        p->setPen(timeColor);
        p->setFont(m_theme.timeFont());
        p->drawText(timeArea, Qt::AlignRight | Qt::AlignVCenter, timeText);

        // Draw status icon
        if (!statusChar.isEmpty()) {
            QRectF sr(tr.right() - sw + 2, tr.top(), sw - 2, tr.height());
            p->setPen(statusColor);
            p->setFont(statusFont);
            p->drawText(sr, Qt::AlignCenter, statusChar);
        }
    }

    p->setOpacity(oldOpacity);
}

// ─── Other person's message ─────────────────────────

void ChatPainter::paintOtherMessage(QPainter *p, const MessageLayout &ml, qreal offsetY)
{
    // Bubble background
    if (!ml.bubbleRect.isNull()) {
        QRectF bubble = ml.bubbleRect.translated(0, offsetY);
        p->setPen(Qt::NoPen);
        p->setBrush(m_darkMode ? QColor(255, 255, 255, 15) : QColor(0, 0, 0, 12));
        p->drawRoundedRect(bubble, PainterTheme::radiusNormal, PainterTheme::radiusNormal);
        if (ml.messageId == m_highlightMessageId && m_highlightTimer && m_highlightTimer->isActive()) {
            p->fillRect(bubble, QColor(46, 196, 182, 50));
        }
    }

    // Avatar (non-grouped)
    if (!ml.isGrouped && !ml.avatarRect.isNull()) {
        QRectF ar = ml.avatarRect.translated(0, offsetY);
        QImage avatarImg = fetchAvatar(ml.actorId);

        if (!avatarImg.isNull()) {
            // Draw the circular avatar image
            p->drawImage(ar, avatarImg);
        } else {
            // Fallback: colored circle with initial
            QColor avatarColor = PainterTheme::authorColor(ml.actorId);
            p->setPen(Qt::NoPen);
            p->setBrush(avatarColor);
            p->drawEllipse(ar);

            QFont letterFont;
            letterFont.setPixelSize(14);
            letterFont.setWeight(QFont::DemiBold);
            p->setFont(letterFont);
            p->setPen(Qt::white);
            QString letter = ml.actorName.isEmpty()
                ? QStringLiteral("?")
                : ml.actorName.left(1).toUpper();
            p->drawText(ar, Qt::AlignCenter, letter);
        }
    }

    // Author name (non-grouped, above bubble)
    if (!ml.isGrouped && !ml.nameRect.isNull()) {
        QRectF nr = ml.nameRect.translated(0, offsetY);
        p->setPen(PainterTheme::authorColor(ml.actorId));
        p->setFont(m_theme.nameFont());
        p->drawText(nr, Qt::AlignLeft | Qt::AlignVCenter, ml.actorName);
    }

    // Reply quote
    if (!ml.replyToText.isEmpty())
        paintReplyQuote(p, ml, offsetY);

    // File attachment
    if (ml.hasFile && !ml.fileRect.isNull())
        paintFileAttachment(p, ml, offsetY);

    // Body text
    if (ml.bodyDoc) {
        p->save();
        QRectF bodyR = ml.bodyRect.translated(0, offsetY);
        p->translate(bodyR.topLeft());
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.palette.setColor(QPalette::Text, m_theme.textPrimary);
        ctx.clip = QRectF(0, 0, bodyR.width(), bodyR.height());

        // Text selection highlight
        if (m_textSelection.hasSelection()) {
            auto range = m_textSelection.normalized();
            int layoutIdx = &ml - m_layouts.constData();
            if (layoutIdx >= range.startIdx && layoutIdx <= range.endIdx) {
                QTextCursor cursor(ml.bodyDoc.get());
                if (layoutIdx == range.startIdx && layoutIdx == range.endIdx) {
                    cursor.setPosition(range.startPos);
                    cursor.setPosition(range.endPos, QTextCursor::KeepAnchor);
                } else if (layoutIdx == range.startIdx) {
                    cursor.setPosition(range.startPos);
                    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
                } else if (layoutIdx == range.endIdx) {
                    cursor.setPosition(0);
                    cursor.setPosition(range.endPos, QTextCursor::KeepAnchor);
                } else {
                    cursor.setPosition(0);
                    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
                }
                QAbstractTextDocumentLayout::Selection sel;
                sel.cursor = cursor;
                QTextCharFormat fmt;
                fmt.setBackground(QColor(46, 196, 182, 77));
                sel.format = fmt;
                ctx.selections.append(sel);
            }
        }

        ml.bodyDoc->documentLayout()->draw(p, ctx);
        p->restore();
    }
    paintMessageEmojis(p, ml, offsetY);

    // Reactions
    if (!ml.reactions.isEmpty() && !ml.reactBarRect.isNull())
        paintReactions(p, ml, offsetY);

    // Time (inside bubble, below body)
    if (!ml.timeRect.isNull()) {
        QRectF tr = ml.timeRect.translated(0, offsetY);
        p->setPen(m_theme.textTime);
        p->setFont(m_theme.timeFont());
        QString timeLabel = ml.lastEditTimestamp > 0
            ? QStringLiteral("(edited) ") + ml.timeString
            : ml.timeString;
        p->drawText(tr, Qt::AlignRight | Qt::AlignVCenter, timeLabel);
    }
}

// ─── Reply quote (shared between own and other) ─────

void ChatPainter::paintReplyQuote(QPainter *p, const MessageLayout &ml, qreal offsetY)
{
    QRectF qr = ml.quoteRect.translated(0, offsetY);
    if (qr.isNull()) return;

    // Background
    QColor quoteBg = m_darkMode
        ? QColor(255, 255, 255, 10)   // rgba(1,1,1,0.04)
        : QColor(0, 0, 0, 10);
    p->setPen(Qt::NoPen);
    p->setBrush(quoteBg);
    p->drawRoundedRect(qr, PainterTheme::radiusSmall, PainterTheme::radiusSmall);

    // Teal left border
    QRectF bar(qr.left(), qr.top(), 3, qr.height());
    p->setBrush(m_theme.accent);
    p->drawRoundedRect(bar, 1.5, 1.5);

    // Author name
    QFont tinyFont = m_theme.timeFont();
    tinyFont.setWeight(QFont::DemiBold);
    p->setFont(tinyFont);
    p->setPen(m_theme.accent);
    QRectF authorR(qr.left() + 10, qr.top() + 4, qr.width() - 18, QFontMetrics(tinyFont).height());
    p->drawText(authorR, Qt::AlignLeft | Qt::AlignVCenter, ml.replyToAuthor);

    // Reply text (truncated, single line)
    p->setFont(m_theme.timeFont());
    p->setPen(m_theme.textSecondary);
    QRectF textR(qr.left() + 10, authorR.bottom() + 1, qr.width() - 18, QFontMetrics(m_theme.timeFont()).height());
    QString truncated = ml.replyToText;
    truncated.remove(QRegularExpression("<[^>]*>"));
    QFontMetrics fm(m_theme.timeFont());
    truncated = fm.elidedText(truncated, Qt::ElideRight, qRound(textR.width()));
    p->drawText(textR, Qt::AlignLeft | Qt::AlignVCenter, truncated);
}

// ─── File attachment ────────────────────────────────

void ChatPainter::paintFileAttachment(QPainter *p, const MessageLayout &ml, qreal offsetY)
{
    QRectF fr = ml.fileRect.translated(0, offsetY);
    bool isImage = ml.fileMime.startsWith(QLatin1String("image/"));

    if (isImage) {
        // Try to draw the loaded preview image
        QImage preview = fetchFilePreview(ml.fileId);
        if (!preview.isNull()) {
            // Scale to fit within the rect while keeping aspect ratio
            QImage scaled = preview.scaled(
                qRound(fr.width()), qRound(fr.height()),
                Qt::KeepAspectRatio, Qt::SmoothTransformation);
            // Center horizontally within the file rect
            qreal imgX = fr.left() + (fr.width() - scaled.width()) / 2.0;
            // Draw with rounded corners
            p->save();
            QPainterPath clip;
            clip.addRoundedRect(QRectF(imgX, fr.top(), scaled.width(), scaled.height()),
                                PainterTheme::radiusSmall, PainterTheme::radiusSmall);
            p->setClipPath(clip);
            p->drawImage(QPointF(imgX, fr.top()), scaled);
            p->restore();
        } else {
            // Placeholder: gray rect with filename
            p->setPen(Qt::NoPen);
            p->setBrush(m_theme.bgSurface);
            p->drawRoundedRect(fr, PainterTheme::radiusSmall, PainterTheme::radiusSmall);

            p->setPen(m_theme.textSecondary);
            p->setFont(m_theme.timeFont());
            p->drawText(fr, Qt::AlignCenter, ml.fileName);
        }
    } else {
        // Non-image file: draw a pill with document icon + filename
        p->setPen(Qt::NoPen);
        p->setBrush(m_theme.bgSurface);
        p->drawRoundedRect(fr, fr.height() / 2.0, fr.height() / 2.0);

        // Document icon (Unicode page symbol)
        QFont iconFont = m_theme.bodyFont();
        iconFont.setPixelSize(18);
        QRectF iconR(fr.left() + 12, fr.top(), 24, fr.height());
        p->setPen(m_theme.accent);
        p->setFont(iconFont);
        p->drawText(iconR, Qt::AlignCenter, QStringLiteral("\U0001F4C4"));

        // Filename + size
        QRectF textR(iconR.right() + 4, fr.top(), fr.width() - 52, fr.height());
        QString sizeStr;
        if (ml.fileSize > 0) {
            if (ml.fileSize < 1024) sizeStr = QString::number(ml.fileSize) + " B";
            else if (ml.fileSize < 1024 * 1024) sizeStr = QString::number(ml.fileSize / 1024) + " KB";
            else sizeStr = QString::number(ml.fileSize / (1024.0 * 1024.0), 'f', 1) + " MB";
        }

        if (sizeStr.isEmpty()) {
            // Just filename
            p->setPen(m_theme.textPrimary);
            p->setFont(m_theme.bodyFont());
            QFontMetrics fm(m_theme.bodyFont());
            p->drawText(textR, Qt::AlignLeft | Qt::AlignVCenter,
                         fm.elidedText(ml.fileName, Qt::ElideMiddle, qRound(textR.width())));
        } else {
            // Filename on top, size below
            qreal halfH = textR.height() / 2.0;
            QRectF nameR(textR.left(), textR.top(), textR.width(), halfH);
            QRectF sizeR(textR.left(), textR.top() + halfH, textR.width(), halfH);

            p->setPen(m_theme.textPrimary);
            p->setFont(m_theme.bodyFont());
            QFontMetrics fm(m_theme.bodyFont());
            p->drawText(nameR, Qt::AlignLeft | Qt::AlignBottom,
                         fm.elidedText(ml.fileName, Qt::ElideMiddle, qRound(nameR.width())));

            p->setPen(m_theme.textMuted);
            p->setFont(m_theme.timeFont());
            p->drawText(sizeR, Qt::AlignLeft | Qt::AlignTop, sizeStr);
        }
    }
}

// ─── Reactions ──────────────────────────────────────

void ChatPainter::paintReactions(QPainter *p, const MessageLayout &ml, qreal offsetY)
{
    QRectF bar = ml.reactBarRect.translated(0, offsetY);

    // Parse: "emoji1 count1  emoji2 count2" (double-space separated tokens)
    QStringList tokens = ml.reactions.split(QStringLiteral("  "), Qt::SkipEmptyParts);

    QFont emojiFont = m_theme.bodyFont();
    emojiFont.setPixelSize(13);
    QFont countFont = m_theme.timeFont();
    countFont.setPixelSize(11);

    QFontMetrics fmEmoji(emojiFont);
    QFontMetrics fmCount(countFont);

    qreal x = bar.left();
    const qreal pillH = bar.height();
    const qreal pillGap = 4;
    const qreal pillPadX = 6;

    QColor pillBg = m_darkMode ? QColor(255, 255, 255, 15) : QColor(0, 0, 0, 10);
    QColor countColor = m_theme.textSecondary;

    for (const QString &token : tokens) {
        // Each token is "emoji count" (single space)
        int lastSpace = token.lastIndexOf(' ');
        if (lastSpace <= 0) continue;

        QString emoji = token.left(lastSpace);
        QString count = token.mid(lastSpace + 1);

        int emojiW = fmEmoji.horizontalAdvance(emoji);
        int countW = fmCount.horizontalAdvance(count);
        qreal pillW = pillPadX + emojiW + 3 + countW + pillPadX;

        // Don't overflow the bar
        if (x + pillW > bar.right())
            break;

        QRectF pill(x, bar.top(), pillW, pillH);

        // Pill background
        p->setPen(Qt::NoPen);
        p->setBrush(pillBg);
        p->drawRoundedRect(pill, pillH / 2.0, pillH / 2.0);

        // Emoji
        QPixmap emojiPix = EmojiData::pixmapFor(emoji, 18);
        if (!emojiPix.isNull()) {
            qreal px = x + pillPadX + (emojiW - emojiPix.width()) / 2.0;
            qreal py = bar.top() + (pillH - emojiPix.height()) / 2.0;
            p->drawPixmap(QPointF(px, py), emojiPix);
        } else {
            p->setPen(m_theme.textPrimary);
            p->setFont(emojiFont);
            p->drawText(QRectF(x + pillPadX, bar.top(), emojiW, pillH),
                         Qt::AlignCenter, emoji);
        }

        // Count
        p->setPen(countColor);
        p->setFont(countFont);
        p->drawText(QRectF(x + pillPadX + emojiW + 3, bar.top(), countW, pillH),
                     Qt::AlignCenter, count);

        x += pillW + pillGap;
    }
}

// ─── Hover bar geometry ─────────────────────────────

QRectF ChatPainter::hoverBarReactRect(const MessageLayout &ml) const
{
    const qreal btnSize = 28;
    const qreal gap = 8;

    // Right of bubble
    qreal x = ml.contentRight + gap;
    qreal refTop = ml.bodyRect.isNull() ? ml.totalY : ml.bodyRect.top();
    qreal refBottom = ml.bodyRect.isNull() ? ml.totalY + ml.totalHeight : ml.bodyRect.bottom();
    qreal y = refTop + (refBottom - refTop - btnSize) / 2.0;
    y = qBound(ml.totalY, y, ml.totalY + ml.totalHeight - btnSize);
    return QRectF(x, y, btnSize, btnSize);
}

QRectF ChatPainter::hoverBarReplyRect(const MessageLayout &ml) const
{
    const qreal btnSize = 28;
    if (ml.isOwn) {
        // Own messages: reply is the only button, goes at first position
        return hoverBarReactRect(ml);
    }
    // Others: reply goes after react button
    QRectF reactR = hoverBarReactRect(ml);
    return QRectF(reactR.right() + 4, reactR.top(), btnSize, btnSize);
}

// ─── Hover bar painting ─────────────────────────────

void ChatPainter::paintHoverBar(QPainter *p, const MessageLayout &ml, qreal offsetY)
{
    const qreal btnSize = 28;
    QRectF replyR = hoverBarReplyRect(ml).translated(0, offsetY);

    auto drawButton = [&](const QRectF &r) {
        p->setPen(QPen(m_theme.divider, 0.5));
        p->setBrush(m_theme.bgSurface);
        p->drawEllipse(r);
    };

    // --- React button (smiley) — only for other people's messages ---
    if (!ml.isOwn) {
    QRectF reactR = hoverBarReactRect(ml).translated(0, offsetY);
    drawButton(reactR);
    {
        qreal cx = reactR.center().x();
        qreal cy = reactR.center().y();
        qreal scale = btnSize / 30.0;  // icons designed for 30px, we use 28

        QPen iconPen(m_theme.textSecondary, 1.8 * scale, Qt::SolidLine, Qt::RoundCap);
        p->setPen(iconPen);
        p->setBrush(Qt::NoBrush);

        // Face circle
        p->drawEllipse(QPointF(cx, cy), 8.0 * scale, 8.0 * scale);

        // Eyes (filled dots)
        p->setPen(Qt::NoPen);
        p->setBrush(m_theme.textSecondary);
        p->drawEllipse(QPointF(cx - 3.0 * scale, cy - 2.0 * scale), 1.2 * scale, 1.2 * scale);
        p->drawEllipse(QPointF(cx + 3.0 * scale, cy - 2.0 * scale), 1.2 * scale, 1.2 * scale);

        // Smile arc
        p->setPen(iconPen);
        p->setBrush(Qt::NoBrush);
        QPainterPath smile;
        QRectF smileRect(cx - 4.5 * scale, cy - 0.5 * scale, 9.0 * scale, 7.0 * scale);
        smile.arcMoveTo(smileRect, -160);
        smile.arcTo(smileRect, -160, -220);
        p->drawPath(smile);
    }
    } // end if (!ml.isOwn) for react button

    // --- Reply button (curved arrow) ---
    drawButton(replyR);
    {
        qreal cx = replyR.center().x();
        qreal cy = replyR.center().y();
        qreal scale = btnSize / 30.0;

        QPen iconPen(m_theme.textSecondary, 2.0 * scale, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p->setPen(iconPen);
        p->setBrush(Qt::NoBrush);

        // Arrow pointing left
        QPainterPath arrow;
        arrow.moveTo(cx - 2.0 * scale, cy - 4.0 * scale);
        arrow.lineTo(cx - 7.0 * scale, cy);
        arrow.lineTo(cx - 2.0 * scale, cy + 4.0 * scale);
        p->drawPath(arrow);

        // Curved line from arrow tip to right
        QPainterPath curve;
        curve.moveTo(cx - 7.0 * scale, cy);
        curve.lineTo(cx + 2.0 * scale, cy);
        curve.quadTo(cx + 7.0 * scale, cy, cx + 7.0 * scale, cy + 5.0 * scale);
        p->drawPath(curve);
    }
}

// ─── Avatar image loading ───────────────────────────

QImage ChatPainter::fetchAvatar(const QString &userId)
{
    auto it = m_avatarCache.find(userId);
    if (it != m_avatarCache.end())
        return it.value();

    // Start async fetch if not already pending
    requestAvatar(userId);
    return QImage(); // empty = use fallback
}

void ChatPainter::requestAvatar(const QString &userId)
{
    if (m_avatarPending.contains(userId))
        return;
    if (!m_model || !m_model->api())
        return;

    m_avatarPending.insert(userId);

    ApiClient *api = m_model->api();
    QNetworkReply *reply = api->getAbsoluteUrl(
        QStringLiteral("/index.php/avatar/") + userId + QStringLiteral("/64"));

    connect(reply, &QNetworkReply::finished, this, [this, reply, userId]() {
        reply->deleteLater();
        m_avatarPending.remove(userId);

        if (reply->error() != QNetworkReply::NoError) {
            // Cache empty image so we don't retry
            m_avatarCache[userId] = QImage();
            return;
        }

        QByteArray data = reply->readAll();
        QImage img;
        if (!img.loadFromData(data)) {
            m_avatarCache[userId] = QImage();
            return;
        }

        m_avatarCache[userId] = PainterTheme::cropToCircle(img, PainterTheme::avatarSize);
        update(); // repaint with the loaded avatar
    });
}

// ─── File preview image loading ─────────────────────

QImage ChatPainter::fetchFilePreview(int fileId)
{
    auto it = m_previewCache.find(fileId);
    if (it != m_previewCache.end())
        return it.value();

    // Start async fetch if not already pending
    requestFilePreview(fileId);
    return QImage(); // empty = show placeholder
}

void ChatPainter::requestFilePreview(int fileId)
{
    if (m_previewPending.contains(fileId))
        return;
    if (!m_model || !m_model->api())
        return;

    m_previewPending.insert(fileId);

    ApiClient *api = m_model->api();
    QString path = QString("/index.php/core/preview?fileId=%1&x=800&y=600&a=1")
        .arg(fileId);
    QNetworkReply *reply = api->getAbsoluteUrl(path);

    connect(reply, &QNetworkReply::finished, this, [this, reply, fileId]() {
        reply->deleteLater();
        m_previewPending.remove(fileId);

        if (reply->error() != QNetworkReply::NoError) {
            // Don't cache on failure — NC's preview service often 404s the
            // first request for a just-uploaded file while it's still
            // generating the thumbnail. Retry with backoff, give up after
            // a few attempts.
            int attempts = m_previewAttempts.value(fileId, 0) + 1;
            m_previewAttempts[fileId] = attempts;
            if (attempts <= 4) {
                const int delayMs = qMin(30000, 1500 * (1 << (attempts - 1)));
                QTimer::singleShot(delayMs, this, [this, fileId]() {
                    requestFilePreview(fileId);
                });
            } else {
                m_previewCache[fileId] = QImage();
            }
            return;
        }

        QByteArray data = reply->readAll();
        QImage img;
        if (!img.loadFromData(data)) {
            m_previewCache[fileId] = QImage();
            return;
        }
        m_previewAttempts.remove(fileId);

        m_previewCache[fileId] = img;
        qreal aspect = img.width() > 0 ? (qreal)img.height() / img.width() : 0.5;
        qreal oldAspect = m_previewAspect.value(fileId, 0.0);
        bool aspectChanged = qAbs(oldAspect - aspect) > 0.01;
        m_previewAspect[fileId] = aspect;

        // Evict oldest if over 50 MB
        qint64 totalBytes = 0;
        for (auto it = m_previewCache.begin(); it != m_previewCache.end(); ++it)
            totalBytes += it.value().sizeInBytes();
        while (totalBytes > 50 * 1024 * 1024 && m_previewCache.size() > 1) {
            auto oldest = m_previewCache.begin();
            totalBytes -= oldest.value().sizeInBytes();
            m_previewCache.erase(oldest);
        }

        if (aspectChanged)
            rebuildAllLayouts();  // aspect ratio now known — recalculate heights
        else
            update();  // just repaint with the loaded preview
    });
}

// ─── Twemoji overlay ────────────────────────────────

void ChatPainter::drawEmoji(QPainter *p, const QString &codepoints, const QRectF &rect)
{
    QPixmap pm = EmojiData::pixmapFor(codepoints, int(rect.height()));
    if (pm.isNull()) {
        // No Twemoji asset for this cluster — leave the system glyph visible.
        return;
    }
    // A fully opaque Twemoji PNG at the measured advance covers the system glyph.
    p->drawPixmap(rect.topLeft(), pm.scaled(int(rect.width()), int(rect.height()),
                                             Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

// Find the QTextLine within `layout` that contains the character offset `relPos`.
// Returns an invalid line if none matches (e.g. offset past the layout).
static QTextLine findLineForOffset(QTextLayout *layout, int relPos)
{
    for (int li = 0; li < layout->lineCount(); ++li) {
        QTextLine candidate = layout->lineAt(li);
        int ts = candidate.textStart();
        if (relPos >= ts && relPos < ts + candidate.textLength())
            return candidate;
    }
    return QTextLine();
}

void ChatPainter::paintMessageEmojis(QPainter *p, const MessageLayout &ml, qreal offsetY)
{
    if (ml.emojiRuns.isEmpty() || !ml.bodyDoc) return;

    const qreal bodyLeft = ml.bodyRect.left();
    const qreal bodyTop  = ml.bodyRect.top() + offsetY;
    const QFontMetricsF fm(ml.bodyDoc->defaultFont());

    for (const auto &r : ml.emojiRuns) {
        QTextBlock block = ml.bodyDoc->findBlock(r.docPosition);
        if (!block.isValid()) continue;
        QTextLayout *blkLay = block.layout();
        if (!blkLay) continue;

        int relPos = r.docPosition - block.position();
        QTextLine line = findLineForOffset(blkLay, relPos);
        if (!line.isValid()) continue;

        qreal x = bodyLeft + blkLay->position().x() + line.cursorToX(relPos);
        qreal y = bodyTop + blkLay->position().y() + line.y();
        qreal h = line.height();

        // Measure advance width of the original cluster in the body font.
        qreal w = fm.horizontalAdvance(r.codepoints);
        if (w <= 0) w = h * 1.1;

        drawEmoji(p, r.codepoints, QRectF(x, y, w, h));
    }
}
