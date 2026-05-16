#include "NotificationStack.h"
#include "NotificationPopup.h"

#include <QApplication>
#include <QScreen>

NotificationStack::NotificationStack(QObject *parent)
    : QObject(parent) {}

NotificationStack::~NotificationStack()
{
    for (const Entry &e : m_stack)
        if (e.popup) e.popup->deleteLater();
}

void NotificationStack::notify(const QString &title, const QString &message,
                                const QString &token)
{
    // Coalesce rapid repeats from the same conversation.
    for (int i = 0; i < m_stack.size(); ++i) {
        Entry &e = m_stack[i];
        if (e.token != token) continue;
        if (e.shownAt.elapsed() > kCoalesceMs) continue;
        ++e.coalescedCount;
        QString collapsed = QObject::tr("%n new message(s)", nullptr, e.coalescedCount);
        QScreen *screen = QApplication::primaryScreen();
        if (!screen) return;
        QRect geom = screen->availableGeometry();
        QPoint pos(geom.right() - e.popup->width() - kEdgeMargin, 0);  // y set by reposition
        e.popup->showNotification(title, collapsed, token, pos);
        e.shownAt.restart();
        reposition();
        return;
    }

    // Cap — drop the oldest (topmost, which is index 0 after reposition).
    while (m_stack.size() >= kMaxVisible) dropOldest();

    auto *popup = new NotificationPopup();
    connect(popup, &NotificationPopup::clicked, this,
            [this, popup](const QString &t) {
        emit clicked(t);
        // Remove this popup from the stack
        for (int i = 0; i < m_stack.size(); ++i) {
            if (m_stack[i].popup == popup) {
                m_stack[i].popup->deleteLater();
                m_stack.removeAt(i);
                break;
            }
        }
        reposition();
    });
    connect(popup, &NotificationPopup::dismissed, this, [this, popup]() {
        for (int i = 0; i < m_stack.size(); ++i) {
            if (m_stack[i].popup == popup) {
                m_stack[i].popup->deleteLater();
                m_stack.removeAt(i);
                break;
            }
        }
        reposition();
    });

    QScreen *screen = QApplication::primaryScreen();
    if (!screen) { popup->deleteLater(); return; }
    QRect geom = screen->availableGeometry();
    QPoint placeholder(geom.right() - popup->width() - kEdgeMargin,
                       geom.bottom() - popup->height() - kEdgeMargin);
    popup->showNotification(title, message, token, placeholder);

    Entry e;
    e.popup = popup;
    e.token = token;
    e.shownAt.start();
    m_stack.append(e);
    reposition();
}

void NotificationStack::reposition()
{
    QScreen *screen = QApplication::primaryScreen();
    if (!screen) return;
    QRect geom = screen->availableGeometry();

    // Stack bottom-up: most recent at the bottom.
    int y = geom.bottom() - kEdgeMargin;
    for (int i = m_stack.size() - 1; i >= 0; --i) {
        NotificationPopup *p = m_stack[i].popup;
        if (!p) continue;
        y -= p->height();
        p->move(geom.right() - p->width() - kEdgeMargin, y);
        y -= kGap;
    }
}

void NotificationStack::dropOldest()
{
    if (m_stack.isEmpty()) return;
    Entry e = m_stack.takeFirst();
    if (e.popup) e.popup->deleteLater();
}
