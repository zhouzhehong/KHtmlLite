#!/bin/bash
# KHtmlLite build script for Windows / MSYS2 MinGW-w64
# Usage: bash scripts/build.sh [build_type]
#   build_type: Release (default) or Debug

set -e

BUILD_TYPE="${1:-Release}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${ROOT_DIR}/build"

echo "=== KHtmlLite Build ==="
echo "Source: ${ROOT_DIR}"
echo "Build:  ${BUILD_DIR}"
echo "Type:   ${BUILD_TYPE}"
echo ""

# --- Step 1: Build QuickJS ---
echo "[1/3] Building QuickJS..."
cd "${ROOT_DIR}/src/quickjs"
make -j"$(nproc)" 2>/dev/null || true

# Install QuickJS headers and lib to MSYS2 prefix if not present
if [ ! -f /mingw64/include/quickjs/quickjs.h ]; then
    echo "  Installing QuickJS headers..."
    mkdir -p /mingw64/include/quickjs
    cp quickjs.h quickjs-atom.h /mingw64/include/quickjs/
fi
if [ ! -f /mingw64/lib/quickjs/libquickjs.a ]; then
    echo "  Installing QuickJS library..."
    mkdir -p /mingw64/lib/quickjs
    cp libquickjs.a /mingw64/lib/quickjs/
fi
echo "  QuickJS ready."

# --- Step 2: Build KHTML engine ---
echo "[2/3] Building KHTML engine..."
mkdir -p "${BUILD_DIR}/khtml"
cd "${BUILD_DIR}/khtml"
cmake "${ROOT_DIR}/src/khtml" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_INSTALL_PREFIX=/mingw64 \
    -DBUILD_TESTING=OFF \
    -DKHTML_BUILD_TESTS=OFF
ninja KF5KHtml
echo "  KHTML engine built."

# --- Step 3: Build browser + renderer ---
echo "[3/3] Building browser + renderer..."
mkdir -p "${BUILD_DIR}/host"
cd "${BUILD_DIR}/host"
cmake "${ROOT_DIR}/src/host" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DKHTML_SOURCE_DIR="${ROOT_DIR}/src/khtml/src" \
    -DKHTML_BUILD_DIR="${BUILD_DIR}/khtml" \
    -DKHTML_LIB="${BUILD_DIR}/khtml/lib/libKF5KHtml.dll.a"
ninja khtml_browser khtml_renderer
echo "  Browser + renderer built."

echo ""
echo "=== Build complete ==="
echo "Output: ${BUILD_DIR}/host/"
ls -la khtml_browser.exe khtml_renderer.exe 2>/dev/null || true
