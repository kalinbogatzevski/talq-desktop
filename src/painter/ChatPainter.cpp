#include "ChatPainter.h"
#include "LayoutEngine.h"
#include "models/MessageListModel.h"
#include <QPainter>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QTextDocument>
#include <QAbstractTextDocumentLayout>
#include <QRegularExpression>
#include <QtMath>

ChatPainter::ChatPainter(QQuickItem *parent)
    : QQuickPaintedItem(parent)
    , m_theme(m_darkMode, m_fontScale)
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setFlag(ItemAcceptsInputMethod, false);

    // RenderTarget: default (Image) is fine for Phase 3
}

// ═══════════════════════════════════════════════════════
// Properties
// ═══════════════════════════════════════════════════════

QObject *ChatPainter::modelObject() const
{
    return m_model;
}

void ChatPainter::setModelObject(QObject *obj)
{
    auto *mdl = qobject_cast<MessageListModel *>(obj);
    if (mdl == m_model)
        return;

    // Disconnect old model
    if (m_model) {
        disconnect(m_model, nullptr, this, nullptr);
    }

    m_model = mdl;

    // Connect new model
    if (m_model) {
        connect(m_model, &QAbstractItemModel::rowsInserted, this, &ChatPainter::onRowsInserted);
        connect(m_model, &QAbstractItemModel::rowsRemoved, this, &ChatPainter::onRowsRemoved);
        connect(m_model, &QAbstractItemModel::dataChanged, this, &ChatPainter::onDataChanged);
        connect(m_model, &QAbstractItemModel::modelReset, this, &ChatPainter::onModelReset);
    }

    rebuildAllLayouts();
    emit modelChanged();
}

void ChatPainter::setMyUserId(const QString &id)
{
    if (m_myUserId == id) return;
    m_myUserId = id;
    rebuildAllLayouts();
    emit myUserIdChanged();
}

void ChatPainter::setDarkMode(bool dark)
{
    if (m_darkMode == dark) return;
    m_darkMode = dark;
    m_theme = PainterTheme(m_darkMode, m_fontScale);
    rebuildAllLayouts();
    emit darkModeChanged();
}

void ChatPainter::setFontScale(qreal scale)
{
    if (qFuzzyCompare(m_fontScale, scale)) return;
    m_fontScale = scale;
    m_theme = PainterTheme(m_darkMode, m_fontScale);
    rebuildAllLayouts();
    emit fontScaleChanged();
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

        auto ml = LayoutEngine::computeLayout(
            m_model, modelRow, width(), m_theme, y,
            m_myUserId, prevActorId, prevTimestamp, prevIsSystem
        );

        // Update prev for next iteration
        auto idx = m_model->index(modelRow);
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

void ChatPainter::geometryChange(const QRectF &newGeom, const QRectF &oldGeom)
{
    QQuickPaintedItem::geometryChange(newGeom, oldGeom);
    if (newGeom.width() != oldGeom.width() || newGeom.height() != oldGeom.height()) {
        rebuildAllLayouts();
        emit visibleHeightChanged();
    }
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
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragStartY = event->position().y();
        m_dragStartScroll = m_scrollY;
        event->accept();
    }
}

void ChatPainter::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        qreal dy = m_dragStartY - event->position().y();
        setScrollY(m_dragStartScroll + dy);
        event->accept();
    }
}

void ChatPainter::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        event->accept();
    }
}

void ChatPainter::hoverMoveEvent(QHoverEvent *event)
{
    qreal viewY = event->position().y() + m_scrollY;
    int idx = layoutIndexAtY(viewY);
    if (idx != m_hoveredIndex) {
        m_hoveredIndex = idx;
        emit hoveredIndexChanged();
    }
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

void ChatPainter::paint(QPainter *painter)
{
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    // Background
    painter->fillRect(QRectF(0, 0, width(), height()), m_theme.bgPrimary);

    if (m_layouts.isEmpty())
        return;

    // Viewport culling: only paint messages that overlap [scrollY, scrollY + height]
    qreal vpTop = m_scrollY;
    qreal vpBottom = m_scrollY + height();

    for (int i = 0; i < m_layouts.size(); ++i) {
        const auto &ml = m_layouts[i];
        qreal msgTop = ml.totalY;
        qreal msgBottom = ml.totalY + ml.totalHeight;

        // Skip if entirely above or below viewport
        if (msgBottom < vpTop || msgTop > vpBottom)
            continue;

        qreal offsetY = -m_scrollY;

        // Date separator
        if (ml.showDateSep)
            paintDateSep(painter, ml, offsetY);

        // Message content
        if (ml.isSystem)
            paintSystemMessage(painter, ml, offsetY);
        else if (ml.isOwn)
            paintOwnMessage(painter, ml, offsetY);
        else
            paintOtherMessage(painter, ml, offsetY);
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

    // Timestamp + read status
    {
        QRectF tr = ml.timeRect.translated(0, offsetY);
        QColor timeColor = m_darkMode
            ? QColor(255, 255, 255, 115)  // rgba(1,1,1,0.45)
            : m_theme.textTime;

        if (ml.sendStatus == QLatin1String("failed"))
            timeColor = m_theme.danger;

        p->setPen(timeColor);
        p->setFont(m_theme.timeFont());

        QString timeText = ml.sendStatus == QLatin1String("sending")
            ? QStringLiteral("Sending...") : ml.timeString;
        p->drawText(tr, Qt::AlignRight | Qt::AlignVCenter, timeText);

        // Read status indicator
        if (ml.sendStatus != QLatin1String("sending")) {
            QFont statusFont = m_theme.timeFont();
            statusFont.setPixelSize(ml.sendStatus == QLatin1String("failed") ? 12 : 9);
            p->setFont(statusFont);

            QColor statusColor = timeColor;
            QString statusChar;
            if (ml.sendStatus == QLatin1String("failed")) {
                statusColor = m_theme.danger;
                statusChar = QStringLiteral("\u26A0"); // warning
            } else if (ml.isRead) {
                statusColor = m_theme.accent;
                statusChar = QStringLiteral("\u25C9"); // filled circle
            } else {
                statusChar = QStringLiteral("\u25CB"); // empty circle
            }

            p->setPen(statusColor);
            QFontMetrics fm(p->font());
            int sw = fm.horizontalAdvance(statusChar);
            QRectF sr(tr.right() - sw, tr.top(), sw, tr.height());
            // Shift time text left to make room
            QRectF tr2 = tr.adjusted(0, 0, -(sw + 4), 0);
            p->setPen(timeColor);
            p->setFont(m_theme.timeFont());
            p->drawText(tr2, Qt::AlignRight | Qt::AlignVCenter, timeText);
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
        QColor avatarColor = PainterTheme::authorColor(ml.actorId);

        // Colored circle
        p->setPen(Qt::NoPen);
        p->setBrush(avatarColor);
        p->drawEllipse(ar);

        // First letter
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
