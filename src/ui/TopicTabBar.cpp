#include "TopicTabBar.h"

#include "models/ThreadListModel.h"

#include <QAbstractItemModel>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

TopicTabBar::TopicTabBar(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(48);
    setStyleSheet(
        "TopicTabBar { background: #141412; border-bottom: 1px solid #26262238; }"
        "QScrollArea, QScrollArea > QWidget, QScrollArea > QWidget > QWidget {"
        "  background: transparent; border: none; }"
        "QScrollBar:horizontal { height: 4px; background: transparent; margin: 0; }"
        "QScrollBar::handle:horizontal { background: #2a2a26; border-radius: 2px; min-width: 24px; }"
        "QScrollBar::handle:horizontal:hover { background: #3a3a34; }"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }"
    );

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

    hide();
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
    }
    rebuild();
}

void TopicTabBar::setSelectedThreadId(int threadId)
{
    if (m_selectedThreadId == threadId) return;
    m_selectedThreadId = threadId;
    rebuild();
}

QPushButton *TopicTabBar::makeChip(const QString &label, int threadId,
                                   int unreadCount, bool active)
{
    auto *b = new QPushButton(m_row);
    QString text = label;
    if (unreadCount > 0)
        text += QStringLiteral("   \u00B7 %1").arg(unreadCount);
    b->setText(text);
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedHeight(32);
    b->setFocusPolicy(Qt::NoFocus);
    b->setStyleSheet(active
        ? QStringLiteral(
            "QPushButton { background: #14b8a6; color: #0e1817; border: none;"
            "  border-radius: 16px; padding: 6px 16px; font-size: 13px;"
            "  font-weight: 600; letter-spacing: 0.1px; }"
            "QPushButton:hover { background: #2dd4bf; }"
            "QPushButton:pressed { background: #0d9488; }"
          )
        : QStringLiteral(
            "QPushButton { background: #1c1c1a; color: #b9b4ac; border: 1px solid #2a2a26;"
            "  border-radius: 16px; padding: 6px 16px; font-size: 13px;"
            "  font-weight: 500; letter-spacing: 0.1px; }"
            "QPushButton:hover { background: #242422; color: #e4e0da; border-color: #3a3a34; }"
            "QPushButton:pressed { background: #2a2a26; }"
          ));
    connect(b, &QPushButton::clicked, this, [this, threadId, label]() {
        if (threadId == 0) emit allMessagesSelected();
        else               emit threadSelected(threadId, label);
    });
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

    const int topicCount = m_model ? m_model->rowCount() : 0;
    if (!m_model || topicCount == 0) {
        // No topics yet — still show the "+ New topic" affordance so the
        // user can create the first one without digging into menus.
        auto *add = new QPushButton(tr("+ New topic"), m_row);
        add->setCursor(Qt::PointingHandCursor);
        add->setFlat(true);
        add->setFixedHeight(28);
        add->setStyleSheet(
            "QPushButton { background: transparent; color: #14b8a6;"
            "  border: 1px dashed #14b8a6; border-radius: 14px; padding: 0 14px;"
            "  font-size: 12px; font-weight: 600; letter-spacing: 0.5px; }"
            "QPushButton:hover { background: rgba(46,196,182,0.08); }"
        );
        connect(add, &QPushButton::clicked, this, &TopicTabBar::newTopicRequested);
        m_rowLayout->insertWidget(m_rowLayout->count() - 1, add);
        // Visibility is controlled from MainWindow::updateTopicMode — a rebuild
    // triggered by a model reset shouldn't unhide a 1-on-1's tab bar.   // visible as an empty-state prompt
        return;
    }

    // "All messages" chip first.
    auto *all = makeChip(QStringLiteral("\u2605  ") + tr("All messages"),
                         0, 0, m_selectedThreadId == 0);
    m_rowLayout->insertWidget(m_rowLayout->count() - 1, all);

    for (int i = 0; i < topicCount; ++i) {
        QModelIndex idx = m_model->index(i);
        const int threadId = idx.data(ThreadListModel::ThreadIdRole).toInt();
        QString title      = idx.data(ThreadListModel::TitleRole).toString();
        if (title.isEmpty()) title = tr("Thread %1").arg(threadId);
        const int unread   = idx.data(ThreadListModel::UnreadCountRole).toInt();

        auto *chip = makeChip(QStringLiteral("#  ") + title, threadId, unread,
                              m_selectedThreadId == threadId);
        m_rowLayout->insertWidget(m_rowLayout->count() - 1, chip);
    }

    // Trailing "+" chip.
    auto *add = new QPushButton(QStringLiteral("+"), m_row);
    add->setCursor(Qt::PointingHandCursor);
    add->setFlat(true);
    add->setFixedSize(28, 28);
    add->setStyleSheet(
        "QPushButton { background: transparent; color: #14b8a6;"
        "  border: 1px dashed #14b8a6; border-radius: 14px;"
        "  font-size: 15px; font-weight: 700; }"
        "QPushButton:hover { background: rgba(46,196,182,0.08); }"
    );
    connect(add, &QPushButton::clicked, this, &TopicTabBar::newTopicRequested);
    m_rowLayout->insertWidget(m_rowLayout->count() - 1, add);

    // Visibility is controlled from MainWindow::updateTopicMode — a rebuild
    // triggered by a model reset shouldn't unhide a 1-on-1's tab bar.
}
