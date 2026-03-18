# Message History Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cache chat messages locally in SQLite so conversations load instantly from cache while fresh data is fetched in the background.

**Architecture:** A `MessageCache` class wraps SQLite (via Qt's QSqlDatabase) and provides simple save/load/merge operations keyed by conversation token. `MessageListModel::setConversationToken()` loads cached messages first (instant), then fetches from server and merges new messages in. The cache stores raw message JSON blobs to avoid schema migrations as the Message model evolves.

**Tech Stack:** Qt 6.8.2, QSqlDatabase + SQLite driver, C++20, existing ApiClient/MessagePoller infrastructure.

---

### File Structure

| Action | File | Responsibility |
|--------|------|----------------|
| Create | `src/core/MessageCache.h` | Cache class header — save, load, merge API |
| Create | `src/core/MessageCache.cpp` | SQLite operations: init DB, upsert messages, query by token |
| Modify | `src/models/MessageListModel.h` | Add `MessageCache*` member, `loadFromCache()` method |
| Modify | `src/models/MessageListModel.cpp` | Load cache on token switch, save to cache after fetch |
| Modify | `CMakeLists.txt` | Add `Qt6::Sql` dependency and `MessageCache.cpp` source |
| Modify | `src/main.cpp` | Create `MessageCache` instance, pass to `MessageListModel` |

---

### Task 1: Add Qt SQL dependency to CMakeLists.txt

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add Sql to find_package**

In `CMakeLists.txt`, add `Sql` to the `find_package(Qt6 REQUIRED COMPONENTS ...)` list:

```cmake
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Network
    Qml
    Quick
    QuickControls2
    Sql
    WebSockets
)
```

- [ ] **Step 2: Add Qt6::Sql to target_link_libraries**

```cmake
target_link_libraries(talk-qt PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Sql
    Qt6::WebSockets
)
```

- [ ] **Step 3: Verify it configures**

Run:
```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
cmake -B C:/build/talk-qt -S C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
```
Expected: Configuring done, no errors about Sql.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: add Qt6::Sql dependency for message cache"
```

---

### Task 2: Create MessageCache class

**Files:**
- Create: `src/core/MessageCache.h`
- Create: `src/core/MessageCache.cpp`
- Modify: `CMakeLists.txt` (add source file)

- [ ] **Step 1: Write MessageCache.h**

```cpp
#pragma once

#include <QObject>
#include <QVector>
#include <QJsonObject>
#include <QSqlDatabase>
#include "models/Message.h"

/**
 * SQLite-backed message cache.
 * Stores messages as JSON blobs keyed by (conversation token, message id).
 * Provides instant loading of cached messages on conversation switch.
 */
class MessageCache : public QObject
{
    Q_OBJECT

public:
    explicit MessageCache(QObject *parent = nullptr);

    /// Load cached messages for a conversation, ordered oldest-first.
    QVector<Message> loadMessages(const QString &token, int limit = 200);

    /// Save messages to cache (upserts by id).
    void saveMessages(const QString &token, const QVector<Message> &messages);

    /// Get the highest cached message ID for a conversation (for incremental fetch).
    int lastMessageId(const QString &token);

    /// Get the oldest cached message ID for a conversation (for history pagination).
    int oldestMessageId(const QString &token);

    /// Delete all cached messages for a conversation.
    void clearConversation(const QString &token);

    /// Delete entire cache.
    void clearAll();

private:
    void initDatabase();

    QSqlDatabase m_db;
};
```

- [ ] **Step 2: Write MessageCache.cpp**

```cpp
#include "core/MessageCache.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>

MessageCache::MessageCache(QObject *parent)
    : QObject(parent)
{
    initDatabase();
}

void MessageCache::initDatabase()
{
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    QString dbPath = dataDir + "/message_cache.db";

    m_db = QSqlDatabase::addDatabase("QSQLITE", "message_cache");
    m_db.setDatabaseName(dbPath);

    if (!m_db.open()) {
        qWarning() << "Failed to open message cache DB:" << m_db.lastError().text();
        return;
    }

    QSqlQuery q(m_db);
    q.exec("CREATE TABLE IF NOT EXISTS messages ("
           "  token TEXT NOT NULL,"
           "  message_id INTEGER NOT NULL,"
           "  timestamp INTEGER NOT NULL,"
           "  json TEXT NOT NULL,"
           "  PRIMARY KEY (token, message_id)"
           ")");
    q.exec("CREATE INDEX IF NOT EXISTS idx_messages_token_ts "
           "ON messages(token, timestamp ASC)");

    qDebug() << "Message cache opened at" << dbPath;
}

QVector<Message> MessageCache::loadMessages(const QString &token, int limit)
{
    QVector<Message> result;

    QSqlQuery q(m_db);
    q.prepare("SELECT json FROM messages WHERE token = :token "
              "ORDER BY timestamp ASC, message_id ASC LIMIT :limit");
    q.bindValue(":token", token);
    q.bindValue(":limit", limit);

    if (!q.exec()) {
        qWarning() << "Cache load failed:" << q.lastError().text();
        return result;
    }

    while (q.next()) {
        QJsonDocument doc = QJsonDocument::fromJson(q.value(0).toString().toUtf8());
        if (!doc.isNull()) {
            result.append(Message::fromJson(doc.object()));
        }
    }

    return result;
}

void MessageCache::saveMessages(const QString &token, const QVector<Message> &messages)
{
    if (messages.isEmpty()) return;

    m_db.transaction();

    QSqlQuery q(m_db);
    q.prepare("INSERT OR REPLACE INTO messages (token, message_id, timestamp, json) "
              "VALUES (:token, :id, :ts, :json)");

    for (const auto &msg : messages) {
        QJsonObject json;
        json["id"] = msg.id;
        json["token"] = msg.token.isEmpty() ? token : msg.token;
        json["actorType"] = msg.actorType;
        json["actorId"] = msg.actorId;
        json["actorDisplayName"] = msg.actorDisplayName;
        json["message"] = msg.message;
        json["timestamp"] = msg.timestamp;
        json["messageType"] = msg.messageType;
        if (!msg.replyTo.isEmpty()) {
            json["parent"] = msg.replyTo;
        }
        if (!msg.reactions.isEmpty()) {
            json["reactions"] = msg.reactions;
        }

        q.bindValue(":token", token);
        q.bindValue(":id", msg.id);
        q.bindValue(":ts", msg.timestamp);
        q.bindValue(":json", QString::fromUtf8(
            QJsonDocument(json).toJson(QJsonDocument::Compact)));
        q.exec();
    }

    m_db.commit();
}

int MessageCache::lastMessageId(const QString &token)
{
    QSqlQuery q(m_db);
    q.prepare("SELECT MAX(message_id) FROM messages WHERE token = :token");
    q.bindValue(":token", token);
    if (q.exec() && q.next())
        return q.value(0).toInt();
    return 0;
}

int MessageCache::oldestMessageId(const QString &token)
{
    QSqlQuery q(m_db);
    q.prepare("SELECT MIN(message_id) FROM messages WHERE token = :token");
    q.bindValue(":token", token);
    if (q.exec() && q.next())
        return q.value(0).toInt();
    return 0;
}

void MessageCache::clearConversation(const QString &token)
{
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM messages WHERE token = :token");
    q.bindValue(":token", token);
    q.exec();
}

void MessageCache::clearAll()
{
    QSqlQuery q(m_db);
    q.exec("DELETE FROM messages");
}
```

- [ ] **Step 3: Add MessageCache.cpp to CMakeLists.txt**

Add `src/core/MessageCache.cpp` to the `qt_add_executable` source list:

```cmake
qt_add_executable(talk-qt
    src/main.cpp
    src/core/ApiClient.cpp
    src/core/AuthManager.cpp
    src/core/MessagePoller.cpp
    src/core/MessageCache.cpp
    src/models/Conversation.cpp
    src/models/Message.cpp
    src/models/ConversationListModel.cpp
    src/models/MessageListModel.cpp
    resources/talq.rc
)
```

- [ ] **Step 4: Verify it compiles**

Run:
```bash
taskkill.exe //IM talk-qt.exe //F 2>/dev/null
sleep 1
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
cmake -B C:/build/talk-qt -S C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build C:/build/talk-qt
```
Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/core/MessageCache.h src/core/MessageCache.cpp CMakeLists.txt
git commit -m "feat: add MessageCache class with SQLite storage"
```

---

### Task 3: Integrate cache into MessageListModel

**Files:**
- Modify: `src/models/MessageListModel.h`
- Modify: `src/models/MessageListModel.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: Update MessageListModel.h**

Add `MessageCache` forward declaration and new members:

```cpp
// Add forward declaration after existing includes:
class MessageCache;

// Add to constructor:
explicit MessageListModel(ApiClient *api, MessageCache *cache, QObject *parent = nullptr);

// Add private member:
MessageCache *m_cache;
```

- [ ] **Step 2: Update MessageListModel constructor in .cpp**

Change constructor to accept and store cache pointer:

```cpp
MessageListModel::MessageListModel(ApiClient *api, MessageCache *cache, QObject *parent)
    : QAbstractListModel(parent)
    , m_api(api)
    , m_cache(cache)
    , m_poller(new MessagePoller(api, this))
{
```

Add include at top:
```cpp
#include "core/MessageCache.h"
```

- [ ] **Step 3: Load from cache in setConversationToken()**

In `setConversationToken()`, after clearing messages and before joining the room, load cached messages:

```cpp
void MessageListModel::setConversationToken(const QString &token)
{
    if (m_token == token)
        return;

    // Stop polling old conversation
    m_poller->stop();

    // Clear messages
    beginResetModel();
    m_messages.clear();
    endResetModel();

    m_token = token;
    m_oldestMessageId = 0;
    emit conversationTokenChanged();

    if (token.isEmpty())
        return;

    // Load cached messages immediately (instant display)
    QVector<Message> cached = m_cache->loadMessages(token);
    if (!cached.isEmpty()) {
        beginInsertRows({}, 0, cached.size() - 1);
        m_messages = cached;
        endInsertRows();
        m_oldestMessageId = m_messages.first().id;
    }

    // Join conversation and fetch fresh data from server
    QString joinToken = token;
    m_api->post("apps/spreed/api/v4/room/" + token + "/participants/active",
        [this, joinToken](bool ok, const QJsonObject &, int) {
            if (m_token != joinToken)
                return;
            if (!ok) {
                emit errorOccurred("Failed to join conversation");
                return;
            }
            loadHistory();
        });
}
```

- [ ] **Step 4: Save to cache after loading history**

In `loadHistory()`, after inserting messages into the model, save them to cache. Add this after the `endInsertRows()` call:

```cpp
        if (!newMsgs.isEmpty()) {
            beginInsertRows({}, 0, newMsgs.size() - 1);
            for (int i = newMsgs.size() - 1; i >= 0; --i)
                m_messages.prepend(newMsgs[i]);
            endInsertRows();

            m_oldestMessageId = m_messages.first().id;

            // Save to cache
            m_cache->saveMessages(m_token, newMsgs);
        }
```

- [ ] **Step 5: Save polled messages to cache**

In `appendMessages()`, save new messages to cache after appending:

```cpp
    if (newMsgs.isEmpty()) return;

    int first = m_messages.size();
    beginInsertRows({}, first, first + newMsgs.size() - 1);
    m_messages.append(newMsgs);
    endInsertRows();

    // Save new messages to cache
    m_cache->saveMessages(m_token, newMsgs);
```

- [ ] **Step 6: Update main.cpp**

Add include and create MessageCache, pass to MessageListModel:

```cpp
#include "core/MessageCache.h"

// In main(), change:
//   MessageListModel messages(&api);
// To:
    MessageCache cache;
    MessageListModel messages(&api, &cache);
```

- [ ] **Step 7: Build and test**

```bash
taskkill.exe //IM talk-qt.exe //F 2>/dev/null
sleep 1
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:/c/Qt/6.8.2/mingw_64/bin:$PATH"
cmake -B C:/build/talk-qt -S C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build C:/build/talk-qt
QT_FORCE_STDERR_LOGGING=1 /c/build/talk-qt/talk-qt.exe
```

Test:
1. Login, open a conversation with messages — should load from server
2. Switch to another conversation, then switch back — should show cached messages instantly before server fetch

- [ ] **Step 8: Commit**

```bash
git add src/models/MessageListModel.h src/models/MessageListModel.cpp src/main.cpp
git commit -m "feat: load cached messages instantly on conversation switch"
```

---

### Task 4: Merge server messages with cached messages

**Files:**
- Modify: `src/models/MessageListModel.cpp`

Currently, `loadHistory()` prepends server messages at the top, which can create duplicates with cached messages. We need to merge properly.

- [ ] **Step 1: Update loadHistory() to merge with existing cached messages**

Replace the message insertion block in `loadHistory()` (the section after `QVector<Message> newMsgs;` loop) with merge-aware logic:

```cpp
        // Merge with existing (cached) messages — avoid duplicates
        QVector<Message> toInsert;
        for (const auto &msg : newMsgs) {
            bool exists = false;
            for (const auto &existing : m_messages) {
                if (existing.id == msg.id) { exists = true; break; }
            }
            if (!exists)
                toInsert.append(msg);
        }

        if (!toInsert.isEmpty()) {
            beginInsertRows({}, 0, toInsert.size() - 1);
            for (int i = toInsert.size() - 1; i >= 0; --i)
                m_messages.prepend(toInsert[i]);
            endInsertRows();

            // Save new messages to cache
            m_cache->saveMessages(m_token, toInsert);
        }

        m_oldestMessageId = m_messages.first().id;
```

- [ ] **Step 2: Build and test**

```bash
taskkill.exe //IM talk-qt.exe //F 2>/dev/null; sleep 1
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:/c/Qt/6.8.2/mingw_64/bin:$PATH"
cmake --build C:/build/talk-qt
```
Expected: No duplicate messages when switching between conversations.

- [ ] **Step 3: Commit**

```bash
git add src/models/MessageListModel.cpp
git commit -m "fix: merge server messages with cache to prevent duplicates"
```
