
include_directories( ${KHTML_SOURCE_DIR}
                     ${KHTML_BINARY_DIR}
)
include_directories(${ZLIB_INCLUDE_DIR})

########### next target ###############

set(kmultipart_PART_SRCS kmultipart.cpp httpfiltergzip.cpp)
ecm_qt_declare_logging_category(kmultipart_PART_SRCS
    HEADER kmultipart_debug.h
    IDENTIFIER KMULTIPART_LOG
    CATEGORY_NAME kf.khtml.multipart
    OLD_CATEGORY_NAMES kf5.khtml.multipart
    DESCRIPTION "multipart"
    EXPORT KHTML
)
add_library(kmultipart MODULE ${kmultipart_PART_SRCS})

target_link_libraries(kmultipart
                        ${ZLIB_LIBRARY}
                        KF5::Parts
                        KF5::Archive
                        KF5::WidgetsAddons
                        KF5::XmlGui
                        KF5::I18n
                        KF5::TextWidgets)
# KF5 dependency targets propagate QT_NO_* strict source guards; this Qt5-era
# code relies on the classic conversions, so cancel them on this module too.
target_compile_options(kmultipart PRIVATE
    -UQT_NO_KEYWORDS
    -UQT_NO_FOREACH
    -UQT_NO_SIGNALS_SLOTS_KEYWORDS
    -UQT_NO_CAST_FROM_ASCII
    -UQT_NO_CAST_TO_ASCII
    -UQT_NO_CAST_FROM_BYTEARRAY
    -UQT_NO_URL_CAST_FROM_STRING
    -UQT_NO_NARROWING_CONVERSIONS_IN_CONNECT
)

install(TARGETS kmultipart  DESTINATION ${KDE_INSTALL_PLUGINDIR}/kf5/parts)
install(FILES kmultipart.desktop  DESTINATION  ${KDE_INSTALL_KSERVICES5DIR} )




