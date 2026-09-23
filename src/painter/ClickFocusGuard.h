#pragma once

// Lets a Qt::ClickFocus widget host a control that must NOT take keyboard focus.
//
// WHY THIS EXISTS: ChatPainter is ClickFocus, so a click anywhere in it -- the
// jump-to-bottom button included -- takes keyboard focus from the composer.
// Qt moves the focus in QApplication::notify BEFORE the widget's own
// mousePressEvent runs (giveFocusAccordingToFocusPolicy, for a press AND a
// double-click), so the handler cannot refuse it. It can only give it back.
// Without that, typing a draft, scrolling up, clicking the button and typing on
// dropped every keystroke until the user clicked the composer again.
//
// What it gives back to is whatever had focus before THIS click -- the
// composer, the search box, or nothing -- and only when this click moved it.
// If the widget already had focus (the user had clicked a message), the click
// changes nothing, exactly like a button with Qt::NoFocus.
//
// Usage, in the owner:
//   ClickFocusGuard m_focusGuard{this};                 // a member
//   mousePressEvent / mouseDoubleClickEvent:
//     m_focusGuard.beginPress();                        // FIRST, every time
//     if (on the non-focusing control) m_focusGuard.giveBack();

#include <QApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <QWidget>

class ClickFocusGuard
{
public:
    explicit ClickFocusGuard(QWidget *owner) : m_owner(owner)
    {
        if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
            m_conn = QObject::connect(app, &QApplication::focusChanged, owner,
                [this](QWidget *old, QWidget *now) {
                    if (now != m_owner) return;
                    // Qt gives click focus while the button is still down, and a
                    // keyboard or programmatic focus change happens with no
                    // button down. Only the former is "this press took it".
                    m_takenByPress = QGuiApplication::mouseButtons() != Qt::NoButton;
                    m_from = old;
                });
        }
    }
    // Disconnect explicitly: the owner's QObject base outlives this member, and
    // a focus change during the owner's teardown must not reach a dead guard.
    ~ClickFocusGuard() { QObject::disconnect(m_conn); }
    ClickFocusGuard(const ClickFocusGuard &) = delete;
    ClickFocusGuard &operator=(const ClickFocusGuard &) = delete;

    // Call first thing in every mousePressEvent and mouseDoubleClickEvent: it
    // consumes the record of the focus change Qt made for this very event, so a
    // stale one can never be replayed by a later click.
    void beginPress()
    {
        m_thisPress = m_takenByPress ? m_from : nullptr;
        m_takenByPress = false;
    }

    // Give focus back to where it was before this press, if this press took it.
    // Does nothing if the previous holder is gone, hidden or disabled, or if
    // focus has already moved on from the owner.
    void giveBack()
    {
        QWidget *to = m_thisPress;
        m_thisPress = nullptr;
        if (to && to != m_owner && m_owner->hasFocus()
            && to->isVisible() && to->isEnabled()) {
            s_returning = true;          // FocusIn is delivered synchronously inside setFocus
            to->setFocus(Qt::OtherFocusReason);
            s_returning = false;
        }
    }

    // True only while giveBack() is handing focus back, i.e. during the FocusIn
    // that causes. A widget that reads FocusIn as "the user is here" must check
    // it: the composer treats FocusIn as interaction, which dismisses the unread
    // divider and marks the room read, and focus coming back to it because the
    // jump button or the scrollbar was clicked is not the user doing anything.
    // A reason code cannot carry this: the composer's own setFocus calls use
    // Qt::OtherFocusReason too.
    static bool returningFocus() { return s_returning; }

private:
    inline static bool s_returning = false;
    QWidget *m_owner;
    QMetaObject::Connection m_conn;
    QPointer<QWidget> m_from;         // who had focus when a press moved it here
    QPointer<QWidget> m_thisPress;    // m_from, if the current press was that press
    bool m_takenByPress = false;
};
