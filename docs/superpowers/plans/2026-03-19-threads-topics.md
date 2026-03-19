# Threads/Topics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Transform existing thread overlay into Telegram-style Topics with a 3-column layout (icons | topics list | messages) for group chats that have threads.

**Architecture:** Reuse existing ThreadListModel, ThreadItem, ThreadListView. Replace Main.qml's SplitView with a custom RowLayout that supports animated width transitions. Add "All Messages" synthetic topic. Sidebar squeezes to icon-only (56px) when a topic group is selected. Thread index persisted in SQLite for reliable topic discovery across restarts.

**Tech Stack:** Qt 6.8.2, QML, C++20, SQLite, MinGW 13.1

**Spec:** `docs/superpowers/specs/2026-03-19-threads-topics-ux-design.md`

**Build:**
```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
taskkill.exe //IM talq.exe //F 2>/dev/null; sleep 3
rm -rf /c/build/talk-qt
cmake -B C:/build/talk-qt -S C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build C:/build/talk-qt
```

**Run:**
```bash
export PATH="/c/Qt/6.8.2/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH"
QT_FORCE_STDERR_LOGGING=1 /c/build/talk-qt/talq.exe
```

---

## File Map

### Modified Files

| File | Responsibility | Changes |
|------|----------------|---------|
| `src/models/ThreadListModel.h` | Thread list model | Add UnreadCountRole, hasTopics signal, "All Messages" synthetic row |
| `src/models/ThreadListModel.cpp` | Thread fetching + parsing | Add unread tracking, synthetic "All Messages" entry at index 0, persist thread index to SQLite |
| `src/models/Conversation.h` | Conversation struct | Add `bool hasTopics` field |
| `src/models/Conversation.cpp` | Conversation JSON parsing | Initialize `hasTopics = false` |
| `src/models/ConversationListModel.h` | Conversation list model | Add `HasTopicsRole` |
| `src/models/ConversationListModel.cpp` | Sidebar model | Expose `HasTopicsRole` in `data()` and `roleNames()` |
| `src/core/MessageCache.h` | SQLite cache | Add thread index table methods |
| `src/core/MessageCache.cpp` | SQLite operations | Create `thread_index` table, CRUD for thread metadata |
| `src/qml/Main.qml` | Main window layout | Replace SplitView with custom RowLayout, add TopicList column, animated sidebar squeeze |
| `src/qml/ChatView.qml` | Chat area | Topic-aware header (show dot + topic title), remove disabled ThreadListView overlay |
| `src/qml/ConversationList.qml` | Sidebar | Add squeezed (icon-only) mode, manual toggle chevron |
| `src/qml/ConversationItem.qml` | Conversation row delegate | Add icon-only variant (avatar circle, no text) |
| `src/qml/ThreadListView.qml` | Topic list panel | Rework as persistent column (not overlay), add "All Messages" row, inline creation input |
| `src/qml/ThreadItem.qml` | Topic row delegate | Add unread badge, selection highlight with colored border |
| `src/qml/MessageComposer.qml` | Message input | Dynamic placeholder: "Reply in [topic]..." |
| `src/main.cpp` | App bootstrap | No changes needed (threadModel already exposed) |
| `CMakeLists.txt` | Build config | No new files to add (all existing files modified) |

---

## Task 1: Thread Index SQLite Table

**Purpose:** Persist discovered threads across restarts so the topics list isn't empty on app launch.

**Files:**
- Modify: `src/core/MessageCache.h`
- Modify: `src/core/MessageCache.cpp`

- [ ] **Step 1: Add thread index methods to MessageCacheWorker**

In `src/core/MessageCache.h`, add to `MessageCacheWorker`:

```cpp
Q_INVOKABLE void doSaveThreadIndex(const QString &token, const QVector<QJsonObject> &threads);
Q_INVOKABLE QVector<QJsonObject> doLoadThreadIndex(const QString &token);
Q_INVOKABLE void doClearThreadIndex(const QString &token);
```

Add metatype registration at the top of `MessageCache.h`:

```cpp
Q_DECLARE_METATYPE(QVector<QJsonObject>)
```

And add public wrapper methods to `MessageCache`:

```cpp
void saveThreadIndex(const QString &token, const QVector<QJsonObject> &threads);
void loadThreadIndex(const QString &token);
signals:
    void threadIndexLoaded(const QString &token, const QVector<QJsonObject> &threads);
```

- [ ] **Step 2: Create thread_index table in doInit()**

In `src/core/MessageCache.cpp`, inside `MessageCacheWorker::doInit()`, after the messages table creation, add:

```cpp
db.exec("CREATE TABLE IF NOT EXISTS thread_index ("
        "token TEXT NOT NULL, "
        "thread_id INTEGER NOT NULL, "
        "title TEXT NOT NULL, "
        "icon_color INTEGER DEFAULT 0, "
        "last_activity INTEGER DEFAULT 0, "
        "last_message TEXT DEFAULT '', "
        "last_author TEXT DEFAULT '', "
        "reply_count INTEGER DEFAULT 0, "
        "last_read_message_id INTEGER DEFAULT 0, "
        "PRIMARY KEY (token, thread_id)"
        ")");
```

- [ ] **Step 3: Implement doSaveThreadIndex**

```cpp
void MessageCacheWorker::doSaveThreadIndex(const QString &token, const QVector<QJsonObject> &threads)
{
    QSqlQuery q(m_db);
    m_db.transaction();
    for (const auto &t : threads) {
        q.prepare("INSERT OR REPLACE INTO thread_index "
                  "(token, thread_id, title, icon_color, last_activity, last_message, last_author, reply_count, last_read_message_id) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
        q.addBindValue(token);
        q.addBindValue(t["threadId"].toInt());
        q.addBindValue(t["title"].toString());
        q.addBindValue(t["iconColor"].toInt());
        q.addBindValue(t["lastActivity"].toInteger());
        q.addBindValue(t["lastMessage"].toString());
        q.addBindValue(t["lastAuthor"].toString());
        q.addBindValue(t["replyCount"].toInt());
        q.addBindValue(t["lastReadMessageId"].toInt());
        q.exec();
    }
    m_db.commit();
}
```

- [ ] **Step 4: Implement doLoadThreadIndex**

```cpp
QVector<QJsonObject> MessageCacheWorker::doLoadThreadIndex(const QString &token)
{
    QVector<QJsonObject> result;
    QSqlQuery q(m_db);
    q.prepare("SELECT thread_id, title, icon_color, last_activity, last_message, last_author, reply_count, last_read_message_id "
              "FROM thread_index WHERE token = ? ORDER BY last_activity DESC");
    q.addBindValue(token);
    if (q.exec()) {
        while (q.next()) {
            QJsonObject t;
            t["threadId"] = q.value(0).toInt();
            t["title"] = q.value(1).toString();
            t["iconColor"] = q.value(2).toInt();
            t["lastActivity"] = q.value(3).toLongLong();
            t["lastMessage"] = q.value(4).toString();
            t["lastAuthor"] = q.value(5).toString();
            t["replyCount"] = q.value(6).toInt();
            t["lastReadMessageId"] = q.value(7).toInt();
            result.append(t);
        }
    }
    return result;
}
```

- [ ] **Step 5: Implement doClearThreadIndex and wrappers**

```cpp
void MessageCacheWorker::doClearThreadIndex(const QString &token)
{
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM thread_index WHERE token = ?");
    q.addBindValue(token);
    q.exec();
}
```

Add wrapper methods in `MessageCache` that dispatch to worker thread via `QMetaObject::invokeMethod`.

- [ ] **Step 6: Build and verify compilation**

```bash
cmake --build C:/build/talk-qt 2>&1 | tail -5
```

Expected: BUILD SUCCESSFUL

- [ ] **Step 7: Commit**

```bash
git add src/core/MessageCache.h src/core/MessageCache.cpp
git commit -m "feat(cache): add thread_index SQLite table for persistent topic discovery"
```

---

## Task 2: ThreadListModel — Unread Tracking + "All Messages" Row

**Purpose:** Add unread count per topic, synthetic "All Messages" entry at index 0, and SQLite persistence.

**Files:**
- Modify: `src/models/ThreadListModel.h`
- Modify: `src/models/ThreadListModel.cpp`

- [ ] **Step 1: Add UnreadCountRole and new fields to ThreadInfo**

In `src/models/ThreadListModel.h`, add to `ThreadInfo`:

```cpp
int unreadCount = 0;
int lastReadMessageId = 0;
bool isAllMessages = false;  // synthetic "All Messages" entry
```

Add to `Roles` enum:

```cpp
UnreadCountRole,
IsAllMessagesRole,
```

Add to class:

```cpp
Q_PROPERTY(bool hasTopics READ hasTopics NOTIFY hasTopicsChanged)
bool hasTopics() const { return m_threads.size() > 1; } // >1 because index 0 is "All Messages"

void setCache(MessageCache *cache) { m_cache = cache; }
Q_INVOKABLE void markTopicRead(int threadId);
Q_INVOKABLE void selectTopic(int threadId);

signals:
    void hasTopicsChanged();

private:
    MessageCache *m_cache = nullptr;
    int m_selectedThreadId = -1;  // -1 = none, 0 = all messages
```

- [ ] **Step 2: Add "All Messages" synthetic entry in fetchThreads()**

In `src/models/ThreadListModel.cpp`, after building the `threads` vector and before `beginResetModel()`:

```cpp
// Insert "All Messages" at index 0
ThreadInfo allMsg;
allMsg.threadId = 0;
allMsg.title = "All Messages";
allMsg.isAllMessages = true;
allMsg.iconColor = 0;  // teal
if (!threads.isEmpty()) {
    allMsg.lastActivity = threads.first().lastActivity;
    allMsg.lastMessage = threads.first().lastMessage;
    allMsg.lastAuthor = threads.first().lastAuthor;
}
threads.prepend(allMsg);
```

- [ ] **Step 3: Add new roles to data() and roleNames()**

In `data()`:
```cpp
case UnreadCountRole:   return t.unreadCount;
case IsAllMessagesRole: return t.isAllMessages;
```

In `roleNames()`:
```cpp
{UnreadCountRole, "unreadCount"},
{IsAllMessagesRole, "isAllMessages"},
```

- [ ] **Step 4: Add markTopicRead() and selectTopic()**

```cpp
void ThreadListModel::markTopicRead(int threadId)
{
    for (int i = 0; i < m_threads.size(); ++i) {
        if (m_threads[i].threadId == threadId && m_threads[i].unreadCount > 0) {
            m_threads[i].unreadCount = 0;
            emit dataChanged(index(i), index(i), {UnreadCountRole});
            break;
        }
    }
}

void ThreadListModel::selectTopic(int threadId)
{
    m_selectedThreadId = threadId;
    markTopicRead(threadId);
}
```

- [ ] **Step 5: Persist thread index to SQLite after fetch**

In `fetchThreads()` callback, capture `hadTopics` BEFORE `beginResetModel()`:

```cpp
bool hadTopics = m_threads.size() > 1;  // capture BEFORE update

beginResetModel();
m_threads = std::move(threads);
endResetModel();

// Persist to SQLite (skip "All Messages" at index 0)
if (m_cache && m_threads.size() > 1) {
    QVector<QJsonObject> toSave;
    for (int i = 1; i < m_threads.size(); ++i) {
        QJsonObject t;
        t["threadId"] = m_threads[i].threadId;
        t["title"] = m_threads[i].title;
        t["iconColor"] = m_threads[i].iconColor;
        t["lastActivity"] = m_threads[i].lastActivity;
        t["lastMessage"] = m_threads[i].lastMessage;
        t["lastAuthor"] = m_threads[i].lastAuthor;
        t["replyCount"] = m_threads[i].replyCount;
        t["lastReadMessageId"] = m_threads[i].lastReadMessageId;
        toSave.append(t);
    }
    m_cache->saveThreadIndex(m_token, toSave);
}

if (hadTopics != (m_threads.size() > 1))
    emit hasTopicsChanged();
```

- [ ] **Step 6: Load cached threads on token change**

In `setConversationToken()`, before fetching from API, load cached threads:

```cpp
if (m_cache && !m_token.isEmpty()) {
    m_cache->loadThreadIndex(m_token);
    // Connect once to handle the loaded data
}
```

- [ ] **Step 7: Wire cache in main.cpp**

In `src/main.cpp`, after creating `threads` and `cache`:

```cpp
threads.setCache(&cache);
```

- [ ] **Step 8: Build and verify**

```bash
cmake --build C:/build/talk-qt 2>&1 | tail -5
```

- [ ] **Step 9: Commit**

```bash
git add src/models/ThreadListModel.h src/models/ThreadListModel.cpp src/main.cpp
git commit -m "feat(threads): add unread tracking, All Messages entry, and SQLite persistence"
```

---

## Task 3: Conversation hasTopics Flag

**Purpose:** Let the sidebar know which conversations should trigger the squeezed 3-column layout.

**Files:**
- Modify: `src/models/Conversation.h`
- Modify: `src/models/Conversation.cpp`
- Modify: `src/models/ConversationListModel.h`
- Modify: `src/models/ConversationListModel.cpp`

- [ ] **Step 1: Add hasTopics to Conversation struct**

In `src/models/Conversation.h`, add field:

```cpp
bool hasTopics = false;
```

- [ ] **Step 2: Add HasTopicsRole to ConversationListModel**

In `src/models/ConversationListModel.h`, add to `Roles` enum:

```cpp
HasTopicsRole,
```

- [ ] **Step 3: Expose in data() and roleNames()**

In `src/models/ConversationListModel.cpp`:

`data()`:
```cpp
case HasTopicsRole:    return c.hasTopics;
```

`roleNames()`:
```cpp
{HasTopicsRole, "hasTopics"},
```

- [ ] **Step 4: Add method to set hasTopics from QML/ThreadListModel**

In `src/models/ConversationListModel.h`:

```cpp
Q_INVOKABLE void setHasTopics(const QString &token, bool has);
```

In `.cpp`:

```cpp
void ConversationListModel::setHasTopics(const QString &token, bool has)
{
    for (int i = 0; i < m_conversations.size(); ++i) {
        if (m_conversations[i].token == token && m_conversations[i].hasTopics != has) {
            m_conversations[i].hasTopics = has;
            emit dataChanged(index(i), index(i), {HasTopicsRole});
            break;
        }
    }
}
```

- [ ] **Step 5: Build and verify**

```bash
cmake --build C:/build/talk-qt 2>&1 | tail -5
```

- [ ] **Step 6: Commit**

```bash
git add src/models/Conversation.h src/models/Conversation.cpp src/models/ConversationListModel.h src/models/ConversationListModel.cpp
git commit -m "feat(conversations): add hasTopics flag for sidebar squeeze detection"
```

---

## Task 4: ConversationList Squeezed Mode

**Purpose:** Add icon-only mode (56px) to ConversationList with animated transition.

**Files:**
- Modify: `src/qml/ConversationList.qml`
- Modify: `src/qml/ConversationItem.qml`

- [ ] **Step 1: Add squeezed property and toggle to ConversationList**

In `src/qml/ConversationList.qml`, add properties to the `sidebar` Item:

```qml
property bool squeezed: false
property real sidebarWidth: squeezed ? 56 : 320

implicitWidth: sidebarWidth
Behavior on sidebarWidth { NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic } }
```

- [ ] **Step 2: Hide header text and search when squeezed**

Wrap the header text elements, theme/refresh/exit buttons, and search field with `visible: !sidebar.squeezed` or use `opacity: sidebar.squeezed ? 0 : 1` with a Behavior.

When squeezed, the header should show only the user avatar centered.

- [ ] **Step 3: Add manual squeeze toggle chevron**

Add a small button at the bottom of the sidebar:

```qml
Rectangle {
    Layout.fillWidth: true
    height: 32
    color: "transparent"

    Label {
        anchors.centerIn: parent
        text: sidebar.squeezed ? "\u276F" : "\u276E"  // ❯ / ❮
        font.pixelSize: 12
        color: Theme.textMuted
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: sidebar.squeezed = !sidebar.squeezed
    }
}
```

- [ ] **Step 4: Add icon-only mode to ConversationItem**

In `src/qml/ConversationItem.qml`, add a `squeezed` property and conditionally render:

```qml
property bool squeezed: false
height: squeezed ? 56 : Theme.conversationHeight
```

When `squeezed`, show only the avatar circle (centered, 40x40) with the unread badge overlaid. Hide all text (name, last message, timestamp).

- [ ] **Step 5: Pass squeezed to ConversationItem delegate**

In `ConversationList.qml`, update the delegate:

```qml
delegate: ConversationItem {
    width: convListView.width
    selected: index === sidebar.selectedIndex
    filterText: searchField.text
    squeezed: sidebar.squeezed
    // ...
}
```

- [ ] **Step 6: Build and test visually**

```bash
cmake --build C:/build/talk-qt && QT_FORCE_STDERR_LOGGING=1 /c/build/talk-qt/talq.exe
```

Test: Click the chevron toggle — sidebar should animate between 320px and 56px. In squeezed mode, only avatars should show.

- [ ] **Step 7: Commit**

```bash
git add src/qml/ConversationList.qml src/qml/ConversationItem.qml
git commit -m "feat(sidebar): add squeezed icon-only mode with animated transition"
```

---

## Task 5: 3-Column Layout in Main.qml

**Purpose:** Replace SplitView with custom RowLayout that supports the animated 3-column layout.

**Files:**
- Modify: `src/qml/Main.qml`

- [ ] **Step 1: Replace SplitView with Row in chatPage**

Replace the `SplitView` in the `chatPage` Component (lines 495-529) with:

```qml
Component {
    id: chatPage
    Item {
        id: chatLayout
        implicitWidth: 1000
        implicitHeight: 700

        property bool showTopics: false  // true when selected conversation has topics
        property string activeConvToken: ""

        ConversationList {
            id: convList
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: convList.sidebarWidth
            squeezed: chatLayout.showTopics

            onConversationSelected: function(token, name, userId, convType, status) {
                chatLayout.activeConvToken = token
                messageModel.threadId = 0
                messageModel.conversationToken = token
                chatView.conversationName = name
                chatView.conversationUserId = userId
                chatView.conversationType = convType
                chatView.peerStatus = status
                conversationModel.clearUnreadForToken(token)
                signaling.joinRoom(token)
                chatView.activeThreadId = 0
                chatView.activeThreadTitle = ""

                // Check if this conversation has topics
                threadModel.conversationToken = token
            }
        }

        // Divider between sidebar and topics
        Rectangle {
            id: divider1
            anchors.left: convList.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 1
            color: Theme.divider
        }

        // Topics list column — slides in/out
        ThreadListView {
            id: topicList
            anchors.left: divider1.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: chatLayout.showTopics ? 240 : 0
            clip: true
            visible: width > 0

            Behavior on width { NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic } }

            onThreadSelected: function(threadId, title) {
                if (threadId === 0) {
                    // "All Messages" — show unfiltered
                    chatView.activeThreadId = 0
                    chatView.activeThreadTitle = ""
                    messageModel.threadId = 0
                } else {
                    chatView.openThread(threadId, title)
                }
                threadModel.selectTopic(threadId)
            }
        }

        // Divider between topics and chat
        Rectangle {
            id: divider2
            anchors.left: topicList.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: chatLayout.showTopics ? 1 : 0
            color: Theme.divider
        }

        // Chat area — fills remaining space
        ChatView {
            id: chatView
            anchors.left: divider2.right
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
        }

        // React to threadModel.hasTopics changes
        Connections {
            target: threadModel
            function onHasTopicsChanged() {
                chatLayout.showTopics = threadModel.hasTopics
                conversationModel.setHasTopics(chatLayout.activeConvToken, threadModel.hasTopics)

                // Adjust minimum window width
                if (threadModel.hasTopics) {
                    root.minimumWidth = 600
                } else {
                    root.minimumWidth = 500
                }
            }
        }
    }
}
```

- [ ] **Step 2: Update restoreChatWindow minimum width**

The `restoreChatWindow()` function sets `root.minimumWidth = 500`. This is fine — the 600px minimum is only applied when topics are visible.

- [ ] **Step 3: Build and test**

```bash
rm -rf /c/build/talk-qt
cmake -B C:/build/talk-qt -S C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build C:/build/talk-qt && QT_FORCE_STDERR_LOGGING=1 /c/build/talk-qt/talq.exe
```

Test: Open a group chat that has threads. Sidebar should squeeze, topics list should slide in. Click a 1:1 chat — sidebar should expand, topics column should slide out.

- [ ] **Step 4: Commit**

```bash
git add src/qml/Main.qml
git commit -m "feat(layout): replace SplitView with animated 3-column layout for topic groups"
```

---

## Task 6: ThreadListView — Persistent Column with Creation

**Purpose:** Rework ThreadListView from an overlay into a persistent column with "All Messages" row, selection state, and inline topic creation.

**Files:**
- Modify: `src/qml/ThreadListView.qml`

- [ ] **Step 1: Update header to show conversation name**

Replace the fixed "Topics" label with a dynamic label showing the current conversation name. Add a property:

```qml
property string groupName: ""
```

Change header label text to `groupName` (set from Main.qml via `topicList.groupName = name`).

- [ ] **Step 2: Add inline creation input**

Add a `TextField` that appears when "+" is clicked:

```qml
property bool creating: false

TextField {
    Layout.fillWidth: true
    Layout.leftMargin: Theme.spacingSmall
    Layout.rightMargin: Theme.spacingSmall
    Layout.topMargin: Theme.spacingSmall
    visible: threadListRoot.creating
    placeholderText: "Topic name..."
    placeholderTextColor: Theme.textMuted
    font.pixelSize: Theme.fontSizeSmall
    color: Theme.textPrimary
    background: Rectangle {
        radius: Theme.radiusSmall
        color: Theme.bgInput
        border.color: parent.activeFocus ? Theme.accent : "transparent"
        border.width: 1
    }
    padding: 8

    Component.onCompleted: if (visible) forceActiveFocus()
    onVisibleChanged: if (visible) forceActiveFocus()

    Keys.onReturnPressed: {
        var name = text.trim()
        if (name.length > 0 && name.length <= 128) {
            // Send a message that becomes the thread root
            messageModel.sendMessage(name, 0)
            text = ""
            threadListRoot.creating = false
        }
    }
    Keys.onEscapePressed: {
        text = ""
        threadListRoot.creating = false
    }
}
```

Wire the "+" button: `onClicked: threadListRoot.creating = true`

- [ ] **Step 3: Remove overlay behavior**

Remove `anchors.fill: parent` — the component's size is now controlled by Main.qml's anchoring. It should just be an `Item` with internal `Rectangle` filling it.

- [ ] **Step 4: Build and test**

Test: Click "+" — text input appears. Type a name, press Enter — message sent. Press Escape — input hides.

- [ ] **Step 5: Commit**

```bash
git add src/qml/ThreadListView.qml
git commit -m "feat(topics): rework ThreadListView as persistent column with inline creation"
```

---

## Task 7: ThreadItem — Unread Badge + Selection Highlight

**Purpose:** Add unread count badge (colored pill) and selection state with topic-colored left border.

**Files:**
- Modify: `src/qml/ThreadItem.qml`

- [ ] **Step 1: Add unread and selection properties**

```qml
required property int unreadCount
required property bool isAllMessages
property bool selected: false
```

- [ ] **Step 2: Replace reply count badge with unread badge**

Change the existing reply count badge to show unread count instead. Match the dot color:

```qml
Rectangle {
    visible: unreadCount > 0 && !selected
    width: Math.max(20, unreadLabel.implicitWidth + 10)
    height: 20
    radius: 10
    color: {
        var colors = ["#2ec4b6", "#e07060", "#f0a050", "#5ec76a", "#9b7cd4", "#e87aae"]
        return colors[Math.abs(iconColor) % colors.length]
    }

    Label {
        id: unreadLabel
        anchors.centerIn: parent
        text: unreadCount > 99 ? "99+" : unreadCount
        font.pixelSize: 10
        font.weight: Font.Bold
        color: "white"
    }
}
```

- [ ] **Step 3: Update selection indicator to use topic color**

Change the selection bar from accent color to the topic's dot color:

```qml
Rectangle {
    width: 3
    height: parent.height
    anchors.left: parent.left
    color: {
        var colors = ["#2ec4b6", "#e07060", "#f0a050", "#5ec76a", "#9b7cd4", "#e87aae"]
        return colors[Math.abs(iconColor) % colors.length]
    }
    visible: selected
}
```

Add tinted background when selected:

```qml
background: Rectangle {
    color: {
        if (selected) {
            var colors = ["#2ec4b6", "#e07060", "#f0a050", "#5ec76a", "#9b7cd4", "#e87aae"]
            var c = colors[Math.abs(iconColor) % colors.length]
            return Qt.rgba(Qt.color(c).r, Qt.color(c).g, Qt.color(c).b, 0.1)
        }
        return threadItem.hovered ? Theme.bgHover : "transparent"
    }
    // ...
}
```

- [ ] **Step 4: Handle "All Messages" row differently**

For the "All Messages" entry (isAllMessages=true), use a teal dot and show the text "All Messages" with a slightly different style (no preview text, just the label).

- [ ] **Step 5: Build and test**

Test: Topics with unread messages show colored badges. Selected topic shows colored left border + tinted background.

- [ ] **Step 6: Commit**

```bash
git add src/qml/ThreadItem.qml
git commit -m "feat(topics): add unread badges and colored selection highlight to ThreadItem"
```

---

## Task 8: ChatView — Topic-Aware Header

**Purpose:** Show topic color dot + title in the chat header when viewing a topic.

**Files:**
- Modify: `src/qml/ChatView.qml`

- [ ] **Step 1: Remove the disabled ThreadListView overlay**

Delete lines 422-429 (the `ThreadListView` block with `visible: false`).

- [ ] **Step 2: Fix openThread to not clear threadModel in topic mode**

The existing `openThread()` function (line 20-25) sets `threadModel.conversationToken = ""` which would empty the topics list. Modify it:

```qml
function openThread(threadId, title) {
    activeThreadId = threadId
    activeThreadTitle = title
    messageModel.threadId = threadId
    // Do NOT clear threadModel.conversationToken — topics list must stay visible
}
```

- [ ] **Step 3: Hide back button in topic mode, add isInTopicMode**

Keep the back button but only show it when NOT in topic mode. Add a property:

```qml
property bool isInTopicMode: false
```

Change back button visibility (line 68): `visible: chatRoot.activeThreadId > 0 && !chatRoot.isInTopicMode`

Set `isInTopicMode` from Main.qml when topics are visible.

- [ ] **Step 3b: Add "Select a topic" empty state**

When in topic mode with no topic selected (`isInTopicMode && activeThreadId === 0 && messageModel.threadId === 0`), show a centered empty state instead of messages:

```qml
Column {
    anchors.centerIn: parent
    spacing: Theme.spacingLarge
    visible: chatRoot.isInTopicMode && chatRoot.activeThreadId === 0

    Label { anchors.horizontalCenter: parent.horizontalCenter; text: "\uD83D\uDCAC"; font.pixelSize: 48; opacity: 0.3 }
    Label { anchors.horizontalCenter: parent.horizontalCenter; text: "Select a topic"; font.pixelSize: Theme.fontSizeNormal; color: Theme.textMuted }
}
```

- [ ] **Step 3: Add topic color dot to header**

When `isInTopicMode && activeThreadId > 0`, show a colored dot before the topic title:

```qml
Rectangle {
    width: 10; height: 10; radius: 5
    visible: chatRoot.isInTopicMode && chatRoot.activeThreadId > 0
    color: {
        var colors = ["#2ec4b6", "#e07060", "#f0a050", "#5ec76a", "#9b7cd4", "#e87aae"]
        return colors[Math.abs(chatRoot.activeThreadColor) % colors.length]
    }
    Layout.alignment: Qt.AlignVCenter
}
```

Add `property int activeThreadColor: 0` to ChatView.

- [ ] **Step 4: Update header subtitle for topic mode**

When in topic mode, show the group name + message count below the topic title:

```qml
Label {
    visible: chatRoot.isInTopicMode && chatRoot.activeThreadId > 0
    text: chatRoot.conversationName + " \u00B7 " + messageModel.count + " messages"
    font.pixelSize: Theme.fontSizeTiny
    color: Theme.textSecondary
    Layout.fillWidth: true
}
```

- [ ] **Step 5: Build and test**

Test: Open a topic group, select a topic. Header shows colored dot + topic title + "Group Name \u00B7 N messages" subtitle.

- [ ] **Step 6: Commit**

```bash
git add src/qml/ChatView.qml
git commit -m "feat(chat): topic-aware header with color dot and group subtitle"
```

---

## Task 9: MessageComposer — Dynamic Placeholder

**Purpose:** Show "Reply in [topic name]..." when inside a topic.

**Files:**
- Modify: `src/qml/MessageComposer.qml`

- [ ] **Step 1: Add topicName property**

```qml
property string topicName: ""
```

- [ ] **Step 2: Update placeholder text**

Change `placeholderText: "Message..."` to:

```qml
placeholderText: composer.topicName.length > 0 ? "Reply in " + composer.topicName + "..." : "Message..."
```

- [ ] **Step 3: Wire from ChatView footer**

In `ChatView.qml`, in the footer's `MessageComposer`:

```qml
MessageComposer {
    Layout.fillWidth: true
    topicName: chatRoot.isInTopicMode && chatRoot.activeThreadId > 0 ? chatRoot.activeThreadTitle : ""
    onSendMessage: function(text) {
        messageModel.sendMessage(text, chatRoot.replyToId > 0 ? chatRoot.replyToId : chatRoot.activeThreadId)
        chatRoot.cancelReply()
    }
}
```

Note the `replyToId > 0 ? replyToId : activeThreadId` — when the user is replying to a specific message inside the thread, use that. Otherwise, post as a reply to the thread root.

- [ ] **Step 4: Build and test**

Test: Open a topic "Bug Reports". Composer placeholder should say "Reply in Bug Reports..."

- [ ] **Step 5: Commit**

```bash
git add src/qml/MessageComposer.qml src/qml/ChatView.qml
git commit -m "feat(composer): dynamic placeholder for topic context"
```

---

## Task 10: Integration Wiring + Final Polish

**Purpose:** Connect all pieces, test end-to-end, fix edge cases.

**Files:**
- Modify: `src/qml/Main.qml` (final wiring)
- Modify: `src/qml/ThreadListView.qml` (groupName binding)

- [ ] **Step 1: Wire groupName and isInTopicMode**

In Main.qml's chatPage, after the conversation selection handler:

```qml
// In onConversationSelected:
topicList.groupName = name
chatView.isInTopicMode = false  // reset until topics load
```

In the `Connections` target threadModel:

```qml
function onHasTopicsChanged() {
    chatLayout.showTopics = threadModel.hasTopics
    chatView.isInTopicMode = threadModel.hasTopics
    // ...
}
```

- [ ] **Step 2: Wire topic color to ChatView**

In ThreadListView's `onThreadSelected`:

```qml
onThreadSelected: function(threadId, title) {
    // ... existing code ...
    chatView.activeThreadColor = threadModel.data(threadModel.index(/* find row */), ThreadListModel.IconColorRole)
}
```

Or simpler — add a `colorForThread(int threadId)` method to ThreadListModel that returns the iconColor.

- [ ] **Step 3: Add capability check**

In Main.qml's `Connections` target `threadModel`, gate topic mode on server capability:

```qml
function onHasTopicsChanged() {
    // Only enable topics if server supports threads (Talk v22+)
    var threadsSupported = auth.capabilities.indexOf("threads") >= 0
    chatLayout.showTopics = threadModel.hasTopics && threadsSupported
    chatView.isInTopicMode = chatLayout.showTopics
    // ...
}
```

Note: `auth.capabilities` must expose the `spreed.features` array. If AuthManager doesn't expose this yet, add a `Q_PROPERTY(QStringList capabilities ...)` that stores the features list from `GET /capabilities`.

- [ ] **Step 4: Test the full flow**

1. Open app, log in
2. Click a group that has thread replies → sidebar squeezes, topics slide in
3. "All Messages" at top of topics list shows unfiltered chat
4. Click a topic → messages filter to that thread, header shows dot + title
5. Click a 1:1 chat in squeezed sidebar → topics slide out, sidebar expands
6. Click back on the group → topics slide in again with cached list
7. Click "+" → inline input, type name, Enter → message sent as new thread root
8. Close and reopen app → topics list loads from cache immediately

- [ ] **Step 4: Clean build and test**

```bash
taskkill.exe //IM talq.exe //F 2>/dev/null; sleep 3
rm -rf /c/build/talk-qt
cmake -B C:/build/talk-qt -S C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build C:/build/talk-qt && QT_FORCE_STDERR_LOGGING=1 /c/build/talk-qt/talq.exe
```

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(topics): wire 3-column layout, topic selection, and end-to-end integration"
```

---

## Summary

| Task | Component | Estimated Steps |
|------|-----------|-----------------|
| 1 | SQLite thread_index table | 7 |
| 2 | ThreadListModel (unread + All Messages + persistence) | 9 |
| 3 | Conversation hasTopics flag | 6 |
| 4 | ConversationList squeezed mode | 7 |
| 5 | Main.qml 3-column layout | 4 |
| 6 | ThreadListView persistent column | 5 |
| 7 | ThreadItem unread + selection | 6 |
| 8 | ChatView topic header | 6 |
| 9 | MessageComposer dynamic placeholder | 5 |
| 10 | Integration wiring | 5 |
| **Total** | | **60 steps** |

Tasks 1-3 are backend (C++). Tasks 4-9 are frontend (QML). Task 10 ties everything together.

Tasks 1, 2, 3 can be done in parallel. Task 4 depends on 3. Task 5 depends on 2, 3, 4. Tasks 6-9 depend on 5 (and transitively on 2). Task 10 depends on all. Serial order: {1, 2, 3} → 4 → 5 → {6, 7, 8, 9} → 10.
