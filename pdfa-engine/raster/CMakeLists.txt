if(EMSCRIPTEN)
  set(KURA_PDFIUM_TARGET wasm)
elseif(WIN32)
  set(KURA_PDFIUM_TARGET win-x64)
elseif(APPLE)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(KURA_PDFIUM_TARGET mac-arm64)
  else()
    set(KURA_PDFIUM_TARGET mac-x64)
  endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(KURA_PDFIUM_TARGET linux-arm64)
else()
  set(KURA_PDFIUM_TARGET linux-x64)
endif()

set(KURA_PDFIUM_DIR
    "${CMAKE_SOURCE_DIR}/third_party/pdfium-build/dist/${KURA_PDFIUM_TARGET}"
    CACHE PATH "prebuilt static PDFium for this target")

if(WIN32)
  set(KURA_PDFIUM_LIB "${KURA_PDFIUM_DIR}/pdfium.lib")
  if(NOT EXISTS "${KURA_PDFIUM_LIB}")
    set(KURA_PDFIUM_LIB "${KURA_PDFIUM_DIR}/libpdfium.a")
  endif()
else()
  set(KURA_PDFIUM_LIB "${KURA_PDFIUM_DIR}/libpdfium.a")
endif()

if(NOT EXISTS "${KURA_PDFIUM_LIB}")
  message(FATAL_ERROR
    "PDFA_WITH_PDFIUM is ON but no PDFium was found for ${KURA_PDFIUM_TARGET}.\n"
    "Build it with:\n"
    "  ./scripts/pdfium/sync.sh && ./scripts/pdfium/build.sh ${KURA_PDFIUM_TARGET}\n"
    "or point -DKURA_PDFIUM_DIR at an existing build, or configure with "
    "-DPDFA_WITH_PDFIUM=OFF to build without raster flattening.")
endif()

add_library(kura_raster STATIC kura_raster.cpp)

target_include_directories(kura_raster
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${KURA_PDFIUM_DIR}/include)

target_link_libraries(kura_raster PUBLIC ${KURA_PDFIUM_LIB})

if(APPLE)
  target_link_libraries(kura_raster PUBLIC
    "-framework CoreFoundation" "-framework CoreGraphics" "-framework AppKit")
endif()
