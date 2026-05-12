# TalQ - Nextcloud Talk Desktop Client

A native Qt6/QML desktop client for Nextcloud Talk with a Telegram-inspired dark UI.

## Features

- Login Flow v2 (browser-based auth, no embedded browser)
- Conversation list with unread badges, favorites, search
- Chat messaging with message bubbles, replies, reactions
- Read receipts (✓ sent, ✓✓ read)
- Date separators
- Long-polling for live message updates
- Dark theme with smooth animations
- Session persistence (auto-login on restart)

## Development Environment Setup (Windows)

### Prerequisites

- **Python 3.10+** (for `aqtinstall`)
- **Git**

### 1. Install Qt 6.8.2 + Build Tools

Open a terminal (PowerShell, cmd, or Git Bash) and run:

```bash
pip install aqtinstall

# Qt 6.8.2 with MinGW + WebSockets module
python -m aqt install-qt windows desktop 6.8.2 win64_mingw --outputdir C:\Qt -m qtwebsockets

# MinGW 13.1 compiler
python -m aqt install-tool windows desktop tools_mingw1310 --outputdir C:\Qt

# CMake
python -m aqt install-tool windows desktop tools_cmake --outputdir C:\Qt

# Ninja build system
python -m aqt install-tool windows desktop tools_ninja --outputdir C:\Qt
```

This installs everything to `C:\Qt`. Total size ~2GB.

After installation you should have:
```
C:\Qt\6.8.2\mingw_64\       # Qt libraries
C:\Qt\Tools\mingw1310_64\   # MinGW compiler (g++)
C:\Qt\Tools\CMake_64\       # CMake
C:\Qt\Tools\Ninja\          # Ninja
```

### 2. Clone the Repository

```bash
git clone https://gitlab.123net.link/kalin/talk-desktop-qt.git C:\Projects\talk-desktop-qt
cd C:\Projects\talk-desktop-qt
```

**IMPORTANT**: Use a path without special/Cyrillic characters. Qt's `qmlimportscanner` breaks on non-ASCII paths (e.g. OneDrive Desktop folders).

### 3. Configure and Build

```bash
# Set PATH (do this in every new terminal, or add to your profile)
set PATH=C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;%PATH%

# Configure
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev

# Build
cmake --build build
```

For Git Bash, use forward slashes:
```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/c/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build build
```

### 4. Run

The app needs Qt DLLs in PATH to find its runtime libraries:

```bash
# Git Bash
export PATH="/c/Qt/6.8.2/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH"
./build/talk-qt.exe

# PowerShell
$env:PATH = "C:\Qt\6.8.2\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;$env:PATH"
.\build\talk-qt.exe
```

To see debug output (Qt messages, QML errors):
```bash
QT_FORCE_STDERR_LOGGING=1 ./build/talk-qt.exe
```

### 5. Rebuild After Changes

```bash
cmake --build build
```

Only changed files are recompiled. If you add/remove source files, re-run the configure step too.

## Project Structure

```
src/
  main.cpp                    # Entry point
  core/
    ApiClient.cpp/.h          # HTTP client for Nextcloud OCS API
    AuthManager.cpp/.h        # Login Flow v2 authentication
    MessagePoller.cpp/.h      # Long-poll for new messages
  models/
    Conversation.cpp/.h       # Conversation data model
    Message.cpp/.h            # Message data model
    ConversationListModel.cpp/.h  # QAbstractListModel for conversations
    MessageListModel.cpp/.h       # QAbstractListModel for messages
  painter/
    ChatPainter.cpp/.h        # QPainter-based message list rendering
    SidebarPainter.cpp/.h     # Conversation sidebar rendering
    HeaderPainter.cpp/.h      # Chat header
    LayoutEngine.cpp/.h       # Message layout computation
  ui/
    MainWindow.cpp/.h         # QMainWindow shell
    ComposerWidget.cpp/.h     # Message input area
    LoginWidget.cpp/.h        # Login screen
    SettingsDialog.cpp/.h     # Settings tabs
    CallDialog.cpp/.h         # In-call window
resources/
    resources.qrc             # Qt resource file
    talq.ico                  # Windows app icon
    talq.rc                   # Windows resource script for icon
scripts/
    build-release.sh          # Production build + installer + ncloud upload
    deploy-dev.sh             # Dev build: Qt + GStreamer DLL deploy
```

## Notes

- The `QT_FORCE_STDERR_LOGGING=1` env var is essential for debugging — without it, GUI apps on Windows send debug output to `OutputDebugString` (invisible).
- The UI is rendered with QPainter on QWidget (no QML / QtQuick). Earlier versions used QML but it was replaced for performance and rendering control.

## Tech Stack

- **Qt 6.8.2 Widgets** (QtNetwork, QtWebSockets, QtMultimedia, QtSvg)
- **C++20** with MinGW 13.1
- **GStreamer** for WebRTC calls
- **Nextcloud Talk API** (OCS v2, Login Flow v2, Chat API v1/v4)
