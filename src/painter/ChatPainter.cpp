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
#include <QtMath>
#include "core/ApiClient.h"

ChatPainter::ChatPainter(QWidget *parent)
    : QWidget(parent)
    , m_theme(m_darkMode, m_fontScale)
{
    setAttribute(Qt::WA_Hover);
    setMouseTracking(true);
    setAcceptDrops(true);
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

    // Connect new model
    if (m_model) {
        connect(m_model, &QAbstractItemModel::rowsInserted, this, &ChatPainter::onRowsInserted);
        connect(m_model, &QAbstractItemModel::rowsRemoved, this, &ChatPainter::onRowsRemoved);
        connect(m_model, &QAbstractItemModel::dataChanged, this, &ChatPainter::onDataChanged);
        connect(m_model, &QAbstractItemModel::modelReset, this, &ChatPainter::onModelReset);
    }

    rebuildAllLayouts();
}

void ChatPainter::setMyUserId(const QString &id)
{
    if (m_myUserId == id) return;
    m_myUserId = id;
    rebuildAllLayouts();
}

void ChatPainter::setDarkMode(bool dark)
{
    if (m_darkMode == dark) return;
    m_darkMode = dark;
    m_theme = PainterTheme(m_darkMode, m_fontScale);
    rebuildAllLayouts();
}

void ChatPainter::setFontScale(qreal scale)
{
    if (qFuzzyCompare(m_fontScale, scale)) return;
    m_fontScale = scale;
    m_theme = PainterTheme(m_darkMode, m_fontScale);
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

void ChatPainter::clearSelection()
{
    m_selectedIds.clear();
    exitSelectionMode();
}

QVector<QVariantMap> ChatPainter::selectedMessages() const
{
    QVector<QVariantMap> result;
    for (const auto &ml : m_layouts) {
        if (m_selectedIds.contains(ml.messageId)) {
            QVariantMap m;
            m["messageId"] = ml.messageId;
            m["isOwn"] = ml.isOwn;
            m["actorName"] = ml.actorName;
            m["messageText"] = ml.bodyHtml;
            m["timeString"] = ml.timeString;
            m["hasFile"] = ml.hasFile;
            m["fileId"] = ml.fileId;
            m["fileName"] = ml.fileName;
            m["fileMime"] = ml.fileMime;
            result.append(m);
        }
    }
    return result;
}

QVariantMap ChatPainter::messageAt(qreal x, qreal y)
{
    qreal canvasY = y + m_scrollY;
    int idx = layoutIndexAtY(canvasY);
    if (idx < 0 || idx >= m_layouts.size()) return {};

    const auto &ml = m_layouts[idx];
    QVariantMap m;
    m["messageId"] = ml.messageId;
    m["isOwn"] = ml.isOwn;
    m["actorName"] = ml.actorName;
    m["messageText"] = ml.bodyHtml;
    m["hasFile"] = ml.hasFile;
    m["fileId"] = ml.fileId;
    m["fileName"] = ml.fileName;
    m["fileMime"] = ml.fileMime;
    return m;
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

void ChatPainter::rebuildAllLayouts()
{
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

    // Model is newest-first. We iterate oldest-first: row (count-1) down to row 0.
    for (int i = 0; i < count; ++i) {
        int modelRow = count - 1 - i;
        auto idx = m_model->index(modelRow);

        // Look up actual image aspect ratio if cached
        int fileId = m_model->data(idx, MessageListModel::FileIdRole).toInt();
        qreal aspect = m_previewAspect.value(fileId, 0.0);  // 0 = unknown, show compact placeholder

        auto ml = LayoutEngine::computeLayout(
            m_model, modelRow, width(), m_theme, y,
            m_myUserId, prevActorId, prevTimestamp, prevIsSystem, aspect
        );

        // Update prev for next iteration
        prevActorId = m_model->data(idx, MessageListModel::ActorIdRole).toString();
        prevTimestamp = m_model->data(idx, MessageListModel::TimestampRole).toLongLong();
        prevIsSystem = m_model->data(idx, MessageListModel::IsSystemRole).toBool();

        y += ml.totalHeight;
        m_layouts.append(ml);
    }

    y += PainterTheme::spacingLarge; // bottom margin

    bool wasAtBottom = atBottom();
    qreal oldH = m_contentHeight;
    m_contentHeight = y;
    clampScroll();

    if (!qFuzzyCompare(oldH, m_contentHeight))
        emit contentHeightChanged();

    // Auto-scroll to bottom if we were at bottom
    if (wasAtBottom)
        scrollToBottom();

    update();
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

void ChatPainter::onDataChanged(const QModelIndex &, const QModelIndex &)
{
    rebuildAllLayouts();
}

void ChatPainter::onModelReset()
{
    rebuildAllLayouts();
}

// ═══════════════════════════════════════════════════════
// Geometry change
// ═══════════════════════════════════════════════════════

void ChatPainter::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    rebuildAllLayouts();
    emit visibleHeightChanged();
}

bool ChatPainter::event(QEvent *e)
{
    if (e->type() == QEvent::HoverMove) {
        auto *he = static_cast<QHoverEvent *>(e);
        setHoveredPos(he->position().x(), he->position().y());
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
        m_pressCanvasPos = event->position();
        m_dragStartY = event->position().y();
        m_dragStartScroll = m_scrollY;
        // Save hit test at press time (hover index is valid now)
        m_pressHit = hitTestAt(event->position().x(), event->position().y());
        event->accept();
    }
}

void ChatPainter::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        qreal dy = m_dragStartY - event->position().y();
        if (qAbs(dy) > 4) m_dragMoved = true;
        if (m_dragMoved) {
            setScrollY(m_dragStartScroll + dy);
        }
        event->accept();
    } else {
        if (!m_selectionMode) {
            setHoveredPos(event->position().x(), event->position().y());
            QString hit = hitTestAt(event->position().x(), event->position().y());
            if (!hit.isEmpty())
                setCursor(Qt::PointingHandCursor);
            else
                setCursor(Qt::ArrowCursor);
        } else {
            setCursor(Qt::ArrowCursor);
        }
    }
}

void ChatPainter::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging) return;
    m_dragging = false;

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
        // Ctrl+Click: toggle selection (enter mode if needed)
        else if (event->modifiers() & Qt::ControlModifier) {
            qreal canvasY = event->position().y() + m_scrollY;
            int idx = layoutIndexAtY(canvasY);
            if (idx >= 0 && idx < m_layouts.size()) {
                const auto &ml = m_layouts[idx];
                if (!ml.isSystem && ml.messageId > 0) {
                    if (!m_selectionMode)
                        enterSelectionMode(ml.messageId);
                    else
                        toggleMessageSelection(ml.messageId);
                }
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

    // Right-click → context menu
    if (!m_dragMoved && event->button() == Qt::RightButton) {
        if (!m_selectionMode) {
            QVariantMap msg = messageAt(event->position().x(), event->position().y());
            if (msg.value("messageId").toInt() > 0) {
                emit contextMenuRequested(msg, mapToGlobal(event->position().toPoint()));
            }
        }
    }
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

    for (int i = 0; i < m_layouts.size(); ++i) {
        const auto &ml = m_layouts[i];

        if (ml.totalY + ml.totalHeight < vpTop || ml.totalY > vpBottom)
            continue;

        // Selection highlight — full-width teal tint
        if (m_selectionMode && m_selectedIds.contains(ml.messageId)) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(46, 196, 182, 30));
            p.drawRect(QRectF(0, ml.totalY + offsetY, width(), ml.totalHeight));
        }

        // Selection checkbox
        if (m_selectionMode && !ml.isSystem) {
            bool selected = m_selectedIds.contains(ml.messageId);
            qreal ckSize = 18;
            qreal ckY = ml.totalY + offsetY + (ml.totalHeight - ckSize) / 2.0;
            qreal ckX = ml.isOwn ? (width() - ckSize - 8) : 8;

            if (selected) {
                p.setPen(Qt::NoPen);
                p.setBrush(m_theme.accent);
                p.drawEllipse(QRectF(ckX, ckY, ckSize, ckSize));
                p.setPen(QPen(Qt::white, 2));
                QPointF c(ckX + ckSize / 2.0, ckY + ckSize / 2.0);
                p.drawLine(QPointF(c.x() - 4, c.y()), QPointF(c.x() - 1, c.y() + 3));
                p.drawLine(QPointF(c.x() - 1, c.y() + 3), QPointF(c.x() + 4, c.y() - 3));
            } else {
                p.setPen(QPen(QColor(85, 85, 85), 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(QRectF(ckX, ckY, ckSize, ckSize));
            }
        }

        if (ml.showDateSep)
            paintDateSep(&p, ml, offsetY);

        if (ml.isSystem)
            paintSystemMessage(&p, ml, offsetY);
        else if (ml.isOwn)
            paintOwnMessage(&p, ml, offsetY);
        else
            paintOtherMessage(&p, ml, offsetY);

        if (!m_selectionMode && i == m_hoveredIndex && !ml.isSystem
            && ml.sendStatus != QLatin1String("sending")
            && ml.sendStatus != QLatin1String("failed"))
            paintHoverBar(&p, ml, offsetY);
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
    // Bubble background
    QRectF bubble = ml.bubbleRect.translated(0, offsetY);
    p->setPen(Qt::NoPen);
    p->setBrush(m_theme.bgMessageOwn);
    p->drawRoundedRect(bubble, PainterTheme::radiusNormal, PainterTheme::radiusNormal);

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
        // Set default text color for QTextDocument
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.palette.setColor(QPalette::Text, m_theme.textPrimary);
        ctx.clip = QRectF(0, 0, bodyR.width(), bodyR.height());
        ml.bodyDoc->documentLayout()->draw(p, ctx);
        p->restore();
    }

    // Reactions
    if (!ml.reactions.isEmpty() && !ml.reactBarRect.isNull())
        paintReactions(p, ml, offsetY);

    // Timestamp + read status (draw once, not twice)
    {
        QRectF tr = ml.timeRect.translated(0, offsetY);
        QColor timeColor = m_darkMode
            ? QColor(255, 255, 255, 115)
            : m_theme.textTime;

        QString timeText = ml.sendStatus == QLatin1String("sending")
            ? QStringLiteral("Sending...") : ml.timeString;

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

    // Author name (non-grouped)
    if (!ml.isGrouped && !ml.nameRect.isNull()) {
        QRectF nr = ml.nameRect.translated(0, offsetY);
        p->setPen(PainterTheme::authorColor(ml.actorId));
        p->setFont(m_theme.nameFont());
        p->drawText(nr, Qt::AlignLeft | Qt::AlignVCenter, ml.actorName);

        // Time next to name
        QRectF tr = ml.timeRect.translated(0, offsetY);
        p->setPen(m_theme.textTime);
        p->setFont(m_theme.timeFont());
        p->drawText(tr, Qt::AlignLeft | Qt::AlignVCenter, ml.timeString);
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
        ml.bodyDoc->documentLayout()->draw(p, ctx);
        p->restore();
    }

    // Reactions
    if (!ml.reactions.isEmpty() && !ml.reactBarRect.isNull())
        paintReactions(p, ml, offsetY);

    // Grouped: time below body
    if (ml.isGrouped && !ml.timeRect.isNull()) {
        QRectF tr = ml.timeRect.translated(0, offsetY);
        p->setPen(m_theme.textTime);
        p->setFont(m_theme.timeFont());
        p->drawText(tr, Qt::AlignLeft | Qt::AlignVCenter, ml.timeString);
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

        // Filename text
        QRectF textR(iconR.right() + 4, fr.top(), fr.width() - 52, fr.height());
        p->setPen(m_theme.textPrimary);
        p->setFont(m_theme.bodyFont());
        QFontMetrics fm(m_theme.bodyFont());
        QString elided = fm.elidedText(ml.fileName, Qt::ElideMiddle, qRound(textR.width()));
        p->drawText(textR, Qt::AlignLeft | Qt::AlignVCenter, elided);
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
        p->setPen(m_theme.textPrimary);
        p->setFont(emojiFont);
        p->drawText(QRectF(x + pillPadX, bar.top(), emojiW, pillH),
                     Qt::AlignCenter, emoji);

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
    const qreal gap = 6;

    if (ml.isOwn) {
        // Left of bubble — single button width (reply only)
        qreal x = ml.bubbleRect.left() - gap - btnSize;
        qreal y = ml.bubbleRect.top() + (ml.bubbleRect.height() - btnSize) / 2.0;
        y = qBound(ml.totalY, y, ml.totalY + ml.totalHeight - btnSize);
        return QRectF(x, y, btnSize, btnSize);
    } else {
        // Right of actual content (uses dynamic contentRight)
        qreal x = ml.contentRight + gap;
        qreal refTop = ml.isGrouped ? ml.bodyRect.top() : (ml.nameRect.isNull() ? ml.bodyRect.top() : ml.nameRect.top());
        qreal refBottom = ml.bodyRect.bottom();
        qreal y = refTop + (refBottom - refTop - btnSize) / 2.0;
        y = qBound(ml.totalY, y, ml.totalY + ml.totalHeight - btnSize);
        return QRectF(x, y, btnSize, btnSize);
    }
}

QRectF ChatPainter::hoverBarReplyRect(const MessageLayout &ml) const
{
    if (ml.isOwn) {
        // Own messages: reply is the only button, takes the react position
        return hoverBarReactRect(ml);
    }
    QRectF reactR = hoverBarReactRect(ml);
    return QRectF(reactR.right() + 4, reactR.top(), reactR.width(), reactR.height());
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
            m_previewCache[fileId] = QImage();
            return;
        }

        QByteArray data = reply->readAll();
        QImage img;
        if (!img.loadFromData(data)) {
            m_previewCache[fileId] = QImage();
            return;
        }

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
