add_library(kura_sdk STATIC kura_c.cpp)
set_target_properties(kura_sdk PROPERTIES OUTPUT_NAME kura)
target_link_libraries(kura_sdk PUBLIC pdfa_core)
target_include_directories(kura_sdk PUBLIC ${CMAKE_SOURCE_DIR}/core/include)

if(PDFA_WITH_PDFIUM)
  target_link_libraries(kura_sdk PUBLIC kura_raster)
  target_compile_definitions(kura_sdk PRIVATE KURA_WITH_PDFIUM=1)
endif()
