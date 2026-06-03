#pragma once

#include "painter/PainterTheme.h"

#include <QObject>
#include <QPoint>
#include <QList>
#include <QElapsedTimer>

class NotificationPopup;

/**
 * Manages a vertical stack of NotificationPopup widgets at the bottom-right
 * of the primary screen. Caps visible count, ages out old popups, merges
 * rapid repeats from the same conversation.
 *
 * Emits clicked(token) when the user picks any popup.
 */
class NotificationStack : public QObject
{
    Q_OBJECT
public:
    explicit NotificationStack(QObject *parent = nullptr);
    ~NotificationStack() override;

    void notify(const QString &title, const QString &message, const QString &token);

    // Theme every popup (current and future) so in-app toasts follow the
    // active theme. Called on construction and on a live theme switch.
    void setTheme(PainterTheme::Theme theme);

signals:
    void clicked(const QString &token);

private:
    void reposition();
    void dropOldest();

    PainterTheme::Theme m_theme = PainterTheme::Theme::Vivid;

    struct Entry {
        NotificationPopup *popup = nullptr;
        QString token;
        QElapsedTimer shownAt;
        int coalescedCount = 1;
    };
    QList<Entry> m_stack;
    static constexpr int kMaxVisible = 4;
    static constexpr int kGap        = 8;     // px between stacked popups
    static constexpr int kEdgeMargin = 16;    // px from screen edge
    // Two notifications from the same token inside this window merge into one
    // (avoids the drawer filling up instantly in a chatty room).
    static constexpr int kCoalesceMs = 3000;
};
