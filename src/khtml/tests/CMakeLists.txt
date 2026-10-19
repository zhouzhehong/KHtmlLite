include(ECMMarkAsTest)

if(NOT WIN32 AND NOT APPLE AND HAVE_X11)
  find_package(Qt5X11Extras "${REQUIRED_QT_VERSION}" CONFIG REQUIRED)
  set_package_properties(Qt5X11Extras PROPERTIES PURPOSE "Required for tests")
endif()

include_directories(
  ${KHTML_SOURCE_DIR}
  ${KHTML_SOURCE_DIR}/
  ${KHTML_SOURCE_DIR}/misc
  ${KHTML_SOURCE_DIR}/dom
  ${KHTML_SOURCE_DIR}/xml
  ${KHTML_SOURCE_DIR}/html
  ${KHTML_SOURCE_DIR}/rendering
  ${KHTML_SOURCE_DIR}/ecma
  ${KHTML_SOURCE_DIR}/imload
  ${KHTML_SOURCE_DIR}/imload/decoders
  ${KHTML_SOURCE_DIR}/svg
  ${KHTML_SOURCE_DIR}/svg/graphics
  ${KHTML_SOURCE_DIR}/platform/graphics
  ${KHTML_SOURCE_DIR}/platform/text
  ${KHTML_SOURCE_DIR}/platform
  ${KHTML_SOURCE_DIR}/compat
  ${KHTML_SOURCE_DIR}/css
)

include_directories(
  ${KHTML_BINARY_DIR}/..
  ${KHTML_BINARY_DIR}/css
)

# msvc linker doesn't like "#define protected public"
if(NOT MSVC)

    ########### next target ###############
    set(khtmltest_SRCS
        testkhtmlnew.cpp
    )

    qt_add_resources( khtmltest_SRCS testkhtml.qrc )
    add_executable(khtmltest ${khtmltest_SRCS} )
    ecm_mark_as_test(khtmltest)
    target_link_libraries( khtmltest KF5::KHtml KF5::XmlGui KF5::TextWidgets KF5::Parts )

    ########### next target ###############
endif()

#######################################

# msvc linker doesn't like "#define protected public"
if(NOT MSVC)

    ########### next target ###############

    set(testkhtml_SRCS
        testkhtml.cpp
    )


    add_executable(testkhtml ${testkhtml_SRCS})
    ecm_mark_as_test(testkhtml)
    target_link_libraries(testkhtml KF5::KHtml KF5::XmlGui KF5::TextWidgets KF5::Parts)

    ########### next target ###############

    set(testkhtml_static_SRCS
        testkhtml.cpp
    )


    add_executable(testkhtml_static ${testkhtml_static_SRCS} )
    ecm_mark_as_test(testkhtml_static)
    target_link_libraries(testkhtml_static KF5::KHtml KF5::XmlGui KF5::TextWidgets KF5::Parts)

    ########### next target ###############
endif()

# Note: the upstream testregression suite (test_regression.cpp) was removed
# with the script interpreter — it was built only against the script bindings
# (the deleted binding headers) and could never compile in a
# QuickJS-only tree.  The GUI variant below is engine-agnostic and stays.
if (KHTML_BUILD_TESTREGRESSION AND HAVE_X11)

    ########### next target ###############

    set(testregressiongui_SRCS
        test_regression_gui_window.cpp
        test_regression_gui_main.cpp
    )

    qt_add_resources(testregressiongui_SRCS test_regression_gui.qrc)

    ki18n_wrap_ui(testregressiongui_SRCS test_regression_gui.ui)


    add_executable(testregressiongui ${testregressiongui_SRCS})
    ecm_mark_as_test(testregressiongui)
    target_link_libraries(testregressiongui KF5::KHtml)
endif ()

