# =============================================================================
# KHTML - Modernized Build System
# Original: 5.116.0 (KDE Frameworks 5, May 2024)
# Changes: CMake 3.16→3.28, C++17→C++20, Qt version abstraction layer,
#          modern compile definitions, IPO support, generator expressions
# =============================================================================

cmake_minimum_required(VERSION 3.28)

set(KF_VERSION "5.116.0") # handled by release scripts
set(KF_DEP_VERSION "5.116.0") # handled by release scripts

project(KHtml
    VERSION ${KF_VERSION}
    LANGUAGES CXX
)

# --- C++ Standard: C++20 (was implicit C++17 via KF5) ---
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Interprocedural Optimization (opt-in, off by default) ---
# GCC turns IPO into -flto=auto, which under MinGW collides with ld's
# --export-all-symbols ("symbol wrong type" on std::swap specializations) and
# multiplies link-time memory. Both are fatal on small machines, so LTO stays
# opt-in: configure with -DKHTML_ENABLE_IPO=ON where the toolchain allows it.
option(KHTML_ENABLE_IPO "Enable interprocedural/LTO optimization" OFF)
if(KHTML_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _khtml_ipo_supported)
    if(_khtml_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()
endif()

# --- Export compile commands for clangd / IDE support ---
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FeatureSummary)

find_package(ECM 5.116.0 NO_MODULE)
set_package_properties(ECM PROPERTIES
    TYPE REQUIRED
    DESCRIPTION "Extra CMake Modules."
    URL "https://commits.kde.org/extra-cmake-modules"
)
feature_summary(WHAT REQUIRED_PACKAGES_NOT_FOUND FATAL_ON_MISSING_REQUIRED_PACKAGES)

set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(GenerateExportHeader)
include(CMakePackageConfigHelpers)
include(ECMSetupVersion)
include(ECMGenerateHeaders)
include(ECMQtDeclareLoggingCategory)
include(ECMAddQch)

# =============================================================================
# Qt Version Abstraction Layer
# -----------------------------------------------------------------------------
# Currently locked to Qt5 (KF5 dependency chain requires it).
# For a future Qt6/KF6 port: set QT_VERSION_MAJOR to 6 and adjust
# the find_package calls below. Note: KHTML was removed from KF6,
# so a full Qt6 port requires replacing KF5JS, KF5Parts, etc.
# =============================================================================
set(QT_VERSION_MAJOR 5 CACHE STRING "Qt major version (5 or 6)")
set(REQUIRED_QT_VERSION 5.15.2)

find_package(Qt${QT_VERSION_MAJOR} "${REQUIRED_QT_VERSION}" CONFIG REQUIRED
    Widgets
    Network
    DBus
    PrintSupport
    Xml
)

# X11Extras was removed in Qt6; only find it on Qt5
if(QT_VERSION_MAJOR EQUAL 5)
    find_package(Qt5X11Extras ${REQUIRED_QT_VERSION} CONFIG)
endif()

include(KDEInstallDirs)
# Use the base compiler settings (warnings/visibility/exceptions) rather
# than KDEFrameworkCompilerSettings, which forces QT_NO_CAST_FROM_ASCII and
# QT_NO_KEYWORDS. This legacy Qt5 tree still relies on those conversions
# and the signals/slots/emit keywords throughout.
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDECMakeSettings)

# Most of KJS doesn't even require Qt, thus we can't use override.
# Applied at target level instead of polluting global CMAKE_CXX_FLAGS.
string(REPLACE "-Wsuggest-override" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

# --- KDE Frameworks 5 Dependencies ---
find_package(KF5Archive ${KF_DEP_VERSION} REQUIRED)
find_package(KF5Codecs ${KF_DEP_VERSION} REQUIRED)
find_package(KF5I18n ${KF_DEP_VERSION} REQUIRED)
find_package(KF5IconThemes ${KF_DEP_VERSION} REQUIRED)
find_package(KF5KIO ${KF_DEP_VERSION} REQUIRED)
# KJS stopped shipping at 5.115 in the MSYS2 Qt5 compatibility set; the
# engine is frozen, so the older package is API-compatible with this tree.
find_package(KF5JS 5.115.0 REQUIRED)
find_package(KF5Notifications ${KF_DEP_VERSION} REQUIRED)
find_package(KF5Parts ${KF_DEP_VERSION} REQUIRED)
find_package(KF5Sonnet ${KF_DEP_VERSION} REQUIRED)
find_package(KF5TextWidgets ${KF_DEP_VERSION} REQUIRED)
find_package(KF5Wallet ${KF_DEP_VERSION} REQUIRED)
find_package(KF5WidgetsAddons ${KF_DEP_VERSION} REQUIRED)
find_package(KF5WindowSystem ${KF_DEP_VERSION} REQUIRED)
find_package(KF5XmlGui ${KF_DEP_VERSION} REQUIRED)

if(NOT WIN32 AND NOT APPLE)
    find_package(KF5GlobalAccel ${KF_DEP_VERSION} REQUIRED)
endif()
set(HAVE_KGLOBALACCEL ${KF5GlobalAccel_FOUND})

ecm_setup_version(PROJECT
    VARIABLE_PREFIX KHTML
    VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/khtml_version.h"
    PACKAGE_VERSION_FILE "${CMAKE_CURRENT_BINARY_DIR}/KF5KHtmlConfigVersion.cmake"
    SOVERSION 5
)

# --- Perl (required for JS binding generation) ---
find_package(Perl)
set_package_properties(Perl PROPERTIES
    URL "https://www.perl.org/"
    TYPE REQUIRED
    PURPOSE "Required for generating JS bindings for elements"
)

# --- X11 (Linux only) ---
if(NOT WIN32 AND NOT APPLE)
    option(WITH_X11 "Build with X11 integration" ON)
    if(WITH_X11)
        find_package(X11 REQUIRED)
        set(HAVE_X11 1)
    endif()
endif()

# --- Phonon (HTML5 multimedia) ---
find_package(Phonon4Qt5 4.6.60)
set_package_properties(Phonon4Qt5 PROPERTIES
    DESCRIPTION "Qt Multimedia Library"
    URL "https://phonon.kde.org/"
    TYPE REQUIRED
    PURPOSE "Required for HTML5 multimedia elements"
)

# --- Image decoding libraries ---
find_package(JPEG)
set_package_properties(JPEG PROPERTIES
    DESCRIPTION "JPEG decoding library"
    URL "https://www.ijg.org"
    TYPE REQUIRED
    PURPOSE "Required for decoding and displaying JPEG images"
)
find_package(GIF)
set_package_properties(GIF PROPERTIES
    DESCRIPTION "GIF decoding library"
    URL "https://sourceforge.net/projects/giflib"
    TYPE REQUIRED
    PURPOSE "Required for decoding and displaying GIF images"
)
find_package(PNG)
set_package_properties(PNG PROPERTIES
    DESCRIPTION "PNG decoding library"
    URL "http://www.libpng.org/pub/png"
    TYPE REQUIRED
    PURPOSE "Required for decoding and displaying PNG images"
)

find_package(Gperf REQUIRED)

# =============================================================================
# Modern Compile Definitions (replaces add_definitions)
# =============================================================================
add_compile_definitions(
    # The legacy engine still uses the signals/slots/emit keywords and a
    # number of ASCII QString/QChar shorthands, so the strict QT_NO_*
    # keyword guards are intentionally left off for this Qt5-only build.
    # This Qt5-era engine still relies on APIs that were only marked
    # deprecated in 5.15 itself (QLinkedList, ...), so do not compile any
    # deprecated API out. A value of 0x050F00 would hide QLinkedList because
    # QT_DEPRECATED_SINCE(5,15) uses a strict-greater comparison.
    QT_DISABLE_DEPRECATED_BEFORE=0
    # KHTML feature flags
    ENABLE_SVG
    ENABLE_SVG_FONTS
    WTF_PLATFORM_QT
    # Translation domain
    TRANSLATION_DOMAIN="khtml5"
)

# =============================================================================
# Compiler Warning Flags (modern generator expressions)
# =============================================================================
add_compile_options(
    # Avoid warning for "this use of defined may not be portable"
    -Wno-expansion-to-defined
    # KF5 frameworks emit many deprecation warnings; suppress during transition
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-deprecated-declarations>
    # KJS portion doesn't use override
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-suggest-override>
)

set(KHTML_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(KHTML_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/src)

ki18n_install(po)

add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(tests)
    add_subdirectory(autotests)
endif()

# =============================================================================
# CMake Package Config Installation
# =============================================================================
set(CMAKECONFIG_INSTALL_DIR "${KDE_INSTALL_CMAKEPACKAGEDIR}/KF5KHtml")

include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/KF5KHtmlConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/KF5KHtmlConfig.cmake"
    INSTALL_DESTINATION ${CMAKECONFIG_INSTALL_DIR}
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/KF5KHtmlConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/KF5KHtmlConfigVersion.cmake"
    DESTINATION "${CMAKECONFIG_INSTALL_DIR}"
    COMPONENT Devel
)

install(EXPORT KF5KHtmlTargets
    DESTINATION "${CMAKECONFIG_INSTALL_DIR}"
    FILE KF5KHtmlTargets.cmake
    NAMESPACE KF5::
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/khtml_version.h
    DESTINATION ${KDE_INSTALL_INCLUDEDIR_KF5}/KHtml
    COMPONENT Devel
)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
