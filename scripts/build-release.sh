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

# 123NET branding lives outside the tree (/private, never published). For a
# branded build, restore the wizard art + installer script into the tree
# (gitignored names, so this can't dirty git) and stamp them to $VERSION,
# so the rest of the pipeline and the .iss relative paths work unchanged.
# The branded logo is pulled straight from /private by CMake (brand.qrc).
if [ "$BRAND" = "123NET" ]; then
    PRIV="$SRC_DIR/private/branding/123net"
    if [ ! -d "$PRIV" ]; then
        echo "ERROR: $PRIV not found — the 123NET branding store is private."
        echo "Build without --brand for the generic/public app."
        exit 1
    fi
    cp "$PRIV/123net-wizard.bmp"       "$SRC_DIR/resources/"
    cp "$PRIV/123net-wizard-small.bmp" "$SRC_DIR/resources/"
    cp "$PRIV/123net-talk-setup.iss"   "$SRC_DIR/installer/"
    ISS="$SRC_DIR/installer/123net-talk-setup.iss"
    sed -i \
        -e "s/^AppVersion=.*/AppVersion=${VERSION}/" \
        -e "s/123NET-TalQ-v[0-9.]*-Setup/123NET-TalQ-v${VERSION}-Setup/g" \
        -e "s/TalQ-v[0-9.]*-win64-123net/TalQ-v${VERSION}-win64-123net/g" \
        "$ISS"
fi

BUILD_DIR="/c/build/talq-release${DIST_SUFFIX}"
DIST_DIR="$SRC_DIR/dist/TalQ-v${VERSION}-win64${DIST_SUFFIX}"

# Version consistency guard (generic build): talq-setup.iss / talq-update.iss
# carry hardcoded versions (AppVersion, the packaged dist path, and the
# OutputBaseFilename). CMakeLists.txt is the single source of truth; if the
# .iss files drift, the installer silently packages the wrong dist folder or
# emits a misnamed exe. Branded builds stamp their .iss from /private, so this
# guard only applies to the generic path. tr -d '\r': the .iss are CRLF.
if [ -z "$BRAND" ]; then
    for iss in talq-setup.iss talq-update.iss; do
        iss_ver=$(grep -m1 '^AppVersion=' "$SRC_DIR/installer/$iss" \
                  | cut -d= -f2 | tr -d '\r')
        if [ "$iss_ver" != "$VERSION" ]; then
            echo "FATAL: installer/$iss is AppVersion=$iss_ver but CMakeLists.txt"
            echo "       is $VERSION. Bump installer/$iss (AppVersion, the dist"
            echo "       path, and OutputBaseFilename) to $VERSION and retry."
            exit 1
        fi
    done
fi

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
QTBIN="/c/Qt/6.8.2/mingw_64/bin"
# Qt bin MUST be on PATH: windeployqt6.exe is itself a Qt app and cannot
# even start without Qt6Core on PATH. Omitting it made windeployqt fail
# silently and shipped installers with no Qt runtime (app wouldn't launch).
export PATH="$NINJA:$MINGW:$QTBIN:$PATH"

# ccache: caches object files across clean rebuilds. Output is bit-identical
# on cache hit; cold first build is unaffected. Skip silently if not installed.
CCACHE_LAUNCHER=""
if [ -x "$MSYS2/ccache.exe" ]; then
    CCACHE_LAUNCHER="-DCMAKE_C_COMPILER_LAUNCHER=$MSYS2/ccache.exe -DCMAKE_CXX_COMPILER_LAUNCHER=$MSYS2/ccache.exe"
fi

echo "[1/6] Configuring..."
rm -rf "$BUILD_DIR"
"$CMAKE" -B "$BUILD_DIR" -S "$SRC_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 \
    -DCMAKE_C_COMPILER="$MINGW/gcc.exe" \
    -DCMAKE_CXX_COMPILER="$MINGW/g++.exe" \
    $CCACHE_LAUNCHER \
    $BRAND_FLAG 2>&1 | tail -1

echo "[2/6] Building..."
"$CMAKE" --build "$BUILD_DIR" --target talq 2>&1 | tail -1

echo "[3/6] Deploying Qt DLLs..."
cd "$BUILD_DIR"
# NB: no --no-qml-import-scan — Qt 6.8 windeployqt rejects that option
# and aborts deploying nothing (this is what shipped Qt-less installers).
"$WINDEPLOYQT" --release --no-translations talq.exe 2>&1 | tail -1
# Fail loudly: a Qt-less dist produces an installer whose app cannot start
# ("no Qt platform plugin"). These must exist after windeployqt.
for must in Qt6Core.dll Qt6Widgets.dll Qt6Network.dll platforms/qwindows.dll; do
    if [ ! -f "$BUILD_DIR/$must" ]; then
        echo "FATAL: windeployqt did not produce $must — Qt runtime missing."
        echo "       (Is $QTBIN on PATH? Is windeployqt6.exe present there?)"
        exit 1
    fi
done
# TalQ uses Qt6Sql with SQLite only. windeployqt copies every SQL driver;
# the unused ones (psql/odbc/mimer/mysql) only drag in third-party client
# libs (libpq/odbc32/mimapi) that aren't shipped, bloating the installer
# and tripping dependency checks. Keep just qsqlite.
if [ -d "$BUILD_DIR/sqldrivers" ]; then
    find "$BUILD_DIR/sqldrivers" -name 'qsql*.dll' ! -name 'qsqlite.dll' -delete
fi
# Drop ~40 MB of windeployqt over-deploy that TalQ never uses:
#  - opengl32sw.dll: software-GL fallback; TalQ is a QPainter raster app,
#    nothing import-links or loads it.
#  - The Qt Multimedia FFmpeg backend (ffmpegmediaplugin + av*/sw*):
#    TalQ only uses QVideoSink/QVideoFrame (frames fed from GStreamer);
#    it has no QMediaPlayer/QSoundEffect/QCamera. windowsmediaplugin.dll
#    is kept so Qt Multimedia still has a (tiny, native) backend present.
#  - generic/qtuiotouchplugin.dll: TUIO touch input, unused on desktop.
rm -f "$BUILD_DIR/opengl32sw.dll"
rm -f "$BUILD_DIR"/avcodec-*.dll "$BUILD_DIR"/avformat-*.dll \
      "$BUILD_DIR"/avutil-*.dll "$BUILD_DIR"/swscale-*.dll \
      "$BUILD_DIR"/swresample-*.dll
rm -f "$BUILD_DIR/multimedia/ffmpegmediaplugin.dll"
rm -f "$BUILD_DIR/generic/qtuiotouchplugin.dll"
rmdir "$BUILD_DIR/generic" 2>/dev/null || true

echo "[4/6] Copying MSYS2 + GStreamer DLLs..."
for dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll \
    liborc-0.4-0.dll zlib1.dll libiconv-2.dll \
    libgstreamer-1.0-0.dll libgstbase-1.0-0.dll libgstapp-1.0-0.dll \
    libgstaudio-1.0-0.dll libgstbadaudio-1.0-0.dll libgstvideo-1.0-0.dll libgstpbutils-1.0-0.dll \
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
    libbrotlicommon.dll libbrotlidec.dll libbrotlienc.dll \
    libwebrtc-audio-processing-1-3.dll; do
    cp "$MSYS2/$dll" . 2>/dev/null || true
done

# GStreamer plugins (copy directly from MSYS2)
mkdir -p gst-plugins
for p in coreelements audioconvert audioresample autodetect audiotestsrc videotestsrc \
    dtls nice opus rtp rtpmanager srtp \
    wasapi wasapi2 webrtc webrtcdsp app level \
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

echo "[6/6] Building installer(s)..."
if [ -f "$ISCC" ]; then
    "$ISCC" "$SRC_DIR/installer/$INSTALLER_ISS" 2>&1 | tail -2
    # Slim "update" installer: ships only talq.exe over an existing full
    # install (point-release upgrade path). Generic-only — the branded
    # channel auto-updates via ncloud with the full installer, so a slim
    # updater there would be dead weight.
    if [ -z "$BRAND" ]; then
        "$ISCC" "$SRC_DIR/installer/talq-update.iss" 2>&1 | tail -2
    fi
else
    echo "Inno Setup not found at $ISCC — skipping installer"
fi

echo ""
echo "=== Release complete ==="
echo "Dist: $DIST_DIR"
ls -lh "$SRC_DIR/dist/"*"v${VERSION}"-*.exe 2>/dev/null || echo "(no installer built)"

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

        # Latest version's section, verbatim (keeps markdown line structure:
        # headings, blank lines, bullet lists). The client renders this with
        # setMarkdown(), so flattening newlines here would collapse it into
        # one run-on paragraph. Build the JSON with python so newlines and
        # quotes are escaped correctly and the notes aren't truncated.
        NOTES=$(awk '/^## v/ { if (found) exit; found=1; next } found { print }' \
                "$SRC_DIR/CHANGELOG.md")

        NOTES="$NOTES" \
        M_VERSION="$VERSION" \
        M_DATE="$(date +%Y-%m-%d)" \
        M_GEN="TalQ-v${VERSION}-Setup.exe" \
        M_BRAND="123NET-TalQ-v${VERSION}-Setup.exe" \
        M_GEN_SHA="$GEN_SHA" \
        M_BRAND_SHA="$BRAND_SHA" \
        M_OUT="/tmp/talq-latest.json" \
        python - <<'PY'
import json, os
m = {
    "version":     os.environ["M_VERSION"],
    "releaseDate": os.environ["M_DATE"],
    "notes":       os.environ["NOTES"].strip(),
    "assets": {
        "generic": os.environ["M_GEN"],
        "123net":  os.environ["M_BRAND"],
    },
    "sha256": {
        "generic": os.environ["M_GEN_SHA"],
        "123net":  os.environ["M_BRAND_SHA"],
    },
}
# Write UTF-8 directly: the CHANGELOG has non-Latin-1 chars (arrows,
# em-dashes) and Windows' redirected-stdout encoding would crash on them.
with open(os.environ["M_OUT"], "w", encoding="utf-8") as fh:
    json.dump(m, fh, indent=2, ensure_ascii=False)
PY

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
