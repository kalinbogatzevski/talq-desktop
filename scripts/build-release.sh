#!/bin/bash
# TalQ Release Build Script
# Usage: bash scripts/build-release.sh [--brand 123NET]
#
# Prerequisites:
#   - Qt 6.8.2 installed at C:/Qt/6.8.2/mingw_64
#   - MSYS2 at C:/msys64 with GStreamer packages
#   - Inno Setup at C:/Users/$USER/InnoSetup/ISCC.exe
#   - GStreamer plugins deployed in a debug build (gst-plugins dir)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VERSION=$(grep 'project(talq VERSION' "$SRC_DIR/CMakeLists.txt" | sed 's/.*VERSION \([^ ]*\).*/\1/')

BRAND=""
BRAND_FLAG=""
DIST_SUFFIX=""
INSTALLER_ISS="talq-setup.iss"

if [ "$1" = "--brand" ] && [ "$2" = "123NET" ]; then
    BRAND="123NET"
    BRAND_FLAG="-DTALQ_BRAND=123NET"
    DIST_SUFFIX="-123net"
    INSTALLER_ISS="123net-talk-setup.iss"
    echo "=== Building 123NET branded release ==="
else
    echo "=== Building generic release ==="
fi

BUILD_DIR="/c/build/talq-release${DIST_SUFFIX}"
DIST_DIR="$SRC_DIR/dist/TalQ-v${VERSION}-win64${DIST_SUFFIX}"

# Tool paths
CMAKE="/c/Qt/Tools/CMake_64/bin/cmake.exe"
WINDEPLOYQT="/c/Qt/6.8.2/mingw_64/bin/windeployqt6.exe"
# Inno Setup lives under the *home* directory, which isn't reliably
# reflected in $USER/$USERNAME on this setup (git-bash leaves USER empty,
# USERNAME sometimes points at a different account). $HOME is the
# trustworthy one.
ISCC="$HOME/InnoSetup/ISCC.exe"
NINJA="/c/Qt/Tools/Ninja"
MINGW="/c/Qt/Tools/mingw1310_64/bin"
MSYS2="/c/msys64/mingw64/bin"
export PATH="$NINJA:$MINGW:$PATH"

echo "[1/6] Configuring..."
rm -rf "$BUILD_DIR"
"$CMAKE" -B "$BUILD_DIR" -S "$SRC_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 \
    -DCMAKE_C_COMPILER="$MINGW/gcc.exe" \
    -DCMAKE_CXX_COMPILER="$MINGW/g++.exe" \
    $BRAND_FLAG 2>&1 | tail -1

echo "[2/6] Building..."
"$CMAKE" --build "$BUILD_DIR" --target talq 2>&1 | tail -1

echo "[3/6] Deploying Qt DLLs..."
cd "$BUILD_DIR"
"$WINDEPLOYQT" --qmldir "$SRC_DIR" talq.exe 2>&1 | tail -1

echo "[4/6] Copying MSYS2 + GStreamer DLLs..."
for dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll \
    liborc-0.4-0.dll zlib1.dll libiconv-2.dll \
    libgstreamer-1.0-0.dll libgstbase-1.0-0.dll libgstapp-1.0-0.dll \
    libgstaudio-1.0-0.dll libgstvideo-1.0-0.dll libgstpbutils-1.0-0.dll \
    libgstrtp-1.0-0.dll libgstsdp-1.0-0.dll libgstwebrtc-1.0-0.dll \
    libgstrtsp-1.0-0.dll libgsttag-1.0-0.dll \
    libgobject-2.0-0.dll libglib-2.0-0.dll libgio-2.0-0.dll \
    libgmodule-2.0-0.dll libintl-8.dll libffi-8.dll libpcre2-8-0.dll \
    libgstnet-1.0-0.dll libgstsctp-1.0-0.dll libgstwebrtcnice-1.0-0.dll \
    libgstd3d11-1.0-0.dll libgstd3dshader-1.0-0.dll libgstd3d12-1.0-0.dll \
    libgstcodecs-1.0-0.dll libgstcodecparsers-1.0-0.dll \
    libgstcuda-1.0-0.dll libgstdxva-1.0-0.dll libgstgl-1.0-0.dll \
    libnice-10.dll libsrtp2-1.dll libopus-0.dll \
    libssl-3-x64.dll libcrypto-3-x64.dll \
    libjpeg-8.dll libopenh264-7.dll libvpx-1.dll \
    libgnutls-30.dll libhogweed-6.dll libgmp-10.dll \
    libidn2-0.dll libnettle-8.dll libp11-kit-0.dll \
    libtasn1-6.dll libunistring-5.dll libzstd.dll \
    libbrotlicommon.dll libbrotlidec.dll libbrotlienc.dll; do
    cp "$MSYS2/$dll" . 2>/dev/null || true
done

# GStreamer plugins (copy directly from MSYS2)
mkdir -p gst-plugins
for p in coreelements audioconvert audioresample autodetect audiotestsrc videotestsrc \
    dtls nice opus rtp rtpmanager srtp \
    wasapi wasapi2 webrtc app level \
    vpx openh264 videoconvertscale sctp jpeg \
    winks mediafoundation winscreencap \
    d3d11 nvcodec; do
    src="$MSYS2/../lib/gstreamer-1.0/libgst${p}.dll"
    [ -f "$src" ] && cp "$src" gst-plugins/
done

echo "[5/6] Packaging dist..."
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"
# Copy everything, then remove cmake artifacts
cp -r "$BUILD_DIR"/* "$DIST_DIR/" 2>&1
rm -rf "$DIST_DIR/CMakeFiles" "$DIST_DIR/cmake_install.cmake" \
    "$DIST_DIR/CMakeCache.txt" "$DIST_DIR/build.ninja" \
    "$DIST_DIR/.ninja"* "$DIST_DIR/talq_autogen" \
    "$DIST_DIR/talq-call-test_autogen" "$DIST_DIR/.qt"

echo "[6/6] Building installer..."
if [ -f "$ISCC" ]; then
    "$ISCC" "$SRC_DIR/installer/$INSTALLER_ISS" 2>&1 | tail -2
else
    echo "Inno Setup not found at $ISCC — skipping installer"
fi

echo ""
echo "=== Release complete ==="
echo "Dist: $DIST_DIR"
ls -lh "$SRC_DIR/dist/"*"v${VERSION}"*Setup* 2>/dev/null || echo "(no installer built)"

# ── [7/7] Auto-upload to ncloud update channel (optional) ──
# Credentials: either export NC_APP_PASSWORD inline, or store it once in
#   ~/.talq-release.env  (shell syntax: NC_APP_PASSWORD=xxxx-xxxx-xxxx-xxxx)
# The dotfile is sourced if present; an already-exported env var wins.
if [ -z "${NC_APP_PASSWORD:-}" ] && [ -f "$HOME/.talq-release.env" ]; then
    # shellcheck disable=SC1090
    . "$HOME/.talq-release.env"
fi

NC_USER="kalin"
NC_FOLDER="https://ncloud.123net.link/remote.php/dav/files/${NC_USER}/TalQ-updates"

if [ -n "${NC_APP_PASSWORD:-}" ]; then
    echo "[7/7] Uploading to ncloud update channel..."
    GEN_INSTALLER="$SRC_DIR/dist/TalQ-v${VERSION}-Setup.exe"
    BRAND_INSTALLER="$SRC_DIR/dist/123NET-TalQ-v${VERSION}-Setup.exe"

    if [ -f "$GEN_INSTALLER" ] && [ -f "$BRAND_INSTALLER" ]; then
        GEN_SHA=$(sha256sum "$GEN_INSTALLER" | awk '{print $1}')
        BRAND_SHA=$(sha256sum "$BRAND_INSTALLER" | awk '{print $1}')

        NOTES=$(awk '/^## v/ { if (found) exit; found=1; next } found { print }' \
                "$SRC_DIR/CHANGELOG.md" | head -c 500 | tr '\n' ' ' | sed 's/"/\\"/g')

        cat > /tmp/talq-latest.json <<MANIFEST
{
  "version": "${VERSION}",
  "releaseDate": "$(date +%Y-%m-%d)",
  "notes": "${NOTES}",
  "assets": {
    "generic": "TalQ-v${VERSION}-Setup.exe",
    "123net":  "123NET-TalQ-v${VERSION}-Setup.exe"
  },
  "sha256": {
    "generic": "${GEN_SHA}",
    "123net":  "${BRAND_SHA}"
  }
}
MANIFEST

        curl -sS -u "${NC_USER}:${NC_APP_PASSWORD}" -T "$GEN_INSTALLER" \
            "${NC_FOLDER}/TalQ-v${VERSION}-Setup.exe" >/dev/null
        curl -sS -u "${NC_USER}:${NC_APP_PASSWORD}" -T "$BRAND_INSTALLER" \
            "${NC_FOLDER}/123NET-TalQ-v${VERSION}-Setup.exe" >/dev/null
        curl -sS -u "${NC_USER}:${NC_APP_PASSWORD}" -T /tmp/talq-latest.json \
            "${NC_FOLDER}/talq-latest.json" >/dev/null

        echo "  uploaded v${VERSION} generic + 123NET + manifest to ncloud"
    else
        echo "  skipped manifest push: need both generic + 123NET installers in dist"
    fi
else
    echo "[7/7] Skipped ncloud upload (NC_APP_PASSWORD not set)"
fi
