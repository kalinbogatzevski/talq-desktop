# In-Bubble Text Selection

**Date:** 2026-04-03
**Status:** Approved
**Scope:** Telegram-style character-level text selection within and across message bubbles, with copy support.

## Problem

TalQ currently supports whole-message selection (Ctrl+click or drag) for bulk actions like delete/forward, but has no way to select and copy text within message bubbles. Users expect standard text selection (click-drag, double-click for word, Ctrl+C to copy) as in Telegram Desktop, WhatsApp, and other chat apps.

## Approach

Use the existing `QTextDocument` (`bodyDoc`) on each `MessageLayout` to provide character-level hit-testing via `QAbstractTextDocumentLayout::hitTest()` and highlight painting via `PaintContext.selections`. Selection state tracks an anchor and active position, each referencing a layout index and cursor position within that message's bodyDoc.

## Selection State

New struct stored as `m_textSelection` in ChatPainter:

```cpp
struct TextSelection {
    int anchorLayoutIdx = -1;   // layout index where mouse was pressed
    int anchorCursorPos = 0;    // character position within anchor's bodyDoc
    int activeLayoutIdx = -1;   // layout index where mouse currently is
    int activeCursorPos = 0;    // character position within active's bodyDoc
    bool active = false;        // true when a selection exists

    bool hasSelection() const { return active && !(anchorLayoutIdx == activeLayoutIdx && anchorCursorPos == activeCursorPos); }
    void clear() { anchorLayoutIdx = activeLayoutIdx = -1; anchorCursorPos = activeCursorPos = 0; active = false; }

    // Returns ordered {startLayoutIdx, startCursorPos, endLayoutIdx, endCursorPos}
    struct Range { int startIdx, startPos, endIdx, endPos; };
    Range normalized() const;
};
```

`normalized()` returns the range in document order (top-to-bottom, left-to-right) regardless of drag direction.

## Mouse Event Changes

### mousePressEvent (left button, no Ctrl)

1. Compute canvas position: `canvasPos = viewportPos + scrollY`
2. Find layout at Y via existing `layoutIndexAtY(canvasY)`
3. If layout has a `bodyDoc` and press is within `bodyRect`:
   - Convert to body-local coordinates: `localPos = canvasPos - bodyRect.topLeft()`
   - Hit-test: `int cursorPos = bodyDoc->documentLayout()->hitTest(localPos, Qt::FuzzyHit)`
   - If `cursorPos >= 0`: set anchor, clear any previous text selection
   - Set `m_textAnchorSet = true` (press was on body text)
4. If press is NOT on body text: clear text selection, fall through to existing behavior (scroll drag, hit-test for links/reactions)

### mouseMoveEvent (during drag)

1. If `m_textAnchorSet` and moved >4px: activate text selection mode
2. Compute current layout index and cursor position (same hit-test as press)
3. If cursor is above/below all messages, clamp to first/last message and position 0/end
4. Update `m_textSelection.activeLayoutIdx` and `activeCursorPos`
5. Set cursor to `Qt::IBeamCursor`
6. `update()` to repaint with highlights
7. If text selection is active, do NOT enter whole-message selection mode and do NOT scroll-drag

### mouseReleaseEvent

1. If text selection was activated (dragged >4px on body text): freeze selection, do nothing else
2. If no drag happened (click):
   - Clear text selection
   - Fall through to existing click logic (links, reactions, files, etc.)

### Double-click

1. Hit-test the bodyDoc at click position
2. Create a `QTextCursor` at that position, call `cursor.select(QTextCursor::WordUnderCursor)`
3. Set anchor and active from the cursor's selectionStart/selectionEnd
4. This gives word selection for free

### Interaction with existing behaviors

- **Ctrl+click** still enters whole-message selection mode (unchanged). Entering whole-message mode clears any text selection.
- **Right-click** on selected text: shows context menu with "Copy" (future). For now, just Ctrl+C.
- **Any left click** when text is selected: clears text selection first, then proceeds with normal logic.
- **Scroll** (wheel) while selecting: selection persists, view scrolls normally. (Auto-scroll on drag-to-edge is out of scope for v1.)

## Highlight Painting

For each message in the selection range, set up `PaintContext.selections` before calling `bodyDoc->documentLayout()->draw()`:

```cpp
QAbstractTextDocumentLayout::PaintContext ctx;
ctx.palette.setColor(QPalette::Text, m_theme.textPrimary);

if (textSelection.hasSelection()) {
    auto range = textSelection.normalized();
    if (layoutIdx >= range.startIdx && layoutIdx <= range.endIdx) {
        QAbstractTextDocumentLayout::Selection sel;
        QTextCursor cursor(bodyDoc.get());

        if (layoutIdx == range.startIdx && layoutIdx == range.endIdx) {
            // Partial selection within single message
            cursor.setPosition(range.startPos);
            cursor.setPosition(range.endPos, QTextCursor::KeepAnchor);
        } else if (layoutIdx == range.startIdx) {
            // First message: from startPos to end
            cursor.setPosition(range.startPos);
            cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        } else if (layoutIdx == range.endIdx) {
            // Last message: from start to endPos
            cursor.setPosition(0);
            cursor.setPosition(range.endPos, QTextCursor::KeepAnchor);
        } else {
            // Middle message: fully selected
            cursor.setPosition(0);
            cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        }

        sel.cursor = cursor;
        QTextCharFormat fmt;
        fmt.setBackground(QColor(46, 196, 182, 77));  // teal at 30% opacity
        sel.format = fmt;
        ctx.selections.append(sel);
    }
}

bodyDoc->documentLayout()->draw(p, ctx);
```

## Copy (Ctrl+C)

When `Ctrl+C` is pressed and `m_textSelection.hasSelection()`:

1. Get the normalized range
2. For each layout from `startIdx` to `endIdx`:
   - Create a `QTextCursor` on the message's `bodyDoc`
   - Set the appropriate range (partial for first/last, full for middle)
   - Extract `cursor.selectedText()`
3. Join all extracted texts with `"\n"` (or `"\n\n"` between messages)
4. `QApplication::clipboard()->setText(joinedText)`

Note: `QTextCursor::selectedText()` uses Unicode paragraph separator (U+2029) instead of `\n`. Replace with `\n` before joining.

## Cursor Appearance

- **Over body text** (no selection active): `Qt::IBeamCursor`
- **During text selection drag**: `Qt::IBeamCursor`
- **Over links**: `Qt::PointingHandCursor` (unchanged)
- **Over non-body areas**: `Qt::ArrowCursor` (unchanged)

The cursor should show IBeam when hovering over body text even when not selecting. This signals to the user that text is selectable.

## Files Modified

| File | Change |
|---|---|
| `src/painter/ChatPainter.h` | Add `TextSelection` struct, `m_textSelection`, `m_textAnchorSet`, helper methods |
| `src/painter/ChatPainter.cpp` | Mouse event changes, highlight painting in paintOwnMessage/paintOtherMessage, copy handler, cursor management |

## What stays the same

- Whole-message selection mode (Ctrl+click, `m_selectionMode`, `m_selectedIds`) is untouched
- `MessageLayout` and `LayoutEngine` are not modified
- Link/reaction/file click handling works as before
- Scroll behavior is unchanged

## Out of scope (v1)

- Auto-scroll when dragging to top/bottom edge
- Right-click context menu with Copy
- Triple-click select-all
- Selecting text in system messages or date separators
- Selecting text in reply quotes
- Keyboard-driven selection (Shift+arrow)
