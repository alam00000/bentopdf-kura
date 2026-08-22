cmake_minimum_required(VERSION 3.20)
project(pdfa_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PDFA_BUILD_CLI "Build the native CLI" ON)
option(PDFA_BUILD_FUZZ "Build the libFuzzer harness (needs clang -fsanitize=fuzzer)" OFF)
option(PDFA_BUILD_SDK "Build the Kura C SDK library" ON)
set(PDFA_WITH_PDFIUM "AUTO" CACHE STRING
    "Bundle PDFium for raster flattening: ON, OFF, or AUTO (use it when a build is present)")
option(PDFA_STATIC_DEPS "Link third-party libs statically (self-contained binary)" OFF)

if(PDFA_WITH_PDFIUM STREQUAL "AUTO")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium-build/dist")
    set(PDFA_WITH_PDFIUM ON)
  else()
    set(PDFA_WITH_PDFIUM OFF)
    message(STATUS
      "PDFium not found under third_party/pdfium-build/dist: building without raster "
      "flattening. Transparency at PDF/A-1 and PDF/X-1a/X-3 will be reported rather than "
      "flattened. Run scripts/pdfium/sync.sh && scripts/pdfium/build.sh <target> to enable "
      "it, or set -DPDFA_WITH_PDFIUM=ON to require it.")
  endif()
endif()

option(PDFA_WERROR "Treat compiler warnings as errors" OFF)
if(MSVC)
  add_compile_options(/W4 /utf-8)
  if(PDFA_WERROR)
    add_compile_options(/WX)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wunused
                      -Wimplicit-fallthrough -Wnull-dereference)
  if(PDFA_WERROR)
    add_compile_options(-Werror)
  endif()
endif()

add_subdirectory(core)
if(PDFA_WITH_PDFIUM)
  add_subdirectory(raster)
endif()
if(PDFA_BUILD_CLI AND NOT EMSCRIPTEN)
  add_subdirectory(cli)
endif()
if(PDFA_BUILD_SDK AND NOT EMSCRIPTEN)
  add_subdirectory(capi)
endif()
if(PDFA_BUILD_FUZZ AND NOT EMSCRIPTEN)
  add_subdirectory(fuzz)
endif()
if(EMSCRIPTEN)
  add_subdirectory(wasm)
endif()
