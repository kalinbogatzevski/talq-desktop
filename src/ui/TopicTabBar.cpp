#include "TopicTabBar.h"

#include "models/ThreadListModel.h"

#include <QAbstractItemModel>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {
struct Pal {
    QString accent, onAccent, barBg, chipBg, border, ink, inkDim, hoverBg, accentSoft, accentSoftInk, unreadBadge, badgeInk;
};
// bug 10 — source chip colors DIRECTLY from PainterTheme tokens (the single
// source of truth every other chrome widget uses), NOT from the QApplication
// QPalette. The old QPalette path stringified roles with HexRgb (dropping
// alpha) and relied on stylesheet-polished palette propagation, which made the
// labels collapse to #000000 in the light (Paper) theme — black, unreadable.
Pal pal(PainterTheme::Theme t)
{
    const PainterTheme th(t, 1.0);
    auto n = [](const QColor &c){ return c.name(QColor::HexRgb); };
    // Pre-blend the accent at low strength over the bar background → a calm,
    // solid tint (no alpha needed in the stylesheet). Used for the SELECTED
    // chip so it reads as "active" without a loud saturated fill.
    auto blend = [](const QColor &fg, const QColor &bg, double a) {
        return QColor(int(fg.red()   * a + bg.red()   * (1 - a)),
                      int(fg.green() * a + bg.green() * (1 - a)),
                      int(fg.blue()  * a + bg.blue()  * (1 - a)));
    };
    return { n(th.accent),         // accent (selected chip TEXT + border now)
             n(th.controlInk),     // on-accent ink (legacy; unused by the calm style)
             n(th.bgSecondary),    // bar background
             n(th.bgSurface),      // unselected chip background
             n(th.divider),        // border
             n(th.textPrimary),    // hover text
             n(th.textSecondary),  // unselected chip TEXT — was black in light theme
             n(th.bgHover),        // hover background
             n(th.accentSoft),     // calm selected tint — now a PainterTheme token
             n(th.accentSoftInk),  // and the ink that actually reads on it
             n(th.unreadBadge),    // unread-badge fill, same token as SidebarPainter
             n(th.inkOn(th.unreadBadge)) };  // its ink: scored per theme, as the sidebar badge
}

// The mark of a topic whose notifications are off: a bell with a slash through it,
// drawn on a 14x14 grid in the chip's own ink. A painted shape, not a glyph or an
// emoji: a glyph's size and baseline change with the font (an emoji would also be
// drawn in colour, by a different engine than the rest of the chip).
QPixmap mutedBellPixmap(const QColor &ink, int size, qreal dpr)
{
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 14.0, size / 14.0);
    p.setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    QPainterPath bell;
    bell.moveTo(2.8, 10.2);
    bell.cubicTo(4.0, 9.2, 4.3, 8.0, 4.3, 6.4);
    bell.cubicTo(4.3, 4.5, 5.5, 3.2, 7.0, 3.2);
    bell.cubicTo(8.5, 3.2, 9.7, 4.5, 9.7, 6.4);
    bell.cubicTo(9.7, 8.0, 10.0, 9.2, 11.2, 10.2);
    bell.closeSubpath();
    p.drawPath(bell);
    p.drawArc(QRectF(5.6, 10.6, 2.8, 2.2), 180 * 16, 180 * 16);   // the clapper
    p.drawLine(QPointF(2.2, 2.0), QPointF(11.8, 12.2));            // the slash
    return pm;
}

// QMessageBox button styling now lives once in the global app sheet
// (AppStyle::sheet → "QMessageBox QPushButton"), so the boxes here inherit it
// like every other message box. No per-box stylesheet needed.
} // namespace

TopicTabBar::TopicTabBar(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(48);
    applyBarChrome();

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(m_scroll, 1);

    m_row = new QWidget(m_scroll);
    m_rowLayout = new QHBoxLayout(m_row);
    m_rowLayout->setContentsMargins(14, 0, 14, 0);
    m_rowLayout->setSpacing(6);
    m_rowLayout->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    m_rowLayout->addStretch();
    m_scroll->setWidget(m_row);
    // Vertical wheel → horizontal scroll over the strip (see eventFilter).
    m_scroll->viewport()->installEventFilter(this);

    hide();
}

void TopicTabBar::applyBarChrome()
{
    const Pal c = pal(m_themeId);
    setStyleSheet(QStringLiteral(
        "TopicTabBar { background: %1; border-bottom: 1px solid %2; }"
        "QScrollArea, QScrollArea > QWidget, QScrollArea > QWidget > QWidget {"
        "  background: transparent; border: none; }"
        "QScrollBar:horizontal { height: 4px; background: transparent; margin: 0; }"
        "QScrollBar::handle:horizontal { background: %2; border-radius: 2px;"
        "  min-width: 24px; }"
        "QScrollBar::handle:horizontal:hover { background: %3; }"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }"
    ).arg(c.barBg, c.border, c.inkDim));
}

void TopicTabBar::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    // ApplicationPaletteChange only — PaletteChange recurses via setStyleSheet.
    if (e->type() == QEvent::ApplicationPaletteChange
        || e->type() == QEvent::ThemeChange) {
        applyBarChrome();
        rebuild();
    }
}

bool TopicTabBar::eventFilter(QObject *watched, QEvent *e)
{
    if (m_scroll && watched == m_scroll->viewport()
        && e->type() == QEvent::Wheel) {
        QScrollBar *h = m_scroll->horizontalScrollBar();
        if (h && h->maximum() > h->minimum()) {
            auto *we = static_cast<QWheelEvent *>(e);
            // Honour a real horizontal wheel; otherwise map the vertical wheel
            // onto horizontal travel so the strip scrolls under a normal mouse.
            const QPoint d = we->angleDelta();
            const int delta = d.x() != 0 ? d.x() : d.y();
            if (delta != 0) {
                h->setValue(h->value() - delta);
                return true;   // consumed — don't let the area swallow it
            }
        }
    }
    return QWidget::eventFilter(watched, e);
}

void TopicTabBar::setModel(ThreadListModel *model)
{
    if (m_model == model) return;
    if (m_model) disconnect(m_model, nullptr, this, nullptr);
    m_model = model;
    if (m_model) {
        connect(m_model, &QAbstractItemModel::modelReset,    this, &TopicTabBar::rebuild);
        connect(m_model, &QAbstractItemModel::rowsInserted,  this, &TopicTabBar::rebuild);
        connect(m_model, &QAbstractItemModel::rowsRemoved,   this, &TopicTabBar::rebuild);
        connect(m_model, &QAbstractItemModel::dataChanged,   this, &TopicTabBar::rebuild);
        // Report a partial topic delete (some messages couldn't be removed).
        connect(m_model, &ThreadListModel::topicDeleteFinished, this,
                [this](int, int deleted, int failed) {
            if (failed <= 0)
                return;
            QMessageBox box(QMessageBox::Information, tr("Delete topic"),
                tr("Deleted %1 message(s); %2 could not be deleted. You can "
                   "only delete your own messages (within the edit window), "
                   "or others' if you moderate this conversation.")
                   .arg(deleted).arg(failed),
                QMessageBox::Ok, this);
            box.exec();
        });
    }
    rebuild();
}

void TopicTabBar::setSelectedThreadId(int threadId)
{
    if (m_selectedThreadId == threadId) return;
    m_selectedThreadId = threadId;
    rebuild();
}

void TopicTabBar::setTheme(PainterTheme::Theme t)
{
    if (m_themeId == t) return;   // first call from a default-constructed bar still applies
    m_themeId = t;
    applyBarChrome();
    rebuild();
}

void TopicTabBar::setCountMuted(bool on)
{
    if (m_countMuted == on) return;
    m_countMuted = on;
    rebuild();
}

QWidget *TopicTabBar::makeChip(const QString &label, int threadId,
                               int unreadCount, bool active, int notifLevel)
{
    auto *b = new QPushButton(m_row);
    const bool hasUnread = unreadCount > 0;
    // Only a real topic has a level of its own. Never (3) is the silenced topic;
    // mentions only (2) is half silenced. Both are marked, so the strip tells you
    // which topics will not pop up, and a silenced one is quiet: no loud unread style.
    const bool silenced     = threadId > 0 && notifLevel == 3;
    const bool mentionsOnly = threadId > 0 && notifLevel == 2;
    const bool marked       = silenced || mentionsOnly;
    // A silenced topic shows no number unless muted topics are counted (the mark says it is silenced).
    const bool showCount    = hasUnread && (!silenced || m_countMuted);

    // The unread count lives INSIDE the chip, after the title: one object per
    // topic, so the count can never be read as belonging to the neighbouring
    // chip. The capsule sits in the chip's right padding, inset kCountInset
    // from the chip edge on every side the height allows, so its round ends
    // nest inside the chip's (radius 16 around radius 9).
    constexpr int kChipH = 32, kCountInset = 7, kTitleGap = 8;
    const QString countStr = unreadCount > 99 ? QStringLiteral("99+")
                                               : QString::number(unreadCount);
    QFont badgeFont;
    badgeFont.setPixelSize(PainterTheme::badgeFontSize);
    badgeFont.setWeight(QFont::DemiBold);
    const int badgeW = qMax(PainterTheme::badgeHeight,
                            QFontMetrics(badgeFont).horizontalAdvance(countStr) + 10);
    constexpr int kMarkW = 14, kMarkGap = 6, kMarkEdge = 12;
    const int padRight = marked
        ? kTitleGap + kMarkW + (showCount ? kMarkGap + badgeW + kCountInset : kMarkEdge)
        : (showCount ? kTitleGap + badgeW + kCountInset : 16);
    // Three chip states, each distinct at a glance:
    //   open   — accent tint + accent border (the only tinted chip: where you are)
    //   unread — the plain chip, but a full-ink SemiBold title plus the count
    //            (the count carries the signal; the weight means unread never
    //            rests on colour alone)
    //   read   — the plain chip, dimmed ink, regular weight
    // Unread chips used to share the open chip's tint and border, which left
    // the open topic indistinguishable from every topic with news.
    b->setText(label);
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedHeight(kChipH);
    b->setFocusPolicy(Qt::NoFocus);
    const Pal c = pal(m_themeId);
    QString css = active
        ? QStringLiteral(
            // Calm selected style: a soft accent TINT fill + accent-coloured
            // text + a 1px accent border. Clearly "active" without the loud
            // solid-accent fill that read as too aggressive.
            "QPushButton { background: %1; color: %2; border: 1px solid %3;"
            "  border-radius: 16px; padding: 6px %PAD%px 6px 16px; font-size: 13px;"
            "  font-weight: 600; letter-spacing: 0.1px; }"
            "QPushButton:hover { background: %1; }"
          ).arg(c.accentSoft, c.accentSoftInk, c.accent)
        : (hasUnread && !silenced)
        ? QStringLiteral(
            "QPushButton { background: %1; color: %2; border: 1px solid %3;"
            "  border-radius: 16px; padding: 6px %PAD%px 6px 16px; font-size: 13px;"
            "  font-weight: 600; letter-spacing: 0.1px; }"
            "QPushButton:hover { background: %4; border-color: %5; }"
          ).arg(c.chipBg, c.ink, c.border, c.hoverBg, c.accent)
        : QStringLiteral(
            "QPushButton { background: %1; color: %2; border: 1px solid %3;"
            "  border-radius: 16px; padding: 6px %PAD%px 6px 16px; font-size: 13px;"
            "  font-weight: 500; letter-spacing: 0.1px; }"
            "QPushButton:hover { background: %4; color: %5; border-color: %6; }"
          ).arg(c.chipBg, c.inkDim, c.border, c.hoverBg, c.ink, c.accent);
    b->setStyleSheet(css.replace(QStringLiteral("%PAD%"), QString::number(padRight)));
    connect(b, &QPushButton::clicked, this, [this, threadId, label]() {
        if (threadId == 0) emit allMessagesSelected();
        else               emit threadSelected(threadId, label);
    });

    // Right-click menu. Real topics get Hide (reliable, client-side) + Delete
    // (best-effort message delete, since Talk has no thread-delete). The
    // "General"/All chip offers to restore any hidden topics.
    b->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(b, &QPushButton::customContextMenuRequested, this,
            [this, b, threadId, label](const QPoint &pos) {
        if (!m_model) return;
        QMenu menu(this);

        if (threadId > 0) {
            // Per-topic notifications. Muting ONE topic is the thing that keeps
            // a busy conversation readable -- muting the whole room is too
            // blunt, and following everything is why people stop reading.
            // Needs `threads`; on an older server the submenu is simply absent.
            QMenu *notif = nullptr;
            if (m_threadsCapable) {
                notif = menu.addMenu(tr("Notifications"));
                const int cur = m_model->notificationLevelForThread(threadId);
                struct Lvl { int v; const char *label; };
                const Lvl levels[] = {
                    {0, QT_TR_NOOP("Default")},
                    {1, QT_TR_NOOP("All messages")},
                    {2, QT_TR_NOOP("Mentions only")},
                    {3, QT_TR_NOOP("Never")},
                };
                for (const Lvl &l : levels) {
                    QAction *a = notif->addAction(tr(l.label));
                    a->setCheckable(true);
                    a->setChecked(cur == l.v);
                    a->setData(l.v);
                }
                menu.addSeparator();
            }
            QAction *hide = menu.addAction(tr("Hide topic"));
            QAction *del  = menu.addAction(tr("Delete topic"));
            QAction *chosen = menu.exec(b->mapToGlobal(pos));
            if (notif && chosen && chosen->parentWidget() == notif) {
                m_model->setThreadNotificationLevel(threadId, chosen->data().toInt());
            } else if (chosen == hide) {
                m_model->hideTopic(threadId);
            } else if (chosen == del) {
                QString name = label;
                name.remove(QStringLiteral("#  "));   // strip the chip prefix
                QMessageBox box(QMessageBox::Warning, tr("Delete topic"),
                    tr("Delete the topic “%1”?\n\nNextcloud Talk has no "
                       "topic-delete, so this removes the topic's messages where "
                       "the server allows — your own (within the edit window) and "
                       "others' only if you moderate this conversation. Anything "
                       "it can't delete is left, and the topic is then hidden from "
                       "your view. This can't be undone.").arg(name),
                    QMessageBox::Yes | QMessageBox::No, this);
                box.setDefaultButton(QMessageBox::No);
                if (box.exec() == QMessageBox::Yes)
                    m_model->deleteTopic(threadId);
            }
        } else {
            // General / All-messages chip: topics-at-large actions.
            QAction *followed = nullptr;
            if (m_threadsCapable)
                followed = menu.addAction(tr("Topics you follow…"));
            const int n = m_model->hiddenTopicCount();
            QAction *unhide = (n > 0)
                ? menu.addAction(tr("Show %n hidden topic(s)", nullptr, n))
                : nullptr;
            if (!followed && !unhide) return;
            QAction *chosen = menu.exec(b->mapToGlobal(pos));
            if (chosen && chosen == followed)      emit subscribedTopicsRequested();
            else if (chosen && chosen == unhide)   m_model->unhideAllTopics();
        }
    });

    if (marked)
        b->setToolTip(silenced ? tr("Notifications are off for this topic")
                               : tr("Only mentions notify you in this topic"));
    if (!showCount && !marked)
        return b;

    // The mark: a slashed bell (notifications off) or "@" (mentions only), in the
    // chip's own ink. Transparent to the mouse like the count, so a click on it is
    // a click on the topic and the chip's tooltip shows over it.
    QLabel *mark = nullptr;
    if (marked) {
        const QColor markInk(active ? c.accentSoftInk : c.inkDim);
        mark = new QLabel(b);
        mark->setObjectName(QStringLiteral("topicNotifMark"));
        mark->setAttribute(Qt::WA_TransparentForMouseEvents);
        mark->setFixedSize(kMarkW, kMarkW);
        mark->setAlignment(Qt::AlignCenter);
        if (silenced) {
            mark->setPixmap(mutedBellPixmap(markInk, kMarkW, b->devicePixelRatioF()));
        } else {
            QFont f;
            f.setPixelSize(13);
            f.setWeight(QFont::Bold);
            mark->setFont(f);
            mark->setText(QStringLiteral("@"));
            mark->setStyleSheet(QStringLiteral("QLabel { color: %1; background: transparent; }")
                                    .arg(markInk.name(QColor::HexRgb)));
        }
        mark->setAccessibleName(silenced ? tr("Notifications off") : tr("Mentions only"));
    }

    if (!showCount) {
        auto *innerMark = new QHBoxLayout(b);
        innerMark->setContentsMargins(0, 0, kMarkEdge, 0);
        innerMark->setSpacing(0);
        innerMark->addStretch(1);
        innerMark->addWidget(mark, 0, Qt::AlignVCenter);
        return b;
    }

    // The count capsule: the same pill as SidebarPainter::paintUnreadBadge
    // (PainterTheme::badgeHeight, demibold, unreadBadge fill with its scored
    // ink), laid inside the chip's right padding. Transparent to the mouse, so
    // a click or right-click on the count is a click on the topic.
    auto *badge = new QLabel(countStr, b);
    badge->setObjectName(QStringLiteral("topicUnreadCount"));
    badge->setAttribute(Qt::WA_TransparentForMouseEvents);
    badge->setFont(badgeFont);
    badge->setAlignment(Qt::AlignCenter);
    badge->setFixedSize(badgeW, PainterTheme::badgeHeight);
    // A silenced topic's count is an OUTLINE in the quiet ink, not the filled
    // unread pill: the number is still there, it just does not call for you.
    badge->setStyleSheet(silenced
        ? QStringLiteral("QLabel { background: transparent; color: %1; border: 1px solid %1;"
                         " border-radius: %2px; }")
              .arg(c.inkDim).arg(PainterTheme::badgeHeight / 2)
        : QStringLiteral("QLabel { background: %1; color: %2; border-radius: %3px; }")
              .arg(c.unreadBadge, c.badgeInk).arg(PainterTheme::badgeHeight / 2));
    badge->setAccessibleName(tr("%n unread", nullptr, unreadCount));

    auto *inner = new QHBoxLayout(b);
    inner->setContentsMargins(0, 0, kCountInset, 0);
    inner->setSpacing(marked ? kMarkGap : 0);
    inner->addStretch(1);
    if (mark)
        inner->addWidget(mark, 0, Qt::AlignVCenter);
    inner->addWidget(badge, 0, Qt::AlignVCenter);
    return b;
}

void TopicTabBar::rebuild()
{
    // Wipe existing chips, keep the trailing stretch.
    while (m_rowLayout->count() > 1) {
        QLayoutItem *it = m_rowLayout->takeAt(0);
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    m_selectedChip = nullptr;   // re-captured below; drives auto-scroll-into-view

    const int topicCount = m_model ? m_model->rowCount() : 0;
    if (!m_model || topicCount == 0) {
        // No topics yet — still show the "+ New topic" affordance so the
        // user can create the first one without digging into menus.
        auto *add = new QPushButton(tr("+ New topic"), m_row);
        add->setCursor(Qt::PointingHandCursor);
        add->setFlat(true);
        add->setFixedHeight(28);
        add->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; color: %1;"
            "  border: 1px dashed %1; border-radius: 14px; padding: 0 14px;"
            "  font-size: 12px; font-weight: 600; letter-spacing: 0.5px; }"
            "QPushButton:hover { background: %2; }"
        ).arg(pal(m_themeId).accent, pal(m_themeId).hoverBg));
        connect(add, &QPushButton::clicked, this, &TopicTabBar::newTopicRequested);
        m_rowLayout->insertWidget(m_rowLayout->count() - 1, add);
        // Visibility is controlled from MainWindow::updateTopicMode — a rebuild
    // triggered by a model reset shouldn't unhide a 1-on-1's tab bar.   // visible as an empty-state prompt
        return;
    }

    // "All messages" chip first.
    auto *all = makeChip(QStringLiteral("\u2605  ") + tr("All messages"),
                         0, 0, m_selectedThreadId == 0);
    if (m_selectedThreadId == 0) m_selectedChip = all;
    m_rowLayout->insertWidget(m_rowLayout->count() - 1, all);

    for (int i = 0; i < topicCount; ++i) {
        QModelIndex idx = m_model->index(i);
        // Row 0 is the synthetic "All Messages" placeholder the model
        // prepends to its own list (threadId=0, isAllMessages=true). We
        // already rendered it as the leading "\u2605 All messages" chip above \u2014
        // looping over it again produced a duplicate "# General" chip that
        // confused users into thinking their newly-created topic wasn't
        // there.
        if (idx.data(ThreadListModel::IsAllMessagesRole).toBool())
            continue;
        const int threadId = idx.data(ThreadListModel::ThreadIdRole).toInt();
        QString title      = idx.data(ThreadListModel::TitleRole).toString();
        if (title.isEmpty()) title = tr("Thread %1").arg(threadId);
        const int unread   = idx.data(ThreadListModel::UnreadCountRole).toInt();
        const int notif    = idx.data(ThreadListModel::NotificationLevelRole).toInt();

        auto *chip = makeChip(QStringLiteral("#  ") + title, threadId, unread,
                              m_selectedThreadId == threadId, notif);
        if (m_selectedThreadId == threadId) m_selectedChip = chip;
        m_rowLayout->insertWidget(m_rowLayout->count() - 1, chip);
    }

    // Trailing "+" chip.
    auto *add = new QPushButton(QStringLiteral("+"), m_row);
    add->setCursor(Qt::PointingHandCursor);
    add->setFlat(true);
    add->setFixedSize(28, 28);
    add->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: %1;"
        "  border: 1px dashed %1; border-radius: 14px;"
        "  font-size: 15px; font-weight: 700; }"
        "QPushButton:hover { background: %2; }"
    ).arg(pal(m_themeId).accent, pal(m_themeId).hoverBg));
    connect(add, &QPushButton::clicked, this, &TopicTabBar::newTopicRequested);
    m_rowLayout->insertWidget(m_rowLayout->count() - 1, add);

    // Bring the selected topic into view (deferred so the row has laid out
    // its chips first). Keeps the active topic visible even when there are
    // more topics than fit the strip.
    if (m_selectedChip) {
        QPointer<TopicTabBar> self(this);
        QTimer::singleShot(0, this, [self]() {
            if (self && self->m_selectedChip && self->m_scroll)
                self->m_scroll->ensureWidgetVisible(self->m_selectedChip, 24, 0);
        });
    }

    // Visibility is controlled from MainWindow::updateTopicMode — a rebuild
    // triggered by a model reset shouldn't unhide a 1-on-1's tab bar.
}
