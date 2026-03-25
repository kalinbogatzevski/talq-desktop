# TalQ - Project Context for Claude Code

## Build Setup

**Qt dev env**: Qt 6.8.2 + MinGW 13.1 installed via `aqtinstall` at `C:\Qt`.

**IMPORTANT**: Source must be accessed via a junction without spaces. The project lives at `My Projects\talk-desktop-qt` but Qt tools break on paths with spaces. Use junction: `C:\src\talk-desktop-qt` → actual path.

**Build commands** (Git Bash):
```bash
# Kill existing, clean build
taskkill.exe //IM talq.exe //F 2>/dev/null; sleep 3
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
rm -rf /c/build/talq
cmake -B C:/build/talq -S C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build C:/build/talq

# Deploy DLLs + launch (handles Qt/MSYS2 MinGW runtime conflict)
bash scripts/deploy-dev.sh
```

**Incremental build + run:**
```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
cmake --build C:/build/talq && bash scripts/deploy-dev.sh
```

**Why deploy-dev.sh exists**: Qt (MinGW 13.1) and GStreamer (MSYS2 MinGW) both ship `libstdc++-6.dll`. MSYS2's version works with both; Qt's causes `0xC0000139`. The script copies all DLLs into the build dir with MSYS2's runtime, so it launches cleanly regardless of PATH.

**Junction setup** (if not exists):
```bash
echo 'mklink /J C:\src\talk-desktop-qt "C:\Users\bogat\Desktop\My Projects\talk-desktop-qt"' > /tmp/mkj.bat && cmd //c /tmp/mkj.bat
```

**Kill before rebuild**: `taskkill.exe //IM talq.exe //F` — MUST wait 3-5s after kill before linking, or get "Permission denied"

**QML changes not detected?** The junction can cause stale build detection. Fix: `rm -rf /c/build/talq` and do a clean build.

## Key Pitfalls (hard-won lessons)

1. **Uppercase QML filenames** — `qt_add_qml_module` only registers types for files starting with uppercase
2. **Singleton declaration order** — `set_source_files_properties(... QT_QML_SINGLETON_TYPE TRUE)` MUST appear BEFORE `qt_add_qml_module`
3. **`import TalkQt`** — All QML files need this for Theme singleton
4. **`QTP0001 NEW`** — Required for correct `:/qt/qml/` resource prefix
5. **MessagePoller::stop()** — Must null `m_currentReply` BEFORE calling `abort()` (abort triggers finished synchronously)
6. **GUI debug output** — `QT_FORCE_STDERR_LOGGING=1` env var required, otherwise Qt messages go to OutputDebugString
7. **Async callbacks** — Always capture conversation token and guard against stale callbacks. Use `QPointer<T>` in lambda captures to prevent use-after-free.
8. **Never use `displayMarginBeginning/End`** on ListView — causes content to render outside bounds, overlapping adjacent items
9. **Never use anchors across sibling subtrees** — causes "Cannot anchor to an item that isn't a parent or sibling" which silently corrupts the entire layout chain
10. **Use `Page` with `header`/`footer`** for chat layout — Page manages sizing between header and footer automatically. ColumnLayout with BottomToTop fails.
11. **TopToBottom + `onCountChanged: positionViewAtEnd()`** — simpler and more reliable than BottomToTop. Store messages oldest-first.
12. **DPI-aware screenshots** — call `ctypes.windll.shcore.SetProcessDpiAwareness(2)` before `GetWindowRect` or `ImageGrab`
13. **QSqlQuery in loops** — prepare() inside the loop or use fresh QSqlQuery per iteration. addBindValue() accumulates across exec() calls if reusing a prepared query.
14. **Centralize shared constants** — color palettes, hash functions go in Theme.qml (e.g., `Theme.topicColor()`, `Theme.stringHash()`)

## Architecture

### C++ Backend
- **ApiClient** — HTTP client wrapping Nextcloud OCS API with Basic auth. `getAbsoluteUrl()` for non-OCS endpoints (avatars)
- **AuthManager** — Login Flow v2, session persistence via QSettings, server capabilities fetch. Exposes `hasThreadsSupport` for Talk v22+ thread detection.
- **MessagePoller** — Long-polls chat API with optional `threadId` filter, emits `messagesReceived`, `lastCommonReadChanged`, `pollSuccess`, `pollError`
- **MessageListModel** — QAbstractListModel, oldest-first storage, optimistic send, reactions, reply-to, retry on failure. Upload progress tracking, paste confirmation via `pasteReady` signal, `createTopic()` for thread bootstrapping.
- **MessageCache** — SQLite cache at `AppData/TalQ/TalQ/message_cache.db`. Worker thread for async I/O. Tables: `messages` (last 200 per conversation), `thread_index` (persisted topic metadata).
- **AvatarProvider** — QQuickAsyncImageProvider, 3-tier cache (memory→disk→server), circular crop in C++
- **ConversationListModel** — QAbstractListModel for sidebar, includes `hasTopics` flag preserved across refresh
- **ThreadListModel** — QAbstractListModel for topics list. Discovers threads by scanning messages with `parent.id`. Synthetic "All Messages" row at index 0. SQLite persistence via MessageCache. `hasTopics` property drives layout changes.
- **SignalingClient** — Standalone signaling WebSocket for typing indicators. `typingRoom` scopes typing to current conversation.

### QML Frontend
- **Main.qml** — Anchors-based 3-column layout: ConversationList | ThreadListView | ChatView. Animated sidebar squeeze (320px → 56px) when topic group selected.
- **ChatView.qml** — `Page` with header/footer. Topic-aware header (color dot + subtitle). Scroll-to-bottom button. Upload progress bar. Paste confirmation bar. `isViewingTopic` helper property.
- **MessageBubble.qml** — Hybrid layout: flat for others (avatar+name+text), teal bubble for own. Context popup with emoji reactions + actions. File type icons with MIME detection.
- **ConversationList.qml** — Sidebar with squeezed (icon-only) mode, manual toggle chevron, avatar + search + controls.
- **ConversationItem.qml** — Shared avatar source between expanded/squeezed views to avoid duplicate image loads.
- **ThreadListView.qml** — Persistent topics column with inline creation input, selection tracking.
- **ThreadItem.qml** — Topic row with colored dot (Theme.topicColor), unread badge, topic-colored selection highlight.
- **Theme.qml** — Singleton, dark/light mode. `topicPalette`, `topicColor(index)`, `stringHash(str)` utilities.
- **MessageComposer.qml** — Dynamic placeholder ("Reply in [topic]..."), file attach, typing indicators.
- **LoginView.qml** — Logo with glow, pre-filled server URL for branded builds.

### Data Flow
- Messages stored oldest-first in `m_messages`. Index 0 = oldest, last = newest.
- Cache loads 200 newest via subquery `ORDER BY DESC LIMIT` then re-ordered ASC. API overlays fresh data.
- Thread index cached in SQLite: loaded instantly on conversation switch, then refreshed from API.
- Server history prepended at index 0. Polled messages appended at end.
- Reactions: POST to `/reaction/{token}/{messageId}`, response is array-per-emoji, converted to counts.
- Reaction system messages filtered by `systemMessage` field ("reaction", "reaction_deleted", "reaction_revoked").
- Mentions resolved via `messageParameters` JSON, wrapped in `<b style='color:#2ec4b6'>@name</b>`.
- File uploads: WebDAV PUT → share to conversation. Progress tracked via QNetworkReply::uploadProgress.

## Nextcloud Talk API Notes

- Chat history: `GET /apps/spreed/api/v1/chat/{token}?lookIntoFuture=0`
- Live poll: `GET /apps/spreed/api/v1/chat/{token}?lookIntoFuture=1&timeout=30`
- Thread filter: `GET /apps/spreed/api/v1/chat/{token}?threadId=X`
- Read receipts: `X-Chat-Last-Common-Read` response header
- Last message ID: `X-Chat-Last-Given` response header
- Join conversation: `POST /apps/spreed/api/v4/room/{token}/participants/active`
- User avatar: `GET {server}/index.php/avatar/{userId}/{size}` (non-OCS, raw image)
- Add reaction: `POST /apps/spreed/api/v1/reaction/{token}/{messageId}` with `{"reaction": "👍"}`
- Capabilities: `GET /cloud/capabilities` — version info, signaling mode, `spreed.features` for thread support
- User status: `PUT /apps/user_status/api/v1/user_status/status` with `{"statusType": "online"}`
- File upload: `PUT /remote.php/dav/files/{user}/Talk/{filename}` then `POST /apps/files_sharing/api/v1/shares`

## Packaging

- **Inno Setup** at `C:\Users\bogat\InnoSetup\ISCC.exe`
- **windeployqt** gathers Qt DLLs: `windeployqt6.exe --qmldir C:/src/talk-desktop-qt/src/qml`
- **Branded build**: `cmake -DTALQ_BRAND=123NET` (hardcoded server, dual logos)
- GitLab Generic Packages: `curl --header "PRIVATE-TOKEN: $PAT" --upload-file ... https://gitlab.123net.link/api/v4/projects/13/packages/generic/talq/{version}/{filename}`

## GitLab

- Repo: https://gitlab.123net.link/kalin/talq-desktop (private, project ID: 13)
- Packages: Generic package registry, uploaded via API
- Git credentials: stored in `~/.git-credentials`
- NC credentials: Registry `HKCU\Software\TalkQt\TalkQt\auth`
- Current release: v0.6.0

## Testers

- **Ilko** (Talk token: `ycy3ht4n`) — gets **generic** TalQ installer
- **Rakesh** (Talk token: `bv86wo4c`) — gets **123NET branded** installer
- Send via: WebDAV upload + share to Talk conversation
