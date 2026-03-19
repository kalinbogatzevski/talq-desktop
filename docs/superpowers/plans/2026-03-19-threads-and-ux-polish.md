# Threads/Topics + UX Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Telegram-style Threads/Topics to group conversations and polish UX (reaction pills, better hover actions, context menu actions).

**Architecture:** Threads are a filtered message view within a conversation. A new `ThreadListModel` fetches thread roots from the Talk API. `MessageListModel` gains a `threadId` property to filter messages by thread. The QML `ChatView` gets a topic list overlay that shows when a group conversation has threads. UX polish items are integrated into existing components.

**Tech Stack:** Qt 6.8.2, C++20, QML, Nextcloud Talk API v22+ threads

**Build:** `cmake --build C:/build/talk-qt` — source at `C:/src/talk-desktop-qt`

---

## File Map

### New Files
| File | Responsibility |
|------|---------------|
| `src/models/ThreadListModel.h/.cpp` | QAbstractListModel for thread list (title, reply count, last activity, unread) |
| `src/qml/ThreadListView.qml` | Topic list overlay inside ChatView |
| `src/qml/ThreadItem.qml` | Single thread row delegate (color dot, title, preview, count) |

### Modified Files
| File | Changes |
|------|---------|
| `CMakeLists.txt` | Add ThreadListModel.cpp, ThreadListView.qml, ThreadItem.qml |
| `src/models/Message.h` | Add `threadId` field (root parent ID) |
| `src/models/Message.cpp` | Parse `parent` chain to extract thread root ID |
| `src/models/MessageListModel.h/.cpp` | Add `threadId` property for filtered view, thread-aware polling |
| `src/core/MessagePoller.h/.cpp` | Support `threadId` param in poll requests |
| `src/main.cpp` | Register ThreadListModel as context property |
| `src/qml/ChatView.qml` | Thread list overlay, back navigation, "Reply in thread" action |
| `src/qml/MessageBubble.qml` | Thread indicator on root messages, reaction pills, hover action fixes |
| `src/qml/ConversationList.qml` | Pass conversation type to ChatView for thread detection |

---

## Phase 1: Thread Data Layer (C++)

### Task 1: Add thread fields to Message

**Files:**
- Modify: `src/models/Message.h`
- Modify: `src/models/Message.cpp`

- [ ] **Step 1:** Add `int threadId = 0;` field to Message class (the root parent's message ID)

In `Message.h`, add after `int replyToId = 0;`:
```cpp
int threadId = 0;       // Root thread message ID (0 = not in a thread)
QString threadTitle;     // Thread title if this is a thread root
int threadReplyCount = 0; // Number of replies (for root messages)
```

- [ ] **Step 2:** Parse thread data in `Message::fromJson`

In `Message.cpp`, in `fromJson()`, extract the topmost parent ID:
```cpp
// Thread: walk parent chain to find root
QJsonObject parent = json["parent"].toObject();
if (!parent.isEmpty()) {
    m.replyToId = parent["id"].toInt();
    m.replyTo = parent;
    // The parent of a thread message IS the thread root
    // (Talk threads are flat — no nested replies within threads)
    m.threadId = parent["id"].toInt();
}

// Thread root messages have threadTitle in the API response
m.threadTitle = json["threadTitle"].toString();
```

- [ ] **Step 3:** Build and verify compilation

```bash
taskkill.exe //IM talk-qt.exe //F 2>/dev/null; sleep 3
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
cmake --build C:/build/talk-qt
```

- [ ] **Step 4:** Commit

```bash
cd C:/src/talk-desktop-qt
git add src/models/Message.h src/models/Message.cpp
git commit -m "feat: add thread fields to Message (threadId, threadTitle, threadReplyCount)"
```

---

### Task 2: Create ThreadListModel

**Files:**
- Create: `src/models/ThreadListModel.h`
- Create: `src/models/ThreadListModel.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/main.cpp`

- [ ] **Step 1:** Create `ThreadListModel.h`

A model that fetches thread roots for a conversation. Each row is a thread with: title, root message text, reply count, last activity, unread indicator.

```cpp
#pragma once
#include <QAbstractListModel>
#include <QVector>
#include "core/ApiClient.h"

struct ThreadInfo {
    int threadId = 0;           // = root message ID
    QString title;              // threadTitle or first line of root message
    QString lastMessage;        // preview of most recent reply
    QString lastAuthor;
    qint64 lastActivity = 0;
    int replyCount = 0;
    int iconColor = 0;          // index into color palette
};

class ThreadListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString conversationToken READ conversationToken WRITE setConversationToken NOTIFY tokenChanged)
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        ThreadIdRole = Qt::UserRole + 1,
        TitleRole,
        LastMessageRole,
        LastAuthorRole,
        LastActivityRole,
        ReplyCountRole,
        IconColorRole,
    };

    explicit ThreadListModel(ApiClient *api, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString conversationToken() const { return m_token; }
    void setConversationToken(const QString &token);
    bool isLoading() const { return m_loading; }

    Q_INVOKABLE void refresh();

signals:
    void tokenChanged();
    void loadingChanged();
    void countChanged();

private:
    void fetchThreads();
    void parseThreadsFromMessages(const QJsonArray &messages);

    ApiClient *m_api;
    QVector<ThreadInfo> m_threads;
    QString m_token;
    bool m_loading = false;
};
```

- [ ] **Step 2:** Create `ThreadListModel.cpp`

Fetches message history and scans for thread roots (messages that have replies). Uses `GET /chat/{token}` to scan for messages with `parent` fields, then groups by root ID.

Key implementation:
- `fetchThreads()`: GET chat history, scan for unique `parent.id` values, build thread list
- Also try `GET /chat/{token}/threads/{threadId}` for individual thread info
- Sort threads by last activity (newest first)
- Color assignment: hash thread title to pick from palette

- [ ] **Step 3:** Add to CMakeLists.txt

Add `src/models/ThreadListModel.cpp` to `qt_add_executable`.

- [ ] **Step 4:** Register in main.cpp

```cpp
ThreadListModel threads(&api);
engine.rootContext()->setContextProperty("threadModel", &threads);
```

- [ ] **Step 5:** Build and verify

- [ ] **Step 6:** Commit

```bash
git add src/models/ThreadListModel.h src/models/ThreadListModel.cpp CMakeLists.txt src/main.cpp
git commit -m "feat: add ThreadListModel for fetching thread/topic list"
```

---

### Task 3: Thread-aware MessageListModel

**Files:**
- Modify: `src/models/MessageListModel.h`
- Modify: `src/models/MessageListModel.cpp`
- Modify: `src/core/MessagePoller.h`
- Modify: `src/core/MessagePoller.cpp`

- [ ] **Step 1:** Add `threadId` property to MessageListModel

```cpp
// In header:
Q_PROPERTY(int threadId READ threadId WRITE setThreadId NOTIFY threadIdChanged)

int m_threadId = 0;

int threadId() const { return m_threadId; }
void setThreadId(int id);

signals:
    void threadIdChanged();
```

- [ ] **Step 2:** Implement `setThreadId` — clears messages, reloads with `threadId` filter

When threadId > 0, `loadHistory()` adds `?threadId=X` to the API call. When threadId == 0, normal (unfiltered) behavior.

- [ ] **Step 3:** Add `threadId` support to MessagePoller

`poll()` includes `threadId` parameter when set:
```cpp
if (m_threadId > 0)
    params.addQueryItem("threadId", QString::number(m_threadId));
```

Add `void setThreadId(int id)` to MessagePoller.

- [ ] **Step 4:** Add ThreadIdRole to expose thread membership in QML

```cpp
case ThreadIdRole: return m.threadId;
```

And in roleNames: `{ThreadIdRole, "threadId"}`

- [ ] **Step 5:** Build and verify

- [ ] **Step 6:** Commit

```bash
git add src/models/MessageListModel.h src/models/MessageListModel.cpp src/core/MessagePoller.h src/core/MessagePoller.cpp
git commit -m "feat: thread-aware message loading and polling with threadId filter"
```

---

## Phase 2: Thread QML UI

### Task 4: ThreadItem.qml delegate

**Files:**
- Create: `src/qml/ThreadItem.qml`
- Modify: `CMakeLists.txt` (add to QML_FILES)

- [ ] **Step 1:** Create ThreadItem.qml

A row delegate showing: colored dot, thread title, last message preview, reply count badge, last activity time. Follows the same visual language as ConversationItem but slightly more compact.

```qml
ItemDelegate {
    id: threadItem
    height: 64
    padding: 0

    required property int index
    required property int threadId
    required property string title
    required property string lastMessage
    required property string lastAuthor
    required property real lastActivity
    required property int replyCount
    required property int iconColor

    property bool selected: false

    // Color dot + title + preview + count badge
    // Similar to ConversationItem but with thread-specific styling
}
```

- [ ] **Step 2:** Add to CMakeLists.txt QML_FILES

- [ ] **Step 3:** Build and verify

- [ ] **Step 4:** Commit

---

### Task 5: ThreadListView.qml overlay

**Files:**
- Create: `src/qml/ThreadListView.qml`
- Modify: `CMakeLists.txt`

- [ ] **Step 1:** Create ThreadListView

A panel that shows the list of threads for a conversation. Includes:
- Header with "Topics" title and "New Topic" button
- ListView of ThreadItem delegates
- Clicking a thread emits `threadSelected(int threadId, string title)`
- Empty state when no threads exist

- [ ] **Step 2:** Add to CMakeLists.txt

- [ ] **Step 3:** Build and verify

- [ ] **Step 4:** Commit

---

### Task 6: Integrate threads into ChatView

**Files:**
- Modify: `src/qml/ChatView.qml`
- Modify: `src/qml/Main.qml`

- [ ] **Step 1:** Add thread state to ChatView

```qml
property int activeThreadId: 0
property string activeThreadTitle: ""
property bool showThreadList: conversationType === 2 || conversationType === 3  // Group/Public
```

- [ ] **Step 2:** Add thread list overlay

When `showThreadList` is true and `activeThreadId === 0`, show ThreadListView instead of the message list. When a thread is selected, set `activeThreadId` and `messageModel.threadId` to filter messages.

- [ ] **Step 3:** Add back navigation

When viewing a thread, show a "← Back to Topics" button in the header. Clicking resets `activeThreadId = 0` and `messageModel.threadId = 0`.

- [ ] **Step 4:** Wire conversation selection to reset thread state

When the user switches conversations in Main.qml, reset `chatView.activeThreadId = 0`.

- [ ] **Step 5:** Build, test, commit

---

### Task 7: Thread indicator on root messages

**Files:**
- Modify: `src/qml/MessageBubble.qml`

- [ ] **Step 1:** Show thread indicator on messages that are thread roots

When a message has replies (is a thread root), show a small "💬 N replies" indicator below the message. Clicking it opens the thread.

```qml
// After reactions row:
Rectangle {
    visible: threadReplyCount > 0
    // "💬 5 replies" pill that opens the thread
}
```

- [ ] **Step 2:** Build and verify

- [ ] **Step 3:** Commit

---

## Phase 3: UX Polish

### Task 8: Reaction pills (clickable)

**Files:**
- Modify: `src/qml/MessageBubble.qml`
- Modify: `src/models/MessageListModel.h/.cpp`

- [ ] **Step 1:** Change reactions from plain text to clickable pills

Currently reactions display as `"👍 3  ❤️ 1"` text. Replace with a Flow of rounded pill rectangles:

```qml
Flow {
    visible: reactions.length > 0
    spacing: 4

    Repeater {
        model: reactions.split("  ")  // or parse from structured data

        Rectangle {
            width: reactionLabel.implicitWidth + 16
            height: 24
            radius: 12
            color: Theme.darkMode ? Qt.rgba(1,1,1,0.08) : Qt.rgba(0,0,0,0.06)

            Label {
                id: reactionLabel
                anchors.centerIn: parent
                text: modelData
                font.pixelSize: Theme.fontSizeSmall
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    // Toggle own reaction
                    var emoji = modelData.split(" ")[0]
                    messageModel.addReaction(messageId, emoji)
                }
            }
        }
    }
}
```

- [ ] **Step 2:** Expose structured reaction data (JSON array instead of string)

Add `ReactionsJsonRole` to MessageListModel that returns the raw reactions object, so QML can parse emoji and count separately. This allows highlighting reactions the user has already added.

- [ ] **Step 3:** Build and verify

- [ ] **Step 4:** Commit

---

### Task 9: Fix hover action button positioning

**Files:**
- Modify: `src/qml/MessageBubble.qml`

- [ ] **Step 1:** Fix action bar clipping and overlap

Currently the hover action Row can overlap adjacent messages or clip off-screen. Fix by:
- Constrain y position to stay within the bubble bounds
- For own messages, position actions to the left of the bubble
- For other messages, position to the right
- Add a subtle background to make actions visible over content

- [ ] **Step 2:** Build and verify

- [ ] **Step 3:** Commit

---

### Task 10: Implement context menu actions

**Files:**
- Modify: `src/qml/MessageBubble.qml`
- Modify: `src/models/MessageListModel.h/.cpp`

- [ ] **Step 1:** Implement "Copy message link" action

Generate a link like `https://server/call/{token}#message_{id}` and copy to clipboard.

- [ ] **Step 2:** Implement "Pin" action

`POST /apps/spreed/api/v1/chat/{token}/{messageId}/pin`

- [ ] **Step 3:** Implement "Delete" action (own messages only)

`DELETE /apps/spreed/api/v1/chat/{token}/{messageId}`

Replace the current `/delete` hack with proper API call.

- [ ] **Step 4:** Build and verify

- [ ] **Step 5:** Commit

---

### Task 11: Final integration and cleanup

- [ ] **Step 1:** Test thread creation — reply to a message, verify it creates a thread
- [ ] **Step 2:** Test thread navigation — open thread, back to topics, switch conversations
- [ ] **Step 3:** Test reaction pills — click to toggle, verify visual state
- [ ] **Step 4:** Test context menu — copy, pin, delete
- [ ] **Step 5:** Clean build and push

```bash
git push origin master
```

---

## Execution Notes

- **Build path**: `C:/build/talk-qt` (source at `C:/src/talk-desktop-qt`)
- **Kill before rebuild**: `taskkill.exe //IM talk-qt.exe //F 2>/dev/null; sleep 3`
- **Run**: `export PATH="/c/Qt/6.8.2/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH" && QT_FORCE_STDERR_LOGGING=1 /c/build/talk-qt/talk-qt.exe`
- **Pitfall**: QML files must start with uppercase. Add `import TalkQt` to new QML files.
- **Pitfall**: New QML files must be added to `QML_FILES` in CMakeLists.txt.
- **Pitfall**: `set_source_files_properties` for singletons must be BEFORE `qt_add_qml_module`.
