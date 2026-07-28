set(PDFA_TP ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

if(NOT EMSCRIPTEN)
  find_package(qpdf CONFIG REQUIRED)
  find_package(Freetype REQUIRED)
  find_package(OpenJPEG CONFIG REQUIRED PATHS /opt/homebrew/opt/openjpeg/lib/cmake)
  find_package(JPEG REQUIRED)
  find_package(ZLIB REQUIRED)
  find_library(LCMS2_LIBRARY NAMES lcms2 PATHS /opt/homebrew/lib /usr/local/lib)
  find_path(LCMS2_INCLUDE_DIR lcms2.h PATHS /opt/homebrew/include /usr/local/include)
  if(NOT LCMS2_LIBRARY OR NOT LCMS2_INCLUDE_DIR)
    message(FATAL_ERROR "lcms2 not found (brew install little-cms2)")
  endif()
endif()

add_library(pdfa_core STATIC
  src/engine.cpp
  src/sanitize.cpp
  src/pages.cpp
  src/color.cpp
  src/colorx.cpp
  src/printx.cpp
  src/fonts.cpp
  src/fonts_glyph.cpp
  src/xmp.cpp
  src/limits.cpp
  src/tagging.cpp
  src/images.cpp
  src/jpx.cpp
  src/dct.cpp
  src/encodings.cpp
  assets/srgb_icc.cpp
  assets/fonts_data.cpp
  assets/agl_names.cpp
  assets/cmyk_icc.cpp
)

target_include_directories(pdfa_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

if(EMSCRIPTEN)
  add_library(qpdf_wasm STATIC IMPORTED)
  set_target_properties(qpdf_wasm PROPERTIES
    IMPORTED_LOCATION ${PDFA_TP}/build-qpdf-wasm/libqpdf/libqpdf.a)
  add_library(openjp2_wasm STATIC IMPORTED)
  set_target_properties(openjp2_wasm PROPERTIES
    IMPORTED_LOCATION ${PDFA_TP}/build-openjpeg-wasm/bin/libopenjp2.a)
  add_library(lcms2_wasm STATIC IMPORTED)
  set_target_properties(lcms2_wasm PROPERTIES
    IMPORTED_LOCATION ${PDFA_TP}/build-lcms2-wasm/src/liblcms2.a)

  target_include_directories(pdfa_core PRIVATE
    ${PDFA_TP}/qpdf-12.3.2/include
    ${PDFA_TP}/build-qpdf-wasm/include
    ${PDFA_TP}/openjpeg-2.5.4/src/lib/openjp2
    ${PDFA_TP}/build-openjpeg-wasm/src/lib/openjp2
    ${PDFA_TP}/lcms2/include
  )
  target_compile_options(pdfa_core PUBLIC -fwasm-exceptions)
  target_compile_options(pdfa_core PRIVATE "SHELL:-s USE_FREETYPE=1" "SHELL:-s USE_LIBJPEG=1" "SHELL:-s USE_ZLIB=1")
  target_link_libraries(pdfa_core PUBLIC qpdf_wasm openjp2_wasm lcms2_wasm)
  target_link_options(pdfa_core INTERFACE
    -fwasm-exceptions "SHELL:-s USE_FREETYPE=1" "SHELL:-s USE_LIBJPEG=1" "SHELL:-s USE_ZLIB=1")
else()
  target_include_directories(pdfa_core PRIVATE ${LCMS2_INCLUDE_DIR})
  if(PDFA_STATIC_DEPS)
    foreach(dep qpdf freetype openjp2 jpeg lcms2 png crypto)
      find_library(PDFA_A_${dep} NAMES lib${dep}.a
        HINTS /opt/homebrew/lib /opt/homebrew/opt/qpdf/lib
              /opt/homebrew/opt/freetype/lib /opt/homebrew/opt/openjpeg/lib
              /opt/homebrew/opt/jpeg-turbo/lib /opt/homebrew/opt/little-cms2/lib
              /opt/homebrew/opt/libpng/lib /opt/homebrew/opt/openssl@3/lib
              ${CMAKE_PREFIX_PATH} $ENV{CMAKE_PREFIX_PATH})
      if(NOT PDFA_A_${dep})
        message(FATAL_ERROR
          "PDFA_STATIC_DEPS is ON but lib${dep}.a was not found. Install the "
          "static library, or configure without -DPDFA_STATIC_DEPS=ON.")
      endif()
    endforeach()
    foreach(tgt qpdf::libqpdf JPEG::JPEG openjp2)
      if(TARGET ${tgt})
        get_target_property(_inc ${tgt} INTERFACE_INCLUDE_DIRECTORIES)
        if(_inc)
          target_include_directories(pdfa_core PRIVATE ${_inc})
        endif()
      endif()
    endforeach()
    target_include_directories(pdfa_core PRIVATE ${FREETYPE_INCLUDE_DIRS})
    target_link_libraries(pdfa_core
      PUBLIC ${PDFA_A_qpdf}
      PRIVATE ${PDFA_A_freetype} ${PDFA_A_png} ${PDFA_A_openjp2}
              ${PDFA_A_jpeg} ${PDFA_A_lcms2} ${PDFA_A_crypto} ZLIB::ZLIB bz2)
  else()
    target_link_libraries(pdfa_core
      PUBLIC qpdf::libqpdf
      PRIVATE Freetype::Freetype openjp2 JPEG::JPEG ZLIB::ZLIB ${LCMS2_LIBRARY})
  endif()
endif()
