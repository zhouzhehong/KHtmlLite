cmake_minimum_required(VERSION 3.16)
project(KHtmlLiteHost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# --- Configurable paths -------------------------------------------------------
# KHTML source tree (the khtml-5.116.0 source, not the build dir).
set(KHTML_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../khtml/src" CACHE PATH
    "Path to KHTML 5.116 source directory (src/ subfolder)")
# KHTML build output directory (where libKF5KHtml.dll / .dll.a are produced).
set(KHTML_BUILD_DIR "${CMAKE_BINARY_DIR}/../khtml" CACHE PATH
    "Path to KHTML build directory (contains lib/ and src/ generated headers)")
# Full path to the KHTML import library.
set(KHTML_LIB "${KHTML_BUILD_DIR}/lib/libKF5KHtml.dll.a" CACHE FILEPATH
    "Path to libKF5KHtml.dll.a (or .lib on MSVC)")
# QuickJS header directory (quickjs.h lives here).
set(QUICKJS_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../quickjs" CACHE PATH
    "Path to QuickJS headers (quickjs.h, quickjs-atom.h)")

find_package(Qt5 REQUIRED COMPONENTS Widgets Gui Network)
find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
find_package(KF5 REQUIRED COMPONENTS
    Parts I18n KIO XmlGui WidgetsAddons TextWidgets IconThemes
    Codecs Archive Notifications Sonnet Wallet WindowSystem GlobalAccel)

file(GLOB KHTML_SUBDIRS LIST_DIRECTORIES true "${KHTML_SOURCE_DIR}/*")
# LIST_DIRECTORIES true returns regular files as well as directories. Every
# entry ends up in KHTML_HOST_INCLUDES, so files were passed to the compiler
# as include directories and cc1plus emitted
#   ".../src/<file>: not a directory"
# for each of them. Keep only the real subdirectories.
set(KHTML_SUBDIR_DIRS "")
foreach(_khtml_subdir IN LISTS KHTML_SUBDIRS)
    if(IS_DIRECTORY "${_khtml_subdir}")
        list(APPEND KHTML_SUBDIR_DIRS "${_khtml_subdir}")
    endif()
endforeach()
set(KHTML_SUBDIRS ${KHTML_SUBDIR_DIRS})

set(KHTML_HOST_INCLUDES
    ${KHTML_SOURCE_DIR}
    ${KHTML_BUILD_DIR}/src
    ${KHTML_SUBDIRS}
    ${QUICKJS_INCLUDE_DIR}
)

set(KHTML_HOST_LIBS
    ${KHTML_LIB}
    Qt5::Widgets Qt5::Gui Qt5::Network
    KF5::Parts KF5::I18n KF5::KIOCore KF5::XmlGui
    KF5::WidgetsAddons KF5::TextWidgets KF5::IconThemes KF5::Codecs
    KF5::Archive KF5::Notifications KF5::SonnetCore KF5::SonnetUi
    KF5::Wallet
)

set(KHTML_HOST_COMPILE_OPTS
    -UQT_NO_KEYWORDS -UQT_NO_FOREACH -UQT_NO_SIGNALS_SLOTS_KEYWORDS
    -UQT_NO_CAST_FROM_ASCII -UQT_NO_CAST_TO_ASCII -UQT_NO_CAST_FROM_BYTEARRAY
    -UQT_NO_URL_CAST_FROM_STRING -UQT_NO_NARROWING_CONVERSIONS_IN_CONNECT)

# --- Browser shell ------------------------------------------------------------
add_executable(khtml_browser WIN32
    khtml_browser.cpp
    pageloader.cpp
    js_transpile.cpp
)
target_include_directories(khtml_browser PRIVATE ${KHTML_HOST_INCLUDES})
target_link_libraries(khtml_browser PRIVATE ${KHTML_HOST_LIBS})
target_compile_options(khtml_browser PRIVATE ${KHTML_HOST_COMPILE_OPTS})

# --- Renderer process (out-of-process, one per tab) --------------------------
add_executable(khtml_renderer WIN32
    khtml_renderer.cpp
    pageloader.cpp
    js_transpile.cpp
)
target_include_directories(khtml_renderer PRIVATE ${KHTML_HOST_INCLUDES})
target_link_libraries(khtml_renderer PRIVATE ${KHTML_HOST_LIBS} dbghelp)
target_compile_options(khtml_renderer PRIVATE ${KHTML_HOST_COMPILE_OPTS})

# Stage the project-built KHTML engine DLL next to the host executables.
# Without this, Windows falls back to PATH lookup and may load the
# MSYS2-packaged libKF5KHtml.dll (same soname, different build/ABI), which
# makes the renderer die at startup with 0xC0000139 (entry point not found).
# The application directory is always searched first, so a local copy is
# authoritative.
if(WIN32)
    set(_khtml_runtime_dll "${KHTML_BUILD_DIR}/bin/libKF5KHtml.dll")
    foreach(_t khtml_browser khtml_renderer)
        add_custom_command(TARGET ${_t} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_khtml_runtime_dll}" "$<TARGET_FILE_DIR:${_t}>/libKF5KHtml.dll"
            VERBATIM)
    endforeach()
endif()

# --- Optional diagnostic / probe tools ----------------------------------------
option(BUILD_PROBE_TOOLS "Build network / CSS / smoke probe tools" OFF)
if(BUILD_PROBE_TOOLS)
    add_executable(khtml_smoke khtml_smoke.cpp)
    target_include_directories(khtml_smoke PRIVATE ${KHTML_HOST_INCLUDES})
    target_link_libraries(khtml_smoke PRIVATE ${KHTML_HOST_LIBS})
    target_compile_options(khtml_smoke PRIVATE ${KHTML_HOST_COMPILE_OPTS})

    add_executable(khtml_netprobe khtml_netprobe.cpp)
    target_include_directories(khtml_netprobe PRIVATE ${KHTML_HOST_INCLUDES})
    target_link_libraries(khtml_netprobe PRIVATE ${KHTML_HOST_LIBS})
    target_compile_options(khtml_netprobe PRIVATE ${KHTML_HOST_COMPILE_OPTS})

    add_executable(khtml_qnam khtml_qnam.cpp)
    target_include_directories(khtml_qnam PRIVATE ${KHTML_HOST_INCLUDES})
    target_link_libraries(khtml_qnam PRIVATE ${KHTML_HOST_LIBS})
    target_compile_options(khtml_qnam PRIVATE ${KHTML_HOST_COMPILE_OPTS})

    add_executable(khtml_csscap khtml_csscap.cpp)
    target_include_directories(khtml_csscap PRIVATE ${KHTML_HOST_INCLUDES})
    target_link_libraries(khtml_csscap PRIVATE ${KHTML_HOST_LIBS})
    target_compile_options(khtml_csscap PRIVATE ${KHTML_HOST_COMPILE_OPTS})

    add_executable(kio_job_test kio_job_test.cpp)
    target_link_libraries(kio_job_test PRIVATE Qt5::Core Qt5::Network KF5::KIOCore KF5::I18n)
endif()
