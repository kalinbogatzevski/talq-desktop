# Threads/Topics UX Design

**Date:** 2026-03-19
**Status:** Approved
**Backend:** Nextcloud Talk Thread API (v22+, `threads` capability)

---

## Overview

Display Nextcloud Talk threads as Telegram-style Topics inside group chats. Groups with topics use a 3-column layout: squeezed icon sidebar | topics list | messages. Non-topic groups keep the existing 2-column layout.

---

## Layout

### 3-Column Layout (Topic Groups)

```
┌────────┬──────────────┬──────────────────────────────┐
│ Icons  │ Topics List  │ Messages                     │
│ (56px) │ (~240px)     │ (fill)                       │
│        │              │                              │
│  [D]   │ Dev Team   + │ ● Bug Reports                │
│  [I]   │              │   Dev Team · 12 messages      │
│  [R]   │ ● General  3 │──────────────────────────────│
│  [T]   │ ● Bugs     1 │ Ilko: status shows offline   │
│        │ ● Features   │ Kalin: found it, fixed       │
│        │ ● Releases   │ Ilko: 👍 testing now          │
│        │              │──────────────────────────────│
│        │              │ Reply in Bug Reports...   [>]│
└────────┴──────────────┴──────────────────────────────┘
```

### 2-Column Layout (Regular Groups / 1:1)

```
┌──────────────────┬───────────────────────────────────┐
│ Sidebar (~320px) │ Messages (fill)                   │
│                  │                                   │
│ Dev Team         │ Chat content...                   │
│ Ilko             │                                   │
│ Rakesh           │                                   │
└──────────────────┴───────────────────────────────────┘
```

### Layout Switching

- **Auto-squeeze:** When selecting a group that has topics, the sidebar animates from full-width (~320px) to icon-only (56px) and the topics list column slides in.
- **Auto-expand:** When selecting a regular group or 1:1 chat from the icon sidebar, the topics list slides out and the sidebar animates back to full-width.
- **Manual toggle:** A small chevron button on the sidebar allows collapsing/expanding manually regardless of topic state.
- **Smooth transitions:** All width changes use animated transitions (200-300ms, OutCubic easing).

---

## Icons Sidebar (Squeezed State)

- Width: 56px
- Shows circular avatars (40x40) for each conversation
- Active conversation has a 2px accent ring (`#2ec4b6`)
- Clicking a different conversation:
  - If it has topics: topics list updates (slide animation), messages area updates
  - If it has no topics: topics column slides out, sidebar expands to full

---

## Topics List

### Header
- Group name (bold, 14px)
- "+" button (accent color) for creating new topics

### "All Messages" Row (Always First)
- Teal dot, title "All Messages"
- Shows unfiltered conversation (no `threadId` filter) — catches standalone messages not in any topic
- Prevents invisible messages from other clients that don't use topics

### Topic Rows
- Colored dot (10px circle) — auto-assigned on creation
- Topic title (bold, 13px) — derived from thread root message text
- Last message preview (muted, 11px, ellipsized)
- Timestamp (muted, 10px)
- Unread badge (colored pill matching the dot, only when unread > 0)

### Selection State
- Left border accent (3px, matching dot color)
- Tinted background (dot color at ~10% opacity)
- Unread badge hidden when selected (messages are visible)

### Sort Order
- By last activity (most recent first)

---

## Topic Creation

- "+" button in topics list header
- Opens inline text input at top of the topics list (replaces the "+" row)
- Type topic name, press Enter to create
- Auto-assigns a color from a rotating palette
- Escape or clicking away cancels
- Max length: 128 characters. Empty names rejected.
- On success: new topic auto-selected, messages area shows it
- On failure: inline error text, input stays open for retry
- **API:** Sends a regular message via `POST /chat/{token}` with the topic name as text. This root message becomes the thread. The topic title is derived from the root message text and stored locally. Note: `threadTitle` only works via the file-sharing endpoint — for text-only topic creation, the root message text IS the title.

---

## Messages Area

### Header
- Topic color dot (10px)
- Topic title (bold, 14px)
- Subtitle: group name + message count (muted, 11px)

### Messages
- Standard message bubbles (same as regular chat)
- Same reactions, context menu, reply-to behavior
- Messages are filtered by `threadId` parameter

### Composer
- Placeholder: "Reply in [topic name]..."
- Same attach button, send button, typing indicators
- **API:** Sends with `replyTo={threadRootMessageId}` — this makes the message part of the thread. In-thread reply-to-specific-message uses the same `replyTo` but still appears in the thread (Talk does not support nested threads).

### Empty State
- When no topic is selected: centered icon + "Select a topic" text

---

## Polling & Data

### Thread Discovery

**Primary method:** Fetch the last 200 messages via `GET /chat/{token}` and group by `parent.id` to reconstruct threads. Extract thread root message text as title.

**Supplementary:** Use `GET /chat/{token}/threads/{threadId}` for individual thread metadata when available.

**Limitation:** Only threads with activity in the last 200 messages are discovered on first load. Older inactive threads are invisible until a message arrives in them.

**Local persistence:** Thread index (threadId, title, color, lastActivity, lastReadMessageId) is stored in the SQLite message cache (`message_cache.db`). This ensures threads discovered in previous sessions remain visible even without recent activity. The index is updated incrementally from polled messages.

**`hasTopics` flag:** A conversation is considered a "topic group" if the local thread index contains at least one thread for its token. This is lazy-populated on first conversation open and cached in `Conversation.hasTopics`. The flag persists across restarts via the SQLite thread index.

### Message Loading
- Use `GET /chat/{token}?threadId={id}` for filtered message loading
- Use `GET /chat/{token}?threadId={id}&lookIntoFuture=1` for live polling
- Same long-poll timeout (30s) as regular chat

### Unread Tracking

- Each thread stores `lastReadMessageId` in the SQLite thread index
- On conversation load: compare each thread's `lastReadMessageId` against `lastActivity` message ID to compute unread count
- On poll: incoming messages with a `parent.id` matching a known thread increment that thread's unread count
- On topic select: update `lastReadMessageId` to the latest message in that thread, set unread to 0
- The "All Messages" row tracks the conversation-level unread (existing `unreadMessages` from room API)
- Talk API `setReadMarker` is conversation-level — calling it marks the whole conversation read. Per-thread read state is client-side only.

### Capability Check
- On login, check `spreed.features` array for `"threads"`
- If not present, topic groups behave as regular groups (no squeeze, no topic list)

---

## Color Palette

Auto-assigned to topics in rotation order:

| Index | Color   | Hex       |
|-------|---------|-----------|
| 0     | Teal    | `#2ec4b6` |
| 1     | Red     | `#e07060` |
| 2     | Orange  | `#f0a050` |
| 3     | Green   | `#5ec76a` |
| 4     | Purple  | `#9b7cd4` |
| 5     | Pink    | `#e87aae` |

Colors stored locally per-topic (Talk API has no icon color field for threads).

---

## Architecture

### New C++ Components

- **TopicListModel** — `QAbstractListModel` for the topics list
  - Roles: threadId, title, color, unreadCount, replyCount, lastActivity, lastMessageText, lastMessageAuthor
  - Fetches thread info from API, maintains local state
  - Sorting by lastActivity

### New QML Components

- **TopicList.qml** — Topics list column with header, ListView, creation input
- **TopicRow.qml** — Individual topic row delegate (dot, title, preview, badge)

### Modified Components

- **Main.qml** — Replace `SplitView` with custom `RowLayout` to support animated width transitions (SplitView's attached properties don't support `Behavior on width`)
- **ConversationList.qml** — Squeeze animation, icon-only mode, manual toggle
- **ChatView.qml** — Topic header variant, threadId-aware message loading
- **MessageListModel** — `threadId` filter parameter for API calls
- **MessagePoller** — `threadId` parameter for long-poll endpoint
- **MessageComposer.qml** — Dynamic placeholder text

---

## Minimum Window Width

With 3-column layout: 56 + 240 + 300 = 596px minimum. Set `root.minimumWidth = 600` when in topic view. If window is narrower than 600px when switching to a topic group, expand to 600px.

---

## Scope Exclusions (v1)

- No topic pinning
- No topic close/reopen
- No topic deletion
- No topic title editing
- No topic search
- No per-topic notification settings (thread subscription deferred to v1.1)
- No draft-per-topic storage
- No topic reordering
- No keyboard navigation between columns (Tab/arrows)
- These can be added incrementally in future versions
