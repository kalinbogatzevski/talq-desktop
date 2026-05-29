#pragma once

#include <QWidget>
#include "painter/PainterTheme.h"

class ThreadListModel;
class QHBoxLayout;
class QScrollArea;
class QPushButton;

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

signals:
    void threadSelected(int threadId, const QString &title);
    void allMessagesSelected();
    void newTopicRequested();

private:
    void rebuild();
    void applyBarChrome();   // bar bg/border + thin scrollbar, palette-driven
    QPushButton *makeChip(const QString &label, int threadId,
                          int unreadCount, bool active);

    ThreadListModel *m_model = nullptr;
    QScrollArea     *m_scroll = nullptr;
    QWidget         *m_row = nullptr;
    QHBoxLayout     *m_rowLayout = nullptr;
    int              m_selectedThreadId = 0;   // 0 = "All messages"
    PainterTheme::Theme m_themeId = PainterTheme::Theme::Vivid;  // bug 10
};
