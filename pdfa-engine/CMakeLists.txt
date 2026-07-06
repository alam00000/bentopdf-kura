cmake_minimum_required(VERSION 3.20)
project(pdfa_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PDFA_BUILD_CLI "Build the native CLI" ON)

add_subdirectory(core)
if(PDFA_BUILD_CLI AND NOT EMSCRIPTEN)
  add_subdirectory(cli)
endif()
if(EMSCRIPTEN)
  add_subdirectory(wasm)
endif()
