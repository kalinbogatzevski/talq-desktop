# Multi-Message Selection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Telegram-style multi-message selection with Copy, Forward, and Delete bulk actions.

**Architecture:** Selection state lives in ChatPainter (`QSet<int>` of message IDs + `bool m_selectionMode`). A new `SelectionBarWidget` replaces the composer when active. Forward uses a `ConversationPickerDialog` to choose a target, then posts messages via the existing API. All painting changes are in ChatPainter — no LayoutEngine changes needed (checkboxes are painted as overlays in existing margins).

**Tech Stack:** Qt 6 Widgets, QPainter, existing ApiClient/MessageListModel/ConversationListModel

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `src/painter/ChatPainter.h` | Modify | Add selection state, signals, public API |
| `src/painter/ChatPainter.cpp` | Modify | Selection painting, mouse handling, copy formatting |
| `src/ui/SelectionBarWidget.h` | Create | Action bar header (widget declaration) |
| `src/ui/SelectionBarWidget.cpp` | Create | Action bar with Forward/Copy/Delete/Cancel buttons |
| `src/ui/ConversationPickerDialog.h` | Create | Forward target picker dialog header |
| `src/ui/ConversationPickerDialog.cpp` | Create | Modal dialog with conversation list + search |
| `src/ui/MainWindow.h` | Modify | Add selection bar + picker members |
| `src/ui/MainWindow.cpp` | Modify | Wire selection signals, context menu "Select", forward logic |
| `src/models/MessageListModel.h` | Modify | Add `sendMessageToToken()` for forwarding |
| `src/models/MessageListModel.cpp` | Modify | Implement forward-to-conversation |
| `CMakeLists.txt` | Modify | Add new source files |

---

### Task 1: ChatPainter selection state and signals

**Files:**
- Modify: `src/painter/ChatPainter.h`
- Modify: `src/painter/ChatPainter.cpp`

- [ ] **Step 1: Add selection state members and public API to ChatPainter.h**

Add after the `m_previewPending` line (line 138) in the private section:

```cpp
    // ── Selection state ──
    bool m_selectionMode = false;
    QSet<int> m_selectedIds;
```

Add public methods after `setHoveredPos` (line 49):

```cpp
    // ── Selection ──
    bool selectionMode() const { return m_selectionMode; }
    QSet<int> selectedIds() const { return m_selectedIds; }
    void enterSelectionMode(int firstMessageId);
    void exitSelectionMode();
    void toggleMessageSelection(int messageId);
    void clearSelection();
    QVector<QVariantMap> selectedMessages() const;  // sorted chronologically
```

Add signals after `fileDropped` (line 64):

```cpp
    void selectionModeChanged(bool active);
    void selectionChanged(int count);
```

- [ ] **Step 2: Implement selection methods in ChatPainter.cpp**

Add at the end of the Properties section (after `setScrollY`, around line 101):

```cpp
void ChatPainter::enterSelectionMode(int firstMessageId)
{
    if (m_selectionMode) return;
    m_selectionMode = true;
    m_selectedIds.clear();
    m_selectedIds.insert(firstMessageId);
    m_hoveredIndex = -1;
    emit selectionModeChanged(true);
    emit selectionChanged(1);
    update();
}

void ChatPainter::exitSelectionMode()
{
    if (!m_selectionMode) return;
    m_selectionMode = false;
    m_selectedIds.clear();
    emit selectionModeChanged(false);
    emit selectionChanged(0);
    update();
}

void ChatPainter::toggleMessageSelection(int messageId)
{
    if (m_selectedIds.contains(messageId))
        m_selectedIds.remove(messageId);
    else
        m_selectedIds.insert(messageId);

    // Auto-exit if nothing selected
    if (m_selectedIds.isEmpty()) {
        exitSelectionMode();
        return;
    }

    emit selectionChanged(m_selectedIds.size());
    update();
}

void ChatPainter::clearSelection()
{
    m_selectedIds.clear();
    exitSelectionMode();
}

QVector<QVariantMap> ChatPainter::selectedMessages() const
{
    // Collect selected messages sorted chronologically (oldest first)
    // m_layouts is oldest-first (index 0 = oldest)
    QVector<QVariantMap> result;
    for (const auto &ml : m_layouts) {
        if (m_selectedIds.contains(ml.messageId)) {
            QVariantMap m;
            m["messageId"] = ml.messageId;
            m["isOwn"] = ml.isOwn;
            m["actorName"] = ml.actorName;
            m["messageText"] = ml.bodyHtml;
            m["timeString"] = ml.timeString;
            m["hasFile"] = ml.hasFile;
            m["fileId"] = ml.fileId;
            m["fileName"] = ml.fileName;
            m["fileMime"] = ml.fileMime;
            result.append(m);
        }
    }
    return result;
}
```

- [ ] **Step 3: Verify it compiles**

Run: `cmake --build /c/build/talq --target talq 2>&1 | tail -5`
Expected: Compiles with no errors (new methods not yet called)

- [ ] **Step 4: Commit**

```bash
git add src/painter/ChatPainter.h src/painter/ChatPainter.cpp
git commit -m "feat: add selection state and signals to ChatPainter"
```

---

### Task 2: Selection painting (checkboxes + row highlight)

**Files:**
- Modify: `src/painter/ChatPainter.cpp`

- [ ] **Step 1: Add selection highlight painting in paintEvent**

In `paintEvent` (line 479), after the viewport culling check (`if (ml.totalY + ml.totalHeight < vpTop || ml.totalY > vpBottom) continue;` at line 498), add selection row highlight before the message painting:

```cpp
        // Selection highlight — full-width teal tint
        if (m_selectionMode && m_selectedIds.contains(ml.messageId)) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(46, 196, 182, 30));  // rgba(46,196,182,0.12)
            p.drawRect(QRectF(0, ml.totalY + offsetY, width(), ml.totalHeight));
        }
```

- [ ] **Step 2: Add checkbox painting in paintEvent**

After the selection highlight block, add checkbox painting for non-system messages when in selection mode:

```cpp
        // Selection checkbox
        if (m_selectionMode && !ml.isSystem) {
            bool selected = m_selectedIds.contains(ml.messageId);
            qreal ckSize = 18;
            qreal ckY = ml.totalY + offsetY + (ml.totalHeight - ckSize) / 2.0;
            qreal ckX = ml.isOwn ? (width() - ckSize - 8) : 8;

            if (selected) {
                // Filled teal circle with checkmark
                p.setPen(Qt::NoPen);
                p.setBrush(m_theme.accent);
                p.drawEllipse(QRectF(ckX, ckY, ckSize, ckSize));
                // White checkmark
                p.setPen(QPen(Qt::white, 2));
                QPointF c(ckX + ckSize / 2.0, ckY + ckSize / 2.0);
                p.drawLine(QPointF(c.x() - 4, c.y()), QPointF(c.x() - 1, c.y() + 3));
                p.drawLine(QPointF(c.x() - 1, c.y() + 3), QPointF(c.x() + 4, c.y() - 3));
            } else {
                // Empty circle outline
                p.setPen(QPen(QColor(85, 85, 85), 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(QRectF(ckX, ckY, ckSize, ckSize));
            }
        }
```

- [ ] **Step 3: Suppress hover bar in selection mode**

Modify the hover bar painting condition (line 511). Change:

```cpp
        if (i == m_hoveredIndex && !ml.isSystem
```

to:

```cpp
        if (!m_selectionMode && i == m_hoveredIndex && !ml.isSystem
```

- [ ] **Step 4: Verify it compiles**

Run: `cmake --build /c/build/talq --target talq 2>&1 | tail -5`
Expected: Compiles. Visual verification comes after mouse handling is wired.

- [ ] **Step 5: Commit**

```bash
git add src/painter/ChatPainter.cpp
git commit -m "feat: paint selection checkboxes and row highlights"
```

---

### Task 3: Mouse handling for selection mode

**Files:**
- Modify: `src/painter/ChatPainter.cpp`

- [ ] **Step 1: Handle left-click in selection mode**

In `mouseReleaseEvent` (line 355), replace the block starting at `if (!m_dragMoved && event->button() == Qt::LeftButton)` (line 360) with:

```cpp
    if (!m_dragMoved && event->button() == Qt::LeftButton) {
        // ── Selection mode: click toggles selection on entire row ──
        if (m_selectionMode) {
            qreal canvasY = event->position().y() + m_scrollY;
            int idx = layoutIndexAtY(canvasY);
            if (idx >= 0 && idx < m_layouts.size()) {
                const auto &ml = m_layouts[idx];
                if (!ml.isSystem && ml.messageId > 0)
                    toggleMessageSelection(ml.messageId);
            }
        }
        // ── Ctrl+Click: toggle selection (enter mode if needed) ──
        else if (event->modifiers() & Qt::ControlModifier) {
            qreal canvasY = event->position().y() + m_scrollY;
            int idx = layoutIndexAtY(canvasY);
            if (idx >= 0 && idx < m_layouts.size()) {
                const auto &ml = m_layouts[idx];
                if (!ml.isSystem && ml.messageId > 0) {
                    if (!m_selectionMode)
                        enterSelectionMode(ml.messageId);
                    else
                        toggleMessageSelection(ml.messageId);
                }
            }
        }
        // ── Normal click: existing hit-test logic ──
        else {
            QString hit = m_pressHit;
            if (hit.startsWith("link:")) {
                QString url = hit.mid(5);
                if (url.startsWith("http://") || url.startsWith("https://"))
                    emit linkActivated(url);
            } else if (hit.startsWith("reaction:")) {
                QStringList rparts = hit.mid(9).split(":");
                if (rparts.size() >= 2)
                    emit reactionClicked(rparts[0].toInt(), rparts.mid(1).join(":"));
            } else if (hit.startsWith("file:")) {
                QStringList parts = hit.mid(5).split(":");
                if (parts.size() >= 3) {
                    int fid = parts[0].toInt();
                    QString mime = parts[1];
                    QString fname = parts.mid(2).join(":");
                    emit fileClicked(fid, mime, fname);
                }
            } else if (hit.startsWith("reply:")) {
                qreal canvasY = event->position().y() + m_scrollY;
                int clickIdx = layoutIndexAtY(canvasY);
                if (clickIdx >= 0 && clickIdx < m_layouts.size()) {
                    const auto &clickMl = m_layouts[clickIdx];
                    emit replyRequested(clickMl.messageId, clickMl.actorName, clickMl.bodyHtml);
                }
            } else if (hit.startsWith("react:")) {
                int rMsgId = hit.mid(6).toInt();
                int rIdx = layoutIndexAtY(event->position().y() + m_scrollY);
                if (rIdx >= 0 && rIdx < m_layouts.size()) {
                    QRectF reactR = hoverBarReactRect(m_layouts[rIdx]);
                    QPoint btnCenter = mapToGlobal(QPoint(
                        qRound(reactR.center().x()),
                        qRound(reactR.center().y() - m_scrollY)));
                    emit reactRequested(rMsgId, btnCenter);
                }
            }
        }
    }
```

- [ ] **Step 2: Suppress context menu in selection mode**

In the right-click block (line 402), wrap with selection mode check:

```cpp
    if (!m_dragMoved && event->button() == Qt::RightButton) {
        if (!m_selectionMode) {
            QVariantMap msg = messageAt(event->position().x(), event->position().y());
            if (msg.value("messageId").toInt() > 0) {
                emit contextMenuRequested(msg, mapToGlobal(event->position().toPoint()));
            }
        }
    }
```

- [ ] **Step 3: Suppress hover tracking in selection mode**

In `mouseMoveEvent` (line 334), in the `else` branch (non-dragging), wrap hover update:

```cpp
    } else {
        if (!m_selectionMode) {
            setHoveredPos(event->position().x(), event->position().y());
            QString hit = hitTestAt(event->position().x(), event->position().y());
            if (!hit.isEmpty())
                setCursor(Qt::PointingHandCursor);
            else
                setCursor(Qt::ArrowCursor);
        } else {
            setCursor(Qt::ArrowCursor);
        }
    }
```

- [ ] **Step 4: Add Escape key handler**

Override `keyPressEvent` in ChatPainter.h — add to the protected section after `dropEvent`:

```cpp
    void keyPressEvent(QKeyEvent *event) override;
```

Implement in ChatPainter.cpp:

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

Add `#include <QKeyEvent>` to the includes at the top of ChatPainter.cpp.

- [ ] **Step 5: Clear selection on conversation switch**

In `setModel` (line 36 of ChatPainter.cpp), add at the start of the method after the `if (mdl == m_model) return;` check:

```cpp
    // Exit selection mode when switching conversations
    if (m_selectionMode)
        exitSelectionMode();
```

- [ ] **Step 6: Build and manually test**

Run: `cmake --build /c/build/talq --target talq 2>&1 | tail -5`
Expected: Compiles. Test manually: Ctrl+Click should enter selection mode, click rows to toggle, Esc to exit.

- [ ] **Step 7: Commit**

```bash
git add src/painter/ChatPainter.h src/painter/ChatPainter.cpp
git commit -m "feat: selection mode mouse handling — click, Ctrl+click, Esc"
```

---

### Task 4: SelectionBarWidget

**Files:**
- Create: `src/ui/SelectionBarWidget.h`
- Create: `src/ui/SelectionBarWidget.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create SelectionBarWidget.h**

```cpp
#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>

class SelectionBarWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SelectionBarWidget(QWidget *parent = nullptr);

    void setCount(int count);
    void setDeleteVisible(bool visible);

signals:
    void forwardClicked();
    void copyClicked();
    void deleteClicked();
    void cancelClicked();

private:
    QLabel *m_countLabel = nullptr;
    QPushButton *m_forwardBtn = nullptr;
    QPushButton *m_copyBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    QPushButton *m_cancelBtn = nullptr;
};
```

- [ ] **Step 2: Create SelectionBarWidget.cpp**

```cpp
#include "SelectionBarWidget.h"

SelectionBarWidget::SelectionBarWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 8, 16, 8);
    layout->setSpacing(6);

    setStyleSheet("background: #252536;");

    m_countLabel = new QLabel(this);
    m_countLabel->setStyleSheet("color: #2ec4b6; font-weight: 600; font-size: 13px; background: transparent;");
    layout->addWidget(m_countLabel);
    layout->addStretch();

    auto makeBtn = [this](const QString &text, const QString &style) {
        auto *btn = new QPushButton(text, this);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(style);
        return btn;
    };

    m_forwardBtn = makeBtn(QStringLiteral("\u2197\uFE0F Forward"),
        "QPushButton { background: #2a2a3e; color: #e0e0e0; border: none; border-radius: 8px; padding: 7px 14px; font-size: 12px; }"
        "QPushButton:hover { background: #353550; }");
    layout->addWidget(m_forwardBtn);

    m_copyBtn = makeBtn(QStringLiteral("\U0001F4CB Copy"),
        "QPushButton { background: #2a2a3e; color: #e0e0e0; border: none; border-radius: 8px; padding: 7px 14px; font-size: 12px; }"
        "QPushButton:hover { background: #353550; }");
    layout->addWidget(m_copyBtn);

    m_deleteBtn = makeBtn(QStringLiteral("\U0001F5D1\uFE0F Delete"),
        "QPushButton { background: rgba(248,81,73,0.15); color: #f85149; border: none; border-radius: 8px; padding: 7px 14px; font-size: 12px; }"
        "QPushButton:hover { background: rgba(248,81,73,0.25); }");
    layout->addWidget(m_deleteBtn);

    m_cancelBtn = makeBtn(QStringLiteral("\u2715 Cancel"),
        "QPushButton { background: #2a2a3e; color: #888; border: none; border-radius: 8px; padding: 7px 14px; font-size: 12px; }"
        "QPushButton:hover { background: #353550; color: #bbb; }");
    layout->addWidget(m_cancelBtn);

    connect(m_forwardBtn, &QPushButton::clicked, this, &SelectionBarWidget::forwardClicked);
    connect(m_copyBtn, &QPushButton::clicked, this, &SelectionBarWidget::copyClicked);
    connect(m_deleteBtn, &QPushButton::clicked, this, &SelectionBarWidget::deleteClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &SelectionBarWidget::cancelClicked);

    setCount(0);
}

void SelectionBarWidget::setCount(int count)
{
    m_countLabel->setText(QString("%1 message%2 selected")
        .arg(count).arg(count == 1 ? "" : "s"));
}

void SelectionBarWidget::setDeleteVisible(bool visible)
{
    m_deleteBtn->setVisible(visible);
}
```

- [ ] **Step 3: Add new files to CMakeLists.txt**

Find the `set(SOURCES` or `add_executable(talq` block and add:

```
    src/ui/SelectionBarWidget.h
    src/ui/SelectionBarWidget.cpp
```

- [ ] **Step 4: Build**

Run: `cmake --build /c/build/talq --target talq 2>&1 | tail -5`
Expected: Compiles (widget not yet shown)

- [ ] **Step 5: Commit**

```bash
git add src/ui/SelectionBarWidget.h src/ui/SelectionBarWidget.cpp CMakeLists.txt
git commit -m "feat: add SelectionBarWidget — action bar for multi-select"
```

---

### Task 5: Wire selection bar into MainWindow

**Files:**
- Modify: `src/ui/MainWindow.h`
- Modify: `src/ui/MainWindow.cpp`

- [ ] **Step 1: Add forward declarations and members to MainWindow.h**

Add forward declaration after `class SettingsDialog;` (line 35):

```cpp
class SelectionBarWidget;
```

Add member after `m_composer` (line 117):

```cpp
    SelectionBarWidget *m_selectionBar = nullptr;
```

- [ ] **Step 2: Create and add selection bar to chat layout**

In `buildChatPage()` in MainWindow.cpp, after `m_composer` is added to layout (around line 451), add:

```cpp
    m_selectionBar = new SelectionBarWidget(chatCol);
    m_selectionBar->hide();
    chatLayout->addWidget(m_selectionBar);
```

Add `#include "SelectionBarWidget.h"` to the includes of MainWindow.cpp.

- [ ] **Step 3: Wire ChatPainter selection signals**

After the existing `fileDropped` connection (line 468), add:

```cpp
    // Selection mode
    connect(m_chatPainter, &ChatPainter::selectionModeChanged, this, [this](bool active) {
        if (active) {
            m_composer->hide();
            m_selectionBar->show();
        } else {
            m_selectionBar->hide();
            if (m_chatMode)
                m_composer->show();
        }
    });

    connect(m_chatPainter, &ChatPainter::selectionChanged, this, [this](int count) {
        m_selectionBar->setCount(count);
        // Delete only visible when all selected messages are own
        bool allOwn = true;
        for (const auto &msg : m_chatPainter->selectedMessages()) {
            if (!msg.value("isOwn").toBool()) {
                allOwn = false;
                break;
            }
        }
        m_selectionBar->setDeleteVisible(allOwn && count > 0);
    });
```

- [ ] **Step 4: Wire selection bar buttons**

After the selection signals, add:

```cpp
    connect(m_selectionBar, &SelectionBarWidget::cancelClicked, this, [this]() {
        m_chatPainter->exitSelectionMode();
    });

    connect(m_selectionBar, &SelectionBarWidget::copyClicked, this, [this]() {
        auto messages = m_chatPainter->selectedMessages();
        static const QRegularExpression htmlRe("<[^>]*>");
        QString text;
        for (const auto &msg : messages) {
            QString author = msg.value("actorName").toString();
            QString time = msg.value("timeString").toString();
            QString body = msg.value("messageText").toString();
            body.remove(htmlRe);

            if (msg.value("hasFile").toBool() && body.isEmpty()) {
                body = QStringLiteral("[File: %1]").arg(msg.value("fileName").toString());
            }

            text += QStringLiteral("[%1, %2]\n%3\n\n").arg(author, time, body);
        }
        if (!text.isEmpty())
            QApplication::clipboard()->setText(text.trimmed());
        m_chatPainter->exitSelectionMode();
    });

    connect(m_selectionBar, &SelectionBarWidget::deleteClicked, this, [this]() {
        auto messages = m_chatPainter->selectedMessages();
        int count = messages.size();
        auto reply = QMessageBox::question(this,
            QStringLiteral("Delete %1 message%2").arg(count).arg(count == 1 ? "" : "s"),
            QStringLiteral("Are you sure you want to delete %1 message%2?")
                .arg(count).arg(count == 1 ? "" : "s"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            for (const auto &msg : messages)
                m_messages->deleteMessage(msg.value("messageId").toInt());
            m_chatPainter->exitSelectionMode();
        }
    });
```

- [ ] **Step 5: Add "Select" to context menu**

In the existing `contextMenuRequested` lambda (line 471), after the Delete action block (line 580), add before `menu->popup(globalPos);`:

```cpp
        menu->addSeparator();
        menu->addAction(QStringLiteral("\u2610  Select"), this, [this, msgId]() {
            m_chatPainter->enterSelectionMode(msgId);
        });
```

- [ ] **Step 6: Add Ctrl+C shortcut for selection copy**

After the selection bar button connections, add:

```cpp
    // Ctrl+C copies selected messages when in selection mode
    auto *copyShortcut = new QShortcut(QKeySequence::Copy, m_chatPainter);
    connect(copyShortcut, &QShortcut::activated, this, [this]() {
        if (m_chatPainter->selectionMode()) {
            emit m_selectionBar->copyClicked();
        }
    });
```

- [ ] **Step 7: Build and test**

Run: `cmake --build /c/build/talq --target talq 2>&1 | tail -5`
Expected: Compiles. Manual test: right-click → Select enters mode, bar appears, Copy/Delete/Cancel work.

- [ ] **Step 8: Commit**

```bash
git add src/ui/MainWindow.h src/ui/MainWindow.cpp
git commit -m "feat: wire selection bar — Select menu, Copy, Delete, Cancel"
```

---

### Task 6: ConversationPickerDialog

**Files:**
- Create: `src/ui/ConversationPickerDialog.h`
- Create: `src/ui/ConversationPickerDialog.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create ConversationPickerDialog.h**

```cpp
#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

class ConversationListModel;

class ConversationPickerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ConversationPickerDialog(ConversationListModel *model,
                                       const QString &excludeToken,
                                       QWidget *parent = nullptr);

    QString selectedToken() const { return m_selectedToken; }
    QString selectedName() const { return m_selectedName; }

private:
    void populateList(const QString &filter = {});

    ConversationListModel *m_model;
    QString m_excludeToken;
    QLineEdit *m_searchField = nullptr;
    QListWidget *m_list = nullptr;
    QString m_selectedToken;
    QString m_selectedName;
};
```

- [ ] **Step 2: Create ConversationPickerDialog.cpp**

```cpp
#include "ConversationPickerDialog.h"
#include "models/ConversationListModel.h"

ConversationPickerDialog::ConversationPickerDialog(ConversationListModel *model,
                                                     const QString &excludeToken,
                                                     QWidget *parent)
    : QDialog(parent)
    , m_model(model)
    , m_excludeToken(excludeToken)
{
    setWindowTitle("Forward to...");
    setFixedSize(380, 480);
    setStyleSheet(
        "QDialog { background: #1e1e2e; }"
        "QLineEdit { background: #2a2a3e; color: #e0e0e0; border: 1px solid #363c48;"
        "  border-radius: 8px; padding: 8px 12px; font-size: 13px; }"
        "QLineEdit:focus { border-color: #2ec4b6; }"
        "QListWidget { background: transparent; border: none; outline: none; }"
        "QListWidget::item { color: #e0e0e0; padding: 10px 12px; border-radius: 8px;"
        "  font-size: 13px; }"
        "QListWidget::item:hover { background: rgba(255,255,255,0.06); }"
        "QListWidget::item:selected { background: rgba(46,196,182,0.15); }"
    );

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    m_searchField = new QLineEdit(this);
    m_searchField->setPlaceholderText("Search conversations...");
    layout->addWidget(m_searchField);

    m_list = new QListWidget(this);
    layout->addWidget(m_list);

    populateList();

    connect(m_searchField, &QLineEdit::textChanged, this, &ConversationPickerDialog::populateList);

    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        m_selectedToken = item->data(Qt::UserRole).toString();
        m_selectedName = item->text();
        accept();
    });
}

void ConversationPickerDialog::populateList(const QString &filter)
{
    m_list->clear();
    int count = m_model->rowCount();
    for (int i = 0; i < count; ++i) {
        QModelIndex idx = m_model->index(i, 0);
        QString token = idx.data(ConversationListModel::TokenRole).toString();
        if (token == m_excludeToken)
            continue;

        QString name = idx.data(ConversationListModel::DisplayNameRole).toString();
        if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive))
            continue;

        auto *item = new QListWidgetItem(name, m_list);
        item->setData(Qt::UserRole, token);
    }
}
```

- [ ] **Step 3: Add to CMakeLists.txt**

Add alongside the SelectionBarWidget entries:

```
    src/ui/ConversationPickerDialog.h
    src/ui/ConversationPickerDialog.cpp
```

- [ ] **Step 4: Build**

Run: `cmake --build /c/build/talq --target talq 2>&1 | tail -5`
Expected: Compiles

- [ ] **Step 5: Commit**

```bash
git add src/ui/ConversationPickerDialog.h src/ui/ConversationPickerDialog.cpp CMakeLists.txt
git commit -m "feat: add ConversationPickerDialog for message forwarding"
```

---

### Task 7: Forward implementation

**Files:**
- Modify: `src/models/MessageListModel.h`
- Modify: `src/models/MessageListModel.cpp`
- Modify: `src/ui/MainWindow.cpp`

- [ ] **Step 1: Add sendMessageToToken to MessageListModel.h**

Add after `createTopic` (line 96):

```cpp
    Q_INVOKABLE void sendMessageToToken(const QString &targetToken, const QString &text);
```

- [ ] **Step 2: Implement sendMessageToToken in MessageListModel.cpp**

Add at the end of the public methods section:

```cpp
void MessageListModel::sendMessageToToken(const QString &targetToken, const QString &text)
{
    QJsonObject body;
    body["message"] = text;
    body["replyTo"] = 0;

    QString path = QStringLiteral("/ocs/v2.php/apps/spreed/api/v1/chat/%1").arg(targetToken);
    m_api->post(path, body, [](QNetworkReply *reply) {
        // Fire and forget — we're not in the target conversation to update the model
        reply->deleteLater();
    });
}
```

- [ ] **Step 3: Wire Forward button in MainWindow.cpp**

After the deleteClicked connection, add:

```cpp
    connect(m_selectionBar, &SelectionBarWidget::forwardClicked, this, [this]() {
        auto messages = m_chatPainter->selectedMessages();
        if (messages.isEmpty()) return;

        auto *picker = new ConversationPickerDialog(m_conversations, m_activeConvToken, this);
        if (picker->exec() == QDialog::Accepted) {
            QString targetToken = picker->selectedToken();
            static const QRegularExpression htmlRe("<[^>]*>");

            for (const auto &msg : messages) {
                QString body = msg.value("messageText").toString();
                body.remove(htmlRe);

                if (body.isEmpty() && msg.value("hasFile").toBool())
                    body = QStringLiteral("[File: %1]").arg(msg.value("fileName").toString());

                if (!body.isEmpty())
                    m_messages->sendMessageToToken(targetToken, body);
            }
            m_chatPainter->exitSelectionMode();
        }
        picker->deleteLater();
    });
```

Add `#include "ConversationPickerDialog.h"` to MainWindow.cpp includes.

- [ ] **Step 4: Build and test**

Run: `cmake --build /c/build/talq --target talq 2>&1 | tail -5`
Expected: Compiles. Manual test: select messages → Forward → pick conversation → messages appear in target.

- [ ] **Step 5: Commit**

```bash
git add src/models/MessageListModel.h src/models/MessageListModel.cpp src/ui/MainWindow.cpp
git commit -m "feat: forward selected messages to another conversation"
```

---

### Task 8: Edge cases and polish

**Files:**
- Modify: `src/painter/ChatPainter.cpp`
- Modify: `src/ui/MainWindow.cpp`

- [ ] **Step 1: Handle deleted messages in selection**

In `onRowsRemoved` in ChatPainter.cpp, add cleanup of selection state. Find the method and add at the start:

```cpp
    // Remove deleted messages from selection
    if (m_selectionMode) {
        bool changed = false;
        for (int i = first; i <= last && i < m_layouts.size(); ++i) {
            if (m_selectedIds.remove(m_layouts[i].messageId))
                changed = true;
        }
        if (m_selectedIds.isEmpty()) {
            exitSelectionMode();
            return;  // exitSelectionMode triggers rebuild
        }
        if (changed)
            emit selectionChanged(m_selectedIds.size());
    }
```

Note: `first`/`last` here are model rows. Since `onRowsRemoved` is called before layouts are rebuilt, we need to map to layout indices. Actually the simpler approach — clean up in `rebuildAllLayouts`:

Instead of the above, add at the end of `rebuildAllLayouts()`:

```cpp
    // Clean up selection — remove IDs that are no longer in the model
    if (m_selectionMode) {
        QSet<int> validIds;
        for (const auto &ml : m_layouts)
            validIds.insert(ml.messageId);
        m_selectedIds &= validIds;  // intersection
        if (m_selectedIds.isEmpty())
            exitSelectionMode();
        else
            emit selectionChanged(m_selectedIds.size());
    }
```

- [ ] **Step 2: Exit selection mode on conversation switch in MainWindow**

In `onConversationSelected` in MainWindow.cpp, add at the start of the method:

```cpp
    if (m_chatPainter->selectionMode())
        m_chatPainter->exitSelectionMode();
```

- [ ] **Step 3: Ensure chatPainter has focus for Esc to work**

In the `selectionModeChanged` connection (from Task 5), add focus:

```cpp
    connect(m_chatPainter, &ChatPainter::selectionModeChanged, this, [this](bool active) {
        if (active) {
            m_composer->hide();
            m_selectionBar->show();
            m_chatPainter->setFocus();  // ensure Esc key works
        } else {
            m_selectionBar->hide();
            if (m_chatMode)
                m_composer->show();
        }
    });
```

- [ ] **Step 4: Build and full manual test**

Run: `cmake --build /c/build/talq --target talq 2>&1 | tail -5`

Manual test checklist:
- Right-click → Select enters mode
- Ctrl+Click enters mode
- Click rows to toggle selection
- Bar shows correct count
- Copy formats as `[Author, HH:MM]\ntext`
- Delete only shows for own messages, confirms before deleting
- Forward opens picker, sends messages to target
- Esc exits mode
- Switching conversations exits mode
- If all selected messages get deleted by server, exits mode

- [ ] **Step 5: Commit**

```bash
git add src/painter/ChatPainter.cpp src/ui/MainWindow.cpp
git commit -m "fix: selection edge cases — cleanup on delete, conv switch, focus"
```

---

### Task 9: Update continue.md and changelog

**Files:**
- Modify: `CONTINUE.md`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Update CONTINUE.md next steps**

Remove the "Multi-message selection (Telegram-style)" line from next steps. Add it to the "What was done" section.

- [ ] **Step 2: Add changelog entry**

Add a new section at the top of CHANGELOG.md for the new version (likely v0.14.0):

```markdown
## v0.14.0 (2026-03-29)

### Multi-Message Selection (Telegram-style)
- **Selection mode** — right-click → "Select" or Ctrl+Click to enter, Esc to exit
- **Full-row click targets** — click anywhere on a message row to toggle selection
- **Visual feedback** — teal row highlight + circular checkboxes (filled when selected)
- **Action bar** — replaces composer with Forward, Copy, Delete, Cancel buttons
- **Copy** — formats as `[Author, HH:MM]\nMessage` for each selected message
- **Forward** — conversation picker dialog, sends messages as text to target conversation
- **Delete** — bulk delete with confirmation (only available when all selected are own)
- **Ctrl+C** shortcut copies selected messages when in selection mode
```

- [ ] **Step 3: Commit**

```bash
git add CONTINUE.md CHANGELOG.md
git commit -m "docs: add multi-message selection to changelog and continue"
```
