#include "ThreadsPainter.h"
#include "models/ThreadListModel.h"
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QFontMetrics>
#include <QCursor>
#include <QtMath>

// ===============================================================
// Constructor
// ===============================================================

ThreadsPainter::ThreadsPainter(QWidget *parent)
    : QWidget(parent)
    , m_theme(m_darkMode, 1.0)
{
    setAttribute(Qt::WA_Hover);
    setMouseTracking(true);
}

// ===============================================================
// Properties
// ===============================================================

void ThreadsPainter::setModel(ThreadListModel *mdl)
{
    if (mdl == m_model) return;

    if (m_model) {
        disconnect(m_model, nullptr, this, nullptr);
    }

    m_model = mdl;

    if (m_model) {
        connect(m_model, &QAbstractItemModel::dataChanged, this, &ThreadsPainter::onDataChanged);
        connect(m_model, &QAbstractItemModel::modelReset, this, &ThreadsPainter::onModelReset);
        connect(m_model, &QAbstractItemModel::rowsInserted, this, &ThreadsPainter::onRowsInserted);
        connect(m_model, &QAbstractItemModel::rowsRemoved, this, &ThreadsPainter::onRowsRemoved);
        connect(m_model, &ThreadListModel::loadingChanged, this, [this]() {
            m_modelLoading = m_model->isLoading();
            update();
        });
    }

    rebuildLayouts();
}

void ThreadsPainter::setDarkMode(bool dark)
{
    if (m_darkMode == dark) return;
    m_darkMode = dark;
    m_theme = PainterTheme(m_darkMode, 1.0);
    update();
}

void ThreadsPainter::setSelectedThreadId(int id)
{
    if (m_selectedThreadId == id) return;
    m_selectedThreadId = id;
    update();
}

void ThreadsPainter::setGroupName(const QString &name)
{
    if (m_groupName == name) return;
    m_groupName = name;
    update();
}

void ThreadsPainter::setCreating(bool c)
{
    if (m_creating == c) return;
    m_creating = c;
    update();
}

// ===============================================================
// Model signal handlers
// ===============================================================

void ThreadsPainter::onDataChanged(const QModelIndex &, const QModelIndex &)
{
    rebuildLayouts();
}

void ThreadsPainter::onModelReset()
{
    rebuildLayouts();
}

void ThreadsPainter::onRowsInserted(const QModelIndex &, int, int)
{
    rebuildLayouts();
}

void ThreadsPainter::onRowsRemoved(const QModelIndex &, int, int)
{
    rebuildLayouts();
}

// ===============================================================
// Layout
// ===============================================================

void ThreadsPainter::rebuildLayouts()
{
    m_layouts.clear();

    if (!m_model) {
        update();
        return;
    }

    m_modelLoading = m_model->isLoading();
    int count = m_model->rowCount();
    m_layouts.reserve(count);

    for (int i = 0; i < count; ++i) {
        QModelIndex idx = m_model->index(i);
        ThreadLayout tl;
        tl.threadId = m_model->data(idx, ThreadListModel::ThreadIdRole).toInt();
        tl.title = m_model->data(idx, ThreadListModel::TitleRole).toString();
        tl.lastMessage = m_model->data(idx, ThreadListModel::LastMessageRole).toString();
        tl.lastAuthor = m_model->data(idx, ThreadListModel::LastAuthorRole).toString();
        tl.lastActivity = m_model->data(idx, ThreadListModel::LastActivityRole).toLongLong();
        tl.replyCount = m_model->data(idx, ThreadListModel::ReplyCountRole).toInt();
        tl.iconColor = m_model->data(idx, ThreadListModel::IconColorRole).toInt();
        tl.unreadCount = m_model->data(idx, ThreadListModel::UnreadCountRole).toInt();
        tl.isAllMessages = m_model->data(idx, ThreadListModel::IsAllMessagesRole).toBool();

        tl.threadColor = PainterTheme::topicColor(tl.iconColor);
        tl.timeString = PainterTheme::formatRelativeTime(tl.lastActivity);
        tl.previewText = PainterTheme::formatPreviewText(tl.lastAuthor, tl.lastMessage);

        m_layouts.append(tl);
    }

    clampScroll();
    update();
}

void ThreadsPainter::clampScroll()
{
    qreal contentH = m_layouts.size() * RowHeight;
    qreal viewH = listBottom() - listTop();
    qreal maxScroll = qMax(0.0, contentH - viewH);
    m_scrollY = qBound(0.0, m_scrollY, maxScroll);
}

int ThreadsPainter::rowAtY(qreal viewportY) const
{
    // viewportY is in widget coordinates
    qreal listY = viewportY - listTop();
    if (listY < 0) return -1;

    qreal canvasY = listY + m_scrollY;
    int row = static_cast<int>(canvasY / RowHeight);
    if (row < 0 || row >= m_layouts.size())
        return -1;
    return row;
}

bool ThreadsPainter::isInNewTopicButton(qreal y) const
{
    return y >= height() - NewTopicHeight;
}

bool ThreadsPainter::isInBackButton(qreal x, qreal y) const
{
    return y < HeaderHeight && x < 40;
}

// ===============================================================
// Geometry
// ===============================================================

void ThreadsPainter::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    clampScroll();
    update();
}

// ===============================================================
// Input handling
// ===============================================================

void ThreadsPainter::wheelEvent(QWheelEvent *event)
{
    qreal delta = event->angleDelta().y();
    qreal scroll = -delta / 120.0 * 40.0;
    m_scrollY += scroll;
    clampScroll();
    update();
    event->accept();
}

void ThreadsPainter::mousePressEvent(QMouseEvent *event)
{
    event->accept();
}

void ThreadsPainter::mouseReleaseEvent(QMouseEvent *event)
{
    qreal mx = event->position().x();
    qreal my = event->position().y();

    // Back button
    if (isInBackButton(mx, my)) {
        emit backClicked();
        event->accept();
        return;
    }

    // New topic button
    if (isInNewTopicButton(my)) {
        emit newTopicClicked();
        event->accept();
        return;
    }

    // Thread row
    int row = rowAtY(my);
    if (row >= 0 && row < m_layouts.size()) {
        const auto &tl = m_layouts[row];
        m_selectedThreadId = tl.threadId;
        emit threadClicked(tl.threadId, tl.title);
        update();
    }

    event->accept();
}

bool ThreadsPainter::event(QEvent *e)
{
    if (e->type() == QEvent::HoverMove) {
        auto *he = static_cast<QHoverEvent *>(e);
        qreal mx = he->position().x();
        qreal my = he->position().y();

        int newHover;
        if (isInBackButton(mx, my)) {
            newHover = -3;
        } else if (isInNewTopicButton(my)) {
            newHover = -2;
        } else {
            newHover = rowAtY(my);
        }

        if (newHover != m_hoveredRow) {
            m_hoveredRow = newHover;
            if (newHover == -3 || newHover == -2 || newHover >= 0)
                setCursor(QCursor(Qt::PointingHandCursor));
            else
                setCursor(QCursor(Qt::ArrowCursor));
            update();
        }
        return true;
    }
    if (e->type() == QEvent::HoverLeave) {
        if (m_hoveredRow != -1) {
            m_hoveredRow = -1;
            setCursor(Qt::ArrowCursor);
            update();
        }
        return true;
    }
    return QWidget::event(e);
}

// ===============================================================
// PAINT
// ===============================================================

void ThreadsPainter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    p.fillRect(QRectF(0, 0, width(), height()), m_theme.bgSecondary);

    paintHeader(&p);

    if (m_layouts.isEmpty() && !m_modelLoading) {
        paintEmptyState(&p);
    } else {
        p.save();
        p.setClipRect(QRectF(0, listTop(), width(), listBottom() - listTop()));

        qreal vpTop = m_scrollY;
        qreal vpBottom = m_scrollY + (listBottom() - listTop());

        for (int i = 0; i < m_layouts.size(); ++i) {
            qreal rowTop = i * RowHeight;
            qreal rowBottom = rowTop + RowHeight;

            if (rowBottom < vpTop || rowTop > vpBottom)
                continue;

            paintRow(&p, m_layouts[i], i);
        }

        paintScrollbar(&p);
        p.restore();
    }

    if (m_modelLoading && m_layouts.isEmpty()) {
        QFont loadFont;
        loadFont.setPixelSize(m_theme.fontSizeSmall);
        p.setFont(loadFont);
        p.setPen(m_theme.textSecondary);
        qreal cy = (listTop() + listBottom()) / 2.0;
        p.drawText(QRectF(0, cy - 10, width(), 20), Qt::AlignCenter, QStringLiteral("Loading..."));
    }

    paintNewTopicButton(&p);
}

// ===============================================================
// Header
// ===============================================================

void ThreadsPainter::paintHeader(QPainter *p)
{
    // Background
    p->fillRect(QRectF(0, 0, width(), HeaderHeight), m_theme.bgSurface);

    QFont arrowFont;
    arrowFont.setPixelSize(16);
    arrowFont.setWeight(QFont::DemiBold);
    p->setFont(arrowFont);
    p->setPen(m_theme.accent);

    // Draw a simple left arrow using text
    QRectF backRect(PainterTheme::spacingSmall, 0, 28, HeaderHeight);
    p->drawText(backRect, Qt::AlignCenter, QStringLiteral("\u276E")); // ❮

    // Group name
    QFont nameFont;
    nameFont.setPixelSize(m_theme.fontSizeLarge);
    nameFont.setWeight(QFont::DemiBold);
    p->setFont(nameFont);
    p->setPen(m_theme.textPrimary);

    QString title = m_groupName.isEmpty() ? QStringLiteral("Topics") : m_groupName;
    qreal textLeft = PainterTheme::spacingSmall + 30;
    qreal textRight = width() - PainterTheme::spacingNormal;
    QFontMetrics fm(nameFont);
    QString elided = fm.elidedText(title, Qt::ElideRight, static_cast<int>(textRight - textLeft));
    p->drawText(QRectF(textLeft, 0, textRight - textLeft, HeaderHeight),
                Qt::AlignLeft | Qt::AlignVCenter, elided);

    // Bottom divider
    p->setPen(Qt::NoPen);
    p->setBrush(m_theme.divider);
    p->drawRect(QRectF(0, HeaderHeight - 1, width(), 1));
}

// ===============================================================
// Thread row
// ===============================================================

void ThreadsPainter::paintRow(QPainter *p, const ThreadLayout &tl, int rowIdx)
{
    qreal rowTop = rowIdx * RowHeight - m_scrollY + listTop();
    qreal w = width();

    bool selected = (tl.threadId == m_selectedThreadId);
    bool hovered = (rowIdx == m_hoveredRow);

    // Background
    if (selected) {
        // Tinted background using thread color at 10% alpha
        QColor tint = tl.threadColor;
        tint.setAlphaF(0.1f);
        p->fillRect(QRectF(0, rowTop, w, RowHeight), tint);
    } else if (hovered) {
        p->fillRect(QRectF(0, rowTop, w, RowHeight), m_theme.bgHover);
    }

    // Selection bar (left edge, thread colored)
    if (selected) {
        p->setPen(Qt::NoPen);
        p->setBrush(tl.threadColor);
        p->drawRect(QRectF(0, rowTop, SelectionBarWidth, RowHeight));
    }

    // Layout constants
    qreal padLeft = PainterTheme::spacingLarge;
    qreal padRight = PainterTheme::spacingNormal;
    qreal padTop = PainterTheme::spacingSmall;

    // Colored dot
    qreal dotY = rowTop + (RowHeight - DotSize) / 2.0;
    p->setPen(Qt::NoPen);
    p->setBrush(tl.threadColor);
    p->drawEllipse(QRectF(padLeft, dotY, DotSize, DotSize));

    qreal textLeft = padLeft + DotSize + PainterTheme::spacingNormal;
    qreal textRight = w - padRight;

    // Top line: Title .......... Time
    QFont titleFont;
    titleFont.setPixelSize(m_theme.fontSizeNormal);
    titleFont.setWeight(QFont::DemiBold);
    p->setFont(titleFont);

    QFont timeFont;
    timeFont.setPixelSize(m_theme.fontSizeTiny);
    QFontMetrics timeFM(timeFont);
    qreal timeW = timeFM.horizontalAdvance(tl.timeString);

    qreal titleY = rowTop + padTop;
    qreal titleRight = textRight - timeW - PainterTheme::spacingSmall;

    // Title (elided)
    QFontMetrics titleFM(titleFont);
    QString elidedTitle = titleFM.elidedText(tl.title, Qt::ElideRight,
                                              static_cast<int>(titleRight - textLeft));
    p->setPen(m_theme.textPrimary);
    p->setFont(titleFont);
    p->drawText(QRectF(textLeft, titleY, titleRight - textLeft, m_theme.fontSizeNormal + 6),
                Qt::AlignLeft | Qt::AlignVCenter, elidedTitle);

    // Timestamp
    p->setPen(m_theme.textTime);
    p->setFont(timeFont);
    p->drawText(QRectF(textRight - timeW, titleY, timeW, m_theme.fontSizeNormal + 6),
                Qt::AlignRight | Qt::AlignVCenter, tl.timeString);

    // Bottom line: Preview .......... [badge]
    qreal bottomY = titleY + m_theme.fontSizeNormal + 6 + 2;

    // Badge width
    qreal rightStuffW = 0;
    if (tl.unreadCount > 0 && !selected) {
        QString countStr = tl.unreadCount > 99
            ? QStringLiteral("99+") : QString::number(tl.unreadCount);
        QFont badgeFont;
        badgeFont.setPixelSize(BadgeFontSize);
        badgeFont.setWeight(QFont::DemiBold);
        QFontMetrics bfm(badgeFont);
        qreal textW2 = bfm.horizontalAdvance(countStr);
        qreal badgeW = qMax(BadgeHeight * 1.0, textW2 + 10.0);
        rightStuffW = badgeW + PainterTheme::spacingSmall;
    }

    // Preview text
    QFont previewFont;
    previewFont.setPixelSize(m_theme.fontSizeSmall);
    QFontMetrics previewFM(previewFont);
    qreal previewRight = textRight - rightStuffW;
    QString elidedPreview = previewFM.elidedText(tl.previewText, Qt::ElideRight,
                                                  static_cast<int>(previewRight - textLeft));
    p->setPen(m_theme.textMuted);
    p->setFont(previewFont);
    p->drawText(QRectF(textLeft, bottomY, previewRight - textLeft, m_theme.fontSizeSmall + 4),
                Qt::AlignLeft | Qt::AlignVCenter, elidedPreview);

    // Unread badge
    if (tl.unreadCount > 0 && !selected) {
        QRectF badgeArea(previewRight, bottomY - 1, textRight - previewRight, BadgeHeight);
        paintUnreadBadge(p, tl.unreadCount, tl.threadColor, badgeArea);
    }
}

// ===============================================================
// Unread badge
// ===============================================================

void ThreadsPainter::paintUnreadBadge(QPainter *p, int count, const QColor &color, const QRectF &badgeArea)
{
    QString countStr = count > 99 ? QStringLiteral("99+") : QString::number(count);
    QFont badgeFont;
    badgeFont.setPixelSize(BadgeFontSize);
    badgeFont.setWeight(QFont::DemiBold);
    QFontMetrics bfm(badgeFont);
    qreal textW = bfm.horizontalAdvance(countStr);
    qreal badgeW = qMax(BadgeHeight * 1.0, textW + 10.0);

    // Right-align within area
    qreal badgeX = badgeArea.right() - badgeW;
    qreal badgeY = badgeArea.top() + (badgeArea.height() - BadgeHeight) / 2.0;

    p->setPen(Qt::NoPen);
    p->setBrush(color);
    p->drawRoundedRect(QRectF(badgeX, badgeY, badgeW, BadgeHeight),
                       BadgeHeight / 2.0, BadgeHeight / 2.0);

    p->setPen(QColor("#000000"));
    p->setFont(badgeFont);
    p->drawText(QRectF(badgeX, badgeY, badgeW, BadgeHeight), Qt::AlignCenter, countStr);
}

// ===============================================================
// New Topic button
// ===============================================================

void ThreadsPainter::paintNewTopicButton(QPainter *p)
{
    qreal btnTop = height() - NewTopicHeight;

    // Top divider
    p->setPen(Qt::NoPen);
    p->setBrush(m_theme.divider);
    p->drawRect(QRectF(0, btnTop, width(), 1));

    // Background (hover)
    bool hovered = (m_hoveredRow == -2);
    if (hovered) {
        p->fillRect(QRectF(0, btnTop, width(), NewTopicHeight), m_theme.bgHover);
    }

    // "+" label
    QFont plusFont;
    plusFont.setPixelSize(18);
    plusFont.setWeight(QFont::Bold);
    p->setFont(plusFont);
    p->setPen(m_theme.accent);

    qreal plusLeft = PainterTheme::spacingLarge;
    QFontMetrics plusFM(plusFont);
    qreal plusW = plusFM.horizontalAdvance(QStringLiteral("+"));
    p->drawText(QRectF(plusLeft, btnTop, plusW + 4, NewTopicHeight),
                Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("+"));

    // "New Topic" text
    QFont labelFont;
    labelFont.setPixelSize(m_theme.fontSizeSmall);
    p->setFont(labelFont);
    p->setPen(m_theme.accent);
    p->drawText(QRectF(plusLeft + plusW + PainterTheme::spacingSmall, btnTop,
                        width() - plusLeft - plusW - PainterTheme::spacingSmall - PainterTheme::spacingLarge,
                        NewTopicHeight),
                Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("New Topic"));
}

// ===============================================================
// Empty state
// ===============================================================

void ThreadsPainter::paintEmptyState(QPainter *p)
{
    qreal cy = (listTop() + listBottom()) / 2.0;

    // Chat bubble emoji (circle background)
    qreal circleSize = 56;
    qreal circleX = (width() - circleSize) / 2.0;
    qreal circleY = cy - circleSize - 10;
    p->setPen(Qt::NoPen);
    p->setBrush(m_theme.bgSurface);
    p->drawEllipse(QRectF(circleX, circleY, circleSize, circleSize));

    QFont emojiFont;
    emojiFont.setPixelSize(24);
    p->setFont(emojiFont);
    p->setPen(m_theme.textPrimary);
    p->drawText(QRectF(circleX, circleY, circleSize, circleSize),
                Qt::AlignCenter, QStringLiteral("\U0001F4AC")); // speech bubble

    // "No topics yet"
    QFont titleFont;
    titleFont.setPixelSize(m_theme.fontSizeNormal);
    titleFont.setWeight(QFont::DemiBold);
    p->setFont(titleFont);
    p->setPen(m_theme.textPrimary);
    p->drawText(QRectF(0, cy - 4, width(), 24), Qt::AlignHCenter | Qt::AlignTop,
                QStringLiteral("No topics yet"));

    // "Reply to a message to start a thread"
    QFont subFont;
    subFont.setPixelSize(m_theme.fontSizeSmall);
    p->setFont(subFont);
    p->setPen(m_theme.textSecondary);
    p->drawText(QRectF(0, cy + 22, width(), 20), Qt::AlignHCenter | Qt::AlignTop,
                QStringLiteral("Reply to a message to start a thread"));
}

// ===============================================================
// Scrollbar
// ===============================================================

void ThreadsPainter::paintScrollbar(QPainter *p)
{
    qreal viewH = listBottom() - listTop();
    qreal contentH = m_layouts.size() * RowHeight;
    if (contentH <= viewH)
        return;

    qreal thumbH = qMax(20.0, (viewH / contentH) * viewH);
    qreal maxScroll = contentH - viewH;
    qreal thumbY = listTop() + (m_scrollY / maxScroll) * (viewH - thumbH);

    qreal scrollbarW = 4;
    qreal scrollbarX = width() - scrollbarW - 1;

    p->setPen(Qt::NoPen);
    QColor thumbColor = m_theme.textMuted;
    thumbColor.setAlphaF(0.3f);
    p->setBrush(thumbColor);
    p->drawRoundedRect(QRectF(scrollbarX, thumbY, scrollbarW, thumbH), 2, 2);
}
