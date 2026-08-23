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
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {
struct Pal {
    QString accent, onAccent, barBg, chipBg, border, ink, inkDim, hoverBg, accentSoft, accentSoftInk, unreadBadge;
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
             n(th.unreadBadge) };  // unread-badge fill, same token as SidebarPainter
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

QWidget *TopicTabBar::makeChip(const QString &label, int threadId,
                               int unreadCount, bool active)
{
    auto *b = new QPushButton(m_row);
    const bool hasUnread = unreadCount > 0;
    // #11 \u2014 per-topic unread count now rides a real badge widget (see below),
    // matching SidebarPainter::paintUnreadBadge's pill \u2014 not text stuffed into
    // the chip label. An UNREAD inactive chip still takes an accent-tinted
    // "has unread" treatment so the topic visibly stands out at a glance; the
    // active chip already pops, so it keeps the plain style.
    b->setText(label);
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedHeight(32);
    b->setFocusPolicy(Qt::NoFocus);
    const Pal c = pal(m_themeId);
    b->setStyleSheet(active
        ? QStringLiteral(
            // Calm selected style: a soft accent TINT fill + accent-coloured
            // text + a 1px accent border. Clearly "active" without the loud
            // solid-accent fill that read as too aggressive.
            "QPushButton { background: %1; color: %2; border: 1px solid %3;"
            "  border-radius: 16px; padding: 6px 16px; font-size: 13px;"
            "  font-weight: 600; letter-spacing: 0.1px; }"
            "QPushButton:hover { background: %1; }"
          ).arg(c.accentSoft, c.accentSoftInk, c.accent)
        : hasUnread
        ? QStringLiteral(
            // #11 \u2014 UNREAD inactive chip: accent-tinted fill + accent text +
            // accent border + bolder weight, so an unread topic reads as a badge.
            "QPushButton { background: %1; color: %2; border: 1px solid %3;"
            "  border-radius: 16px; padding: 6px 16px; font-size: 13px;"
            "  font-weight: 600; letter-spacing: 0.1px; }"
            "QPushButton:hover { background: %1; }"
          ).arg(c.accentSoft, c.accentSoftInk, c.accent)
        : QStringLiteral(
            "QPushButton { background: %1; color: %2; border: 1px solid %3;"
            "  border-radius: 16px; padding: 6px 16px; font-size: 13px;"
            "  font-weight: 500; letter-spacing: 0.1px; }"
            "QPushButton:hover { background: %4; color: %5; border-color: %6; }"
          ).arg(c.chipBg, c.inkDim, c.border, c.hoverBg, c.ink, c.accent));
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
            // General / All-messages chip: restore hidden topics.
            const int n = m_model->hiddenTopicCount();
            if (n <= 0) return;
            QAction *unhide = menu.addAction(tr("Show %n hidden topic(s)", nullptr, n));
            if (menu.exec(b->mapToGlobal(pos)) == unhide)
                m_model->unhideAllTopics();
        }
    });

    if (!hasUnread)
        return b;

    // #11 — real unread badge: a filled stadium pill sized/colored exactly
    // like SidebarPainter::paintUnreadBadge (PainterTheme::badgeHeight, demibold
    // count text, unreadBadge fill + controlInk text), placed beside the chip
    // instead of stuffed into the button's own label text. Was a third local
    // copy of BadgeHeight=18/BadgeFontSize=10 (also independently defined in
    // SidebarPainter.h and ThreadsPainter.h) -- now the one promoted home.
    const QString countStr = unreadCount > 99 ? QStringLiteral("99+")
                                               : QString::number(unreadCount);
    QFont badgeFont;
    badgeFont.setPixelSize(PainterTheme::badgeFontSize);
    badgeFont.setWeight(QFont::DemiBold);
    const QFontMetrics bfm(badgeFont);
    const int textW = bfm.horizontalAdvance(countStr);
    const int badgeW = qMax(PainterTheme::badgeHeight, textW + 10);

    auto *badge = new QLabel(countStr, m_row);
    badge->setFont(badgeFont);
    badge->setAlignment(Qt::AlignCenter);
    badge->setFixedSize(badgeW, PainterTheme::badgeHeight);
    badge->setStyleSheet(QStringLiteral(
        "QLabel { background: %1; color: %2; border-radius: %3px; }"
    ).arg(c.unreadBadge, c.onAccent)
     .arg(PainterTheme::badgeHeight / 2));

    auto *wrap = new QWidget(m_row);
    auto *wrapLayout = new QHBoxLayout(wrap);
    wrapLayout->setContentsMargins(0, 0, 0, 0);
    wrapLayout->setSpacing(6);
    wrapLayout->setAlignment(Qt::AlignVCenter);
    wrapLayout->addWidget(b);
    wrapLayout->addWidget(badge, 0, Qt::AlignVCenter);
    return wrap;
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

        auto *chip = makeChip(QStringLiteral("#  ") + title, threadId, unread,
                              m_selectedThreadId == threadId);
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
