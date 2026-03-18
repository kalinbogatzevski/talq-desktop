# CMake toolchain file for cross-compiling to Windows x64 from Linux
# Uses mingw-w64 compiler + Qt6 Windows binaries from aqtinstall

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Cross-compiler
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# Qt6 Windows installation path (from aqtinstall)
set(QT_WIN_ROOT "$ENV{HOME}/qt6-win/6.8.2/mingw_64")

# Tell CMake where to find Qt target libraries
set(CMAKE_PREFIX_PATH "${QT_WIN_ROOT}")

# Host Qt path — tells Qt to use Linux-native moc, rcc, qmlcachegen, etc.
# instead of trying to run the Windows .exe versions
set(QT_HOST_PATH "/usr")
set(QT_HOST_PATH_CMAKE_DIR "/usr/lib/x86_64-linux-gnu/cmake")

# Search paths — only search cross-compile sysroot for libraries
set(CMAKE_FIND_ROOT_PATH "${QT_WIN_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
