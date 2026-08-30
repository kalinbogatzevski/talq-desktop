#include "SidebarPainter.h"
#include "models/ConversationListModel.h"
#include "core/ApiClient.h"
#include "core/SignalingClient.h"
#include "core/ShiftStatusService.h"
#include "EmojiTextRenderer.h"
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QHelpEvent>
#include <QToolTip>
#include <QNetworkReply>
#include <QFontMetrics>
#include <QtMath>
#include <QDateTime>
#include <algorithm>

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

qint64 SidebarPainter::avatarCacheBytes() const
{
    qint64 total = 0;
    for (auto it = m_avatarCache.cbegin(); it != m_avatarCache.cend(); ++it)
        total += it.value().sizeInBytes();
    return total;
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

void SidebarPainter::setShiftStatus(ShiftStatusService *shiftStatus)
{
    if (shiftStatus == m_shiftStatus) return;
    m_shiftStatus = shiftStatus;
    // Repaint when a batch lands, so markers appear without waiting for the
    // next model refresh.
    if (m_shiftStatus) {
        connect(m_shiftStatus, &ShiftStatusService::statusesChanged,
                this, qOverload<>(&QWidget::update));
    }
}

void SidebarPainter::setSignaling(SignalingClient *signaling)
{
    if (signaling == m_signaling) return;
    m_signaling = signaling;
    // Repaint when peer client info arrives so the TalQ badge appears
    // without waiting for the next model update.
    if (m_signaling) {
        connect(m_signaling, &SignalingClient::peerClientInfoChanged,
                this, [this]() { update(); });
    }
}

void SidebarPainter::setDarkMode(bool dark)
{
    if (m_darkMode == dark) return;
    m_darkMode = dark;
    m_theme = PainterTheme(m_darkMode, 1.0);
    update();
}

void SidebarPainter::setTheme(PainterTheme::Theme t)
{
    if (m_themeId == t) return;
    m_themeId = t;
    m_theme = PainterTheme(t, 1.0);
    update();
}

void SidebarPainter::setSelectedIndex(int idx)
{
    if (m_selectedIndex == idx) return;
    m_selectedIndex = idx;
    m_selectedToken = (idx >= 0 && idx < m_layouts.size()) ? m_layouts[idx].token : QString();
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

void SidebarPainter::setSortMode(int mode)
{
    if (m_sortMode == mode) return;
    m_sortMode = mode;
    rebuildLayouts();
}

void SidebarPainter::setFilterMode(int mode)
{
    if (m_filterMode == mode) return;
    m_filterMode = mode;
    rebuildLayouts();
}

void SidebarPainter::setTagFilter(const QString &tagId)
{
    if (m_tagFilterId == tagId) return;
    m_tagFilterId = tagId;
    rebuildLayouts();
}

QString SidebarPainter::tagFilterName() const
{
    return m_tagFilterName;
}

void SidebarPainter::setTagFilterName(const QString &name)
{
    // Display-only: the empty-state message names the tag the user picked
    // rather than showing its numeric id, which would mean nothing to them.
    if (m_tagFilterName == name) return;
    m_tagFilterName = name;
    update();
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
    // Preserve selection across rebuilds: save the selected token
    QString prevSelectedToken = m_selectedToken;
    if (prevSelectedToken.isEmpty() && m_selectedIndex >= 0 && m_selectedIndex < m_layouts.size())
        prevSelectedToken = m_layouts[m_selectedIndex].token;

    m_layouts.clear();
    m_rows.clear();
    // Conversation indices that survive the text/mode/tag filters, in display
    // order. buildVisibleRows() turns this into m_rows — flat, or split into
    // tag sections when grouping is on.
    QVector<int> matching;

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
        cl.userStatusMessage = m_model->data(idx, ConversationListModel::UserStatusMessageRole).toString();
        cl.userStatusIcon    = m_model->data(idx, ConversationListModel::UserStatusIconRole).toString();
        cl.participantUserId = m_model->data(idx, ConversationListModel::ActorIdRole).toString();
        cl.conversationType = m_model->data(idx, ConversationListModel::TypeRole).toInt();
        cl.unreadCount = m_model->data(idx, ConversationListModel::UnreadCountRole).toInt();
        cl.unreadMention = m_model->data(idx, ConversationListModel::UnreadMentionRole).toBool();
        cl.unreadMentionDirect = m_model->data(idx, ConversationListModel::UnreadMentionDirectRole).toBool();
        cl.isArchived = m_model->data(idx, ConversationListModel::ArchivedRole).toBool();
        cl.isFavorite = m_model->data(idx, ConversationListModel::FavoriteRole).toBool();
        cl.lastActivity = m_model->data(idx, ConversationListModel::LastActivityRole).toLongLong();
        cl.notificationLevel = m_model->data(idx, ConversationListModel::NotificationLevelRole).toInt();
        cl.tagIds = m_model->data(idx, ConversationListModel::TagIdsRole).toStringList();

        cl.timeString = PainterTheme::formatRelativeTime(cl.lastActivity);
        cl.previewText = PainterTheme::formatPreviewText(cl.lastAuthor, cl.lastMessage);

        m_layouts.append(cl);
    }

    // Build visible indices (text filter + filter mode)
    for (int i = 0; i < m_layouts.size(); ++i) {
        const ConversationLayout &cl = m_layouts[i];

        if (!m_filterText.isEmpty() &&
            !cl.displayName.contains(m_filterText, Qt::CaseInsensitive))
            continue;

        // Archived rooms are hidden from EVERY mode except the archived one.
        // Filtering them out here rather than per-case is deliberate: an
        // archived room must not reappear just because it is unread, a
        // favourite, or a group. That is the whole point of archiving it.
        if (cl.isArchived != (m_filterMode == FilterArchived))
            continue;

        bool modeOk = true;
        switch (m_filterMode) {
        case FilterUnread:    modeOk = cl.unreadCount > 0;          break;
        case FilterFavorites: modeOk = cl.isFavorite;               break;
        case FilterDirect:    modeOk = cl.conversationType == 1;    break;  // OneToOne
        case FilterGroups:    modeOk = cl.conversationType == 2
                                    || cl.conversationType == 3;    break;  // Group / Public
        case FilterArchived:  modeOk = true;                        break;  // already filtered above
        case FilterAll:
        default:              modeOk = true;                        break;
        }
        if (!modeOk)
            continue;

        // Talk 24 tag filter, applied on top of the built-in mode filter so
        // the two compose (e.g. "unread" AND tagged "Work"). The two "!"-
        // prefixed pseudo-tags mirror the server's special tag types:
        // favourites, and "Other" = carries no tags at all.
        if (!m_tagFilterId.isEmpty()) {
            bool tagOk;
            if (m_tagFilterId == QLatin1String("!favorites"))
                tagOk = cl.isFavorite;
            else if (m_tagFilterId == QLatin1String("!other"))
                tagOk = cl.tagIds.isEmpty();
            else
                tagOk = cl.tagIds.contains(m_tagFilterId);
            if (!tagOk)
                continue;
        }

        matching.append(i);
    }

    // Sort visible indices. Favorites are always grouped first (except when
    // already filtering to favorites), then the chosen sort key applies.
    const QVector<ConversationLayout> &L = m_layouts;
    // When grouping by tag is on, the Favourites SECTION does the favourites-
    // first job; keeping the flat favourites-float as well would pull
    // favourites to the top of every other section too.
    const bool groupFavs = (m_filterMode != FilterFavorites) && !m_groupByTag;
    const int sortMode = m_sortMode;
    std::stable_sort(matching.begin(), matching.end(),
                     [&L, groupFavs, sortMode](int a, int b) {
        const ConversationLayout &x = L[a];
        const ConversationLayout &y = L[b];
        if (groupFavs && x.isFavorite != y.isFavorite)
            return x.isFavorite;            // favorites first
        switch (sortMode) {
        case SortName:
            return x.displayName.localeAwareCompare(y.displayName) < 0;
        case SortUnread: {
            const bool xu = x.unreadCount > 0;
            const bool yu = y.unreadCount > 0;
            if (xu != yu) return xu;        // unread first
            if (xu && x.unreadCount != y.unreadCount)
                return x.unreadCount > y.unreadCount;
            return x.lastActivity > y.lastActivity;
        }
        case SortRecent:
        default:
            return x.lastActivity > y.lastActivity;
        }
    });

    buildVisibleRows(matching);

    // Restore selection: find the new index for the previously selected token
    if (!prevSelectedToken.isEmpty()) {
        m_selectedIndex = -1;
        for (int i = 0; i < m_layouts.size(); ++i) {
            if (m_layouts[i].token == prevSelectedToken) {
                m_selectedIndex = i;
                break;
            }
        }
        m_selectedToken = prevSelectedToken;
    }

    clampScroll();
    update();
}

void SidebarPainter::buildVisibleRows(const QVector<int> &matching)
{
    m_rows.clear();
    m_tagSectionCounts.clear();
    const int rowH = m_squeezed ? RowHeightSqueezed : RowHeight;

    // Grouping is off, no tags, or the icon rail (56 px — no room for a header
    // label): emit the flat list. This is byte-for-byte the 0.64 layout, and
    // sidebar-row-layout-test pins that the geometry still matches the old
    // `visibleIdx * rowH` arithmetic exactly.
    if (!m_groupByTag || m_tags.isEmpty() || m_squeezed) {
        m_rows.reserve(matching.size());
        for (int idx : matching) {
            talq::SidebarRow r;
            r.layoutIndex = idx;
            m_rows.push_back(r);
        }
        talq::assignRowGeometry(m_rows, rowH, HeaderHeight);
        return;
    }

    // Grouped. A conversation appears under EVERY section it belongs to, which
    // is how a favourite that is also tagged "Work" shows in both — matching
    // how favourites have always behaved here (pinned above, still listed).
    // Sections with no conversations are omitted entirely rather than rendered
    // empty; "Other" in particular is empty exactly when everything is tagged.
    for (int t = 0; t < m_tags.size(); ++t) {
        const SidebarTag &tag = m_tags[t];

        QVector<int> members;
        for (int idx : matching) {
            const ConversationLayout &cl = m_layouts[idx];
            bool in;
            if (tag.id == QLatin1String("!favorites"))
                in = cl.isFavorite;
            else if (tag.id == QLatin1String("!other"))
                in = cl.tagIds.isEmpty();
            else
                in = cl.tagIds.contains(tag.id);
            if (in)
                members.append(idx);
        }
        if (members.isEmpty())
            continue;
        m_tagSectionCounts.insert(tag.id, members.size());

        talq::SidebarRow h;
        h.layoutIndex = -1;
        h.tagIndex = t;
        m_rows.push_back(h);

        // A collapsed section keeps its header (that is how you re-open it)
        // but contributes no conversation rows.
        if (tag.collapsed)
            continue;
        for (int idx : members) {
            talq::SidebarRow r;
            r.layoutIndex = idx;
            r.tagIndex = t;
            m_rows.push_back(r);
        }
    }

    talq::assignRowGeometry(m_rows, rowH, HeaderHeight);
}

void SidebarPainter::setTags(const QVector<SidebarTag> &tags)
{
    m_tags = tags;
    if (m_groupByTag)
        rebuildLayouts();
}

void SidebarPainter::setGroupByTag(bool on)
{
    if (m_groupByTag == on) return;
    m_groupByTag = on;
    rebuildLayouts();
}

void SidebarPainter::clampScroll()
{
    qreal contentH = talq::totalHeight(m_rows);
    qreal maxScroll = qMax(0.0, contentH - height());
    m_scrollY = qBound(0.0, m_scrollY, maxScroll);
}

int SidebarPainter::rowAtY(qreal viewportY) const
{
    // No longer `canvasY / rowH`: with section headers the rows are two
    // different heights, and the division would silently return a wrong row.
    return talq::rowAtY(m_rows, viewportY + m_scrollY);
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
    if (e->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(e);
        int row = rowAtY(he->pos().y());
        if (row >= 0 && row < static_cast<int>(m_rows.size())
            && !m_rows[row].isHeader()) {
            const ConversationLayout &cl = m_layouts[m_rows[row].layoutIndex];
            if (cl.nameElided && cl.nameRect.contains(he->pos())) {
                QToolTip::showText(he->globalPos(), cl.displayName, this);
                return true;
            }
        }
        QToolTip::hideText();
        return true;
    }
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
    if (row < 0 || row >= static_cast<int>(m_rows.size())) {
        event->accept();
        return;
    }

    // A tag section header: left-click toggles the section. The new state is
    // applied optimistically AND reported so MainWindow can persist it
    // server-side — collapsed state is per-user server state, not local UI
    // state, so it must survive a restart and follow the user to other
    // devices. Right-click on a header does nothing (there is no per-section
    // context menu), rather than opening a conversation's menu.
    if (m_rows[row].isHeader()) {
        if (event->button() == Qt::LeftButton) {
            const int t = m_rows[row].tagIndex;
            if (t >= 0 && t < m_tags.size()) {
                m_tags[t].collapsed = !m_tags[t].collapsed;
                emit tagSectionToggled(m_tags[t].id, m_tags[t].collapsed);
                rebuildLayouts();
            }
        }
        event->accept();
        return;
    }

    int modelIdx = m_rows[row].layoutIndex;
    const auto &cl = m_layouts[modelIdx];

    if (event->button() == Qt::RightButton) {
        // Map to global for context menu
        QPointF global = mapToGlobal(event->position());
        emit contextMenuRequested(modelIdx, cl.notificationLevel, global.x(), global.y());
    } else if (event->button() == Qt::LeftButton) {
        m_selectedIndex = modelIdx;
        m_selectedToken = cl.token;
        emit selectedIndexChanged();
        emit conversationClicked(cl.token, cl.displayName, cl.participantUserId,
                                 cl.conversationType, cl.userStatus,
                                 cl.userStatusMessage, cl.userStatusIcon);
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
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    p.fillRect(QRectF(0, 0, width(), height()), m_theme.bgSidebar);

    if (m_rows.empty() && m_squeezed) {
        // Squeezed is a 56px icon-only rail (MainWindow.cpp:1861-1864) with no
        // room for a sentence — rows never show text there either. Stay a
        // bare panel rather than clip a message into an unreadable fragment.
        return;
    }

    if (m_rows.empty()) {
        // Name the reason the panel is bare — a text search finding nothing
        // reads differently from a filter (Unread/Favorites/Direct/Groups)
        // hiding everything, which reads differently from a genuinely empty
        // conversation list. Idiom copied from ThreadsPainter::paintEmptyState.
        QString msg;
        if (!m_filterText.isEmpty()) {
            msg = tr("No conversations match \"%1\"").arg(m_filterText);
        } else if (!m_tagFilterId.isEmpty()) {
            // A tag filter hiding everything reads differently again — and
            // naming the tag is the difference between "this is broken" and
            // "nothing is tagged that way yet".
            msg = m_tagFilterName.isEmpty()
                      ? tr("Nothing tagged here yet")
                      : tr("Nothing tagged “%1” yet").arg(m_tagFilterName);
        } else {
            switch (m_filterMode) {
            case FilterUnread:    msg = tr("No unread conversations"); break;
            case FilterFavorites: msg = tr("No favorites yet"); break;
            case FilterDirect:    msg = tr("No direct messages"); break;
            case FilterGroups:    msg = tr("No groups"); break;
            case FilterAll:
            default:              msg = tr("No conversations yet"); break;
            }
        }
        p.setPen(m_theme.textSecondary);
        QFont f = m_theme.bodyFont();
        p.setFont(f);
        // msg can embed m_filterText, which is unbounded user input (the
        // other branches are all fixed strings) — elide it against the
        // available width so a long search doesn't clip mid-glyph.
        qreal maxTextWidth = width() - 2 * PainterTheme::spacingLarge;
        QString elided = QFontMetrics(f).elidedText(msg, Qt::ElideRight,
                                                      static_cast<int>(maxTextWidth));
        p.drawText(QRectF(0, 0, width(), height()), Qt::AlignCenter, elided);
        return;
    }

    qreal vpTop = m_scrollY;
    qreal vpBottom = m_scrollY + height();

    for (int vi = 0; vi < static_cast<int>(m_rows.size()); ++vi) {
        const talq::SidebarRow &row = m_rows[vi];
        qreal rowTop = row.y;
        qreal rowBottom = rowTop + row.height;

        if (rowBottom < vpTop || rowTop > vpBottom)
            continue;

        if (row.isHeader()) {
            paintSectionHeader(&p, row);
            continue;
        }

        const auto &cl = m_layouts[row.layoutIndex];
        if (m_squeezed)
            paintRowSqueezed(&p, cl, vi);
        else
            paintRowNormal(&p, cl, vi);
    }

    paintScrollbar(&p);
}

// ═══════════════════════════════════════════════════════
// Normal row painting (expanded sidebar)
// ═══════════════════════════════════════════════════════

void SidebarPainter::paintRowNormal(QPainter *p, const ConversationLayout &cl, int visibleIdx)
{
    int rowH = RowHeight;
    qreal rowTop = m_rows[visibleIdx].y - m_scrollY;
    qreal w = width();
    int modelIdx = m_rows[visibleIdx].layoutIndex;

    // ── Background (selection / hover) ──
    bool selected = (modelIdx == m_selectedIndex);
    bool hovered = (visibleIdx == m_hoveredRow);

    if (selected) {
        p->fillRect(QRectF(0, rowTop, w, rowH), m_theme.bgSelected);
    } else if (hovered) {
        p->fillRect(QRectF(0, rowTop, w, rowH), m_theme.bgHover);
    }

    // ── Selection indicator bar (+ soft accent glow flavour) ──
    if (selected) {
        qreal barH = rowH * 0.5;
        qreal barY = rowTop + (rowH - barH) / 2.0;
        QLinearGradient gg(0, 0, 28, 0);
        QColor g0 = m_theme.glow; g0.setAlpha(46);
        QColor g1 = m_theme.glow; g1.setAlpha(0);
        gg.setColorAt(0, g0);
        gg.setColorAt(1, g1);
        p->setPen(Qt::NoPen);
        p->fillRect(QRectF(0, rowTop, 28, rowH), gg);
        p->setBrush(m_theme.accent);
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
    // ── Shift status ──
    // Every known state is marked, on-shift included. An earlier draft marked
    // only the exceptions, on the theory that badging the normal case is
    // noise -- but that made "on shift" and "we have no idea" look identical
    // in the list, which is the one question this feature exists to answer.
    //
    // It cannot be a corner badge shaped like the presence dot: the dot owns
    // bottom-right, the TalQ "Q" pill owns top-right, and `online` and
    // `success` are literally the same green in every theme. So this takes
    // the one free corner, bottom-LEFT, and is a rounded SQUARE -- position
    // and shape carry the distinction, not hue.
    //
    // Green here alongside a green presence dot is deliberate rather than
    // accidental: two greens means "at work AND reachable", which is exactly
    // the state you are scanning the list for.
    talq::ShiftState shiftState = talq::ShiftState::Unknown;
    if (m_shiftStatus && cl.conversationType == 1 && !cl.participantUserId.isEmpty())
        shiftState = m_shiftStatus->stateFor(cl.participantUserId);

    // Off-shift also dims the avatar. The marker says which state it is; the
    // dimming is what makes a row recede when you are scanning for someone to
    // ask right now.
    const bool dimForOffShift = (shiftState == talq::ShiftState::OffShift);
    if (dimForOffShift)
        p->setOpacity(0.55);
    paintAvatar(p, cl, avatarRect);
    if (dimForOffShift)
        p->setOpacity(1.0);

    if (talq::shiftStateIsDrawable(shiftState)) {
        QColor shiftColor;
        switch (shiftState) {
        case talq::ShiftState::OnShift:  shiftColor = m_theme.online;    break;
        case talq::ShiftState::OnBreak:  shiftColor = m_theme.amber;     break;
        case talq::ShiftState::OffShift: shiftColor = m_theme.textMuted; break;
        case talq::ShiftState::Unknown:  break;
        }
        const qreal s = StatusDotSize;
        const qreal x = avatarRect.left() - 1;
        const qreal y = avatarRect.bottom() - s + 1;
        // Ring it in the sidebar background, same as the presence dot, so it
        // reads against any avatar image behind it.
        p->setPen(Qt::NoPen);
        p->setBrush(m_theme.bgSidebar);
        p->drawRoundedRect(QRectF(x - 1.5, y - 1.5, s + 3, s + 3), 3.5, 3.5);
        p->setBrush(shiftColor);
        p->drawRoundedRect(QRectF(x, y, s, s), 2.5, 2.5);
    }

    // ── Status dot (1:1 conversations only, colored by presence) ──
    if (cl.conversationType == 1) {
        QColor dotColor;
        if (cl.userStatus == QStringLiteral("online")) dotColor = m_theme.online;
        else if (cl.userStatus == QStringLiteral("away")) dotColor = m_theme.amber;       // warm secondary
        else if (cl.userStatus == QStringLiteral("dnd")) dotColor = m_theme.danger;      // warm clay, not fire-engine
        if (dotColor.isValid()) {
            qreal dotSize = StatusDotSize;
            qreal dotX = avatarRect.right() - dotSize + 1;
            qreal dotY = avatarRect.bottom() - dotSize + 1;
            // Border ring
            p->setPen(Qt::NoPen);
            p->setBrush(m_theme.bgSidebar);
            p->drawEllipse(QRectF(dotX - 1.5, dotY - 1.5, dotSize + 3, dotSize + 3));
            // Colored dot
            p->setBrush(dotColor);
            p->drawEllipse(QRectF(dotX, dotY, dotSize, dotSize));
        }
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

    // Favorite dot — affinity, not a "needs you" signal. One-Signal Rule:
    // it must NOT use the teal accent (which competes with unread/active).
    if (cl.isFavorite) {
        p->setPen(Qt::NoPen);
        p->setBrush(m_theme.amber);   // warm "starred"
        qreal dotY2 = nameY + (m_theme.fontSizeNormal - FavDotSize) / 2.0 + 3;
        p->drawEllipse(QRectF(nameLeft, dotY2, FavDotSize, FavDotSize));
        nameLeft += FavDotSize + PainterTheme::spacingSmall;
    }

    // Display name (elided)
    QFontMetrics nameFM(nameFont);
    QString elidedName = nameFM.elidedText(cl.displayName, Qt::ElideRight,
                                            static_cast<int>(nameRight - nameLeft));
    const QRectF nameDrawRect(nameLeft, nameY, nameRight - nameLeft, m_theme.fontSizeNormal + 6);
    cl.nameRect = nameDrawRect;
    cl.nameElided = (elidedName != cl.displayName);
    p->setPen(m_theme.textPrimary);
    p->setFont(nameFont);
    p->drawText(nameDrawRect, Qt::AlignLeft | Qt::AlignVCenter, elidedName);

    // Timestamp
    // accentText, not accent: this is an 11px timestamp, and the fill-tuned
    // accent scores 2.85:1 on the selected row in the light theme.
    QColor timeColor = cl.unreadCount > 0 ? m_theme.accentText : m_theme.textTime;
    p->setPen(timeColor);
    p->setFont(timeFont);
    p->drawText(QRectF(textRight - timeW, nameY, timeW, m_theme.fontSizeNormal + 6),
                Qt::AlignRight | Qt::AlignVCenter, cl.timeString);

    // ── Bottom row: Preview ............ [badge] [muted] ──
    qreal bottomY = nameY + m_theme.fontSizeNormal + 6 + 3;

    // Defined once so the measured and drawn strings stay identical.
    const QString mutedLabel = tr("Muted");

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
    // Single-sourced: measured once here (both to reserve layout width and,
    // below, to size the actual draw rect) so a longer translation can never
    // clip against a width computed from a different font/string pairing.
    QFont mutedFont;
    mutedFont.setPixelSize(m_theme.fontSizeTiny);
    qreal mutedLabelW = 0;
    if (cl.notificationLevel == 3) {
        QFontMetrics mfm(mutedFont);
        mutedLabelW = mfm.horizontalAdvance(mutedLabel);
        rightStuffW += mutedLabelW + PainterTheme::spacingSmall;
    }

    // Preview text — textSecondary gives readable contrast against the warm
    // sidebar bg; textMuted is reserved for disabled-looking states.
    QFont previewFont;
    previewFont.setPixelSize(m_theme.fontSizeSmall);
    qreal previewRight = textRight - rightStuffW;
    p->setPen(m_theme.textSecondary);
    p->setFont(previewFont);
    EmojiTextRenderer::drawElided(p,
        QRectF(textLeft, bottomY, previewRight - textLeft, m_theme.fontSizeSmall + 4),
        cl.previewText,
        Qt::ElideRight);

    // Unread badge
    if (cl.unreadCount > 0) {
        QRectF badgeArea(previewRight, bottomY - 1, textRight - previewRight, BadgeHeight);
        paintUnreadBadge(p, cl.unreadCount, cl.unreadMention, cl.unreadMentionDirect, badgeArea);
    }

    // Muted label — full-alpha textMuted (the dedicated de-emphasized token,
    // not a 60%-tinted textSecondary — was ~2.5:1 on Paper, effectively
    // invisible), drawn into the width measured above rather than a
    // hardcoded 30px rect that clipped longer translations.
    if (cl.notificationLevel == 3) {
        p->setPen(m_theme.textMuted);
        p->setFont(mutedFont);
        p->drawText(QRectF(textRight - mutedLabelW, bottomY, mutedLabelW, m_theme.fontSizeSmall + 4),
                    Qt::AlignRight | Qt::AlignVCenter, mutedLabel);
    }
}

// ═══════════════════════════════════════════════════════
// Squeezed row painting (avatar-only sidebar)
// ═══════════════════════════════════════════════════════

void SidebarPainter::paintRowSqueezed(QPainter *p, const ConversationLayout &cl, int visibleIdx)
{
    int rowH = RowHeightSqueezed;
    qreal rowTop = m_rows[visibleIdx].y - m_scrollY;
    qreal w = width();
    int modelIdx = m_rows[visibleIdx].layoutIndex;

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

        // Three levels rather than two. `unreadMention` is set by an @all
        // just as much as by a direct @you, so before 0.65.3 a room-wide
        // announcement raised the same alarm-red badge as someone actually
        // asking you something -- which trains people to ignore the red one.
        //   accent = plain unread   amber = @all   danger = @you
        // Servers without `direct-mention-flag` never set the direct bit, so
        // every mention stays amber there rather than being wrongly promoted.
        QColor bgColor = cl.unreadMentionDirect ? m_theme.danger
                       : cl.unreadMention       ? m_theme.amber
                                                : m_theme.accent;
        p->setPen(Qt::NoPen);
        p->setBrush(bgColor);
        p->drawRoundedRect(QRectF(badgeX, badgeY, badgeW, BadgeHeight),
                           BadgeHeight / 2.0, BadgeHeight / 2.0);

        // Score the ink against the ACTUAL fill: this badge is accent for a
        // plain unread but danger for a mention, and controlInk is calibrated
        // for accent alone. Same idiom already used for author-colour fills.
        p->setPen(m_theme.inkOn(bgColor));
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
    // Note to self (type 6) — special bookmark icon
    if (cl.conversationType == 6) {
        const QColor fill = PainterTheme::authorColor(QStringLiteral("note-to-self"));  // in-palette, not gray
        p->setPen(Qt::NoPen);
        p->setBrush(fill);
        p->drawEllipse(rect);
        QFont iconFont;
        iconFont.setPixelSize(static_cast<int>(rect.width() * 0.5));
        p->setPen(m_theme.inkOn(fill));   // No-Gray: ink scored against the actual fill
        p->setFont(iconFont);
        p->drawText(rect, Qt::AlignCenter, QStringLiteral("\U0001F516"));  // 🔖
        return;
    }

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
        p->setPen(m_theme.inkOn(bgColor));   // No-Gray: ink scored against the actual fill
        p->setFont(initFont);
        p->drawText(rect, Qt::AlignCenter, QString(initial));
    }

    // TalQ marker: small "Q" pill on the top-right of the avatar when
    // this conversation's primary participant is known to be using TalQ.
    // Only meaningful for 1-on-1 chats (conversationType == 1).
    if (m_signaling && cl.conversationType == 1 && !cl.participantUserId.isEmpty()) {
        const QString info = m_signaling->peerClientInfo(cl.participantUserId);
        if (info.startsWith("TalQ")) {
            const qreal badgeSize = qMax(qreal(12), rect.width() * 0.35);
            // Top-right corner: the presence status dot occupies bottom-right.
            QRectF badge(rect.right() - badgeSize, rect.top(),
                         badgeSize, badgeSize);
            // Identity marker, not a "needs you" signal — must NOT use the
            // teal One-Signal accent nor the old foreign cobalt. In-palette
            // violet keeps it distinct and warm.
            p->setPen(QPen(m_theme.bgSidebar, qMax(1.0, badgeSize * 0.08)));
            p->setBrush(PainterTheme::topicColor(4));   // in-palette violet, per-theme
            p->drawEllipse(badge);

            QFont qFont;
            qFont.setPixelSize(int(badgeSize * 0.7));
            qFont.setWeight(QFont::Bold);
            // Same contrast bug slice D fixed for authorColor fills, via a
            // different palette: topicColor(4) is #9b7cd4, the SAME hex as
            // s_authorPalette[4], and controlInk measures ~3.31:1 against it
            // on Paper -- an AA fail. Score against the actual fill instead.
            p->setPen(m_theme.inkOn(PainterTheme::topicColor(4)));
            p->setFont(qFont);
            p->drawText(badge, Qt::AlignCenter, QStringLiteral("Q"));
        }
    }
}

// ═══════════════════════════════════════════════════════
// Unread badge
// ═══════════════════════════════════════════════════════

void SidebarPainter::paintUnreadBadge(QPainter *p, int count, bool mention, bool directMention,
                                      const QRectF &badgeArea)
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

    // Three levels, matching the full-row path: direct @you = clay (danger),
    // @all = amber, plain unread = teal. Before 0.65.3 an @all raised the same
    // alarm colour as a direct mention, which is how a red badge stops meaning
    // anything. Servers without `direct-mention-flag` never set the direct
    // bit, so mentions stay amber there rather than being wrongly promoted.
    QColor bgColor = directMention ? m_theme.danger
                   : mention       ? m_theme.amber
                                   : m_theme.unreadBadge;
    p->setPen(Qt::NoPen);
    p->setBrush(bgColor);
    p->drawRoundedRect(QRectF(badgeX, badgeY, badgeW, BadgeHeight),
                       BadgeHeight / 2.0, BadgeHeight / 2.0);

    p->setPen(m_theme.inkOn(bgColor));   // accent OR danger fill -- score it
    p->setFont(badgeFont);
    p->drawText(QRectF(badgeX, badgeY, badgeW, BadgeHeight), Qt::AlignCenter, countStr);
}

// ═══════════════════════════════════════════════════════
// Scrollbar
// ═══════════════════════════════════════════════════════

void SidebarPainter::paintSectionHeader(QPainter *p, const talq::SidebarRow &row)
{
    if (row.tagIndex < 0 || row.tagIndex >= m_tags.size())
        return;
    const SidebarTag &tag = m_tags[row.tagIndex];

    const qreal top = row.y - m_scrollY;
    const qreal w = width();
    const int padX = PainterTheme::spacingLarge;

    // No fill: the header reads as a label over the sidebar, not as a
    // selectable row. Painting a band here would make it look clickable in the
    // same way a conversation is, and the two do very different things.
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);

    // Disclosure triangle, rotated by state. Drawn rather than glyphed so it
    // matches at any DPI and needs no font that carries the character.
    const qreal cx = padX + 4;
    const qreal cy = top + row.height / 2.0;
    QPainterPath tri;
    if (tag.collapsed) {
        tri.moveTo(cx - 2.5, cy - 4);
        tri.lineTo(cx + 3.5, cy);
        tri.lineTo(cx - 2.5, cy + 4);
    } else {
        tri.moveTo(cx - 4, cy - 2.5);
        tri.lineTo(cx + 4, cy - 2.5);
        tri.lineTo(cx, cy + 3.5);
    }
    tri.closeSubpath();
    p->fillPath(tri, m_theme.textSecondary);

    // Eyebrow treatment, matching the app's section-label convention.
    QFont f = m_theme.bodyFont();
    f.setPointSizeF(f.pointSizeF() * 0.82);
    f.setWeight(QFont::DemiBold);
    f.setCapitalization(QFont::AllUppercase);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    p->setFont(f);
    p->setPen(m_theme.textSecondary);

    const qreal textX = cx + 12;
    const QFontMetrics fm(f);
    // Reserve room for the count so a long tag name elides against the count
    // rather than painting through it.
    const QString count = QString::number(m_tagSectionCounts.value(tag.id, 0));
    const qreal countW = fm.horizontalAdvance(count) + 8;
    const qreal availW = w - textX - padX - countW;
    const QString name = fm.elidedText(tag.name, Qt::ElideRight,
                                       static_cast<int>(qMax(0.0, availW)));
    p->drawText(QRectF(textX, top, availW, row.height),
                Qt::AlignVCenter | Qt::AlignLeft, name);

    // Count on the right. Its value is the section's TOTAL, so a collapsed
    // section still says how much is hidden under it.
    p->drawText(QRectF(w - padX - countW, top, countW, row.height),
                Qt::AlignVCenter | Qt::AlignRight, count);

    p->restore();
}

void SidebarPainter::paintScrollbar(QPainter *p)
{
    qreal contentH = talq::totalHeight(m_rows);
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

// Avatar cache freshness — see the matching note in ChatPainter.cpp. No HTTP
// cache exists, so the server always returns the current avatar; re-fetch once
// the cached copy ages past the TTL (shorter backoff for a failed fetch).
static constexpr qint64 kAvatarTtlMs      = 15 * 60 * 1000;  // 15 min
static constexpr qint64 kAvatarErrorTtlMs = 60 * 1000;       // 1 min

QImage SidebarPainter::fetchAvatar(const QString &userId, const QString &token, int convType, int size)
{
    QString key = avatarCacheKey(userId, token, convType);
    auto it = m_avatarCache.find(key);
    if (it != m_avatarCache.end()) {
        const QImage &cached = it.value();
        // Re-fetch in the background once the cached entry is stale so a
        // changed avatar appears without a restart; a failed (null) entry
        // retries on the shorter error backoff instead of staying blank.
        const qint64 age = QDateTime::currentMSecsSinceEpoch()
                           - m_avatarFetchedMs.value(key, 0);
        const qint64 ttl = cached.isNull() ? kAvatarErrorTtlMs : kAvatarTtlMs;
        if (age >= ttl)
            requestAvatar(userId, token, convType, size);
        // Return cached, but re-scale if size differs
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
        // Cache-buster: a hardcoded ?v=1 never changes, so any HTTP cache in the
        // path (reverse proxy / Nextcloud) keeps serving the OLD room avatar even
        // after it's changed. Stamp a fresh value so each fetch bypasses caches and
        // returns the CURRENT avatar (paired with invalidateAvatars() on roomChanged).
        url = QStringLiteral("/ocs/v2.php/apps/spreed/api/v1/room/") + token
              + QStringLiteral("/avatar?v=")
              + QString::number(QDateTime::currentSecsSinceEpoch());
    }

    QNetworkReply *reply = m_api->getAbsoluteUrl(url);

    connect(reply, &QNetworkReply::finished, this, [this, reply, key, size]() {
        reply->deleteLater();
        m_avatarPending.remove(key);
        // Stamp fetch time (success OR failure) so the TTL/backoff in
        // fetchAvatar drives the next retry instead of caching forever.
        m_avatarFetchedMs[key] = QDateTime::currentMSecsSinceEpoch();

        if (reply->error() != QNetworkReply::NoError) {
            m_avatarCache[key] = QImage(); // short error TTL lets it retry
            return;
        }

        QByteArray data = reply->readAll();
        QImage img;
        if (!img.loadFromData(data)) {
            m_avatarCache[key] = QImage();
            return;
        }

        m_avatarCache[key] = PainterTheme::cropToCircle(img, size);
        qInfo().nospace() << "SidebarPainter: avatar fetched key=" << key
                          << " bytes=" << data.size();
        update(); // repaint with loaded avatar
    });
}
