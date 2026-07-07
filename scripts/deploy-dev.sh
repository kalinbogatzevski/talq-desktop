#!/bin/bash
# deploy-dev.sh — Deploy Qt + GStreamer DLLs to build dir and launch.
# Run after cmake --build. Handles the Qt/MSYS2 MinGW runtime conflict.
#
# Usage: bash scripts/deploy-dev.sh [--no-run] [--clean]

set -euo pipefail

BUILD_DIR="/c/build/talq"
# Toolchain (0.57.x): unified on the MSYS2 mingw64 stack (gcc-15 / Qt 6.10.1) —
# keep in lockstep with scripts/build-release.sh. (Was Qt 6.8.2 + mingw1310,
# which can no longer be mixed with the updated MSYS2 libs — see the 0.55.1 RCA.)
QT_DIR="/c/msys64/mingw64"
MINGW_DIR="/c/msys64/mingw64"
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
    # No --no-qml-import-scan: Qt 6.10 windeployqt rejects it and deploys nothing.
    windeployqt6.exe --release --no-translations "$BUILD_DIR/talq.exe" > /dev/null 2>&1
else
    echo "[1/4] Qt DLLs already deployed (use --clean to redo)"
fi
# Qt6Gui's D3D11/D3D12 RHI backend LoadLibrary()s D3Dcompiler_47.dll at RUNTIME
# to compile HLSL shaders, so it's invisible to any import-table check and
# windeployqt only bundles it as an undocumented side effect of whatever D3D
# compiler redistributable the Windows SDK exposes on this machine. Lockstep
# with scripts/build-release.sh: keep windeployqt's copy if present, else fall
# back to System32, else FATAL (a dev build without it renders blank/crashes
# on first D3D11/D3D12 RHI use — this is Pavel's missing-DLL report).
if [ -f "$BUILD_DIR/D3Dcompiler_47.dll" ]; then
    : # windeployqt already deployed it
elif [ -f "/c/Windows/System32/D3Dcompiler_47.dll" ]; then
    cp "/c/Windows/System32/D3Dcompiler_47.dll" "$BUILD_DIR/D3Dcompiler_47.dll"
else
    echo "FATAL: D3Dcompiler_47.dll missing — windeployqt didn't bundle it and"
    echo "       System32 has none; Qt6Gui's D3D11/D3D12 RHI backend LoadLibrary()s"
    echo "       it at runtime, so the app will render blank/crash without it."
    exit 1
fi

# Step 2: Copy all GStreamer runtime DLLs into build dir
# These are needed because talq links against GStreamer directly.
echo "[2/4] Copying GStreamer runtime DLLs..."
GST_RUNTIME_DLLS=(
    libgstreamer-1.0-0 libgstbase-1.0-0 libgstapp-1.0-0
    libgstsdp-1.0-0 libgstwebrtc-1.0-0 libgstrtp-1.0-0
    libgstpbutils-1.0-0 libgstaudio-1.0-0 libgstbadaudio-1.0-0 libgsttag-1.0-0
    libgstvideo-1.0-0 libgstnet-1.0-0 libgstsctp-1.0-0
    libgstwebrtcnice-1.0-0
    libgstd3d11-1.0-0 libgstd3dshader-1.0-0 libgstd3d12-1.0-0
    libgstcodecs-1.0-0 libgstcodecparsers-1.0-0
    libgstcuda-1.0-0 libgstdxva-1.0-0 libgstgl-1.0-0
    libglib-2.0-0 libgobject-2.0-0 libgio-2.0-0 libgmodule-2.0-0
    libintl-8 libiconv-2 libffi-8 libpcre2-8-0 libz
    libx264-165
)
for dll in "${GST_RUNTIME_DLLS[@]}"; do
    src="$MSYS2_DIR/bin/${dll}.dll"
    [ -f "$src" ] && cp "$src" "$BUILD_DIR/"
done

# Step 3: Copy GStreamer plugins
echo "[3/4] Copying GStreamer plugins..."
mkdir -p "$BUILD_DIR/gst-plugins"
# Keep in lockstep with scripts/build-release.sh and the startup gate in
# src/main.cpp. typefindfunctions+playback+audioparsers are mandatory:
# webrtcsrc builds decodebin3 internally and __fastfail-aborts the whole
# process (uncatchable) if it or its autoplug helpers are missing.
GST_PLUGINS=(
    coreelements typefindfunctions playback
    audioconvert audioresample audiomixer audioparsers opusparse autodetect audiotestsrc videotestsrc
    dtls nice opus rtp rtpmanager srtp
    wasapi wasapi2 webrtc webrtcdsp app level volume
    vpx openh264 videoconvertscale videorate sctp jpeg
    winks mediafoundation winscreencap
    d3d11 nvcodec qsv x264
    videoparsersbad
    rsrtp rswebrtc
)
for p in "${GST_PLUGINS[@]}"; do
    src="$MSYS2_DIR/lib/gstreamer-1.0/libgst${p}.dll"
    [ -f "$src" ] && cp "$src" "$BUILD_DIR/gst-plugins/"
done

# #32 — gst-libav (avdec_h264) + minimal decode-only FFmpeg for the software H264
# decode fallback (vendored under third_party/ffmpeg-min/). Lockstep with build-release.sh.
FFMIN="$(cd "$(dirname "$0")/.." && pwd)/third_party/ffmpeg-min"
if [ -f "$FFMIN/libgstlibav.dll" ]; then
    cp "$FFMIN/libgstlibav.dll" "$BUILD_DIR/gst-plugins/"
    for d in avcodec-62 avutil-60 avformat-62 avfilter-11 swscale-9 swresample-6; do
        cp "$FFMIN/$d.dll" "$BUILD_DIR/"
    done
    echo "  gst-libav avdec_h264 + minimal FFmpeg deployed"
fi

# Out-of-process plugin scanner next to talq.exe (main.cpp points
# GST_PLUGIN_SCANNER at it) — without it GStreamer scans every plugin in-process,
# ballooning the process ~450 MB per launch. Lockstep with build-release.sh.
SCANNER="$MSYS2_DIR/libexec/gstreamer-1.0/gst-plugin-scanner.exe"
[ -f "$SCANNER" ] && cp "$SCANNER" "$BUILD_DIR/"

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
    libbrotlicommon.dll libbrotlidec.dll libbrotlienc.dll \
    libwebrtc-audio-processing-1-3.dll \
    libb2-1.dll libbz2-1.dll libdouble-conversion.dll libfreetype-6.dll \
    libgraphite2.dll libharfbuzz-0.dll libmd4c.dll libpcre2-16-0.dll \
    libpng16-16.dll libsqlite3-0.dll; do
    cp "$MSYS2_DIR/bin/$dll" "$BUILD_DIR/" 2>/dev/null || true
done
# Qt 6.10.1 links ICU (uc/in/dt) — required or the app won't even start.
# Lockstep with build-release.sh; glob the soname (it bumps).
cp "$MSYS2_DIR"/bin/libicuuc*.dll "$MSYS2_DIR"/bin/libicuin*.dll \
   "$MSYS2_DIR"/bin/libicudt*.dll "$BUILD_DIR/" 2>/dev/null || true
# onnxruntime (load-time import of talq.exe) + its MSVC runtime — same as the
# release deploy; without them the dev build fails at the loader before main.
ORT_DLL="$(cd "$(dirname "$0")/.." && pwd)/third_party/onnxruntime/bin/onnxruntime.dll"
[ -f "$ORT_DLL" ] && cp "$ORT_DLL" "$BUILD_DIR/"
for vc in msvcp140.dll msvcp140_1.dll vcruntime140.dll vcruntime140_1.dll; do
    [ -f "/c/Windows/System32/$vc" ] && cp "/c/Windows/System32/$vc" "$BUILD_DIR/"
done

# Step 5: WGC single-window capture helper DLL (MSVC-built, self-contained
# /MT). Required to share ONE application window instead of a whole monitor —
# MinGW's gstd3d11 has no Windows Graphics Capture. Built via native/wgc/build.bat.
WGC_DLL="$(cd "$(dirname "$0")/.." && pwd)/native/wgc/talq_wgc.dll"
if [ -f "$WGC_DLL" ]; then
    cp "$WGC_DLL" "$BUILD_DIR/"
    echo "[wgc] talq_wgc.dll deployed (single-window capture)"
else
    echo "[wgc] WARNING: native/wgc/talq_wgc.dll missing — window sharing will"
    echo "      fall back to an error (run: cmd //c native/wgc/build.bat)"
fi

echo "Deploy complete."

if [ "$NO_RUN" = false ]; then
    echo "Launching talq..."
    powershell.exe -Command "Start-Process -FilePath 'C:\build\talq\talq.exe' -WorkingDirectory 'C:\build\talq'"
fi
