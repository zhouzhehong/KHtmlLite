cmake_minimum_required(VERSION 3.16)
project(KHtmlLite VERSION 0.1.0 LANGUAGES CXX)

# KHtmlLite — a minimal browser engine based on KHTML 5.116 with QuickJS.
#
# Build order:
#   1. Build QuickJS (src/quickjs) and install headers + static lib.
#   2. Build KHTML engine (src/khtml) → produces libKF5KHtml.dll.
#   3. Build browser + renderer (src/host) → links against libKF5KHtml.
#
# This top-level file is provided for IDE indexing and convenience.
# The recommended build flow is described in README.md and scripts/build.sh.

option(BUILD_KHTML_ENGINE "Build the KHTML engine (libKF5KHtml)" ON)
option(BUILD_HOST "Build browser shell and renderer" ON)
option(BUILD_PROBE_TOOLS "Build diagnostic probe tools" OFF)

if(BUILD_KHTML_ENGINE)
    # KHTML is a KDE Frameworks component with its own CMake system.
    # It requires ECM, Qt5, and KF5 dependencies. We add it as a subdirectory
    # so that targets are visible, but it can also be built standalone.
    add_subdirectory(src/khtml)
endif()

if(BUILD_HOST)
    # The host (browser + renderer) needs the KHTML build output.
    # When built via this top-level CMake, point it at the in-tree build dir.
    set(KHTML_BUILD_DIR "${CMAKE_BINARY_DIR}/src/khtml" CACHE PATH "" FORCE)
    set(KHTML_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/khtml/src" CACHE PATH "" FORCE)
    set(KHTML_LIB "${CMAKE_BINARY_DIR}/src/khtml/lib/libKF5KHtml.dll.a" CACHE FILEPATH "" FORCE)
    add_subdirectory(src/host)
endif()
