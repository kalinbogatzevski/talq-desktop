# In-Bubble Text Selection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Telegram-style character-level text selection within and across message bubbles, with Ctrl+C copy support.

**Architecture:** Selection state tracks anchor and active positions as {layoutIndex, cursorPosition} pairs. QTextDocument's built-in hitTest() converts mouse positions to cursor positions. PaintContext.selections paints character-level highlights. Mouse events are reworked so drags starting on body text enter text selection instead of whole-message selection.

**Tech Stack:** QTextDocument, QAbstractTextDocumentLayout, QTextCursor, QClipboard

**Spec:** `docs/superpowers/specs/2026-04-03-text-selection-design.md`

---

## File Map

| File | Change | Responsibility |
|---|---|---|
| `src/painter/ChatPainter.h` | Modify | Add TextSelection struct, new members, helper methods |
| `src/painter/ChatPainter.cpp` | Modify | Mouse events, highlight painting, copy, cursor appearance |

---

### Task 1: Add TextSelection struct and members to ChatPainter.h

**Files:**
- Modify: `src/painter/ChatPainter.h`

- [ ] **Step 1: Add TextSelection struct before the ChatPainter class**

In `src/painter/ChatPainter.h`, add after the `#include "PainterTheme.h"` line (line 9) and before `class MessageListModel;` (line 11):

```cpp
/**
 * Character-level text selection state, spanning one or more messages.
 * Anchor = where the mouse was pressed. Active = where the mouse is now.
 */
struct TextSelection {
    int anchorLayoutIdx = -1;
    int anchorCursorPos = 0;
    int activeLayoutIdx = -1;
    int activeCursorPos = 0;
    bool active = false;

    bool hasSelection() const {
        return active && !(anchorLayoutIdx == activeLayoutIdx
                           && anchorCursorPos == activeCursorPos);
    }
    void clear() {
        anchorLayoutIdx = activeLayoutIdx = -1;
        anchorCursorPos = activeCursorPos = 0;
        active = false;
    }

    struct Range { int startIdx, startPos, endIdx, endPos; };
    Range normalized() const {
        if (anchorLayoutIdx < activeLayoutIdx
            || (anchorLayoutIdx == activeLayoutIdx && anchorCursorPos <= activeCursorPos))
            return {anchorLayoutIdx, anchorCursorPos, activeLayoutIdx, activeCursorPos};
        return {activeLayoutIdx, activeCursorPos, anchorLayoutIdx, anchorCursorPos};
    }
};
```

- [ ] **Step 2: Add new members and helpers to ChatPainter**

In `src/painter/ChatPainter.h`, add to the private section after `m_selectionMode`/`m_selectedIds` (after line 155):

```cpp
    // ── Text selection state (character-level, Telegram-style) ──
    TextSelection m_textSelection;
    bool m_textAnchorSet = false;  // true if mousePress landed on body text
```

Add a private helper method declaration, after `hitTestReaction` (after line 100):

```cpp
    int hitTestBodyCursor(const MessageLayout &ml, const QPointF &canvasPos) const;
    bool isOnBodyText(const QPointF &canvasPos, int layoutIdx) const;
    void copySelectedText();
```

- [ ] **Step 3: Build and verify compilation**

Run:
```bash
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5
```

Expected: build succeeds (new members/methods declared but not yet defined — linker won't complain since nothing calls them yet, but declarations must parse).

Note: if the build fails because the methods are referenced somewhere, add empty stubs in the .cpp. But since nothing calls them yet, declarations alone should be fine.

- [ ] **Step 4: Commit**

```bash
cd /c/src/talk-desktop-qt
git add src/painter/ChatPainter.h
git commit -m "feat(selection): add TextSelection struct and members to ChatPainter"
```

---

### Task 2: Implement helper methods (hitTestBodyCursor, isOnBodyText, copySelectedText)

**Files:**
- Modify: `src/painter/ChatPainter.cpp`

- [ ] **Step 1: Add #include for QClipboard and QApplication**

In `src/painter/ChatPainter.cpp`, add with the other includes (after line 21):

```cpp
#include <QApplication>
#include <QClipboard>
#include <QTextCursor>
```

- [ ] **Step 2: Implement hitTestBodyCursor()**

Add after the `hitTestReaction` function (after line 603):

```cpp
int ChatPainter::hitTestBodyCursor(const MessageLayout &ml, const QPointF &canvasPos) const
{
    if (!ml.bodyDoc || ml.bodyRect.isNull()) return -1;
    QPointF bodyLocal(canvasPos.x() - ml.bodyRect.x(), canvasPos.y() - ml.bodyRect.y());
    if (bodyLocal.x() < 0 || bodyLocal.y() < 0
        || bodyLocal.x() > ml.bodyRect.width() || bodyLocal.y() > ml.bodyRect.height())
        return -1;
    return ml.bodyDoc->documentLayout()->hitTest(bodyLocal, Qt::FuzzyHit);
}
```

- [ ] **Step 3: Implement isOnBodyText()**

Add after `hitTestBodyCursor`:

```cpp
bool ChatPainter::isOnBodyText(const QPointF &canvasPos, int layoutIdx) const
{
    if (layoutIdx < 0 || layoutIdx >= m_layouts.size()) return false;
    const auto &ml = m_layouts[layoutIdx];
    if (ml.isSystem || !ml.bodyDoc || ml.bodyRect.isNull()) return false;
    return ml.bodyRect.contains(canvasPos);
}
```

- [ ] **Step 4: Implement copySelectedText()**

Add after `isOnBodyText`:

```cpp
void ChatPainter::copySelectedText()
{
    if (!m_textSelection.hasSelection()) return;
    auto range = m_textSelection.normalized();

    QStringList parts;
    for (int i = range.startIdx; i <= range.endIdx; ++i) {
        if (i < 0 || i >= m_layouts.size()) continue;
        const auto &ml = m_layouts[i];
        if (!ml.bodyDoc) continue;

        QTextCursor cursor(ml.bodyDoc.get());
        if (i == range.startIdx && i == range.endIdx) {
            cursor.setPosition(range.startPos);
            cursor.setPosition(range.endPos, QTextCursor::KeepAnchor);
        } else if (i == range.startIdx) {
            cursor.setPosition(range.startPos);
            cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        } else if (i == range.endIdx) {
            cursor.setPosition(0);
            cursor.setPosition(range.endPos, QTextCursor::KeepAnchor);
        } else {
            cursor.setPosition(0);
            cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        }

        QString text = cursor.selectedText();
        text.replace(QChar(0x2029), QChar('\n'));  // paragraph separator → newline
        if (!text.isEmpty())
            parts.append(text);
    }

    if (!parts.isEmpty())
        QApplication::clipboard()->setText(parts.join(QStringLiteral("\n")));
}
```

- [ ] **Step 5: Build and verify compilation**

Run:
```bash
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5
```

Expected: successful build.

- [ ] **Step 6: Commit**

```bash
cd /c/src/talk-desktop-qt
git add src/painter/ChatPainter.cpp
git commit -m "feat(selection): implement text selection helper methods"
```

---

### Task 3: Rework mouse events for text selection

**Files:**
- Modify: `src/painter/ChatPainter.cpp` (mousePressEvent, mouseMoveEvent, mouseReleaseEvent)

This is the most complex task. The key change: drags that start on body text now enter text selection mode instead of whole-message selection mode.

- [ ] **Step 1: Replace mousePressEvent**

Replace the entire `mousePressEvent` function (currently lines 400-411):

Old:
```cpp
void ChatPainter::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton) {
        m_dragging = true;
        m_dragMoved = false;
        m_pressCanvasPos = event->position();
        m_dragStartY = event->position().y();
        m_dragStartScroll = m_scrollY;
        m_pressHit = hitTestAt(event->position().x(), event->position().y());
        event->accept();
    }
}
```

New:
```cpp
void ChatPainter::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton) {
        m_dragging = true;
        m_dragMoved = false;
        m_pressCanvasPos = event->position();
        m_dragStartY = event->position().y();
        m_dragStartScroll = m_scrollY;
        m_pressHit = hitTestAt(event->position().x(), event->position().y());

        // Check if press lands on body text (for text selection)
        m_textAnchorSet = false;
        if (event->button() == Qt::LeftButton && !m_selectionMode
            && !(event->modifiers() & Qt::ControlModifier)) {
            QPointF canvasPos(event->position().x(), event->position().y() + m_scrollY);
            int idx = layoutIndexAtY(canvasPos.y());
            if (isOnBodyText(canvasPos, idx)) {
                // Don't start text selection if clicking a link
                const auto &ml = m_layouts[idx];
                QString link = hitTestLink(ml, canvasPos);
                if (link.isEmpty()) {
                    int cursorPos = hitTestBodyCursor(ml, canvasPos);
                    if (cursorPos >= 0) {
                        m_textAnchorSet = true;
                        m_textSelection.anchorLayoutIdx = idx;
                        m_textSelection.anchorCursorPos = cursorPos;
                        m_textSelection.activeLayoutIdx = idx;
                        m_textSelection.activeCursorPos = cursorPos;
                        m_textSelection.active = false;  // not yet — wait for drag
                    }
                }
            }
        }

        // Click clears any existing text selection (unless we just set a new anchor)
        if (!m_textAnchorSet && m_textSelection.hasSelection()) {
            m_textSelection.clear();
            update();
        }

        event->accept();
    }
}
```

- [ ] **Step 2: Replace mouseMoveEvent**

Replace the entire `mouseMoveEvent` function (currently lines 413-463):

```cpp
void ChatPainter::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        qreal dy = event->position().y() - m_dragStartY;
        if (!m_dragMoved && qAbs(dy) > 4)
            m_dragMoved = true;

        if (m_dragMoved && event->buttons() & Qt::LeftButton) {
            // Text selection: drag started on body text
            if (m_textAnchorSet) {
                m_textSelection.active = true;
                QPointF canvasPos(event->position().x(), event->position().y() + m_scrollY);
                int idx = layoutIndexAtY(canvasPos.y());

                // Clamp to valid range
                if (idx < 0) idx = 0;
                if (idx >= m_layouts.size()) idx = m_layouts.size() - 1;

                if (idx >= 0 && idx < m_layouts.size()) {
                    const auto &ml = m_layouts[idx];
                    if (ml.bodyDoc && !ml.isSystem) {
                        int cursorPos = hitTestBodyCursor(ml, canvasPos);
                        if (cursorPos < 0) {
                            // Mouse is outside body rect — clamp to start or end
                            if (canvasPos.y() < ml.bodyRect.top())
                                cursorPos = 0;
                            else
                                cursorPos = ml.bodyDoc->characterCount() - 1;
                        }
                        m_textSelection.activeLayoutIdx = idx;
                        m_textSelection.activeCursorPos = cursorPos;
                    } else {
                        // System message or no body — clamp to nearest boundary
                        if (idx < m_textSelection.anchorLayoutIdx) {
                            // Dragging upward past a system msg — use the message above it
                            for (int j = idx; j >= 0; --j) {
                                if (!m_layouts[j].isSystem && m_layouts[j].bodyDoc) {
                                    m_textSelection.activeLayoutIdx = j;
                                    m_textSelection.activeCursorPos = 0;
                                    break;
                                }
                            }
                        } else {
                            // Dragging downward past a system msg
                            for (int j = idx; j < m_layouts.size(); ++j) {
                                if (!m_layouts[j].isSystem && m_layouts[j].bodyDoc) {
                                    m_textSelection.activeLayoutIdx = j;
                                    m_textSelection.activeCursorPos = m_layouts[j].bodyDoc->characterCount() - 1;
                                    break;
                                }
                            }
                        }
                    }
                }

                setCursor(Qt::IBeamCursor);
                update();
                event->accept();
                return;
            }

            // Whole-message selection: drag NOT on body text + Ctrl held, or already in selection mode
            if (event->modifiers() & Qt::ControlModifier || m_selectionMode) {
                if (!m_selectionMode) {
                    qreal pressCanvasY = m_pressCanvasPos.y() + m_scrollY;
                    int pressIdx = layoutIndexAtY(pressCanvasY);
                    if (pressIdx >= 0 && pressIdx < m_layouts.size()) {
                        const auto &ml = m_layouts[pressIdx];
                        if (!ml.isSystem && ml.messageId > 0)
                            enterSelectionMode(ml.messageId);
                    }
                }

                if (m_selectionMode) {
                    qreal canvasY = event->position().y() + m_scrollY;
                    int idx = layoutIndexAtY(canvasY);
                    if (idx >= 0 && idx < m_layouts.size()) {
                        const auto &ml = m_layouts[idx];
                        if (!ml.isSystem && ml.messageId > 0
                            && !m_selectedIds.contains(ml.messageId)) {
                            m_selectedIds.insert(ml.messageId);
                            emit selectionChanged(m_selectedIds.size());
                            update();
                        }
                    }
                }
            }
        }

        event->accept();
        return;
    }

    // Not dragging — update hover state and cursor
    if (!m_selectionMode) {
        setHoveredPos(event->position().x(), event->position().y());
        QPointF canvasPos(event->position().x(), event->position().y() + m_scrollY);
        int idx = layoutIndexAtY(canvasPos.y());

        // IBeam cursor over body text, pointing hand over links
        QString hit = hitTestAt(event->position().x(), event->position().y());
        if (!hit.isEmpty())
            setCursor(Qt::PointingHandCursor);
        else if (isOnBodyText(canvasPos, idx))
            setCursor(Qt::IBeamCursor);
        else
            setCursor(Qt::ArrowCursor);
    } else {
        setCursor(Qt::ArrowCursor);
    }
}
```

- [ ] **Step 3: Update mouseReleaseEvent**

Replace the entire `mouseReleaseEvent` function (currently lines 465 to about line 540):

Read the full current function first — it's long. The key change: if text selection was activated, just freeze it and return. If no drag happened, clear text selection and proceed with existing click logic.

In the existing function, add at the very top (after `m_dragging = false;`):

```cpp
    // Text selection: if drag happened on body text, freeze selection and done
    if (m_textAnchorSet && m_dragMoved && m_textSelection.active) {
        m_textAnchorSet = false;
        event->accept();
        return;
    }
    m_textAnchorSet = false;

    // If we had a text selection and user clicked (no drag), clear it
    if (!m_dragMoved && m_textSelection.hasSelection()) {
        m_textSelection.clear();
        update();
    }
```

Insert this block right after `m_dragging = false;` (line 468) and before the existing `if (!m_dragMoved && event->button() == Qt::LeftButton)` block.

- [ ] **Step 4: Build and verify compilation**

Run:
```bash
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5
```

Expected: successful build.

- [ ] **Step 5: Commit**

```bash
cd /c/src/talk-desktop-qt
git add src/painter/ChatPainter.cpp
git commit -m "feat(selection): rework mouse events for character-level text selection"
```

---

### Task 4: Paint selection highlights

**Files:**
- Modify: `src/painter/ChatPainter.cpp` (paintOwnMessage, paintOtherMessage)

- [ ] **Step 1: Create a helper lambda for painting body text with selection**

The body text painting code is duplicated in `paintOwnMessage` and `paintOtherMessage`. Instead of duplicating the selection logic, we'll modify both sites.

In `paintOwnMessage`, replace the body text painting block (currently around lines 781-791):

Old:
```cpp
    // Body text
    if (ml.bodyDoc) {
        p->save();
        QRectF bodyR = ml.bodyRect.translated(0, offsetY);
        p->translate(bodyR.topLeft());
        // Set default text color for QTextDocument
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.palette.setColor(QPalette::Text, m_theme.textPrimary);
        ctx.clip = QRectF(0, 0, bodyR.width(), bodyR.height());
        ml.bodyDoc->documentLayout()->draw(p, ctx);
        p->restore();
    }
```

New:
```cpp
    // Body text
    if (ml.bodyDoc) {
        p->save();
        QRectF bodyR = ml.bodyRect.translated(0, offsetY);
        p->translate(bodyR.topLeft());
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.palette.setColor(QPalette::Text, m_theme.textPrimary);
        ctx.clip = QRectF(0, 0, bodyR.width(), bodyR.height());

        // Text selection highlight
        if (m_textSelection.hasSelection()) {
            auto range = m_textSelection.normalized();
            int layoutIdx = &ml - m_layouts.constData();
            if (layoutIdx >= range.startIdx && layoutIdx <= range.endIdx) {
                QTextCursor cursor(ml.bodyDoc.get());
                if (layoutIdx == range.startIdx && layoutIdx == range.endIdx) {
                    cursor.setPosition(range.startPos);
                    cursor.setPosition(range.endPos, QTextCursor::KeepAnchor);
                } else if (layoutIdx == range.startIdx) {
                    cursor.setPosition(range.startPos);
                    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
                } else if (layoutIdx == range.endIdx) {
                    cursor.setPosition(0);
                    cursor.setPosition(range.endPos, QTextCursor::KeepAnchor);
                } else {
                    cursor.setPosition(0);
                    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
                }
                QAbstractTextDocumentLayout::Selection sel;
                sel.cursor = cursor;
                QTextCharFormat fmt;
                fmt.setBackground(QColor(46, 196, 182, 77));
                sel.format = fmt;
                ctx.selections.append(sel);
            }
        }

        ml.bodyDoc->documentLayout()->draw(p, ctx);
        p->restore();
    }
```

- [ ] **Step 2: Apply the same change to paintOtherMessage**

In `paintOtherMessage`, replace the body text painting block (currently around lines 897-906) with the exact same code as Step 1. The code is identical — it uses `&ml - m_layouts.constData()` to find the layout index.

Old:
```cpp
    // Body text
    if (ml.bodyDoc) {
        p->save();
        QRectF bodyR = ml.bodyRect.translated(0, offsetY);
        p->translate(bodyR.topLeft());
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.palette.setColor(QPalette::Text, m_theme.textPrimary);
        ctx.clip = QRectF(0, 0, bodyR.width(), bodyR.height());
        ml.bodyDoc->documentLayout()->draw(p, ctx);
        p->restore();
    }
```

New (same as Step 1):
```cpp
    // Body text
    if (ml.bodyDoc) {
        p->save();
        QRectF bodyR = ml.bodyRect.translated(0, offsetY);
        p->translate(bodyR.topLeft());
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.palette.setColor(QPalette::Text, m_theme.textPrimary);
        ctx.clip = QRectF(0, 0, bodyR.width(), bodyR.height());

        // Text selection highlight
        if (m_textSelection.hasSelection()) {
            auto range = m_textSelection.normalized();
            int layoutIdx = &ml - m_layouts.constData();
            if (layoutIdx >= range.startIdx && layoutIdx <= range.endIdx) {
                QTextCursor cursor(ml.bodyDoc.get());
                if (layoutIdx == range.startIdx && layoutIdx == range.endIdx) {
                    cursor.setPosition(range.startPos);
                    cursor.setPosition(range.endPos, QTextCursor::KeepAnchor);
                } else if (layoutIdx == range.startIdx) {
                    cursor.setPosition(range.startPos);
                    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
                } else if (layoutIdx == range.endIdx) {
                    cursor.setPosition(0);
                    cursor.setPosition(range.endPos, QTextCursor::KeepAnchor);
                } else {
                    cursor.setPosition(0);
                    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
                }
                QAbstractTextDocumentLayout::Selection sel;
                sel.cursor = cursor;
                QTextCharFormat fmt;
                fmt.setBackground(QColor(46, 196, 182, 77));
                sel.format = fmt;
                ctx.selections.append(sel);
            }
        }

        ml.bodyDoc->documentLayout()->draw(p, ctx);
        p->restore();
    }
```

- [ ] **Step 3: Build and verify compilation**

Run:
```bash
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5
```

Expected: successful build.

- [ ] **Step 4: Commit**

```bash
cd /c/src/talk-desktop-qt
git add src/painter/ChatPainter.cpp
git commit -m "feat(selection): paint character-level selection highlights via PaintContext"
```

---

### Task 5: Ctrl+C copy and double-click word selection

**Files:**
- Modify: `src/painter/ChatPainter.cpp` (keyPressEvent, mouseDoubleClickEvent)

- [ ] **Step 1: Add Ctrl+C handling to keyPressEvent**

In `src/painter/ChatPainter.cpp`, find `keyPressEvent` (around line 560). Replace the entire function:

Old:
```cpp
void ChatPainter::keyPressEvent(QKeyEvent *event)
{
    if (m_selectionMode && event->key() == Qt::Key_Escape) {
        exitSelectionMode();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}
```

New:
```cpp
void ChatPainter::keyPressEvent(QKeyEvent *event)
{
    if (m_selectionMode && event->key() == Qt::Key_Escape) {
        exitSelectionMode();
        event->accept();
        return;
    }
    // Ctrl+C: copy selected text
    if (event->matches(QKeySequence::Copy) && m_textSelection.hasSelection()) {
        copySelectedText();
        event->accept();
        return;
    }
    // Escape clears text selection
    if (event->key() == Qt::Key_Escape && m_textSelection.hasSelection()) {
        m_textSelection.clear();
        update();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}
```

- [ ] **Step 2: Add mouseDoubleClickEvent override to ChatPainter.h**

In `src/painter/ChatPainter.h`, add in the protected section after `bool event(QEvent *event) override;` (after line 84):

```cpp
    void mouseDoubleClickEvent(QMouseEvent *event) override;
```

- [ ] **Step 3: Implement mouseDoubleClickEvent**

In `src/painter/ChatPainter.cpp`, add after the `mouseReleaseEvent` function:

```cpp
void ChatPainter::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || m_selectionMode) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    QPointF canvasPos(event->position().x(), event->position().y() + m_scrollY);
    int idx = layoutIndexAtY(canvasPos.y());
    if (idx < 0 || idx >= m_layouts.size()) return;

    const auto &ml = m_layouts[idx];
    if (ml.isSystem || !ml.bodyDoc) return;

    int cursorPos = hitTestBodyCursor(ml, canvasPos);
    if (cursorPos < 0) return;

    QTextCursor cursor(ml.bodyDoc.get());
    cursor.setPosition(cursorPos);
    cursor.select(QTextCursor::WordUnderCursor);

    m_textSelection.anchorLayoutIdx = idx;
    m_textSelection.anchorCursorPos = cursor.selectionStart();
    m_textSelection.activeLayoutIdx = idx;
    m_textSelection.activeCursorPos = cursor.selectionEnd();
    m_textSelection.active = true;
    update();
    event->accept();
}
```

- [ ] **Step 4: Build and verify compilation**

Run:
```bash
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5
```

Expected: successful build.

- [ ] **Step 5: Commit**

```bash
cd /c/src/talk-desktop-qt
git add src/painter/ChatPainter.h src/painter/ChatPainter.cpp
git commit -m "feat(selection): Ctrl+C copy and double-click word selection"
```

---

### Task 6: Smoke test — deploy and verify

- [ ] **Step 1: Deploy debug build**

```bash
cmd.exe //c "taskkill /IM talq.exe /F" 2>/dev/null
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5
cd /c/build/talq && bash /c/src/talk-desktop-qt/scripts/deploy-dev.sh
```

- [ ] **Step 2: Manual test checklist**

1. Open TalQ, navigate to a conversation with messages
2. Hover over message body text → cursor should change to IBeam
3. Click and drag within a message → characters should highlight in teal
4. Release → selection stays
5. Press Ctrl+C → text should appear in clipboard (paste in notepad to verify)
6. Click elsewhere → selection clears
7. Double-click a word → word highlights
8. Click and drag from one message into another → selection spans both messages, middle messages fully highlighted
9. Ctrl+click a message → should still enter whole-message selection mode (not text selection)
10. Click a link → should still open the link (text selection should not interfere)
