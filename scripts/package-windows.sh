#!/bin/bash
# Package Talk Qt for Windows distribution
# Creates a self-contained directory with .exe + all DLLs + QML modules

set -e

QT_WIN="${HOME}/qt6-win/6.8.2/mingw_64"
BUILD_DIR="build-win64"
PACKAGE_DIR="dist/TalkQt-Windows-x64"
MINGW_BIN="/usr/x86_64-w64-mingw32/lib"

echo "=== Packaging Talk Qt for Windows ==="

# Clean and create package dir
rm -rf "${PACKAGE_DIR}"
mkdir -p "${PACKAGE_DIR}"

# Copy main executable
cp "${BUILD_DIR}/talk-qt.exe" "${PACKAGE_DIR}/"

# Copy Qt DLLs
for dll in Qt6Core Qt6Gui Qt6Network Qt6Qml Qt6QmlModels Qt6Quick Qt6QuickControls2 \
           Qt6QuickTemplates2 Qt6QuickLayouts Qt6WebSockets Qt6OpenGL Qt6Svg; do
    if [ -f "${QT_WIN}/bin/${dll}.dll" ]; then
        cp "${QT_WIN}/bin/${dll}.dll" "${PACKAGE_DIR}/"
    fi
done

# Copy platform plugin
mkdir -p "${PACKAGE_DIR}/platforms"
cp "${QT_WIN}/plugins/platforms/qwindows.dll" "${PACKAGE_DIR}/platforms/"

# Copy QML modules
mkdir -p "${PACKAGE_DIR}/qml"
for mod in QtQuick QtQml QtQuick/Controls QtQuick/Layouts QtQuick/Templates; do
    src="${QT_WIN}/qml/${mod}"
    if [ -d "${src}" ]; then
        mkdir -p "${PACKAGE_DIR}/qml/${mod}"
        cp -r "${src}/"* "${PACKAGE_DIR}/qml/${mod}/"
    fi
done

# Copy style plugin
if [ -d "${QT_WIN}/plugins/styles" ]; then
    mkdir -p "${PACKAGE_DIR}/styles"
    cp "${QT_WIN}/plugins/styles/"*.dll "${PACKAGE_DIR}/styles/" 2>/dev/null || true
fi

# Copy TLS plugin for HTTPS
if [ -d "${QT_WIN}/plugins/tls" ]; then
    mkdir -p "${PACKAGE_DIR}/tls"
    cp "${QT_WIN}/plugins/tls/"*.dll "${PACKAGE_DIR}/tls/"
fi

# Copy mingw runtime DLLs
MINGW_GCC_DIR=$(dirname $(x86_64-w64-mingw32-g++-posix -print-file-name=libstdc++-6.dll))
for dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    if [ -f "${MINGW_GCC_DIR}/${dll}" ]; then
        cp "${MINGW_GCC_DIR}/${dll}" "${PACKAGE_DIR}/"
    fi
done

# Create qt.conf to tell Qt where to find plugins
cat > "${PACKAGE_DIR}/qt.conf" << 'CONF'
[Paths]
Plugins = .
Qml2Imports = qml
CONF

echo ""
echo "=== Package contents ==="
find "${PACKAGE_DIR}" -name "*.dll" -o -name "*.exe" | wc -l
echo "files in ${PACKAGE_DIR}"
du -sh "${PACKAGE_DIR}"
echo ""
echo "Done! Transfer ${PACKAGE_DIR}/ to a Windows machine to run."
