# TalQ - Project Context for Claude Code

## Build Setup

**Qt dev env**: Qt 6.8.2 + MinGW 13.1 installed via `aqtinstall` at `C:\Qt`.

**IMPORTANT**: Source must be accessed via a junction without spaces. The project lives at `My Projects\talk-desktop-qt` but Qt tools break on paths with spaces. Use junction: `C:\src\talk-desktop-qt` → actual path.

**Build commands** (Git Bash):
```bash
# Kill existing, clean build, launch
taskkill.exe //IM talk-qt.exe //F 2>/dev/null; sleep 3
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
rm -rf /c/build/talk-qt
cmake -B C:/build/talk-qt -S C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build C:/build/talk-qt

# Run
export PATH="/c/Qt/6.8.2/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH"
QT_FORCE_STDERR_LOGGING=1 /c/build/talk-qt/talk-qt.exe
```

**Junction setup** (if not exists):
```bash
echo 'mklink /J C:\src\talk-desktop-qt "C:\Users\bogat\Desktop\My Projects\talk-desktop-qt"' > /tmp/mkj.bat && cmd //c /tmp/mkj.bat
```

**Kill before rebuild**: `taskkill.exe //IM talk-qt.exe //F` — MUST wait 3-5s after kill before linking, or get "Permission denied"

**QML changes not detected?** The junction can cause stale build detection. Fix: `rm -rf /c/build/talk-qt` and do a clean build.

## Key Pitfalls (hard-won lessons)

1. **Uppercase QML filenames** — `qt_add_qml_module` only registers types for files starting with uppercase
2. **Singleton declaration order** — `set_source_files_properties(... QT_QML_SINGLETON_TYPE TRUE)` MUST appear BEFORE `qt_add_qml_module`
3. **`import TalkQt`** — All QML files need this for Theme singleton
4. **`QTP0001 NEW`** — Required for correct `:/qt/qml/` resource prefix
5. **MessagePoller::stop()** — Must null `m_currentReply` BEFORE calling `abort()` (abort triggers finished synchronously)
6. **GUI debug output** — `QT_FORCE_STDERR_LOGGING=1` env var required, otherwise Qt messages go to OutputDebugString
7. **Async callbacks** — Always capture conversation token and guard against stale callbacks
8. **Never use `displayMarginBeginning/End`** on ListView — causes content to render outside bounds, overlapping adjacent items
9. **Never use anchors across sibling subtrees** — causes "Cannot anchor to an item that isn't a parent or sibling" which silently corrupts the entire layout chain
10. **Use `Page` with `header`/`footer`** for chat layout — Page manages sizing between header and footer automatically. ColumnLayout with BottomToTop fails.
11. **TopToBottom + `onCountChanged: positionViewAtEnd()`** — simpler and more reliable than BottomToTop. Store messages oldest-first.
12. **DPI-aware screenshots** — call `ctypes.windll.shcore.SetProcessDpiAwareness(2)` before `GetWindowRect` or `ImageGrab`

## Architecture

### C++ Backend
- **ApiClient** — HTTP client wrapping Nextcloud OCS API with Basic auth. `getAbsoluteUrl()` for non-OCS endpoints (avatars)
- **AuthManager** — Login Flow v2, session persistence via QSettings, server capabilities fetch
- **MessagePoller** — Long-polls chat API, emits `messagesReceived`, `lastCommonReadChanged`, `pollSuccess`, `pollError`
- **MessageListModel** — QAbstractListModel, oldest-first storage, optimistic send, reactions, reply-to, retry on failure
- **MessageCache** — SQLite cache at `AppData/TalQ/TalQ/message_cache.db`. Newest 50 messages loaded on conversation switch.
- **AvatarProvider** — QQuickAsyncImageProvider, 3-tier cache (memory→disk→server), circular crop in C++
- **ConversationListModel** — QAbstractListModel for sidebar, includes `name` field for 1:1 user IDs

### QML Frontend
- **ChatView.qml** — `Page` with header/footer. ListView TopToBottom, `onCountChanged: positionViewAtEnd()`
- **MessageBubble.qml** — Hybrid layout: flat for others (avatar+name+text), teal bubble for own. Context popup with emoji reactions + actions.
- **ConversationList.qml** — Sidebar with avatar, search, theme toggle, refresh, logout
- **Theme.qml** — Singleton, dark/light mode via `darkMode` property. Warm teal accent `#2ec4b6`
- **LoginView.qml** — Logo with glow, pre-filled server URL

### Data Flow
- Messages stored oldest-first in `m_messages`. Index 0 = oldest, last = newest.
- Cache loads 50 newest via subquery `ORDER BY DESC LIMIT` then re-ordered ASC.
- Server history prepended at index 0. Polled messages appended at end.
- Reactions: POST to `/reaction/{token}/{messageId}`, response is array-per-emoji, converted to counts.
- Reaction system messages filtered by `systemMessage` field ("reaction", "reaction_deleted", "reaction_revoked").
- Mentions resolved via `messageParameters` JSON, wrapped in `<b style='color:#2ec4b6'>@name</b>`.

## Nextcloud Talk API Notes

- Chat history: `GET /apps/spreed/api/v1/chat/{token}?lookIntoFuture=0`
- Live poll: `GET /apps/spreed/api/v1/chat/{token}?lookIntoFuture=1&timeout=30`
- Read receipts: `X-Chat-Last-Common-Read` response header
- Last message ID: `X-Chat-Last-Given` response header
- Join conversation: `POST /apps/spreed/api/v4/room/{token}/participants/active`
- User avatar: `GET {server}/index.php/avatar/{userId}/{size}` (non-OCS, raw image)
- Add reaction: `POST /apps/spreed/api/v1/reaction/{token}/{messageId}` with `{"reaction": "👍"}`
- Capabilities: `GET /cloud/capabilities` — version info, signaling mode
- Threads (v22+): `GET /chat/{token}?threadId=X`, `GET /chat/{token}/threads/{threadId}`

## Packaging

- **Inno Setup** at `C:\Users\bogat\InnoSetup\ISCC.exe` — builds `dist/TalQ-v0.3.0-Setup.exe`
- **windeployqt** gathers Qt DLLs to `dist/TalQ-v0.3.0-win64/`
- **Qt IFW** at `C:\Qt\Tools\QtInstallerFramework\4.7\bin\binarycreator.exe` — alternative installer
- GitLab Generic Packages: `curl --upload-file ... https://gitlab.123net.link/api/v4/projects/13/packages/generic/talq/0.3.0/filename`

## GitLab

- Repo: https://gitlab.123net.link/kalin/talk-desktop-qt (private, project ID: 13)
- Packages: Generic package registry, uploaded via API
- Git credentials: stored in `~/.git-credentials`
- Current release: v0.3.0

## Next: Threads/Topics Feature

Research completed — see `docs/research/2026-03-19-talk-threads-research.md`. Talk v22+ has native thread API. Plan: display threads inline like Telegram Topics within group chats.
