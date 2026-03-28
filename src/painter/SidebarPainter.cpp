#include "SidebarPainter.h"
#include "models/ConversationListModel.h"
#include "core/ApiClient.h"
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QNetworkReply>
#include <QDateTime>
#include <QFontMetrics>
#include <QtMath>

// ═══════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════

SidebarPainter::SidebarPainter(QWidget *parent)
    : QWidget(parent)
    , m_theme(m_darkMode, 1.0)
{
    setAttribute(Qt::WA_Hover);
    setMouseTracking(true);
}

// ═══════════════════════════════════════════════════════
// Properties
// ═══════════════════════════════════════════════════════

void SidebarPainter::setModel(ConversationListModel *mdl)
{
    if (mdl == m_model) return;

    if (m_model) {
        disconnect(m_model, nullptr, this, nullptr);
    }

    m_model = mdl;

    if (m_model) {
        connect(m_model, &QAbstractItemModel::dataChanged, this, &SidebarPainter::onDataChanged);
        connect(m_model, &QAbstractItemModel::modelReset, this, &SidebarPainter::onModelReset);
        connect(m_model, &QAbstractItemModel::rowsInserted, this, &SidebarPainter::onRowsInserted);
        connect(m_model, &QAbstractItemModel::rowsRemoved, this, &SidebarPainter::onRowsRemoved);
    }

    rebuildLayouts();
}

void SidebarPainter::setApi(ApiClient *api)
{
    if (api == m_api) return;
    m_api = api;
}

void SidebarPainter::setDarkMode(bool dark)
{
    if (m_darkMode == dark) return;
    m_darkMode = dark;
    m_theme = PainterTheme(m_darkMode, 1.0);
    update();
}

void SidebarPainter::setSelectedIndex(int idx)
{
    if (m_selectedIndex == idx) return;
    m_selectedIndex = idx;
    update();
    emit selectedIndexChanged();
}

void SidebarPainter::setSqueezed(bool sq)
{
    if (m_squeezed == sq) return;
    m_squeezed = sq;
    m_scrollY = 0;
    update();
}

void SidebarPainter::setFilterText(const QString &text)
{
    if (m_filterText == text) return;
    m_filterText = text;
    rebuildLayouts();
}

// ═══════════════════════════════════════════════════════
// Model signal handlers
// ═══════════════════════════════════════════════════════

void SidebarPainter::onDataChanged(const QModelIndex &, const QModelIndex &)
{
    rebuildLayouts();
}

void SidebarPainter::onModelReset()
{
    rebuildLayouts();
}

void SidebarPainter::onRowsInserted(const QModelIndex &, int, int)
{
    rebuildLayouts();
}

void SidebarPainter::onRowsRemoved(const QModelIndex &, int, int)
{
    rebuildLayouts();
}

// ═══════════════════════════════════════════════════════
// Layout
// ═══════════════════════════════════════════════════════

void SidebarPainter::rebuildLayouts()
{
    m_layouts.clear();
    m_visibleIndices.clear();

    if (!m_model) {
        update();
        return;
    }

    int count = m_model->rowCount();
    m_layouts.reserve(count);

    for (int i = 0; i < count; ++i) {
        QModelIndex idx = m_model->index(i);
        ConversationLayout cl;
        cl.token = m_model->data(idx, ConversationListModel::TokenRole).toString();
        cl.displayName = m_model->data(idx, ConversationListModel::DisplayNameRole).toString();
        cl.lastMessage = m_model->data(idx, ConversationListModel::LastMessageRole).toString();
        cl.lastAuthor = m_model->data(idx, ConversationListModel::LastAuthorRole).toString();
        cl.userStatus = m_model->data(idx, ConversationListModel::UserStatusRole).toString();
        cl.participantUserId = m_model->data(idx, ConversationListModel::ActorIdRole).toString();
        cl.conversationType = m_model->data(idx, ConversationListModel::TypeRole).toInt();
        cl.unreadCount = m_model->data(idx, ConversationListModel::UnreadCountRole).toInt();
        cl.unreadMention = m_model->data(idx, ConversationListModel::UnreadMentionRole).toBool();
        cl.isFavorite = m_model->data(idx, ConversationListModel::FavoriteRole).toBool();
        cl.lastActivity = m_model->data(idx, ConversationListModel::LastActivityRole).toLongLong();
        cl.notificationLevel = m_model->data(idx, ConversationListModel::NotificationLevelRole).toInt();

        // Compute time string
        if (cl.lastActivity > 0) {
            QDateTime dt = QDateTime::fromSecsSinceEpoch(cl.lastActivity);
            QDateTime now = QDateTime::currentDateTime();
            if (dt.date() == now.date()) {
                cl.timeString = dt.toString(QStringLiteral("HH:mm"));
            } else {
                QDate yesterday = now.date().addDays(-1);
                if (dt.date() == yesterday) {
                    cl.timeString = QStringLiteral("Yesterday");
                } else {
                    cl.timeString = dt.toString(QStringLiteral("dd MMM"));
                }
            }
        }

        // Compute preview text
        if (!cl.lastMessage.isEmpty()) {
            if (!cl.lastAuthor.isEmpty())
                cl.previewText = cl.lastAuthor + QStringLiteral(": ") + cl.lastMessage;
            else
                cl.previewText = cl.lastMessage;
        }

        m_layouts.append(cl);
    }

    // Build visible indices (filter)
    for (int i = 0; i < m_layouts.size(); ++i) {
        if (m_filterText.isEmpty() ||
            m_layouts[i].displayName.contains(m_filterText, Qt::CaseInsensitive)) {
            m_visibleIndices.append(i);
        }
    }

    clampScroll();
    update();
}

void SidebarPainter::clampScroll()
{
    int rowH = m_squeezed ? RowHeightSqueezed : RowHeight;
    qreal contentH = m_visibleIndices.size() * rowH;
    qreal maxScroll = qMax(0.0, contentH - height());
    m_scrollY = qBound(0.0, m_scrollY, maxScroll);
}

int SidebarPainter::rowAtY(qreal viewportY) const
{
    int rowH = m_squeezed ? RowHeightSqueezed : RowHeight;
    qreal canvasY = viewportY + m_scrollY;
    int row = static_cast<int>(canvasY / rowH);
    if (row < 0 || row >= m_visibleIndices.size())
        return -1;
    return row;
}

// ═══════════════════════════════════════════════════════
// Geometry
// ═══════════════════════════════════════════════════════

void SidebarPainter::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    clampScroll();
    update();
}

bool SidebarPainter::event(QEvent *e)
{
    if (e->type() == QEvent::HoverMove) {
        auto *he = static_cast<QHoverEvent *>(e);
        // Replicate hoverMoveEvent
        int row = rowAtY(he->position().y());
        if (row != m_hoveredRow) {
            m_hoveredRow = row;
            update();
        }
        return true;
    }
    if (e->type() == QEvent::HoverLeave) {
        if (m_hoveredRow != -1) {
            m_hoveredRow = -1;
            update();
        }
        return true;
    }
    return QWidget::event(e);
}

// ═══════════════════════════════════════════════════════
// Input handling
// ═══════════════════════════════════════════════════════

void SidebarPainter::wheelEvent(QWheelEvent *event)
{
    qreal delta = event->angleDelta().y();
    qreal scroll = -delta / 120.0 * 40.0;
    m_scrollY += scroll;
    clampScroll();
    update();
    event->accept();
}

void SidebarPainter::mousePressEvent(QMouseEvent *event)
{
    event->accept(); // claim the event so release fires
}

void SidebarPainter::mouseReleaseEvent(QMouseEvent *event)
{
    int row = rowAtY(event->position().y());
    if (row < 0 || row >= m_visibleIndices.size()) {
        event->accept();
        return;
    }

    int modelIdx = m_visibleIndices[row];
    const auto &cl = m_layouts[modelIdx];

    if (event->button() == Qt::RightButton) {
        // Map to global for context menu
        QPointF global = mapToGlobal(event->position());
        emit contextMenuRequested(modelIdx, cl.notificationLevel, global.x(), global.y());
    } else if (event->button() == Qt::LeftButton) {
        m_selectedIndex = modelIdx;
        emit selectedIndexChanged();
        emit conversationClicked(cl.token, cl.displayName, cl.participantUserId,
                                 cl.conversationType, cl.userStatus);
        update();
    }

    event->accept();
}

// hoverMoveEvent/hoverLeaveEvent handled in event() override

// ═══════════════════════════════════════════════════════
// PAINT
// ═══════════════════════════════════════════════════════

void SidebarPainter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QPainter *painter = &p;
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    // Background
    painter->fillRect(QRectF(0, 0, width(), height()), m_theme.bgSidebar);

    if (m_visibleIndices.isEmpty())
        return;

    int rowH = m_squeezed ? RowHeightSqueezed : RowHeight;
    qreal vpTop = m_scrollY;
    qreal vpBottom = m_scrollY + height();

    for (int vi = 0; vi < m_visibleIndices.size(); ++vi) {
        qreal rowTop = vi * rowH;
        qreal rowBottom = rowTop + rowH;

        // Skip if entirely outside viewport
        if (rowBottom < vpTop || rowTop > vpBottom)
            continue;

        int modelIdx = m_visibleIndices[vi];
        const auto &cl = m_layouts[modelIdx];

        if (m_squeezed)
            paintRowSqueezed(painter, cl, vi);
        else
            paintRowNormal(painter, cl, vi);
    }

    // Scrollbar
    paintScrollbar(painter);
}

// ═══════════════════════════════════════════════════════
// Normal row painting (expanded sidebar)
// ═══════════════════════════════════════════════════════

void SidebarPainter::paintRowNormal(QPainter *p, const ConversationLayout &cl, int visibleIdx)
{
    int rowH = RowHeight;
    qreal rowTop = visibleIdx * rowH - m_scrollY;
    qreal w = width();
    int modelIdx = m_visibleIndices[visibleIdx];

    // ── Background (selection / hover) ──
    bool selected = (modelIdx == m_selectedIndex);
    bool hovered = (visibleIdx == m_hoveredRow);

    if (selected) {
        p->fillRect(QRectF(0, rowTop, w, rowH), m_theme.bgSelected);
    } else if (hovered) {
        p->fillRect(QRectF(0, rowTop, w, rowH), m_theme.bgHover);
    }

    // ── Selection indicator bar ──
    if (selected) {
        p->setPen(Qt::NoPen);
        p->setBrush(m_theme.accent);
        qreal barH = rowH * 0.5;
        qreal barY = rowTop + (rowH - barH) / 2.0;
        p->drawRoundedRect(QRectF(0, barY, SelectionBarWidth, barH), 2, 2);
    }

    // ── Layout constants ──
    qreal padLeft = PainterTheme::spacingLarge;
    qreal padRight = PainterTheme::spacingNormal;
    qreal padTop = PainterTheme::spacingSmall;
    qreal avatarY = rowTop + (rowH - AvatarSize) / 2.0;
    qreal textLeft = padLeft + AvatarSize + PainterTheme::spacingNormal;
    qreal textRight = w - padRight;

    // ── Avatar ──
    QRectF avatarRect(padLeft, avatarY, AvatarSize, AvatarSize);
    paintAvatar(p, cl, avatarRect);

    // ── Online status dot (1:1 conversations only) ──
    if (cl.conversationType == 1 && cl.userStatus == QStringLiteral("online")) {
        qreal dotSize = StatusDotSize;
        qreal dotX = avatarRect.right() - dotSize + 1;
        qreal dotY = avatarRect.bottom() - dotSize + 1;

        // White border
        p->setPen(Qt::NoPen);
        p->setBrush(m_theme.bgSidebar);
        p->drawEllipse(QRectF(dotX - 1.5, dotY - 1.5, dotSize + 3, dotSize + 3));
        // Green dot
        p->setBrush(m_theme.online);
        p->drawEllipse(QRectF(dotX, dotY, dotSize, dotSize));
    }

    // ── Top row: [fav dot] Name ............ Time ──
    QFont nameFont;
    nameFont.setPixelSize(m_theme.fontSizeNormal);
    if (cl.unreadCount > 0)
        nameFont.setWeight(QFont::DemiBold);
    p->setFont(nameFont);

    QFont timeFont;
    timeFont.setPixelSize(m_theme.fontSizeTiny);
    QFontMetrics timeFM(timeFont);
    qreal timeW = timeFM.horizontalAdvance(cl.timeString);

    qreal nameY = rowTop + padTop;
    qreal nameLeft = textLeft;
    qreal nameRight = textRight - timeW - PainterTheme::spacingSmall;

    // Favorite dot
    if (cl.isFavorite) {
        p->setPen(Qt::NoPen);
        p->setBrush(m_theme.accent);
        qreal dotY2 = nameY + (m_theme.fontSizeNormal - FavDotSize) / 2.0 + 3;
        p->drawEllipse(QRectF(nameLeft, dotY2, FavDotSize, FavDotSize));
        nameLeft += FavDotSize + PainterTheme::spacingSmall;
    }

    // Display name (elided)
    QFontMetrics nameFM(nameFont);
    QString elidedName = nameFM.elidedText(cl.displayName, Qt::ElideRight,
                                            static_cast<int>(nameRight - nameLeft));
    p->setPen(m_theme.textPrimary);
    p->setFont(nameFont);
    p->drawText(QRectF(nameLeft, nameY, nameRight - nameLeft, m_theme.fontSizeNormal + 6),
                Qt::AlignLeft | Qt::AlignVCenter, elidedName);

    // Timestamp
    QColor timeColor = cl.unreadCount > 0 ? m_theme.accent : m_theme.textTime;
    p->setPen(timeColor);
    p->setFont(timeFont);
    p->drawText(QRectF(textRight - timeW, nameY, timeW, m_theme.fontSizeNormal + 6),
                Qt::AlignRight | Qt::AlignVCenter, cl.timeString);

    // ── Bottom row: Preview ............ [badge] [muted] ──
    qreal bottomY = nameY + m_theme.fontSizeNormal + 6 + 3;

    // Compute right-side width for badge + muted label
    qreal rightStuffW = 0;
    if (cl.unreadCount > 0) {
        QString countStr = cl.unreadCount > 99
            ? QStringLiteral("99+") : QString::number(cl.unreadCount);
        QFont badgeFont;
        badgeFont.setPixelSize(BadgeFontSize);
        badgeFont.setWeight(QFont::DemiBold);
        QFontMetrics bfm(badgeFont);
        qreal textW = bfm.horizontalAdvance(countStr);
        qreal badgeW = qMax(BadgeHeight * 1.0, textW + 10.0);
        rightStuffW = badgeW + PainterTheme::spacingSmall;
    }
    if (cl.notificationLevel == 3) {
        QFont mutedFont;
        mutedFont.setPixelSize(9);
        QFontMetrics mfm(mutedFont);
        rightStuffW += mfm.horizontalAdvance(QStringLiteral("Muted")) + PainterTheme::spacingSmall;
    }

    // Preview text
    QFont previewFont;
    previewFont.setPixelSize(m_theme.fontSizeSmall);
    QFontMetrics previewFM(previewFont);
    qreal previewRight = textRight - rightStuffW;
    QString elidedPreview = previewFM.elidedText(cl.previewText, Qt::ElideRight,
                                                  static_cast<int>(previewRight - textLeft));
    p->setPen(m_theme.textMuted);
    p->setFont(previewFont);
    p->drawText(QRectF(textLeft, bottomY, previewRight - textLeft, m_theme.fontSizeSmall + 4),
                Qt::AlignLeft | Qt::AlignVCenter, elidedPreview);

    // Unread badge
    if (cl.unreadCount > 0) {
        QRectF badgeArea(previewRight, bottomY - 1, textRight - previewRight, BadgeHeight);
        paintUnreadBadge(p, cl.unreadCount, cl.unreadMention, badgeArea);
    }

    // Muted label
    if (cl.notificationLevel == 3) {
        QFont mutedFont;
        mutedFont.setPixelSize(9);
        p->setPen(QColor(m_theme.textSecondary.red(), m_theme.textSecondary.green(),
                         m_theme.textSecondary.blue(), 153));  // 0.6 opacity
        p->setFont(mutedFont);
        p->drawText(QRectF(textRight - 30, bottomY, 30, m_theme.fontSizeSmall + 4),
                    Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("Muted"));
    }
}

// ═══════════════════════════════════════════════════════
// Squeezed row painting (avatar-only sidebar)
// ═══════════════════════════════════════════════════════

void SidebarPainter::paintRowSqueezed(QPainter *p, const ConversationLayout &cl, int visibleIdx)
{
    int rowH = RowHeightSqueezed;
    qreal rowTop = visibleIdx * rowH - m_scrollY;
    qreal w = width();
    int modelIdx = m_visibleIndices[visibleIdx];

    // Selection highlight
    bool selected = (modelIdx == m_selectedIndex);
    bool hovered = (visibleIdx == m_hoveredRow);
    if (selected) {
        p->fillRect(QRectF(0, rowTop, w, rowH), m_theme.bgSelected);
    } else if (hovered) {
        p->fillRect(QRectF(0, rowTop, w, rowH), m_theme.bgHover);
    }

    // Avatar centered
    qreal avatarX = (w - AvatarSizeSqueezed) / 2.0;
    qreal avatarY = rowTop + (rowH - AvatarSizeSqueezed) / 2.0;
    QRectF avatarRect(avatarX, avatarY, AvatarSizeSqueezed, AvatarSizeSqueezed);
    paintAvatar(p, cl, avatarRect);

    // Unread badge overlay (top-right of avatar)
    if (cl.unreadCount > 0) {
        QString countStr = cl.unreadCount > 99
            ? QStringLiteral("99+") : QString::number(cl.unreadCount);
        QFont badgeFont;
        badgeFont.setPixelSize(BadgeFontSize);
        badgeFont.setWeight(QFont::DemiBold);
        QFontMetrics bfm(badgeFont);
        qreal textW = bfm.horizontalAdvance(countStr);
        qreal badgeW = qMax(BadgeHeight * 1.0, textW + 10.0);
        qreal badgeX = avatarRect.right() - badgeW / 2.0;
        qreal badgeY = avatarRect.top() - BadgeHeight / 2.0 + 2;

        QColor bgColor = cl.unreadMention ? m_theme.danger : m_theme.accent;
        p->setPen(Qt::NoPen);
        p->setBrush(bgColor);
        p->drawRoundedRect(QRectF(badgeX, badgeY, badgeW, BadgeHeight),
                           BadgeHeight / 2.0, BadgeHeight / 2.0);

        p->setPen(Qt::white);
        p->setFont(badgeFont);
        p->drawText(QRectF(badgeX, badgeY, badgeW, BadgeHeight),
                    Qt::AlignCenter, countStr);
    }
}

// ═══════════════════════════════════════════════════════
// Avatar painting
// ═══════════════════════════════════════════════════════

void SidebarPainter::paintAvatar(QPainter *p, const ConversationLayout &cl, const QRectF &rect)
{
    int size = static_cast<int>(rect.width());
    QImage img = fetchAvatar(cl.participantUserId, cl.token, cl.conversationType, size);

    if (!img.isNull()) {
        p->drawImage(rect, img);
    } else {
        // Fallback: colored circle with initial
        QColor bgColor = PainterTheme::authorColor(
            cl.participantUserId.isEmpty() ? cl.displayName : cl.participantUserId);
        p->setPen(Qt::NoPen);
        p->setBrush(bgColor);
        p->drawEllipse(rect);

        // Initial letter
        QChar initial = cl.displayName.isEmpty() ? QChar('?') : cl.displayName.at(0).toUpper();
        QFont initFont;
        initFont.setPixelSize(size / 2);
        initFont.setWeight(QFont::DemiBold);
        p->setPen(Qt::white);
        p->setFont(initFont);
        p->drawText(rect, Qt::AlignCenter, QString(initial));
    }
}

// ═══════════════════════════════════════════════════════
// Unread badge
// ═══════════════════════════════════════════════════════

void SidebarPainter::paintUnreadBadge(QPainter *p, int count, bool mention, const QRectF &badgeArea)
{
    QString countStr = count > 99 ? QStringLiteral("99+") : QString::number(count);
    QFont badgeFont;
    badgeFont.setPixelSize(BadgeFontSize);
    badgeFont.setWeight(QFont::DemiBold);
    QFontMetrics bfm(badgeFont);
    qreal textW = bfm.horizontalAdvance(countStr);
    qreal badgeW = qMax(BadgeHeight * 1.0, textW + 10.0);

    // Right-align within badgeArea
    qreal badgeX = badgeArea.right() - badgeW;
    qreal badgeY = badgeArea.top() + (badgeArea.height() - BadgeHeight) / 2.0;

    QColor bgColor = mention ? m_theme.accent : m_theme.unreadBadge;
    p->setPen(Qt::NoPen);
    p->setBrush(bgColor);
    p->drawRoundedRect(QRectF(badgeX, badgeY, badgeW, BadgeHeight),
                       BadgeHeight / 2.0, BadgeHeight / 2.0);

    p->setPen(Qt::white);
    p->setFont(badgeFont);
    p->drawText(QRectF(badgeX, badgeY, badgeW, BadgeHeight), Qt::AlignCenter, countStr);
}

// ═══════════════════════════════════════════════════════
// Scrollbar
// ═══════════════════════════════════════════════════════

void SidebarPainter::paintScrollbar(QPainter *p)
{
    int rowH = m_squeezed ? RowHeightSqueezed : RowHeight;
    qreal contentH = m_visibleIndices.size() * rowH;
    if (contentH <= height())
        return; // no scrollbar needed

    qreal viewH = height();
    qreal thumbH = qMax(20.0, (viewH / contentH) * viewH);
    qreal maxScroll = contentH - viewH;
    qreal thumbY = (m_scrollY / maxScroll) * (viewH - thumbH);

    qreal scrollbarW = 4;
    qreal scrollbarX = width() - scrollbarW - 1;

    p->setPen(Qt::NoPen);
    QColor thumbColor = m_theme.textMuted;
    thumbColor.setAlphaF(0.3f);
    p->setBrush(thumbColor);
    p->drawRoundedRect(QRectF(scrollbarX, thumbY, scrollbarW, thumbH), 2, 2);
}

// ═══════════════════════════════════════════════════════
// Avatar image loading
// ═══════════════════════════════════════════════════════

QString SidebarPainter::avatarCacheKey(const QString &userId, const QString &token, int convType) const
{
    // For 1:1 chats, use the userId. For groups, use the token (room avatar).
    if (convType == 1 && !userId.isEmpty())
        return QStringLiteral("user:") + userId;
    return QStringLiteral("room:") + token;
}

QImage SidebarPainter::fetchAvatar(const QString &userId, const QString &token, int convType, int size)
{
    QString key = avatarCacheKey(userId, token, convType);
    auto it = m_avatarCache.find(key);
    if (it != m_avatarCache.end()) {
        // Return cached, but re-scale if size differs
        const QImage &cached = it.value();
        if (cached.isNull()) return cached;
        if (cached.width() == size) return cached;
        return cached.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    requestAvatar(userId, token, convType, size);
    return QImage();
}

void SidebarPainter::requestAvatar(const QString &userId, const QString &token, int convType, int size)
{
    QString key = avatarCacheKey(userId, token, convType);
    if (m_avatarPending.contains(key))
        return;
    if (!m_api)
        return;

    m_avatarPending.insert(key);

    // Build URL: user avatar for 1:1, room avatar for groups
    QString url;
    if (convType == 1 && !userId.isEmpty()) {
        url = QStringLiteral("/index.php/avatar/") + userId + QStringLiteral("/64");
    } else {
        url = QStringLiteral("/ocs/v2.php/apps/spreed/api/v1/room/") + token + QStringLiteral("/avatar?v=1");
    }

    QNetworkReply *reply = m_api->getAbsoluteUrl(url);

    connect(reply, &QNetworkReply::finished, this, [this, reply, key, size]() {
        reply->deleteLater();
        m_avatarPending.remove(key);

        if (reply->error() != QNetworkReply::NoError) {
            m_avatarCache[key] = QImage(); // cache empty so we don't retry
            return;
        }

        QByteArray data = reply->readAll();
        QImage img;
        if (!img.loadFromData(data)) {
            m_avatarCache[key] = QImage();
            return;
        }

        // Crop to circle
        QImage scaled = img.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        int cx = (scaled.width() - size) / 2;
        int cy = (scaled.height() - size) / 2;
        if (cx > 0 || cy > 0)
            scaled = scaled.copy(cx, cy, size, size);

        QImage result(size, size, QImage::Format_ARGB32_Premultiplied);
        result.fill(Qt::transparent);
        {
            QPainter painter(&result);
            painter.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            path.addEllipse(0, 0, size, size);
            painter.setClipPath(path);
            painter.drawImage(0, 0, scaled);
        }

        m_avatarCache[key] = result;
        update(); // repaint with loaded avatar
    });
}
