# KHtmlLite

Minimal browser engine based on **KHTML 5.116** (KDE HTML rendering engine) with **QuickJS 2026-06-04** as the JavaScript engine.

## Overview

KHtmlLite is a lightweight, experimental web browser built on the classic KHTML rendering engine. It uses a multi-process architecture with a browser shell and out-of-process renderer per tab.

- **Rendering engine**: KHTML 5.116 (KDE Frameworks)
- **JavaScript engine**: QuickJS 2026-06-04 (ES2020 support), with legacy KJS as fallback
- **Platform**: Windows x64
- **Toolchain**: MSYS2 + GCC (C++20)
- **UI framework**: Qt 5 + KDE Frameworks 5

## Current Status

**Active development. Not production-ready.**

This project is under active development. Modern website compatibility is limited and there are known stability issues. The goal is to provide a clean, auditable codebase for a minimal browser engine.

## Repository Structure

```
KHtmlLite/
├── src/
│   ├── khtml/          # KHTML 5.116 engine source (preserves upstream structure)
│   │   ├── src/
│   │   │   ├── css/        # CSS parser and selector engine
│   │   │   ├── dom/        # DOM implementation
│   │   │   ├── ecma/       # JavaScript bindings (KJS + QuickJS bridge)
│   │   │   ├── html/       # HTML parser and elements
│   │   │   ├── rendering/  # Render tree and layout
│   │   │   ├── svg/        # SVG support
│   │   │   └── ...
│   ├── quickjs/        # QuickJS 2026-06-04 source
│   └── host/           # Browser shell, renderer process, page loader
│       ├── khtml_browser.cpp
│       ├── khtml_renderer.cpp
│       ├── pageloader.cpp / .h
│       └── ...
├── scripts/            # Build and utility scripts
├── CMakeLists.txt      # Top-level CMake
├── README.md
├── LICENSE
└── .gitignore
```

## Build

### Prerequisites (Windows / MSYS2)

- MSYS2 with MinGW-w64 GCC
- Qt 5 (mingw-w64-x86_64-qt5)
- KDE Frameworks 5 (KF5) packages
- ECM (extra-cmake-modules)
- CMake ≥ 3.16
- Ninja

### Build Steps

```bash
# 1. Build QuickJS and install to MSYS2 prefix
cd src/quickjs
make
# Copy quickjs.h to /mingw64/include/quickjs/
# Copy libquickjs.a to /mingw64/lib/quickjs/

# 2. Build KHTML engine
mkdir -p build/khtml && cd build/khtml
cmake ../../src/khtml -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/mingw64
ninja KF5KHtml

# 3. Build browser + renderer
cd ../..
mkdir -p build/host && cd build/host
cmake ../../src/host -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DKHTML_SOURCE_DIR=../../src/khtml/src \
    -DKHTML_BUILD_DIR=../khtml \
    -DKHTML_LIB=../khtml/lib/libKF5KHtml.dll.a
ninja
```

Alternatively, use the provided build script:

```bash
bash scripts/build.sh
```

## Architecture

- **khtml_browser.exe**: Main browser shell (tab management, address bar, window embedding)
- **khtml_renderer.exe**: Out-of-process renderer (one per tab). Crashes are isolated.
- **libKF5KHtml.dll**: KHTML rendering engine with QuickJS DOM bridge
- **PageLoader**: Handles URL navigation, resource loading, and final URL tracking

## JavaScript

QuickJS is the primary JavaScript engine for both page scripts and DOM events. Legacy KJS remains as a fallback path. The DOM bridge (`src/khtml/src/ecma/qjs_dom_bridge.cpp`) provides QuickJS wrappers for DOM nodes, styles, and events.

## License

KHTML is licensed under the GNU LGPL v2.1+. See `LICENSE` and `src/khtml/COPYING.LIB`.

QuickJS is licensed under the MIT license. See `src/quickjs/LICENSE`.

## Disclaimer

This is experimental software. Do not use for production browsing. Modern web standards support is incomplete.
