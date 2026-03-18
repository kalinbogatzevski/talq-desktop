# Nextcloud Talk Threads & Telegram Topics: Research for TalQ

**Date:** 2026-03-19
**Purpose:** Comprehensive research on thread/topic APIs for implementing Telegram-style Topics in TalQ

---

## Table of Contents

1. [Nextcloud Talk Thread API](#1-nextcloud-talk-thread-api)
2. [How Threads Work in Talk](#2-how-threads-work-in-talk)
3. [Telegram Forum Topics API](#3-telegram-forum-topics-api)
4. [Matrix Protocol Threads (MSC3440)](#4-matrix-protocol-threads-msc3440)
5. [Open Source Client Implementations](#5-open-source-client-implementations)
6. [Comparison Table](#6-comparison-table)
7. [Key Answers](#7-key-answers)
8. [Recommended Approach for TalQ](#8-recommended-approach-for-talq)
9. [Sources](#9-sources)

---

## 1. Nextcloud Talk Thread API

### 1.1 Version History

- **Talk v22.0.0** (Nextcloud 32, "Hub 25 Autumn"): Introduced the `threads` capability and thread functionality
- Tracked in GitHub issues: spreed#9679 (API), spreed#9680 (Frontend), spreed#1469 (original proposal)
- spreed#9869 (Threads 2.0) was closed as "not planned" -- the enhanced thread features (naming, search, sidebar list) were partially folded into v22 itself
- Bot compatibility with threads is still being tracked (spreed#15725)

### 1.2 Capability Flag

The server advertises thread support via the capabilities endpoint:

```
GET /ocs/v2.php/cloud/capabilities
```

Response includes (when supported):

```json
{
  "ocs": {
    "data": {
      "capabilities": {
        "spreed": {
          "features": ["threads", ...]
        }
      }
    }
  }
}
```

The `threads` capability (introduced in Talk API version 22) indicates "whether the chat supports threads."

### 1.3 Thread Data Model

**Threads in Nextcloud Talk are NOT separate rooms.** A thread is defined as "all messages that reply to the same root parent message." The thread ID equals the message ID of the root (first) message.

Key concepts:
- A **thread** is a filtered view of messages sharing a common root parent
- The **thread ID** = the `id` of the root message
- Messages within a thread reference the root via the `parent` field
- Threads can have a **title** (set at creation time via `threadTitle`)
- Users can **subscribe/unsubscribe** to threads for notification management
- Thread attendees (subscribers) are tracked and automatically removed when leaving the conversation

### 1.4 API Endpoints

#### 1.4.1 Receiving Messages (with thread filtering)

```
GET /ocs/v2.php/apps/spreed/api/v1/chat/{token}
```

Parameters (standard + thread):
| Parameter | Type | Description |
|-----------|------|-------------|
| `lookIntoFuture` | int | 0 = history, 1 = poll for new |
| `limit` | int | Max messages (default 100, max 200) |
| `lastKnownMessageId` | int | Pagination offset |
| `timeout` | int | Long-poll wait (default 30, max 60) |
| `setReadMarker` | int | Auto-mark read (default 1) |
| `includeLastKnown` | int | Include offset message (default 0) |
| `noStatusUpdate` | int | Skip online status (default 0) |
| `markNotificationsAsRead` | int | Clear notifications (default 1) |

**Thread support**: Long polling with `threadId` was added in spreed PR #15578. The `threadId` parameter filters the message stream to show only messages belonging to that thread.

#### 1.4.2 Getting Message Context (with thread)

```
GET /ocs/v2.php/apps/spreed/api/v1/chat/{token}/{messageId}/context
```

Also supports `threadId` parameter (PR #15578) to get context within a thread.

#### 1.4.3 Sending a Message (replying in thread)

```
POST /ocs/v2.php/apps/spreed/api/v1/chat/{token}
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `message` | string | Message content |
| `replyTo` | int | Parent message ID (for direct reply) |
| `referenceId` | string | Client-side SHA256 identifier |
| `silent` | bool | Suppress notifications |

To reply within a thread: set `replyTo` to the thread root message ID. This makes the message part of that thread.

#### 1.4.4 Creating a New Thread (via file share)

```
POST /ocs/v2.php/apps/files_sharing/api/v1/shares
```

The `talkMetaData` JSON parameter supports thread creation:

| Field | Type | Description |
|-------|------|-------------|
| `messageType` | string | "voice-message" or "comment" |
| `caption` | string | Display caption |
| `replyTo` | int | Parent message ID |
| `threadId` | int | Existing thread ID (posts without notification) |
| `threadTitle` | string | **When non-empty, starts a NEW thread** |
| `silent` | bool | Suppress notifications |

**Important**: `threadId` associates content with an existing thread *without* sending a notification. `threadTitle` creates a brand new thread.

#### 1.4.5 Get Single Thread Info

```
GET /ocs/v2.php/apps/spreed/api/{apiVersion}/chat/{token}/threads/{threadId}
```

Retrieves information about a specific thread when the client doesn't already have it. Returns thread metadata including title, reply count, and subscriber info.

#### 1.4.6 Thread Subscription

Users can subscribe/unsubscribe from threads to manage notifications. The exact endpoint is not yet fully documented in readthedocs, but the functionality is confirmed in the Android client implementation (talk-android#3074) and user documentation:

- Subscribe to a thread: receive notifications for new replies
- Unsubscribe: stop notifications
- Configurable notification levels per thread
- Subscribed threads appear in a "Threads navigation" section

#### 1.4.7 Thread List in Shared Items

Threads can be accessed via the shared items overview:

```
GET /ocs/v2.php/apps/spreed/api/v1/chat/{token}/share/overview
```

### 1.5 Message Data Structure

```json
{
  "id": 123,
  "token": "abc123",
  "actorType": "users",
  "actorId": "admin",
  "actorDisplayName": "Admin User",
  "timestamp": 1710864000,
  "systemMessage": "",
  "messageType": "comment",
  "isReplyable": true,
  "referenceId": "",
  "message": "Hello thread!",
  "messageParameters": {},
  "expirationTimestamp": 0,
  "parent": {
    "id": 100,
    "deleted": false,
    "actorType": "users",
    "actorId": "user1",
    "actorDisplayName": "User One",
    "timestamp": 1710860000,
    "message": "Original message that started the thread",
    "messageType": "comment"
  },
  "reactions": {"👍": 2},
  "reactionsSelf": ["👍"],
  "markdown": true,
  "lastEditTimestamp": 0,
  "silent": false
}
```

The `parent` field:
- **Present** when the message is a reply to another message
- Contains the full parent message data OR `{"id": X, "deleted": true}` if parent was deleted
- The **topmost parent** (root) defines the thread ID
- `parent` is **never set** on the root message itself

### 1.6 Capabilities Summary

| Version | Capability | Description |
|---------|-----------|-------------|
| 8.0 | `chat-replies` | Messages can be replied to (prerequisite for threads) |
| 9.0 | `chat-reference-id` | Client-side message identification |
| 16 | `chat-get-context` | Message context endpoint |
| 18 | `media-caption` | File captions (used with threadTitle) |
| **22** | **`threads`** | **Full thread support** |

---

## 2. How Threads Work in Talk

### 2.1 Architecture

Threads in Nextcloud Talk are **not separate rooms** or sub-conversations. They are a **filtered view** of messages within an existing conversation (room) that share a common root parent message.

```
Conversation (Room)
├── Message A (standalone)
├── Message B (thread root, id=100)
│   ├── Message C (parent.id=100, in thread 100)
│   ├── Message D (parent.id=100, in thread 100)
│   └── Message E (parent.id=100, in thread 100)
├── Message F (standalone)
└── Message G (thread root, id=200, has threadTitle)
    ├── Message H (parent.id=200, in thread 200)
    └── Message I (parent.id=200, in thread 200)
```

### 2.2 Thread Creation

There are two ways to create a thread:

1. **Implicit**: Reply to any message using `replyTo`. The replied-to message becomes the thread root. Further replies to the same root form the thread.

2. **Explicit with title**: When sharing a file, set `threadTitle` in `talkMetaData`. This creates a named thread. The "thread creation option is available in the new message additional actions" where you can "add a title and description for the thread."

**Important constraint**: "It should not be possible to create a thread for some random message" -- threads are created at message-send time, not retroactively assigned to existing messages.

### 2.3 Thread Navigation UI (Web Client)

The Nextcloud Talk web client implements threads with:

1. **Thread indicator on messages**: Messages that are thread roots show a reply count and "Reply in thread" / "Open thread" actions
2. **Filtered view**: Clicking opens a filtered chat showing only thread messages
3. **Back button**: Returns to the unfiltered main conversation
4. **Thread header**: `ThreadHeader.vue` component in `src/components/RightSidebar/Threads/`
5. **Sidebar access**: Threads visible in the Shared Items tab of the content sidebar
6. **Navigation bar**: Subscribed threads accessible from "Threads navigation" in the nav bar
7. **Title editing**: Thread titles can be edited from within the thread or from the sidebar

### 2.4 Thread Notifications

- Users mentioned in threads are automatically marked as thread participants
- Thread attendees are removed when they leave the parent conversation
- Users can subscribe/unsubscribe with configurable notification levels
- Different notification handling for thread replies vs. message quotes
- `threadId` in talkMetaData posts *without* notification (silent thread association)
- `replyTo` posts *with* notification (explicit reply)

### 2.5 Recent Thread-Related PRs (v22.x)

| PR | Description |
|----|-------------|
| #15578 | Support long polling and getContext with threadId |
| #15858 | Add threadId attribute for thread messages |
| #15872 | Return attendee IDs for array filters |
| #15780 | Show temporary thread starter message |
| #15802 | Replace thread author avatar with colored icons |
| #15935 | Add thread controls in sidebar / in-call chat |
| #15943 | Count shared files as thread replies and last message |
| #15945 | Document talkMetaData thread parameters |

---

## 3. Telegram Forum Topics API

### 3.1 Conceptual Model

Telegram's "Topics" (also called "Forum Topics") are fundamentally different from threads/replies. Topics are **first-class organizational units** within a forum-enabled supergroup:

```
Forum Supergroup
├── General Topic (id=1, always exists, cannot be deleted)
├── Topic "Development" (id=42)
│   ├── Message 1
│   ├── Message 2 (reply to Message 1)
│   └── Message 3
├── Topic "Design" (id=85)
│   ├── Message 4
│   └── Message 5
└── Topic "Off-topic" (id=120, closed)
```

Key differences from Talk threads:
- Topics are **explicitly created** with a title and icon
- Topics have **their own message stream** (not filtered from a shared stream)
- Topics can be **pinned, closed, reopened, deleted**
- The **General topic** (id=1) always exists and cannot be deleted (but can be hidden)
- Topics within a forum **cannot have nested threads** (because topics ARE threads internally)
- Forums require **supergroup** status

### 3.2 Forum Creation

Forums are created by either:
1. `channels.createChannel` with the `forum` flag set
2. `channels.toggleForum` on an existing supergroup (requires owner rights)

### 3.3 ForumTopic Data Structure

```
forumTopic#71701da9 flags:#
  my:flags.1?true              // Created by current user
  closed:flags.2?true          // No new messages allowed
  pinned:flags.3?true          // Pinned in topic list
  short:flags.5?true           // Reduced info version
  hidden:flags.6?true          // Only for General topic
  id:int                       // Unique topic ID
  date:int                     // Creation timestamp
  title:string                 // Topic name (1-128 chars)
  icon_color:int               // RGB fallback icon color
  icon_emoji_id:flags.0?long   // Custom emoji for icon
  top_message:int              // Latest message ID
  read_inbox_max_id:int        // Read tracking
  read_outbox_max_id:int       // Read tracking
  unread_count:int             // Unread messages
  unread_mentions_count:int    // Unread @mentions
  unread_reactions_count:int   // Unread reactions
  from_id:Peer                 // Creator
  notify_settings:PeerNotifySettings  // Per-topic notifications
  draft:flags.4?DraftMessage   // Saved draft
```

### 3.4 Bot API Methods (Higher-Level)

| Method | Parameters | Description |
|--------|-----------|-------------|
| `createForumTopic` | chat_id, name, icon_color?, icon_custom_emoji_id? | Create a topic |
| `editForumTopic` | chat_id, message_thread_id, name?, icon_custom_emoji_id? | Edit topic |
| `closeForumTopic` | chat_id, message_thread_id | Close topic |
| `reopenForumTopic` | chat_id, message_thread_id | Reopen topic |
| `deleteForumTopic` | chat_id, message_thread_id | Delete topic |
| `getForumTopics` | chat_id, offset?, limit? | List topics |
| `getForumTopicIconStickers` | (none) | Get allowed icon emojis |
| `unpinAllForumTopicMessages` | chat_id, message_thread_id | Unpin all in topic |

**Bot API ForumTopic object:**
```json
{
  "message_thread_id": 42,
  "name": "Development",
  "icon_color": 7322096,
  "icon_custom_emoji_id": "5368324170671202286"
}
```

### 3.5 MTProto API Methods (Lower-Level)

| Method | Description |
|--------|-------------|
| `channels.createForumTopic` | Create topic with title, color, emoji |
| `channels.editForumTopic` | Modify title, emoji, closed/hidden state |
| `channels.getForumTopics` | List topics with search and pagination |
| `channels.getForumTopicsByID` | Fetch specific topics by ID |
| `channels.updatePinnedForumTopic` | Pin/unpin a topic |
| `channels.reorderPinnedForumTopics` | Reorder pinned topics |
| `channels.deleteTopicHistory` | Delete a non-General topic |
| `channels.toggleViewForumAsMessages` | Toggle flat message view |
| `channels.toggleForum` | Enable/disable forum mode |

### 3.6 Sending Messages to Topics

To send a message to a specific topic (id != 1):
```
// In inputReplyToMessage:
top_msg_id = <topic_id>     // Required for non-General topics
reply_to_msg_id = <msg_id>  // Optional, for replies within the topic
```

For the General topic (id=1): use standard `messages.sendMessage` without special routing.

### 3.7 Icon Colors (Predefined)

Only these RGB values are allowed for `icon_color`:
- `0x6FB9F0` (7322096) - Blue
- `0xFFD67E` (16766590) - Yellow
- `0xCB86DB` (13338331) - Purple
- `0x8EEE98` (9367192) - Green
- `0xFF93B2` (16749490) - Pink
- `0xFB6F5F` (16478047) - Red

### 3.8 UI Modes

Forums support two presentation modes:
- **Tabbed/List view**: Topics shown as a list, each opening its own message stream
- **Flat message view**: All messages from all topics in a single chronological stream (toggled via `channels.toggleViewForumAsMessages`)

---

## 4. Matrix Protocol Threads (MSC3440)

### 4.1 Data Model

Matrix threads use **event relations**. A thread is created by relating events to a root event:

```json
{
  "type": "m.room.message",
  "content": {
    "body": "Reply in thread",
    "m.relates_to": {
      "rel_type": "m.thread",
      "event_id": "$thread_root_event_id",
      "m.in_reply_to": {
        "event_id": "$previous_event_in_thread"
      },
      "is_falling_back": true
    }
  }
}
```

### 4.2 Thread Aggregation (Bundled on Root Event)

```json
{
  "event_id": "$thread_root",
  "unsigned": {
    "m.relations": {
      "m.thread": {
        "latest_event": { /* full event object */ },
        "count": 7,
        "current_user_participated": true
      }
    }
  }
}
```

### 4.3 API Endpoint

```
GET /_matrix/client/v1/rooms/{roomId}/relations/{threadRootEventId}/m.thread
```

Returns all events in the thread, paginated.

### 4.4 Key Properties

- **No nested threads**: Only one level of threading
- **Thread root**: Any message can become a thread root
- **Backward compatible**: Thread-unaware clients see reply chains
- **Server capability**: Advertised in `/versions` as `org.matrix.msc3440`
- Threads were standardized in **Matrix v1.4** (September 2022)

---

## 5. Open Source Client Implementations

### 5.1 Nextcloud Talk Web Client (spreed frontend)

- **Framework**: Vue.js with Nextcloud Vue components
- **Thread components**: Located in `src/components/RightSidebar/Threads/`
  - `ThreadHeader.vue` -- header component for thread view
- **Message component**: `src/components/MessagesList/MessagesGroup/Message/Message.vue`
  - Handles thread indicators, reply buttons, context menus
  - Distinguishes "Reply in thread" vs "Open thread" actions
- **Thread view**: Filtered `MessagesList` showing only messages with matching thread root
- **Navigation**: Thread list accessible from conversation sidebar and dedicated "Threads" navigation entry

### 5.2 Nextcloud Talk Android

- **Thread overview screen**: Shows recent threads with reply counts
- **Followed threads overview**: Indicator for tracked threads
- **Thread title display**: Shown in normal chat for root messages
- **Reply indicators**: "Reply" (no responses) or "x replies" (has responses)
- **Context menu**: "Reply in thread" and "Open thread" options
- **Draft storage**: Supports drafts for both messages and thread replies
- **Refresh interval**: Followed threads checked every 2 hours

### 5.3 Element (Matrix Client)

- **Side panel**: Threads open in a dedicated right-side panel
- **Thread list**: Accessible via thread icon in top-right corner
- **Thread start**: Hover over message, select "Reply in thread"
- **Isolation**: Thread panel is independent of main timeline scrolling
- **Cross-platform**: Available on Desktop, Mobile, and Web
- **Notification model**: Known limitations in beta (badge counts may not sync)

### 5.4 NeoChat (KDE Matrix Client)

- **Threads NOT supported** as of 2026
- Listed as a notable exception alongside VoIP
- Based on **libQuotient** (Qt/C++ Matrix SDK)
- Eventual support planned as Matrix spec evolves
- **Relevant for TalQ**: Shows that even mature Qt-based Matrix clients haven't implemented threads yet, indicating the complexity involved

---

## 6. Comparison Table

| Feature | Nextcloud Talk Threads | Telegram Topics | Matrix Threads |
|---------|----------------------|-----------------|----------------|
| **Unit** | Filtered message stream | First-class entity | Event relation |
| **Creation** | Reply to message / explicit with title | Explicit create with title+icon | Reply with `m.thread` relation |
| **ID** | Root message ID | Topic ID (unique per group) | Root event ID |
| **Title** | Optional (threadTitle) | Required (1-128 chars) | None (inherits root message) |
| **Icon** | Colored author icons | Custom emoji / 6 preset colors | None |
| **Close/Reopen** | No | Yes | No |
| **Delete** | No (delete root message) | Yes (except General) | No |
| **Pin** | No (message pinning exists) | Yes | No |
| **Nested threads** | No | No (topics can't have sub-threads) | No |
| **Notifications** | Subscribe/unsubscribe per thread | Per-topic notification settings | Thread notification rules |
| **General/Default** | No concept | General topic (id=1) | Main timeline |
| **Search** | Not yet | By topic name | By thread root |
| **Flat view toggle** | Main chat is flat, threads are filtered | toggleViewForumAsMessages | Main timeline is flat |
| **Room type** | Same room, filtered view | Forum supergroup | Same room, side panel |
| **API maturity** | New (v22, 2025) | Mature (2022+) | Standardized (v1.4, 2022) |

---

## 7. Key Answers

### Can we list all threads in a conversation?

**Partially.** There is no dedicated "list all threads" endpoint documented in the public API docs. Threads are accessible through:
- The shared items overview endpoint: `GET /chat/{token}/share/overview`
- The sidebar's shared items tab
- The "Threads navigation" section for subscribed threads
- Potentially via the single thread endpoint: `GET /chat/{token}/threads/{threadId}`

In the Talk web client, threads are listed in the right sidebar and in a dedicated "Threads" navigation entry.

### Can we fetch messages filtered by thread?

**Yes.** The `GET /chat/{token}` endpoint supports a `threadId` parameter (since PR #15578) that filters messages to show only those belonging to a specific thread. The `getContext` endpoint also supports `threadId`.

### Can we create a new thread?

**Yes, two ways:**
1. **Via reply**: Send a message with `replyTo` pointing to the message that becomes the thread root. Any message that receives a reply implicitly becomes a thread root.
2. **Explicit with title**: When sharing a file, include `threadTitle` in `talkMetaData` to create a named thread. In the web UI, thread creation is available from the "new message additional actions" menu where you add a title and description.

### Is a thread just "all messages that reply to the same root message"?

**Essentially yes.** A thread is defined by its root message ID. All messages whose topmost parent equals that root ID are part of the thread. The thread ID = root message ID. However, threads have evolved beyond a simple filter:
- They can have titles
- They support subscription/notification management
- They have dedicated UI navigation
- Thread attendees are tracked

### Is there a dedicated thread object in the API?

**Partially.** There is a single-thread info endpoint (`GET /chat/{token}/threads/{threadId}`) that returns thread metadata. However, threads are not a separate database entity like Telegram topics -- they are a logical grouping of messages sharing a root parent, enhanced with metadata (title, subscribers, attendee tracking).

---

## 8. Recommended Approach for TalQ

### 8.1 Architecture Decision

For implementing Telegram-style Topics in TalQ, we have two viable approaches:

#### Option A: Map Topics to Talk Threads (Recommended)

Use Nextcloud Talk's native thread API as the backend for Topics.

**Pros:**
- Uses existing server API -- no server modifications needed
- Thread creation, subscription, and notification management already exist
- Compatible with other Talk clients (web, mobile)
- Thread title maps to Topic name

**Cons:**
- No custom icon/emoji support (Telegram has colored icons)
- No close/reopen functionality
- No dedicated "list all threads" endpoint (requires workarounds)
- Threads are always within a conversation (no standalone topics)
- Limited compared to Telegram's first-class Topic model

**Implementation plan:**
1. Check `threads` capability on connection
2. Use `threadTitle` to create named threads (= Topics)
3. Use `threadId` parameter on `GET /chat/{token}` for filtered view
4. Build a local thread index from message scanning
5. Implement thread subscription for notification management
6. Add Topic-like UI: list view with colored indicators, title, reply count, last activity

#### Option B: Map Topics to Separate Conversations

Create a new Talk conversation for each "Topic."

**Pros:**
- Full conversation features (participants, permissions, avatars)
- Independent notification settings
- Can be archived
- Full API support for listing, searching

**Cons:**
- Loses the "sub-conversation within a room" semantic
- Pollutes the user's conversation list
- No visual grouping as topics under a parent
- Not how Talk is designed to be used

**Recommendation: Option A** -- Thread-based Topics with enhanced client-side UI.

### 8.2 Data Model for TalQ Topics

```cpp
struct Topic {
    int threadId;           // = root message ID in Talk
    QString title;          // = threadTitle from Talk
    QString description;    // = root message content
    int iconColor;          // Client-side only (Telegram-style)
    QString iconEmoji;      // Client-side only
    int unreadCount;        // Tracked client-side
    int replyCount;         // From thread info
    QDateTime lastActivity; // From latest message timestamp
    bool isSubscribed;      // Thread subscription state
    bool isPinned;          // Client-side pinning
    int notificationLevel;  // 0=default, 1=always, 2=mention, 3=never
};
```

### 8.3 UI Layout Proposal

```
┌──────────────────────────────────────────────┐
│ [Conversation Name]              [≡] [+Topic]│
├──────────────────────────────────────────────┤
│ ● Development          3 new │ 15 replies    │
│ ● Design               ─── │ 8 replies      │
│ ● Off-topic            1 new │ 42 replies    │
│ ● Bug Reports          ─── │ 3 replies      │
│ ● General              5 new │ 200+ replies  │
├──────────────────────────────────────────────┤
│ [Selected Topic: Development]    [← Back]    │
│──────────────────────────────────────────────│
│ [Thread messages displayed here]             │
│ ...                                          │
│ [Message input] [Send]                       │
└──────────────────────────────────────────────┘
```

### 8.4 Required API Calls

| Action | API Call |
|--------|---------|
| Check support | `GET /capabilities` -- look for `threads` |
| Create topic | `POST /chat/{token}` with message + `talkMetaData.threadTitle` |
| List topic messages | `GET /chat/{token}?threadId={id}` |
| Reply in topic | `POST /chat/{token}` with `replyTo={threadId}` |
| Get thread info | `GET /chat/{token}/threads/{threadId}` |
| Poll for new messages | `GET /chat/{token}?threadId={id}&lookIntoFuture=1` |
| Get thread context | `GET /chat/{token}/{messageId}/context?threadId={id}` |

### 8.5 Client-Side Enhancements

Since Talk's thread API doesn't provide everything Telegram Topics offers, TalQ should add client-side features:

1. **Local topic index**: Scan conversation messages on sync to build/update a list of threads with titles
2. **Icon colors**: Store per-topic colors in local settings (not synced to server)
3. **Pinned topics**: Track pinned state locally
4. **Topic order**: Sort by last activity, pinned first
5. **Unread tracking**: Maintain per-thread unread counts from message IDs
6. **Draft per topic**: Store draft messages per thread in local storage

### 8.6 Migration Path

If Nextcloud Talk adds richer thread features in the future (Threads 2.0 concepts like search integration, dedicated listing endpoint, topic icons), TalQ can adopt them incrementally. The thread-based architecture ensures forward compatibility.

---

## 9. Sources

### Nextcloud Talk / Spreed
- [Chat Management API](https://nextcloud-talk.readthedocs.io/en/latest/chat/) -- Official chat endpoints documentation
- [Conversation Management API](https://nextcloud-talk.readthedocs.io/en/latest/conversation/) -- Room/conversation endpoints
- [Capabilities API](https://nextcloud-talk.readthedocs.io/en/stable/capabilities/) -- Feature capability flags
- [API - Message threads (spreed#9679)](https://github.com/nextcloud/spreed/issues/9679) -- Thread API design issue
- [Frontend - Message threads (spreed#9680)](https://github.com/nextcloud/spreed/issues/9680) -- Thread frontend issue
- [Threads 2.0 (spreed#9869)](https://github.com/nextcloud/spreed/issues/9869) -- Enhanced threads proposal (closed)
- [Thread discussion (spreed#2450)](https://github.com/nextcloud/spreed/issues/2450) -- Original threads discussion
- [Talk Android threads (talk-android#3074)](https://github.com/nextcloud/talk-android/issues/3074) -- Android implementation
- [Thread PR #15578](https://github.com/nextcloud/spreed/pull/15578) -- threadId support for long polling and getContext
- [Nextcloud Talk threads announcement](https://help.nextcloud.com/t/the-quiet-work-chat-app-threads-dashboard-and-important-conversations-in-nextcloud-talk/236165)
- [Thread configuration discussion](https://help.nextcloud.com/t/configuration-of-threads-in-talk/201385)
- [Bot API and threads](https://help.nextcloud.com/t/bot-api-and-threads/241620)
- [Spreed releases](https://github.com/nextcloud/spreed/releases)

### Telegram
- [Telegram Forum Topics API](https://core.telegram.org/api/forum) -- Forum/topics technical documentation
- [Telegram Threads API](https://core.telegram.org/api/threads) -- Message threads documentation
- [forumTopic constructor](https://core.telegram.org/constructor/forumTopic) -- Topic data structure
- [channels.getForumTopics](https://core.telegram.org/method/channels.getForumTopics) -- Topic listing method
- [channels.createForumTopic](https://core.telegram.org/method/channels.createForumTopic) -- Topic creation
- [Telegram Bot API](https://core.telegram.org/bots/api) -- Bot API methods for topics

### Matrix / Element
- [MSC3440: Threading via m.thread relation](https://github.com/matrix-org/matrix-spec-proposals/pull/3440) -- Thread spec proposal
- [MSC3440 full text](https://github.com/matrix-org/matrix-spec-proposals/blob/gsouquet/threading-via-relations/proposals/3440-threading-via-relations.md)
- [Element Threads Beta announcement](https://element.io/blog/introducing-threads-in-beta/)
- [Matrix v1.4 release (threads)](https://matrix.org/blog/2022/09/29/matrix-v-1-4-release/)

### NeoChat
- [NeoChat GitHub](https://github.com/KDE/neochat) -- KDE Matrix client (no thread support)
- [NeoChat on KDE Apps](https://apps.kde.org/neochat/)
