kde_enable_exceptions()

find_package(OpenSSL)
set_package_properties(OpenSSL PROPERTIES DESCRIPTION "Support for secure network communications (SSL and TLS)"
                       URL "https://openssl.org/"
                       TYPE RECOMMENDED
                       PURPOSE "KDE uses OpenSSL for the bulk of secure communications, including secure web browsing via HTTPS"
                      )
if(OPENSSL_FOUND)
   set(KSSL_HAVE_SSL 1)
   include_directories(${OPENSSL_INCLUDE_DIR})
endif()

# QuickJS provides the modern (ES2020) script engine behind the
# jsengine abstraction. Distributors place it in one of two layouts:
# headers/libs at the default search root, or under a quickjs/ subdir
# (MSYS2 ships include/quickjs/quickjs.h and lib/quickjs/libquickjs.a).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
   pkg_check_modules(QUICKJS QUIET quickjs)
endif()
if(NOT QUICKJS_FOUND)
   find_path(QUICKJS_INCLUDE_ROOT NAMES quickjs/quickjs.h quickjs.h
             PATH_SUFFIXES include)
   find_library(QUICKJS_LIB NAMES quickjs quickjs-ng
                PATH_SUFFIXES quickjs)
   if(QUICKJS_INCLUDE_ROOT AND QUICKJS_LIB)
      if(EXISTS "${QUICKJS_INCLUDE_ROOT}/quickjs/quickjs.h")
         set(QUICKJS_INCLUDE_DIRS "${QUICKJS_INCLUDE_ROOT}/quickjs")
      else()
         set(QUICKJS_INCLUDE_DIRS "${QUICKJS_INCLUDE_ROOT}")
      endif()
      set(QUICKJS_LIBRARIES "${QUICKJS_LIB}")
      set(QUICKJS_FOUND TRUE)
   endif()
endif()
set_package_properties(QUICKJS PROPERTIES DESCRIPTION "Small embeddable ES2020 JavaScript engine"
                       URL "https://bellard.org/quickjs/"
                       TYPE REQUIRED
                       PURPOSE "The only JavaScript engine used by this build")
if(QUICKJS_FOUND)
   include_directories(${QUICKJS_INCLUDE_DIRS})
   set(HAVE_QUICKJS 1)
   message(STATUS "Found QuickJS: ${QUICKJS_LIBRARIES} (headers in ${QUICKJS_INCLUDE_DIRS})")
endif()

configure_file(config-khtml.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-khtml.h )

set(khtmlgraphics_STAT_SRCS
  platform/graphics/AffineTransform.cpp
  platform/graphics/FloatPoint.cpp
  platform/graphics/FloatRect.cpp
  platform/graphics/FloatSize.cpp
  platform/graphics/IntRect.cpp
  platform/graphics/Path.cpp
  platform/graphics/GraphicsTypes.cpp
  platform/graphics/PathTraversalState.cpp
  #platform/graphics/Image.cpp
  #platform/MIMETypeRegistry.cpp
  platform/graphics/qt/AffineTransformQt.cpp
  platform/graphics/qt/FloatPointQt.cpp
  platform/graphics/qt/FloatRectQt.cpp
  platform/graphics/qt/IntPointQt.cpp
  platform/graphics/qt/IntRectQt.cpp
  platform/graphics/qt/IntSizeQt.cpp
  platform/graphics/qt/PathQt.cpp
  #platform/qt/MIMETypeRegistryQt.cpp
)

set(khtmlsvg_STAT_SRCS
  svg/SVGDocument.cpp
  svg/SVGSVGElement.cpp
  svg/SVGStyledLocatableElement.cpp
  svg/SVGLocatable.cpp
  svg/SVGStyledElement.cpp
  svg/SVGStylable.cpp
  svg/SVGElement.cpp
  svg/SVGLength.cpp
  svg/SVGTransformable.cpp
  svg/SVGTransform.cpp
  svg/SVGAngle.cpp
  svg/SVGDocumentExtensions.cpp
  svg/SVGParserUtilities.cpp
  svg/SVGTransformDistance.cpp
  svg/SVGTransformList.cpp
  svg/SVGStringList.cpp
  svg/SVGStyledTransformableElement.cpp
  svg/SVGTests.cpp
  svg/SVGLangSpace.cpp
  svg/SVGExternalResourcesRequired.cpp
  svg/SVGRectElement.cpp
  svg/SVGPreserveAspectRatio.cpp
  svg/SVGFitToViewBox.cpp
  svg/SVGCircleElement.cpp
  svg/SVGStyleElement.cpp
  svg/SVGEllipseElement.cpp
  svg/SVGPointList.cpp
  svg/SVGAnimatedPoints.cpp
  svg/SVGPolyElement.cpp
  svg/SVGPolygonElement.cpp
  svg/SVGPolylineElement.cpp
  svg/SVGAnimatedPathData.cpp
  svg/SVGPathSegArc.cpp
  svg/SVGPathSegClosePath.cpp
  svg/SVGPathSegCurvetoCubic.cpp
  svg/SVGPathSegCurvetoCubicSmooth.cpp
  svg/SVGPathSegCurvetoQuadratic.cpp
  svg/SVGPathSegCurvetoQuadraticSmooth.cpp
  svg/SVGPathSegLineto.cpp
  svg/SVGPathSegLinetoHorizontal.cpp
  svg/SVGPathSegLinetoVertical.cpp
  svg/SVGPathSegList.cpp
  svg/SVGPathSegMoveto.cpp
  svg/SVGPathElement.cpp
  svg/SVGURIReference.cpp
  svg/SVGStopElement.cpp
  svg/SVGGradientElement.cpp
  svg/SVGLinearGradientElement.cpp
  svg/SVGRadialGradientElement.cpp
  svg/SVGDefsElement.cpp
  svg/SVGClipPathElement.cpp
  svg/SVGGElement.cpp
  svg/SVGElementInstance.cpp
  svg/SVGElementInstanceList.cpp
  svg/SVGUseElement.cpp
  svg/SVGLineElement.cpp
  svg/SVGTextPathElement.cpp
  svg/SVGTextContentElement.cpp
  svg/SVGNumberList.cpp
  svg/SVGTextPositioningElement.cpp
  svg/SVGLengthList.cpp
  svg/SVGTextElement.cpp
  svg/SVGAElement.cpp
  svg/SVGScriptElement.cpp
  svg/SVGTitleElement.cpp
  svg/SVGDescElement.cpp
  svg/SVGNames.cpp
  svg/SVGZoomAndPan.cpp
  svg/SVGViewSpec.cpp
  svg/SVGTSpanElement.cpp
  svg/SVGGlyphElement.cpp
  svg/SVGAltGlyphElement.cpp
  svg/SVGHKernElement.cpp
  svg/SVGMissingGlyphElement.cpp
  svg/SVGFontElement.cpp
  svg/SVGFontData.cpp
#############################################################
## GRAPHICS #################################################
#############################################################
  svg/graphics/SVGResource.cpp
  svg/graphics/SVGPaintServer.cpp
  svg/graphics/qt/SVGPaintServerQt.cpp
  svg/graphics/SVGPaintServerSolid.cpp
  svg/graphics/qt/SVGPaintServerSolidQt.cpp
  svg/graphics/SVGResourceClipper.cpp
  svg/graphics/qt/SVGResourceClipperQt.cpp
  svg/graphics/SVGPaintServerGradient.cpp
  svg/graphics/SVGPaintServerLinearGradient.cpp
  svg/graphics/SVGPaintServerRadialGradient.cpp
  svg/graphics/qt/SVGPaintServerGradientQt.cpp
  svg/graphics/qt/SVGPaintServerLinearGradientQt.cpp
  svg/graphics/qt/SVGPaintServerRadialGradientQt.cpp
#############################################################
## RENDERING PART BELOW   ###################################
#############################################################
  rendering/RenderSVGRoot.cpp
  rendering/RenderPath.cpp
  rendering/SVGRenderStyleDefs.cpp
  rendering/SVGRenderStyle.cpp
  rendering/RenderSVGGradientStop.cpp
  rendering/RenderSVGContainer.cpp
  rendering/SVGRenderSupport.cpp
  rendering/RenderSVGHiddenContainer.cpp
  rendering/RenderSVGTransformableContainer.cpp
  rendering/SVGInlineFlowBox.cpp
  rendering/SVGRootInlineBox.cpp
  rendering/RenderSVGTextPath.cpp
  rendering/SVGInlineTextBox.cpp
  rendering/SVGCharacterLayoutInfo.cpp
  rendering/RenderSVGInline.cpp
  rendering/RenderSVGText.cpp
  rendering/RenderSVGBlock.cpp
  rendering/RenderSVGInlineText.cpp
  rendering/RenderSVGTSpan.cpp
)

# khtml/dom/Makefile.am: khtmldom

set(khtmldom_STAT_SRCS
  dom/dom_misc.cpp
  dom/html_block.cpp
  dom/html_inline.cpp
  dom/css_rule.cpp
  dom/dom_node.cpp
  dom/html_document.cpp
  dom/html_list.cpp
  dom/css_stylesheet.cpp
  dom/dom_string.cpp
  dom/html_element.cpp
  dom/html_misc.cpp
  dom/css_value.cpp
  dom/dom_text.cpp
  dom/html_object.cpp
  dom/dom_doc.cpp
  dom/dom_xml.cpp
  dom/html_head.cpp
  dom/dom_element.cpp
  dom/html_base.cpp
  dom/html_image.cpp
  dom/html_form.cpp
  dom/dom2_range.cpp
  dom/html_table.cpp
  dom/dom2_traversal.cpp
  dom/dom2_events.cpp
  dom/dom2_views.cpp
  dom/QualifiedName.cpp
  dom/dom3_xpath.cpp
)

#set_source_files_properties(${khtmldom_STAT_SRCS} PROPERTIES COMPILE_FLAGS ${KDE_ENABLE_EXCEPTIONS})

# khtml/misc/Makefile.am: khtmlmisc

set(khtmlmisc_STAT_SRCS
  misc/loader.cpp
  misc/helper.cpp
  misc/arena.cpp
  misc/stringit.cpp
  misc/paintbuffer.cpp
  misc/imagefilter.cpp
  misc/borderarcstroker.cpp
  misc/idstring.cpp
  misc/htmlnames.cpp
  misc/AtomicString.cpp
  misc/woff.cpp
  misc/guess_ja.cpp
  misc/kencodingdetector.cpp
)

set(khtmlediting_STAT_SRCS
  editing/jsediting.cpp
  editing/editing.cpp
  editing/editor.cpp
  editing/htmlediting_impl.cpp
)

set(khtmlfind_STAT_SRCS
  ui/findbar/khtmlfind.cpp
  ui/findbar/khtmlfindbar.cpp
)

ki18n_wrap_ui(khtmlfind_STAT_SRCS
  ui/findbar/khtmlfindbar_base.ui
)

set(passwordbar_STAT_SRCS
  ui/passwordbar/storepassbar.cpp
)

ki18n_wrap_ui(passwordbar_STAT_SRCS
  ui/passwordbar/storepassbar_base.ui
)

# khtml/html/Makefile.am: khtmlhtml

set(khtmlhtml_STAT_SRCS
  html/htmlparser.cpp
  html/htmltokenizer.cpp
  html/htmlprospectivetokenizer.cpp
  html/dtd.cpp
  html/html_headimpl.cpp
  html/html_blockimpl.cpp
  html/html_elementimpl.cpp
  html/html_inlineimpl.cpp
  html/html_documentimpl.cpp
  html/html_baseimpl.cpp
  html/html_imageimpl.cpp
  html/html_listimpl.cpp
  html/html_miscimpl.cpp
  html/html_formimpl.cpp
  html/html_objectimpl.cpp
  html/html_tableimpl.cpp
  html/html_canvasimpl.cpp
  html/HTMLMediaElement.cpp
  html/HTMLAudioElement.cpp
  html/HTMLVideoElement.cpp
  html/HTMLSourceElement.cpp
  html/TimeRanges.cpp
  html/ksslkeygen.cpp
  html/kopenssl.cpp
  html/kentities.cpp
)
ecm_gperf_generate(html/doctypes.gperf ${CMAKE_CURRENT_BINARY_DIR}/doctypes.h khtmlhtml_STAT_SRCS
                   GENERATION_FLAGS "--key-positions=*")
ecm_gperf_generate(html/kentities.gperf ${CMAKE_CURRENT_BINARY_DIR}/kentities-gperf.h khtmlhtml_STAT_SRCS
                   GENERATION_FLAGS "--key-positions=* -D -s 2")

# khtml/ecma/Makefile.am: kjs_html
#
# The legacy KF5JS (KJS) interpreter and all of its generated DOM bindings
# have been removed. QuickJS is the only execution engine; the sources below
# are the engine abstraction, the DOM bridge, and the KJSProxy shim that
# routes inline handlers and programmatic script evaluation to QuickJS.

set(kjs_html_STAT_SRCS
  ecma/kjs_proxy.cpp
  ecma/quickjs_engine.cpp
  ecma/qjs_dom_bridge.cpp
)

# khtml/rendering/Makefile.am: khtmlrender

set(khtmlrender_STAT_SRCS
  rendering/bidi.cpp
  rendering/break_lines.cpp
  rendering/render_block.cpp
  rendering/render_flexbox.cpp
  rendering/render_inline.cpp
  rendering/render_style.cpp
  rendering/render_object.cpp
  rendering/render_container.cpp
  rendering/render_box.cpp
  rendering/render_flow.cpp
  rendering/render_text.cpp
  rendering/render_arena.cpp
  rendering/render_layer.cpp
  rendering/render_image.cpp
  rendering/render_table.cpp
  rendering/table_layout.cpp
  rendering/render_replaced.cpp
  rendering/render_form.cpp
  rendering/render_list.cpp
  rendering/render_canvas.cpp
  rendering/render_frames.cpp
  rendering/render_br.cpp
  rendering/render_body.cpp
  rendering/font.cpp
  rendering/render_line.cpp
  rendering/render_generated.cpp
  rendering/enumerate.cpp
  rendering/counter_tree.cpp
  rendering/render_canvasimage.cpp
  rendering/render_position.cpp
  rendering/render_media.cpp
  rendering/media_controls.cpp
  rendering/animation_controller.cpp
)

# khtml/css/Makefile.am: khtmlcss

set(khtmlcss_STAT_SRCS
  css/css_stylesheetimpl.cpp
  css/css_ruleimpl.cpp
  css/css_valueimpl.cpp
  css/css_svgvalueimpl.cpp
  css/css_base.cpp
  css/cssparser.cpp
  css/cssstyleselector.cpp
  css/csshelper.cpp
  css/parser.cpp
  css/css_renderstyledeclarationimpl.cpp
  css/css_mediaquery.cpp
  css/css_svgcssparser.cpp
  css/SVGCSSStyleSelector.cpp
  css/css_webfont.cpp
  css/css_animation.cpp
  css/css_transition.cpp
)

# khtml/xml/Makefile.am: khtmlxml

set(khtmlxml_STAT_SRCS
  xml/security_origin.cpp
  xml/dom_docimpl.cpp
  xml/dom_nodeimpl.cpp
  xml/dom_nodelistimpl.cpp
  xml/dom_textimpl.cpp
  xml/dom_elementimpl.cpp
  xml/dom_stringimpl.cpp
  xml/dom2_rangeimpl.cpp
  xml/dom2_traversalimpl.cpp
  xml/xml_tokenizer.cpp
  xml/dom_xmlimpl.cpp
  xml/dom2_eventsimpl.cpp
  xml/dom2_viewsimpl.cpp
  xml/dom_restyler.cpp
  xml/ClassNames.cpp
  xml/dom_position.cpp
  xml/dom_positioniterator.cpp
  xml/dom_selection.cpp
  xml/wa_selectors.cpp
  xml/dom3_xpathimpl.cpp
)

# khtml/imload/Makefile.am: khtmlimload

set(khtmlimload_STAT_SRCS
  imload/imageplane.cpp
  imload/rawimageplane.cpp
  imload/scaledimageplane.cpp
  imload/pixmapplane.cpp
  imload/animprovider.cpp
  imload/imagepainter.cpp
  imload/updater.cpp
  imload/image.cpp
  imload/imagemanager.cpp
  imload/animtimer.cpp
  imload/canvasimage.cpp
)

# khtml/imload/decoders/Makefile.am: decoders

set(decoders_STAT_SRCS
  imload/decoders/jpegloader.cpp
  imload/decoders/pngloader.cpp
  imload/decoders/gifloader.cpp
  imload/decoders/qimageioloader.cpp
)

# khtml/xpath/
set(xpath_STAT_SRCS
  xpath/expression.cpp
  xpath/functions.cpp
  xpath/parsedstatement.cpp
  xpath/path.cpp
  xpath/predicate.cpp
  xpath/step.cpp
  xpath/tokenizer.cpp
  xpath/parser.cpp
  xpath/util.cpp
  xpath/variablereference.cpp
#  xpath/XPathNSResolverImpl.cpp
)

add_subdirectory( css )

include_directories(
   ${PHONON_INCLUDE_DIR}
   ${KWindowSystem_INCLUDE_DIRS}
)

include_directories(
#   ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/misc
  ${CMAKE_CURRENT_SOURCE_DIR}/dom
  ${CMAKE_CURRENT_SOURCE_DIR}/xml
  ${CMAKE_CURRENT_SOURCE_DIR}/html
  ${CMAKE_CURRENT_SOURCE_DIR}/rendering
  ${CMAKE_CURRENT_SOURCE_DIR}/ecma
  ${CMAKE_CURRENT_SOURCE_DIR}/imload
  ${CMAKE_CURRENT_SOURCE_DIR}/imload/decoders
  ${CMAKE_CURRENT_SOURCE_DIR}/svg
  ${CMAKE_CURRENT_SOURCE_DIR}/svg/graphics
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/graphics
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/text
  ${CMAKE_CURRENT_SOURCE_DIR}/platform
  ${CMAKE_CURRENT_SOURCE_DIR}/compat
  ${CMAKE_CURRENT_SOURCE_DIR}/css
  ${CMAKE_CURRENT_SOURCE_DIR}/xpath
)
include_directories(
 ${CMAKE_CURRENT_BINARY_DIR}
# ${CMAKE_CURRENT_BINARY_DIR}/misc
# ${CMAKE_CURRENT_BINARY_DIR}/dom
# ${CMAKE_CURRENT_BINARY_DIR}/xml
# ${CMAKE_CURRENT_BINARY_DIR}/html
# ${CMAKE_CURRENT_BINARY_DIR}/rendering
# ${CMAKE_CURRENT_BINARY_DIR}/imload
# ${CMAKE_CURRENT_BINARY_DIR}/imload/decoders
# ${CMAKE_CURRENT_BINARY_DIR}/svg
# ${CMAKE_CURRENT_BINARY_DIR}/svg/graphics
# ${CMAKE_CURRENT_BINARY_DIR}/platform/graphics
# ${CMAKE_CURRENT_BINARY_DIR}/platform/text
# ${CMAKE_CURRENT_SOURCE_DIR}/platform
# ${CMAKE_CURRENT_BINARY_DIR}/compat
  ${CMAKE_CURRENT_BINARY_DIR}/css
)

include_directories(${JPEG_INCLUDE_DIR} ${GIF_INCLUDE_DIR} ${PNG_INCLUDE_DIR} )

set( KDELIBSUFF ${LIB_SUFFIX} )
configure_file(html/ksslconfig.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/ksslconfig.h )

add_subdirectory( kmultipart )

add_definitions(-DENABLE_VIDEO)

########### libkhtml ###############

set(khtml_LIB_SRCS
   khtmlview.cpp
   khtmlviewbar.cpp
   khtmlviewbarwidget.cpp
   khtml_part.cpp
   khtml_childframe.cpp
   khtml_run.cpp
   khtml_global.cpp
   khtml_settings.cpp
   khtml_filter.cpp
   khtml_events.cpp
   khtml_ext.cpp
   khtml_pagecache.cpp
   khtml_printsettings.cpp
   khtml_iface.cpp
   khtmlimage.cpp
   ${xpath_STAT_SRCS}
   ${khtmlfind_STAT_SRCS}
   ${passwordbar_STAT_SRCS}
   ${khtmlxml_STAT_SRCS}
   ${khtmlhtml_STAT_SRCS}
   ${khtmlrender_STAT_SRCS}
   ${khtmlcss_STAT_SRCS}
   ${khtmlmisc_STAT_SRCS}
   ${khtmlediting_STAT_SRCS}
   ${kjs_html_STAT_SRCS}
   ${khtmldom_STAT_SRCS}
   ${khtmlimload_STAT_SRCS}
   ${decoders_STAT_SRCS}
   ${khtmlgraphics_STAT_SRCS} # for WebCore compatibility
   ${khtmlsvg_STAT_SRCS} # for SVG
   #   ${CMAKE_CURRENT_BINARY_DIR}/org.kde.KHtmlPart.xml # just so that it gets generated
   )

# Fairly certain this isn't used anywhere
# qt_generate_dbus_interface(khtml_iface.h org.kde.KHtmlPart.xml)

qt_add_dbus_interface(khtml_LIB_SRCS ${KCookieServer_DBUS_INTERFACE} kcookiejar_interface)

ecm_qt_declare_logging_category(khtml_LIB_SRCS
    HEADER khtml_debug.h
    IDENTIFIER KHTML_LOG
    CATEGORY_NAME kf.khtml
    OLD_CATEGORY_NAMES kf5.khtml
    DESCRIPTION "khtml (lib)"
    EXPORT KHTML
)

ki18n_wrap_ui(khtml_LIB_SRCS
   htmlpageinfo.ui
   html/keygenwizard.ui
   html/keygenwizard2.ui
)

qt_add_resources( khtml_LIB_SRCS khtml.qrc )

add_library(KF5KHtml ${khtml_LIB_SRCS})
add_library(KF5::KHtml ALIAS KF5KHtml)

generate_export_header(KF5KHtml BASE_NAME KHtml)

# The pulled-in KF5 dependency targets propagate QT_NO_* strict source guards
# through their interface compile definitions. This Qt5-era engine predates
# those conventions (it uses the emit keyword and implicit ASCII QString
# conversions), so cancel the inherited guards on each KHTML target's own
# command line. Its private flags are emitted after the inherited -D entries,
# making the -U undefs win, without weakening any linked KF5 library itself.
set(KHTML_RELAX_QT_GUARDS
    -UQT_NO_KEYWORDS
    -UQT_NO_FOREACH
    -UQT_NO_SIGNALS_SLOTS_KEYWORDS
    -UQT_NO_CAST_FROM_ASCII
    -UQT_NO_CAST_TO_ASCII
    -UQT_NO_CAST_FROM_BYTEARRAY
    -UQT_NO_URL_CAST_FROM_STRING
    -UQT_NO_NARROWING_CONVERSIONS_IN_CONNECT
)
target_compile_options(KF5KHtml PRIVATE ${KHTML_RELAX_QT_GUARDS})

target_include_directories(KF5KHtml INTERFACE "$<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR_KF5}/KHtml>")

target_link_libraries(KF5KHtml
  PUBLIC
    Qt5::Gui
    KF5::KIOCore
    KF5::I18n
    KF5::Parts
    KF5::TextWidgets
    KF5::Codecs
  PRIVATE
    ${PHONON_LIBRARIES}
    ${JPEG_LIBRARIES}
    ${GIF_LIBRARIES}
    ${PNG_LIBRARIES}
    Qt5::PrintSupport
    KF5::Archive
    KF5::SonnetCore
    KF5::WidgetsAddons
    KF5::WindowSystem
    KF5::Wallet
    KF5::IconThemes
    KF5::Notifications
    KF5::Bookmarks
    KF5::KIOWidgets # KUrlRequester
)
if(QUICKJS_FOUND)
  target_link_libraries(KF5KHtml PRIVATE ${QUICKJS_LIBRARIES})
endif()
if(NOT WIN32 AND NOT APPLE AND HAVE_X11)
  target_link_libraries(KF5KHtml PRIVATE ${X11_LIBRARIES} Qt5::X11Extras)
endif()

if(TARGET KF5::GlobalAccel)
  target_link_libraries(KF5KHtml PRIVATE KF5::GlobalAccel)
endif()

set_target_properties(KF5KHtml PROPERTIES VERSION ${KHTML_VERSION}
                                          SOVERSION ${KHTML_SOVERSION}
                                          EXPORT_NAME KHtml
                     )


# The legacy KJS hash-table (create_lut) and Perl generate-bindings.pl
# steps have been removed along with KF5JS. QuickJS is the only engine.


########### khtmlpart ###############

set(khtmlpart_PART_SRCS khtml_factory.cpp )


add_library(khtmlpart MODULE ${khtmlpart_PART_SRCS})
kcoreaddons_desktop_to_json(khtmlpart khtml.desktop)

target_link_libraries(khtmlpart KF5::KHtml KF5::XmlGui KF5::TextWidgets KF5::Parts KF5::I18n)
target_compile_options(khtmlpart PRIVATE ${KHTML_RELAX_QT_GUARDS})

ecm_generate_headers(KHtml_HEADERS
  HEADER_NAMES
  KHTMLPart
  KHTMLView
  KHTMLSettings

  REQUIRED_HEADERS KHtml_HEADERS
)

install(TARGETS khtmlpart  DESTINATION ${KDE_INSTALL_PLUGINDIR}/kf5/parts)


########### next target ###############

# Note that khtmlimage.cpp is part of libkhtml because it uses internal objects (render tree and loader)
# Only the entry point is separated into khtmlimage_init.cpp
add_library(khtmlimagepart MODULE khtmlimage_init.cpp)

kcoreaddons_desktop_to_json(khtmlimagepart khtmlimage.desktop)

target_link_libraries(khtmlimagepart KF5::KHtml KF5::WidgetsAddons KF5::XmlGui KF5::TextWidgets KF5::Parts KF5::I18n)
target_compile_options(khtmlimagepart PRIVATE ${KHTML_RELAX_QT_GUARDS})

install(TARGETS khtmlimagepart  DESTINATION ${KDE_INSTALL_PLUGINDIR}/kf5/parts)

########### next target ###############

add_library(khtmladaptorpart MODULE khtmladaptorpart.cpp)

target_link_libraries(khtmladaptorpart KF5::Parts KF5::I18n KF5::XmlGui)
target_compile_options(khtmladaptorpart PRIVATE ${KHTML_RELAX_QT_GUARDS})

install(TARGETS khtmladaptorpart DESTINATION ${KDE_INSTALL_PLUGINDIR}/kf5/parts)


########### install files ###############

install(TARGETS KF5KHtml EXPORT KF5KHtmlTargets ${KF5_INSTALL_TARGETS_DEFAULT_ARGS})

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/khtml_export.h
  ${CMAKE_CURRENT_BINARY_DIR}/khtml_debug.h
  khtml_part.h
  khtml_events.h
  khtml_settings.h
  khtmldefaults.h
  misc/kencodingdetector.h
  ${KHtml_HEADERS}
  DESTINATION ${KDE_INSTALL_INCLUDEDIR_KF5}/KHtml COMPONENT Devel
)

install(FILES
  dom/dom_exception.h
  dom/html_base.h
  dom/html_inline.h
  dom/css_rule.h
  dom/dom_html.h
  dom/html_block.h
  dom/html_list.h
  dom/css_stylesheet.h
  dom/dom_misc.h
  dom/html_document.h
  dom/html_misc.h
  dom/css_value.h
  dom/dom_node.h
  dom/html_element.h
  dom/html_object.h
  dom/dom_core.h
  dom/dom_string.h
  dom/html_form.h
  dom/html_table.h
  dom/dom_doc.h
  dom/dom_text.h
  dom/html_head.h
  dom/dom_element.h
  dom/dom_xml.h
  dom/html_image.h
  dom/dom2_range.h
  dom/dom2_traversal.h
  dom/dom2_events.h
  dom/dom2_views.h
  DESTINATION ${KDE_INSTALL_INCLUDEDIR_KF5}/KHtml/dom COMPONENT Devel
)

ecm_qt_install_logging_categories(
    EXPORT KHTML
    FILE khtml.categories
    DESTINATION ${KDE_INSTALL_LOGGINGCATEGORIESDIR}
)

install(FILES khtml.desktop khtmlimage.desktop
         khtmladaptorpart.desktop DESTINATION  ${KDE_INSTALL_KSERVICES5DIR} )

# install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kde.KHtmlPart.xml DESTINATION ${KDE_INSTALL_DBUSINTERFACEDIR} )

install(FILES error.html DESTINATION ${KDE_INSTALL_DATADIR_KF5}/khtml )

install(FILES khtmlrc DESTINATION ${KDE_INSTALL_CONFDIR} )

include(ECMGeneratePriFile)
ecm_generate_pri_file(BASE_NAME KHtml LIB_NAME KF5KHtml DEPS "gui KIOCore KI18n KParts" FILENAME_VAR PRI_FILENAME INCLUDE_INSTALL_DIR ${KDE_INSTALL_INCLUDEDIR_KF5}/KHtml)
install(FILES ${PRI_FILENAME} DESTINATION ${ECM_MKSPECS_INSTALL_DIR})
