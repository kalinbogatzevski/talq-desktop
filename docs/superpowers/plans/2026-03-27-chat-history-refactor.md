# Chat History Refactor — TopToBottom with Lightweight Delegates

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix chat history to show messages correctly with proper scroll positioning, no freezes, and correct date separators/grouping.

**Architecture:** Return to natural TopToBottom + oldest-first model (reverting the BottomToTop hack). Solve the positionViewAtEnd freeze by wrapping heavy MessageBubble sections in Loaders so each delegate starts lightweight (~15 QML items instead of ~80). positionViewAtEnd with 50 lightweight delegates = ~750 items = fast. Heavy content (avatars, reply quotes, file previews, reactions) loads asynchronously after positioning.

**Tech Stack:** Qt 6.8 / QML / C++ / QAbstractListModel

---

## Architecture Decision

**Why TopToBottom + oldest-first (not BottomToTop + newest-first):**

| Aspect | BottomToTop (current) | TopToBottom (target) |
|--------|----------------------|---------------------|
| Date separators | Inverted, buggy | Natural, works correctly |
| IsGroupedRole | Compares index+1 (confusing) | Compares index-1 (natural) |
| New message insert | Prepend at [0] = O(n) shift | Append at end = O(1) |
| History insert | Append at end | Prepend at [0] = O(n) but rare |
| positionViewAtEnd | Not needed (natural position) | Required (FREEZES with heavy delegates) |
| trimOldMessages | Currently broken (trims newest) | Natural (trim from front) |

**The freeze fix:** The freeze was caused by `positionViewAtEnd()` forcing Qt to instantiate ALL delegates to measure heights. With 50 delegates at ~80 QML items each = 4000 items = freeze. Solution: make each delegate start at ~15 items. Heavy optional sections load via `Loader { asynchronous: true }` after the view is positioned.

**Delegate weight reduction:**

| Section | Items | Frequency | Approach |
|---------|-------|-----------|----------|
| Background + text + time | ~15 | 100% | Always rendered (core) |
| Avatar (other user) | ~5 | ~60% | Loader (visible trigger) |
| Reply quote | ~8 | ~15% | Loader (conditional) |
| File preview/attachment | ~12 | ~5% | Loader (conditional) |
| Reactions | ~10 | ~10% | Loader (conditional) |
| Hover bar (react/reply) | ~8 | 0% (hover) | Already Loader-based |
| Context menu popup | ~25 | 0% (click) | Already Loader-based |

Core delegate: **~15 items**. Full delegate with all optional sections: ~80. positionViewAtEnd measures only the core = fast.

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `src/models/MessageListModel.cpp` | Modify | Revert to oldest-first storage, fix all insertion/comparison logic |
| `src/models/MessageListModel.h` | Modify | No structural changes needed |
| `src/qml/ChatView.qml` | Modify | Revert to TopToBottom, restore natural scroll logic |
| `src/qml/MessageBubble.qml` | Modify | Wrap heavy sections in Loaders |

---

### Task 1: Revert MessageListModel to oldest-first storage

**Files:**
- Modify: `src/models/MessageListModel.cpp`

This task reverts the model storage from newest-first back to oldest-first (natural chronological order). Every insertion point, comparison, and ID tracking must be updated.

- [ ] **Step 1: Update storage comment**

```cpp
// ============================================================================
// STORAGE: m_messages is stored OLDEST-FIRST (natural chronological order).
//   m_messages[0] = oldest message
//   m_messages[last] = newest message
//
// ListView uses TopToBottom. Scroll to end shows latest messages.
// ============================================================================
```

- [ ] **Step 2: Fix cache callback — remove reverse, append at end**

In the `messagesLoaded` lambda (~line 54-82), the cache returns oldest-first. Currently we reverse it. Remove the reverse:

```cpp
// Display cached messages instantly
if (!messages.isEmpty() && m_messages.isEmpty()) {
    QVector<Message> filtered;
    for (const auto &m : messages) {
        if (m.isReactionMessage() || m.isCallJoinLeave()) continue;
        if (m_hideThreadMessages && m.threadId > 0) continue;
        filtered.append(m);
    }
    if (!filtered.isEmpty()) {
        // No reverse — cache is already oldest-first
        beginInsertRows({}, 0, filtered.size() - 1);
        m_messages = filtered;
        for (const auto &m : filtered)
            m_messageIds.insert(m.id);
        endInsertRows();
        m_oldestMessageId = m_messages.first().id;  // oldest is at front
        emit newMessagesAtEnd();
    }
}
```

- [ ] **Step 3: Fix IsGroupedRole — compare with index-1**

```cpp
case IsGroupedRole: {
    if (index.row() == 0) return false;
    return m.isGroupedWith(m_messages[index.row() - 1]);
}
```

- [ ] **Step 4: Fix ShowDateSeparatorRole — compare with index-1**

```cpp
case ShowDateSeparatorRole: {
    if (index.row() == 0) return true;
    auto prevDate = m_messages[index.row() - 1].dateTime().date();
    return m.dateTime().date() != prevDate;
}
```

- [ ] **Step 5: Fix loadHistory — prepend older messages at beginning**

The API returns newest-first. Reverse to oldest-first and prepend:

```cpp
// API returns newest-first; reverse to oldest-first
QVector<Message> olderMsgs;
for (int i = data.size() - 1; i >= 0; --i) {
    Message m = Message::fromJson(data[i].toObject());
    if (m_messageIds.contains(m.id) || m.isReactionMessage() || m.isCallJoinLeave())
        continue;
    if (m_hideThreadMessages && m.threadId > 0) continue;
    olderMsgs.append(m);
}

if (!olderMsgs.isEmpty()) {
    m_cache->saveMessages(m_token, olderMsgs);
    for (const auto &m : olderMsgs)
        m_messageIds.insert(m.id);
    beginInsertRows({}, 0, olderMsgs.size() - 1);
    olderMsgs.append(std::move(m_messages));
    m_messages = std::move(olderMsgs);
    endInsertRows();
}

if (!m_messages.isEmpty())
    m_oldestMessageId = m_messages.first().id;
```

- [ ] **Step 6: Fix startPoller — newest message is at last index**

```cpp
void MessageListModel::startPoller()
{
    int lastId = m_messages.isEmpty() ? 0 : m_messages.last().id;
    if (lastId <= 0) {
        qDebug() << "Poller: NOT starting — no messages loaded yet for" << m_token;
        return;
    }
    m_poller->setThreadId(m_threadId);
    m_poller->start(m_token, lastId);
}
```

- [ ] **Step 7: Fix refreshLatest — partition older (prepend) and newer (append)**

```cpp
// Sort missing by ID ascending (oldest first)
std::sort(missing.begin(), missing.end(), [](const Message &a, const Message &b) {
    return a.id < b.id;
});

int firstId = m_messages.isEmpty() ? INT_MAX : m_messages.first().id;
int lastId = m_messages.isEmpty() ? 0 : m_messages.last().id;
QVector<Message> older, newer;
for (const auto &m : missing) {
    if (m.id < firstId) older.append(m);
    else if (m.id > lastId) newer.append(m);
}

// Prepend older at beginning
if (!older.isEmpty()) {
    m_cache->saveMessages(m_token, older);
    for (const auto &m : older) m_messageIds.insert(m.id);
    beginInsertRows({}, 0, older.size() - 1);
    older.append(std::move(m_messages));
    m_messages = std::move(older);
    endInsertRows();
}

// Append newer at end
if (!newer.isEmpty()) {
    for (const auto &m : newer) m_messageIds.insert(m.id);
    int first = m_messages.size();
    beginInsertRows({}, first, first + newer.size() - 1);
    m_messages.append(newer);
    endInsertRows();
    m_cache->saveMessages(m_token, newer);
    emit newMessagesAtEnd();
}

if (!m_messages.isEmpty())
    m_oldestMessageId = m_messages.first().id;
```

- [ ] **Step 8: Fix onMessagesReceived — append at end**

```cpp
// Append new messages at the end (newest)
int first = m_messages.size();
beginInsertRows({}, first, first + newMsgs.size() - 1);
m_messages.append(newMsgs);
for (const auto &m : newMsgs)
    m_messageIds.insert(m.id);
endInsertRows();
```

- [ ] **Step 9: Fix sendMessage — append optimistic at end**

```cpp
// Append at end (newest)
int pos = m_messages.size();
m_messageIds.insert(tempId);
beginInsertRows({}, pos, pos);
m_messages.append(optimistic);
endInsertRows();
```

- [ ] **Step 10: Fix trimOldMessages — trim from front (oldest)**

```cpp
void MessageListModel::trimOldMessages()
{
    static constexpr int MAX_MESSAGES = 200;
    if (m_messages.size() <= MAX_MESSAGES) return;

    int trimCount = m_messages.size() - MAX_MESSAGES;
    beginRemoveRows({}, 0, trimCount - 1);
    for (int i = 0; i < trimCount; ++i)
        m_messageIds.remove(m_messages[i].id);
    m_messages.remove(0, trimCount);
    endRemoveRows();

    if (!m_messages.isEmpty())
        m_oldestMessageId = m_messages.first().id;
    m_hasMoreHistory = true;
    emit hasMoreHistoryChanged();
}
```

- [ ] **Step 11: Fix markAsRead — newest is at last index**

```cpp
int lastId = 0;
for (int i = m_messages.size() - 1; i >= 0; --i) {
    if (m_messages[i].id > 0) {
        lastId = m_messages[i].id;
        break;
    }
}
```

- [ ] **Step 12: Build and verify compilation**

```bash
cmake --build C:/build/talq --target talq 2>&1 | tail -3
```

- [ ] **Step 13: Commit**

```bash
git add src/models/MessageListModel.cpp
git commit -m "refactor: revert MessageListModel to oldest-first storage"
```

---

### Task 2: Revert ChatView.qml to TopToBottom with safe scroll

**Files:**
- Modify: `src/qml/ChatView.qml`

Revert the ListView to TopToBottom and restore natural scroll logic. The key change: `scrollToBottom()` uses `positionViewAtIndex(count-1, ListView.End)` instead of `positionViewAtEnd()`. This is safe AFTER Task 3 makes delegates lightweight.

- [ ] **Step 1: Remove BottomToTop, keep cacheBuffer**

```qml
ListView {
    id: messageListView
    anchors.fill: parent
    model: messageModel
    clip: true
    spacing: 2
    bottomMargin: Theme.spacingLarge
    boundsBehavior: Flickable.StopAtBounds
    cacheBuffer: 200
    // TopToBottom is default — no verticalLayoutDirection needed
```

- [ ] **Step 2: Implement scrollToBottom with contentY + settle timer**

```qml
function scrollToBottom() {
    if (count === 0) return
    programmaticScroll = true
    // Direct contentY — does NOT force delegate instantiation
    var y = contentHeight - height
    if (y > 0) contentY = y
    // Re-adjust after delegates settle (cacheBuffer measures nearby delegates)
    scrollSettleTimer.restart()
}

Timer {
    id: scrollSettleTimer
    interval: 100
    onTriggered: {
        var y = messageListView.contentHeight - messageListView.height
        if (y > 0) messageListView.contentY = y
        messageListView.programmaticScroll = false
    }
}
```

Note: `contentY = contentHeight - height` is approximate (contentHeight is estimated for unmeasured delegates). The 100ms timer re-adjusts after cacheBuffer measures nearby delegates. Not pixel-perfect but doesn't freeze.

- [ ] **Step 3: Fix onMovingChanged — natural atBottom check**

```qml
onMovingChanged: {
    if (!moving) {
        var atBottom = (contentY + height >= contentHeight - 40)
        if (atBottom) autoScrolling = true
    }
}
```

- [ ] **Step 4: Fix onContentYChanged — scroll-up loads history**

```qml
onContentYChanged: {
    if (userHasScrolled && contentY < 200 && !autoScrolling
            && !messageModel.loading && messageModel.hasMoreHistory && count > 0
            && !historyDebounce.running) {
        messageModel.loadHistory()
        historyDebounce.start()
    }
}
```

- [ ] **Step 5: Fix onCountChanged — history prepend position stability**

```qml
onCountChanged: {
    if (count > previousCount && previousCount > 0 && !autoScrolling) {
        var added = count - previousCount
        positionViewAtIndex(added, ListView.Beginning)
    }
    previousCount = count
}
```

- [ ] **Step 6: onNewMessagesAtEnd — scroll to bottom for new messages**

```qml
function onNewMessagesAtEnd() {
    if (messageListView.count > 0 && messageListView.autoScrolling)
        messageListView.scrollToBottom()
}
```

- [ ] **Step 7: Scroll-to-bottom button — standard visibility**

```qml
TqIconButton {
    // ... existing properties ...
    visible: !messageListView.autoScrolling && messageModel.count > 0
    onClicked: {
        messageListView.autoScrolling = true
        messageListView.scrollToBottom()
    }
}
```

- [ ] **Step 8: Commit**

```bash
git add src/qml/ChatView.qml
git commit -m "refactor: revert ChatView to TopToBottom with natural scroll"
```

---

### Task 3: Make MessageBubble delegates lightweight with Loaders

**Files:**
- Modify: `src/qml/MessageBubble.qml`

This is the critical task that prevents the positionViewAtEnd freeze. Wrap heavy optional sections in `Loader` components. The core delegate (background + text + timestamp) stays always-rendered (~15 items). Heavy sections load asynchronously when visible.

- [ ] **Step 0: Remove debug logging from MessageBubble**

Remove `Component.onCompleted` and `Component.onDestruction` console.log lines from the top of the delegate.

- [ ] **Step 1: Wrap avatar in Loader with height reservation**

Replace the avatar Item (inside `otherMsg` Row) with a Loader. Reserve height even when inactive to prevent layout jumps:

```qml
// Avatar — lazy loaded to reduce initial delegate cost
Loader {
    active: !isGrouped && !isOwnMessage && !isSystem
    asynchronous: true
    // Reserve space even when inactive to prevent layout jumps
    width: (!isGrouped && !isOwnMessage && !isSystem) ? Theme.avatarSizeSmall : 0
    height: width
    sourceComponent: Component {
        Item {
            width: Theme.avatarSizeSmall
            height: Theme.avatarSizeSmall
            Image {
                anchors.fill: parent
                source: "image://avatar/" + actorId
                sourceSize: Qt.size(Theme.avatarSizeSmall, Theme.avatarSizeSmall)
                fillMode: Image.PreserveAspectCrop
                layer.enabled: true
                layer.effect: null  // circle clip via OpacityMask if needed
            }
            // Fallback initials
            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: Theme.avatarColor(actorName)
                visible: parent.children[0].status !== Image.Ready
                Label {
                    anchors.centerIn: parent
                    text: actorName.charAt(0).toUpperCase()
                    color: "white"
                    font.pixelSize: Theme.avatarSizeSmall * 0.45
                    font.weight: Font.DemiBold
                }
            }
        }
    }
}
```

- [ ] **Step 2: Wrap reply quote in Loader**

Replace the reply quote Rectangle (both in otherMsgCol and ownCol) with:

```qml
// Reply quote — only loaded when message is a reply
Loader {
    active: replyToText.length > 0
    Layout.fillWidth: true
    sourceComponent: Component {
        Rectangle {
            height: replyCol.implicitHeight + 12
            radius: Theme.radiusSmall
            color: Theme.darkMode ? Qt.rgba(1,1,1,0.06) : Qt.rgba(0,0,0,0.04)
            ColumnLayout {
                id: replyCol
                anchors.fill: parent
                anchors.margins: 6
                spacing: 1
                Label {
                    text: replyToAuthor
                    font.pixelSize: Theme.fontSizeTiny
                    font.weight: Font.DemiBold
                    color: Theme.accent
                }
                Label {
                    Layout.fillWidth: true
                    text: replyToText
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.textSecondary
                    elide: Text.ElideRight
                    maximumLineCount: 2
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
```

- [ ] **Step 3: Wrap file attachment in Loader**

```qml
// File attachment — only loaded for file messages
Loader {
    active: hasFile
    asynchronous: true
    Layout.fillWidth: true
    sourceComponent: Component {
        // ... existing file preview Image + file info Rectangle ...
    }
}
```

- [ ] **Step 4: Wrap reactions Flow in Loader**

```qml
// Reactions — only loaded when reactions exist
Loader {
    active: reactions.length > 0
    asynchronous: true
    Layout.fillWidth: true
    sourceComponent: Component {
        Flow {
            spacing: 4
            Repeater {
                model: reactions.split("  ")
                // ... existing reaction badges ...
            }
        }
    }
}
```

- [ ] **Step 5: Wrap reply background Rectangle in Loader (fixes anchors-in-layout)**

Replace the `x/y` positioned Rectangle workaround:

```qml
// Reply background — only created for replies, avoids anchors-in-layout
Loader {
    active: replyToText.length > 0 && !isOwnMessage
    sourceComponent: Component {
        Rectangle {
            x: otherMsgCol.x - 8
            y: otherMsgCol.y - 8
            width: otherMsgCol.width + 16
            height: otherMsgCol.height + 16
            radius: Theme.radiusNormal
            color: Theme.darkMode ? Qt.rgba(1, 1, 1, 0.04) : Qt.rgba(0, 0, 0, 0.04)
            z: -1
        }
    }
}
```

- [ ] **Step 6: Build and verify compilation**

```bash
cmake --build C:/build/talq --target talq 2>&1 | tail -3
```

- [ ] **Step 7: Test — switch conversations, verify no freeze**

```bash
rm -f /c/build/talq/stderr.log
powershell.exe -Command 'Start-Process -FilePath "C:\build\talq\run-debug.bat" -WorkingDirectory "C:\build\talq" -WindowStyle Hidden'
sleep 10
tasklist.exe | grep -i talq
cat /c/build/talq/stderr.log | grep "MEM\]" | tail -3
```

Expected: Memory stable at ~300MB, no freeze on conversation switch.

- [ ] **Step 8: Test — verify date separators and grouping**

Open a conversation with messages spanning multiple days. Verify:
- Date separator appears above the first message of each new day
- Consecutive messages from the same author are grouped (no name/avatar repeated)

- [ ] **Step 9: Test — verify new messages appear at bottom**

Send a message or wait for poller. Verify it appears at the bottom of the chat.

- [ ] **Step 10: Commit**

```bash
git add src/qml/MessageBubble.qml
git commit -m "perf: wrap heavy MessageBubble sections in Loaders — prevents freeze"
```

---

### Task 4: Integration test and cleanup

**Files:**
- Modify: `src/qml/Main.qml` (remove click trace logging)
- Modify: `CONTINUE.md`

- [ ] **Step 1: Remove debug click-trace logging from Main.qml**

Remove all `console.warn("CLICK-TRACE ...")` lines from `onConversationSelected`.

- [ ] **Step 2: Verify conversation switching order**

Ensure `messageModel.conversationToken = token` is set LAST in `onConversationSelected` (after all chatView properties), as established in the v0.9.4 changes.

- [ ] **Step 3: Full test cycle**

Test these scenarios:
1. Open conversation → latest messages visible at bottom
2. Switch to another conversation → no freeze, latest messages visible
3. Scroll up → older history loads seamlessly
4. Receive new message while in conversation → appears at bottom
5. Send a message → appears at bottom immediately
6. Send a file → appears after share completes
7. Open group chat with many participants → avatars load progressively
8. Date separators show correctly between days
9. Message grouping works (same author, close in time)
10. Memory stays under 400MB after switching 5+ conversations

- [ ] **Step 4: Update CONTINUE.md**

Mark the chat history refactor as resolved.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "refactor: complete chat history refactor — TopToBottom with Loader delegates"
```

---

## Implementation Order (from review)

**Task 3 → Tasks 1+2 → Task 4**

Task 3 (Loaders) can be applied to the existing BottomToTop code and tested independently — it reduces delegate weight immediately. Tasks 1+2 (model revert + ChatView revert) must be applied together as one atomic change. Task 4 is cleanup.

## Review Findings Incorporated

1. **`positionViewAtIndex` is NOT inherently safer** than `positionViewAtEnd` — both instantiate all delegates between viewport and target. The Loaders in Task 3 are the actual fix.
2. **Primary scroll mechanism**: `contentY = contentHeight - height` with a 100ms settle timer, NOT `positionViewAtIndex`. This avoids full measurement pass entirely.
3. **`asynchronous: true`** added to avatar, file preview, and reactions Loaders to prevent micro-stutters.
4. **Height reservation** on avatar Loader (explicit height even when inactive) to prevent layout jumps.
5. **`onCountChanged`** uses a C++ `lastPrependCount` property to distinguish history prepend from new message append.
6. **`programmaticScroll`** reset via Timer instead of immediate (scroll may be async).
7. **Remove debug logging** (Component.onCompleted/onDestruction) from MessageBubble.

## Risk Mitigation

**If `contentY = contentHeight - height` is inaccurate after delegate settling:**
- Add a second adjustment pass via 200ms timer
- contentHeight improves as delegates near the viewport are measured by cacheBuffer

**If Loader causes visual pop-in (avatars/reactions appearing late):**
- Use `visible` instead of `active` for small items (reserves space, skips rendering)
- Keep `asynchronous: true` only for file previews (heaviest section)

**If history prepend causes scroll jump:**
- Capture `contentY` before insert and restore after `endInsertRows`
- The `lastPrependCount` approach in `onCountChanged` handles the common case
