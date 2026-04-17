# TalQ Build Guide

How to set up a machine and build TalQ from scratch. Both home and office machines must follow the same steps.

## Prerequisites

### 1. Qt 6.8.2

Download from [qt.io](https://www.qt.io/download-open-source) or use the online installer.

- Install to `C:\Qt`
- Select: **Qt 6.8.2 → MinGW 13.1.0 64-bit**
- Select: **Developer and Designer Tools → MinGW 13.1.0 64-bit, CMake, Ninja**

Expected paths after install:
```
C:\Qt\6.8.2\mingw_64\          # Qt libraries
C:\Qt\Tools\mingw1310_64\      # MinGW compiler
C:\Qt\Tools\CMake_64\bin\       # CMake
C:\Qt\Tools\Ninja\              # Ninja build system
```

### 2. MSYS2 + GStreamer

Install MSYS2 from [msys2.org](https://www.msys2.org/) to `C:\msys64`.

Open **MSYS2 MINGW64** terminal and install GStreamer:

```bash
pacman -Syu

# Core GStreamer
pacman -S mingw-w64-x86_64-gstreamer mingw-w64-x86_64-gst-plugins-base \
    mingw-w64-x86_64-gst-plugins-good mingw-w64-x86_64-gst-plugins-bad \
    mingw-w64-x86_64-gst-plugins-ugly

# WebRTC dependencies
pacman -S mingw-w64-x86_64-libnice mingw-w64-x86_64-libsrtp \
    mingw-w64-x86_64-openssl

# Video codecs
pacman -S mingw-w64-x86_64-libvpx mingw-w64-x86_64-openh264 \
    mingw-w64-x86_64-x264

# Audio codec
pacman -S mingw-w64-x86_64-opus

# NVIDIA hardware acceleration (optional — only works on NVIDIA GPU machines)
pacman -S mingw-w64-x86_64-gst-plugin-nvcodec
```

### 3. Directory Layout (junction)

Both machines use the same paths via Windows junctions:

```cmd
:: Run from an ADMIN command prompt
mklink /J C:\src\talk-desktop-qt "C:\Users\bogat\Desktop\My Projects\talk-desktop-qt"
mkdir C:\build\talq
mkdir C:\build\talq-release
mkdir C:\build\talq-123net
```

**IMPORTANT:** Create junctions from an admin prompt — admin-created junctions are trusted by Qt6/Windows.

### 4. Inno Setup (for installers)

Download from [jrsoftware.org](https://jrsoftware.org/isdl.php). Install to `C:\Users\bogat\InnoSetup\`.

### 5. Clone the repo

```bash
cd /c/src/talk-desktop-qt
git clone https://gitlab.123net.link/kalin/talq-desktop.git .
```

## Building

### Debug build (daily development)

```bash
# Configure (only needed once or after CMakeLists.txt changes)
/c/Qt/Tools/CMake_64/bin/cmake.exe -B /c/build/talq -S /c/src/talk-desktop-qt -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 \
    -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe \
    -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe

# Build
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq

# Deploy DLLs + launch
cd /c/build/talq && bash /c/src/talk-desktop-qt/scripts/deploy-dev.sh

# Deploy without launching
cd /c/build/talq && bash /c/src/talk-desktop-qt/scripts/deploy-dev.sh --no-run

# Kill running TalQ
cmd.exe //c "taskkill /IM talq.exe /F"
```

### Release installers

```bash
cd /c/src/talk-desktop-qt

# Generic installer (for Ilko)
USER=bogat bash scripts/build-release.sh

# 123NET branded installer (for Rakesh)
USER=bogat bash scripts/build-release.sh --brand 123NET
```

Output:
- `dist/TalQ-vX.Y.Z-Setup.exe` — generic installer
- `dist/123NET-TalQ-vX.Y.Z-Setup.exe` — branded installer

## Required GStreamer Plugins

These plugins must be deployed with TalQ. Both `deploy-dev.sh` and `build-release.sh` copy them automatically from MSYS2.

### Runtime DLLs (in app directory)

| DLL | Purpose |
|-----|---------|
| `libgstreamer-1.0-0.dll` | Core GStreamer |
| `libgstbase-1.0-0.dll` | Base library |
| `libgstapp-1.0-0.dll` | Appsink/appsrc |
| `libgstsdp-1.0-0.dll` | SDP parsing |
| `libgstwebrtc-1.0-0.dll` | WebRTC |
| `libgstrtp-1.0-0.dll` | RTP |
| `libgstpbutils-1.0-0.dll` | Plugin utilities |
| `libgstaudio-1.0-0.dll` | Audio support |
| `libgsttag-1.0-0.dll` | Tag/metadata |
| `libgstvideo-1.0-0.dll` | Video support |
| `libgstnet-1.0-0.dll` | Network |
| `libgstsctp-1.0-0.dll` | SCTP (data channels) |
| `libgstwebrtcnice-1.0-0.dll` | ICE/libnice |
| `libgstd3d11-1.0-0.dll` | Direct3D 11 |
| `libgstd3dshader-1.0-0.dll` | D3D shader |
| `libgstd3d12-1.0-0.dll` | Direct3D 12 (nvcodec transitive) |
| `libgstcodecs-1.0-0.dll` | Codec base (needed by d3d11/nvcodec) |
| `libgstcodecparsers-1.0-0.dll` | Codec bitstream parsers (needed by nvcodec) |
| `libgstcuda-1.0-0.dll` | CUDA runtime wrapper (needed by nvcodec) |
| `libgstdxva-1.0-0.dll` | DXVA decode (needed by d3d11) |
| `libgstgl-1.0-0.dll` | OpenGL support (needed by nvcodec) |

### GStreamer Plugins (in `gst-plugins/` subdirectory)

| Plugin | DLL | Purpose |
|--------|-----|---------|
| coreelements | `libgstcoreelements.dll` | queue, tee, valve, funnel |
| audioconvert | `libgstaudioconvert.dll` | Audio format conversion |
| audioresample | `libgstaudioresample.dll` | Audio sample rate |
| autodetect | `libgstautodetect.dll` | Auto audio/video sinks |
| audiotestsrc | `libgstaudiotestsrc.dll` | Test audio (debug) |
| videotestsrc | `libgstvideotestsrc.dll` | Dummy video source |
| dtls | `libgstdtls.dll` | DTLS encryption |
| nice | `libgstnice.dll` | ICE/STUN/TURN |
| opus | `libgstopus.dll` | Opus audio codec |
| rtp | `libgstrtp.dll` | RTP payloading |
| rtpmanager | `libgstrtpmanager.dll` | RTP session management |
| srtp | `libgstsrtp.dll` | SRTP encryption |
| wasapi | `libgstwasapi.dll` | Windows audio (fallback) |
| wasapi2 | `libgstwasapi2.dll` | Windows audio (primary) |
| webrtc | `libgstwebrtc.dll` | WebRTC bin |
| app | `libgstapp.dll` | Appsink/appsrc elements |
| level | `libgstlevel.dll` | Audio level metering |
| vpx | `libgstvpx.dll` | VP8/VP9 codec |
| openh264 | `libgstopenh264.dll` | H264 codec (software) |
| videoconvertscale | `libgstvideoconvertscale.dll` | Video convert + scale |
| sctp | `libgstsctp.dll` | SCTP data channels |
| jpeg | `libgstjpeg.dll` | JPEG images |
| winks | `libgstwinks.dll` | DirectShow camera |
| mediafoundation | `libgstmediafoundation.dll` | Media Foundation camera |
| **winscreencap** | `libgstwinscreencap.dll` | **Screen capture (dx9/gdi)** |
| **d3d11** | `libgstd3d11.dll` | **GPU hardware decode (VP8/H264 DXVA)** |
| **nvcodec** | `libgstnvcodec.dll` | **NVIDIA GPU decode/encode** |

The last 3 (bold) are the ones that were missing on the work laptop.

### Transitive DLLs (dependencies of GStreamer plugins)

```
liborc-0.4-0.dll  zlib1.dll  libnice-10.dll  libsrtp2-1.dll
libopus-0.dll  libssl-3-x64.dll  libcrypto-3-x64.dll
libjpeg-8.dll  libopenh264-7.dll  libvpx-1.dll
libgnutls-30.dll  libhogweed-6.dll  libgmp-10.dll
libidn2-0.dll  libnettle-8.dll  libp11-kit-0.dll
libtasn1-6.dll  libunistring-5.dll  libzstd.dll
libbrotlicommon.dll  libbrotlidec.dll  libbrotlienc.dll
```

## Keeping Machines in Sync

Both machines must have matching MSYS2/GStreamer packages. Mismatched DLLs cause silent audio failure, crashes, or missing features.

```bash
# Run on each machine when starting work (from MSYS2 MINGW64 terminal):
pacman -Syu

# Then re-deploy debug DLLs:
cd /c/build/talq && bash /c/src/talk-desktop-qt/scripts/deploy-dev.sh --no-run
```

## Verifying the Build

After deploying, launch TalQ and check the welcome page:

1. **GStreamer plugin pills** — all should be green. Red = missing DLL.
2. **GPU acceleration status** — should show "NVIDIA NVDEC" or "Intel DXVA". "Software only" = missing `d3d11`/`nvcodec` plugins.

If any pills are red, check both locations:
- GStreamer plugins live in `C:\msys64\mingw64\lib\gstreamer-1.0\` — copy missing `libgstXXX.dll` into `gst-plugins\` next to `talq.exe`.
- Plugin support libraries (e.g. `libgstcodecs-1.0-0.dll`, `libgstcuda-1.0-0.dll`, `libgstdxva-1.0-0.dll`, `libgstd3d12-1.0-0.dll`) live in `C:\msys64\mingw64\bin\` — copy them into the main app directory.

The `d3d11` and `nvcodec` plugins in particular each depend on several of these support DLLs and will fail silently (red pill) if any are missing.

## Publishing a Release

```bash
cd /c/src/talk-desktop-qt

# 1. Bump version in CMakeLists.txt, CHANGELOG.md, installer/*.iss
# 2. Commit, tag, push
git add -A && git commit -m "chore: bump to vX.Y.Z"
git tag vX.Y.Z && git push origin master --tags

# 3. Build installers
USER=bogat bash scripts/build-release.sh
USER=bogat bash scripts/build-release.sh --brand 123NET

# 4. Create GitLab release (token from git credentials)
TOKEN=$(git credential fill <<< "protocol=https
host=gitlab.123net.link" 2>/dev/null | grep password | cut -d= -f2)
API="https://gitlab.123net.link/api/v4"
BASE="https://gitlab.123net.link/kalin/talq-desktop"

# Create release
curl -s --header "PRIVATE-TOKEN: $TOKEN" \
  --header "Content-Type: application/json" \
  --data '{"tag_name":"vX.Y.Z","name":"vX.Y.Z","description":"..."}' \
  "$API/projects/13/releases"

# Upload + link each installer
for f in dist/*vX.Y.Z*Setup.exe; do
  PATH_JSON=$(curl -s --header "PRIVATE-TOKEN: $TOKEN" \
    --form "file=@$f" "$API/projects/13/uploads" | python3 -c "import sys,json; print(json.load(sys.stdin)['full_path'])")
  NAME=$(basename "$f")
  curl -s --header "PRIVATE-TOKEN: $TOKEN" \
    --header "Content-Type: application/json" \
    --data "{\"name\":\"$NAME\",\"url\":\"${BASE}${PATH_JSON}\",\"link_type\":\"package\"}" \
    "$API/projects/13/releases/vX.Y.Z/assets/links"
done
```
