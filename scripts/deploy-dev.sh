#!/bin/bash
# deploy-dev.sh — Deploy Qt + GStreamer DLLs to build dir and launch.
# Run after cmake --build. Handles the Qt/MSYS2 MinGW runtime conflict.
#
# Usage: bash scripts/deploy-dev.sh [--no-run] [--clean]

set -euo pipefail

BUILD_DIR="/c/build/talq"
QT_DIR="/c/Qt/6.8.2/mingw_64"
MINGW_DIR="/c/Qt/Tools/mingw1310_64"
MSYS2_DIR="/c/msys64/mingw64"

NO_RUN=false
CLEAN=false
for arg in "$@"; do
    case "$arg" in
        --no-run) NO_RUN=true ;;
        --clean)  CLEAN=true ;;
    esac
done

if [ ! -f "$BUILD_DIR/talq.exe" ]; then
    echo "ERROR: $BUILD_DIR/talq.exe not found. Build first."
    exit 1
fi

export PATH="$MINGW_DIR/bin:$QT_DIR/bin:$MSYS2_DIR/bin:$PATH"

# Step 1: windeployqt (Qt DLLs)
if [ "$CLEAN" = true ] || [ ! -f "$BUILD_DIR/Qt6Core.dll" ]; then
    echo "[1/4] Running windeployqt..."
    windeployqt6.exe --no-qml-import-scan "$BUILD_DIR/talq.exe" > /dev/null 2>&1
else
    echo "[1/4] Qt DLLs already deployed (use --clean to redo)"
fi

# Step 2: Copy all GStreamer runtime DLLs into build dir
# These are needed because talq links against GStreamer directly.
echo "[2/4] Copying GStreamer runtime DLLs..."
GST_RUNTIME_DLLS=(
    libgstreamer-1.0-0 libgstbase-1.0-0 libgstapp-1.0-0
    libgstsdp-1.0-0 libgstwebrtc-1.0-0 libgstrtp-1.0-0
    libgstpbutils-1.0-0 libgstaudio-1.0-0 libgsttag-1.0-0
    libgstvideo-1.0-0 libgstnet-1.0-0 libgstsctp-1.0-0
    libgstwebrtcnice-1.0-0
    libgstd3d11-1.0-0 libgstd3dshader-1.0-0 libgstd3d12-1.0-0
    libgstcodecs-1.0-0 libgstcodecparsers-1.0-0
    libgstcuda-1.0-0 libgstdxva-1.0-0 libgstgl-1.0-0
    libglib-2.0-0 libgobject-2.0-0 libgio-2.0-0 libgmodule-2.0-0
    libintl-8 libiconv-2 libffi-8 libpcre2-8-0 libz
)
for dll in "${GST_RUNTIME_DLLS[@]}"; do
    src="$MSYS2_DIR/bin/${dll}.dll"
    [ -f "$src" ] && cp "$src" "$BUILD_DIR/"
done

# Step 3: Copy GStreamer plugins
echo "[3/4] Copying GStreamer plugins..."
mkdir -p "$BUILD_DIR/gst-plugins"
GST_PLUGINS=(
    coreelements audioconvert audioresample autodetect audiotestsrc videotestsrc
    dtls nice opus rtp rtpmanager srtp
    wasapi wasapi2 webrtc app level
    vpx openh264 videoconvertscale sctp jpeg
    winks mediafoundation winscreencap
    d3d11 nvcodec
)
for p in "${GST_PLUGINS[@]}"; do
    src="$MSYS2_DIR/lib/gstreamer-1.0/libgst${p}.dll"
    [ -f "$src" ] && cp "$src" "$BUILD_DIR/gst-plugins/"
done

# Step 4: Copy MSYS2's MinGW runtime DLLs (overwrite any Qt copies)
# Both Qt and GStreamer DLLs work with MSYS2's libstdc++ (tested).
# Qt's bundled MinGW 13.1 libstdc++ causes 0xC0000139 (Entry Point Not Found).
echo "[4/4] Copying MSYS2 MinGW runtime DLLs..."
cp "$MSYS2_DIR/bin/libstdc++-6.dll"       "$BUILD_DIR/"
cp "$MSYS2_DIR/bin/libgcc_s_seh-1.dll"    "$BUILD_DIR/"
cp "$MSYS2_DIR/bin/libwinpthread-1.dll"   "$BUILD_DIR/"
# Transitive deps needed by GStreamer plugins
for dll in liborc-0.4-0.dll zlib1.dll \
    libnice-10.dll libsrtp2-1.dll libopus-0.dll \
    libssl-3-x64.dll libcrypto-3-x64.dll \
    libjpeg-8.dll libopenh264-7.dll libvpx-1.dll \
    libgnutls-30.dll libhogweed-6.dll libgmp-10.dll \
    libidn2-0.dll libnettle-8.dll libp11-kit-0.dll \
    libtasn1-6.dll libunistring-5.dll libzstd.dll \
    libbrotlicommon.dll libbrotlidec.dll libbrotlienc.dll; do
    cp "$MSYS2_DIR/bin/$dll" "$BUILD_DIR/" 2>/dev/null || true
done

echo "Deploy complete."

if [ "$NO_RUN" = false ]; then
    echo "Launching talq..."
    powershell.exe -Command "Start-Process -FilePath 'C:\build\talq\talq.exe' -WorkingDirectory 'C:\build\talq'"
fi
