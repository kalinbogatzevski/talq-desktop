#pragma once

#include <QWidget>
#include <QPointer>
#include "painter/PainterTheme.h"

class ThreadListModel;
class QHBoxLayout;
class QScrollArea;
class QPushButton;
class QWheelEvent;

/**
 * Horizontal topic/thread picker shown above the chat area, inspired by
 * Telegram Desktop's forum view. One chip per thread, plus a leading
 * "All messages" chip and a trailing "+" chip for creating a new topic.
 *
 * Hidden automatically when the bound model is empty (no topics yet).
 */
class TopicTabBar : public QWidget
{
    Q_OBJECT
public:
    explicit TopicTabBar(QWidget *parent = nullptr);

    void setModel(ThreadListModel *model);
    void setSelectedThreadId(int threadId);
    // bug 10 — drive chip colors from PainterTheme (single source of truth) so
    // the labels are readable in ALL four themes. Mirrors how every other
    // chrome widget receives the theme; call on creation and on theme change.
    void setTheme(PainterTheme::Theme t);

protected:
    // Re-theme when the app palette changes (PainterTheme drives it).
    void changeEvent(QEvent *e) override;
    // Vertical wheel scrolls the topic strip horizontally — the strip is a
    // single row, so a normal wheel must move through the topics, not be lost.
    // Filters the scroll viewport (the QScrollArea would otherwise eat wheels).
    bool eventFilter(QObject *watched, QEvent *e) override;

signals:
    // The General chip's menu asked for the cross-conversation followed-topics
    // list. The window owns the dialog and the navigation it produces.
    void subscribedTopicsRequested();
    void threadSelected(int threadId, const QString &title);
    void allMessagesSelected();
    void newTopicRequested();

public:
    // Whether the server supports per-topic notification levels
    // (`threads`). False hides the submenu rather than offering a
    // control that would 404.
    void setThreadsCapable(bool c) { m_threadsCapable = c; }

private:
    bool m_threadsCapable = false;

    void rebuild();
    void applyBarChrome();   // bar bg/border + thin scrollbar, palette-driven
    // Returns the chip's top-level widget: either the QPushButton itself (no
    // unread), or a small container wrapping the button + a real unread-badge
    // pill (see #11) beside it.
    QWidget *makeChip(const QString &label, int threadId,
                      int unreadCount, bool active);

    ThreadListModel *m_model = nullptr;
    QScrollArea     *m_scroll = nullptr;
    QWidget         *m_row = nullptr;
    QHBoxLayout     *m_rowLayout = nullptr;
    QPointer<QWidget> m_selectedChip;  // for auto-scroll-into-view on rebuild
    int              m_selectedThreadId = 0;   // 0 = "All messages"
    PainterTheme::Theme m_themeId = PainterTheme::Theme::Vivid;  // bug 10
};
