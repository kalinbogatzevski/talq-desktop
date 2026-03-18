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
  qml/
    Main.qml                  # App window, splash, navigation
    LoginView.qml             # Login screen
    ConversationList.qml      # Sidebar conversation list
    ConversationItem.qml      # Single conversation row delegate
    ChatView.qml              # Chat area with message list
    MessageBubble.qml         # Message bubble delegate
    MessageComposer.qml       # Message input area
    Theme.qml                 # Singleton with colors, spacing, animation constants
resources/
    resources.qrc             # Qt resource file
    talq.ico                  # Windows app icon
    talq.rc                   # Windows resource script for icon
cmake/
    win64-mingw-cross.cmake   # Toolchain file for Linux cross-compilation
scripts/
    package-windows.sh        # Creates distributable Windows package
```

## Key Qt6/QML Notes

- **Uppercase QML filenames** are required for `qt_add_qml_module` to register types (e.g. `Main.qml`, not `main.qml`)
- **`set_source_files_properties(... QT_QML_SINGLETON_TYPE TRUE)`** must appear BEFORE `qt_add_qml_module`
- **`QTP0001 NEW`** sets the correct `:/qt/qml/` resource prefix for module resolution
- All QML files need `import TalkQt` to access the `Theme` singleton
- The `QT_FORCE_STDERR_LOGGING=1` env var is essential for debugging — without it, GUI apps on Windows send debug output to `OutputDebugString` (invisible)

## Cross-Compilation (Linux to Windows)

See `cmake/win64-mingw-cross.cmake` and `scripts/package-windows.sh` for the cross-compile setup using mingw-w64 + aqtinstall on Linux.

## Tech Stack

- **Qt 6.8.2** (QtQuick, QML, QtNetwork, QtWebSockets)
- **C++20** with MinGW 13.1
- **Nextcloud Talk API** (OCS v2, Login Flow v2, Chat API v1/v4)
