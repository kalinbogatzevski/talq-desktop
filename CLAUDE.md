# TalQ - Project Context for Claude Code

## Build Setup

**Qt dev env**: Qt 6.8.2 + MinGW 13.1 installed via `aqtinstall` at `C:\Qt`.

**IMPORTANT**: Build path must be ASCII only (e.g. `C:\Projects\talk-desktop-qt`). Qt's `qmlimportscanner` breaks on paths with Cyrillic/special characters (like OneDrive Desktop folders).

**Build commands** (Git Bash):
```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/c/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build build
```

**Run** (needs Qt DLLs in PATH):
```bash
export PATH="/c/Qt/6.8.2/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH"
QT_FORCE_STDERR_LOGGING=1 ./build/talk-qt.exe
```

**Kill before rebuild**: `taskkill.exe //IM talk-qt.exe //F`

## Key Pitfalls (hard-won lessons)

1. **Uppercase QML filenames** — `qt_add_qml_module` only registers types for files starting with uppercase (e.g. `Main.qml` not `main.qml`)
2. **Singleton declaration order** — `set_source_files_properties(... QT_QML_SINGLETON_TYPE TRUE)` MUST appear BEFORE `qt_add_qml_module` in CMakeLists.txt
3. **`import TalkQt`** — All QML files need this to access the Theme singleton; same-module auto-import doesn't work for singletons
4. **`QTP0001 NEW`** — Required for correct `:/qt/qml/` resource prefix
5. **MessagePoller::stop()** — Must null `m_currentReply` BEFORE calling `abort()`, because abort triggers `finished` synchronously which nulls the pointer via handlePollResponse
6. **GUI debug output** — Windows GUI apps need `QT_FORCE_STDERR_LOGGING=1` env var; without it, Qt messages go to OutputDebugString (invisible)
7. **Async callbacks** — Always capture the conversation token and guard against stale callbacks when switching conversations

## Architecture

- **ApiClient** — HTTP client wrapping Nextcloud OCS API with Basic auth
- **AuthManager** — Login Flow v2 (browser-based), session persistence via QSettings
- **MessagePoller** — Long-polls Talk chat API for live messages, emits read receipt headers
- **MessageListModel** — QAbstractListModel with history loading, live polling, read tracking
- **ConversationListModel** — QAbstractListModel for sidebar
- **Theme.qml** — Singleton with all design tokens (colors, spacing, animation durations)

## Nextcloud Talk API Notes

- Chat history: `GET /apps/spreed/api/v1/chat/{token}?lookIntoFuture=0`
- Live poll: `GET /apps/spreed/api/v1/chat/{token}?lookIntoFuture=1&timeout=30`
- Read receipts: `X-Chat-Last-Common-Read` response header
- Last message ID: `X-Chat-Last-Given` response header
- Join conversation: `POST /apps/spreed/api/v4/room/{token}/participants/active`

## GitLab

- Repo: https://gitlab.123net.link/kalin/talk-desktop-qt (private)
- Packages: Generic package registry, uploaded via API with `api`-scoped token
- Git credentials: stored in `~/.git-credentials`
